/*
 * Copyright (C) 2012  Leo Singer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "bayestar_sky_map.h"

#include <complex.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include <lal/DetResponse.h>
#include <lal/LALSimulation.h>

#include <chealpix.h>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_integration.h>
#include <gsl/gsl_sf_bessel.h>
#include <gsl/gsl_sort_vector_double.h>
#include <gsl/gsl_statistics_double.h>
#include <gsl/gsl_vector.h>


static double square(double a)
{
    return a * a;
}


/* Copied from lal's TimeDelay.c */
/* scalar product of two 3-vectors */
static double dotprod(const double vec1[3], const double vec2[3])
{
    return vec1[0] * vec2[0] + vec1[1] * vec2[1] + vec1[2] * vec2[2];
}


/* Exponentiate and normalize a log probability sky map. */
static int exp_normalize(long npix, double *P)
{
    long i;
    double accum, max_log_p;

    /* Sort pixel indices from greatest log probability. */
    gsl_permutation *pix_perm = gsl_permutation_alloc(npix);
    if (!pix_perm)
        GSL_ERROR("failed to allocate space for permutation data", GSL_ENOMEM);
    gsl_vector_view P_vector = gsl_vector_view_array(P, npix);
    gsl_sort_vector_index(pix_perm, &P_vector.vector);
    gsl_permutation_reverse(pix_perm);

    /* Find the value of the greatest log probability. */
    max_log_p = P[gsl_permutation_get(pix_perm, 0)];

    /* Subtract it off. */
    for (i = 0; i < npix; i ++)
        P[i] -= max_log_p;

    /* Exponentiate to convert from log probability to probability. */
    for (i = 0; i < npix; i ++)
        P[i] = exp(P[i]);

    /* Sum entire sky map to find normalization. */
    for (accum = 0, i = 0; i < npix; i ++)
        accum += P[gsl_permutation_get(pix_perm, i)];

    /* Normalize. */
    for (i = 0; i < npix; i ++)
        P[i] /= accum;

    /* Free permutation. */
    gsl_permutation_free(pix_perm);

    /* Done! */
    return GSL_SUCCESS;
}


/* Perform sky localization based on TDOAs alone. Returns log probability; not normalized. */
static int bayestar_sky_map_tdoa_not_normalized_log(
    long npix, /* Input: number of HEALPix pixels. */
    double *P, /* Output: pre-allocated array of length npix to store posterior map. */
    double gmst, /* Greenwich mean sidereal time in radians. */
    int nifos, /* Input: number of detectors. */
    const double **locs, /* Input: array of detector positions. */
    const double *toas, /* Input: array of times of arrival. */
    const double *s2_toas /* Input: uncertainties in times of arrival. */
) {
    double t[nifos], w[nifos];
    long nside;
    long i;

    /* Determine the lateral HEALPix resolution. */
    nside = npix2nside(npix);
    if (nside < 0)
        GSL_ERROR("output is not a valid HEALPix array", GSL_EINVAL);

    /* Take reciprocal of measurement variances to get sum-of-squares weights. */
    for (i = 0; i < nifos; i ++)
        w[i] = 1 / s2_toas[i];

    /* Subtract off zeroth TOA so that when we compare these with the expected
     * time delays we are subtracting numbers of similar size (rather than
     * subtracting gigaseconds from milliseconds). In exact arithmetic, this
     * would not affect the final answer. */
    for (i = 0; i < nifos; i ++)
        t[i] = toas[i] - toas[0];

    /* Loop over pixels. */
    for (i = 0; i < npix; i ++)
    {
        int j;

        /* Determine polar coordinates of this pixel. */
        double theta, phi;
        pix2ang_ring(nside, i, &theta, &phi);

        /* Convert from equatorial to geographic coordinates. */
        phi -= gmst;

        /* Convert to Cartesian coordinates. */
        const double n[] = {sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta)};

        /* Loop over detectors. */
        double dt[nifos];
        for (j = 0; j < nifos; j ++)
            dt[j] = t[j] + dotprod(n, locs[j]) / LAL_C_SI;

        /* Evaluate the (un-normalized) Gaussian log likelihood. */
        P[i] = -0.5 * gsl_stats_wtss(w, 1, dt, 1, nifos);
    }

    /* Done! */
    return GSL_SUCCESS;
}


/* Perform sky localization based on TDOAs alone. */
int bayestar_sky_map_tdoa(
    long npix, /* Input: number of HEALPix pixels. */
    double *P, /* Output: pre-allocated array of length npix to store posterior map. */
    double gmst, /* Greenwich mean sidereal time in radians. */
    int nifos, /* Input: number of detectors. */
    const double **locs, /* Input: array of detector positions. */
    const double *toas, /* Input: array of times of arrival. */
    const double *s2_toas /* Input: uncertainties in times of arrival. */
) {
    int ret = bayestar_sky_map_tdoa_not_normalized_log(npix, P, gmst, nifos, locs, toas, s2_toas);
    if (ret == GSL_SUCCESS)
        ret = exp_normalize(npix, P);
    return ret;
}


typedef struct {
    double A;
    double B;
    double log_offset;
} inner_integrand_params;


/* Radial integrand for uniform-in-log-distance prior. */
static double radial_integrand_uniform_in_log_distance(double log_r, void *params)
{
    const inner_integrand_params *integrand_params = (const inner_integrand_params *) params;

    const double onebyr = exp(-log_r);
    const double onebyr2 = onebyr * onebyr;
    return exp(integrand_params->A * onebyr2 + integrand_params->B * onebyr - integrand_params->log_offset);
}


/* Radial integrand for uniform-in-volume prior. */
static double radial_integrand_uniform_in_volume(double log_r, void *params)
{
    const inner_integrand_params *integrand_params = (const inner_integrand_params *) params;

    const double onebyr = exp(-log_r);
    const double onebyr2 = onebyr * onebyr;
    return exp(integrand_params->A * onebyr2 + integrand_params->B * onebyr - integrand_params->log_offset + 3 * log_r);
}


int bayestar_sky_map_tdoa_snr(
    long npix, /* Input: number of HEALPix pixels. */
    double *P, /* Output: pre-allocated array of length npix to store posterior map. */
    double gmst, /* Greenwich mean sidereal time in radians. */
    int nifos, /* Input: number of detectors. */
    const float **responses, /* Pointers to detector responses. */
    const double **locations, /* Pointers to locations of detectors in Cartesian geographic coordinates. */
    const double *toas, /* Input: array of times of arrival with arbitrary relative offset. (Make toas[0] == 0.) */
    const double complex *snrs, /* Input: array of SNRs. */
    const double *s2_toas, /* Measurement variance of TOAs. */
    const double *horizons, /* Distances at which a source would produce an SNR of 1 in each detector. */
    double min_distance,
    double max_distance,
    bayestar_prior_t prior)
{
    long nside;
    long maxpix;
    long i;
    double d1[nifos];
    gsl_permutation *pix_perm;

    /* Function pointer to hold radial integrand. */
    double (* radial_integrand) (double x, void *params);

    /* Will point to memory for storing GSL return values for each thread. */
    int *gsl_errnos;

    /* Storage for old GSL error handler. */
    gsl_error_handler_t *old_handler;

    /* Maximum number of subdivisions for adaptive integration. */
    static const size_t subdivision_limit = 64;

    /* Subdivide radial integral where likelihood is this fraction of the maximum,
     * will be used in solving the quadratic to find the breakpoints */
    static const double eta = 0.01;

    /* Use this many integration samples in 2*psi  */
    static const int ntwopsi = 16;

    /* Number of integration steps in cosine integration */
    static const int nu = 16;

    /* Determine the lateral HEALPix resolution. */
    nside = npix2nside(npix);
    if (nside < 0)
        GSL_ERROR("output is not a valid HEALPix array", GSL_EINVAL);

    /* Choose radial integrand function based on selected prior. */
    switch (prior)
    {
        case BAYESTAR_PRIOR_UNIFORM_IN_LOG_DISTANCE:
            radial_integrand = radial_integrand_uniform_in_log_distance;
            break;
        case BAYESTAR_PRIOR_UNIFORM_IN_VOLUME:
            radial_integrand = radial_integrand_uniform_in_volume;
            break;
        default:
            GSL_ERROR("unrecognized choice of prior", GSL_EINVAL);
            break;
    }

    /* Rescale distances so that furthest horizon distance is 1. */
    {
        double d1max;
        memcpy(d1, horizons, sizeof(d1));
        for (d1max = d1[0], i = 1; i < nifos; i ++)
            if (d1[i] > d1max)
                d1max = d1[i];
        for (i = 0; i < nifos; i ++)
            d1[i] /= d1max;
        min_distance /= d1max;
        max_distance /= d1max;
    }

    /* Evaluate posterior term only first. */
    {
        int ret = bayestar_sky_map_tdoa_not_normalized_log(npix, P, gmst, nifos, locations, toas, s2_toas);
        if (ret != GSL_SUCCESS)
            return ret;
    }

    /* Allocate temporary spaces. */
    pix_perm = gsl_permutation_alloc(npix);
    if (!pix_perm)
        GSL_ERROR("failed to allocate space for permutation data", GSL_ENOMEM);

    /* Sort pixel indices by ascending significance. */
    {
        gsl_vector_view P_vector = gsl_vector_view_array(P, npix);
        gsl_sort_vector_index(pix_perm, &P_vector.vector);
    }

    /* Reverse the permutation so that the pixel indices are sorted by
     * descending significance. */
    gsl_permutation_reverse(pix_perm);

    /* Find the number of pixels needed to account for 99.99% of the posterior
     * conditioned on TDOAs. */
    {
        double accum, Ptotal;
        for (Ptotal = 0, i = 0; i < npix; i ++)
            Ptotal += exp(P[gsl_permutation_get(pix_perm, i)]);
        for (accum = 0, maxpix = 0; maxpix < npix && accum <= 0.9999 * Ptotal; maxpix ++)
            accum += exp(P[gsl_permutation_get(pix_perm, maxpix)]);
    }

    /* Zero pixels that didn't meet the TDOA cut. */
    for (i = maxpix; i < npix; i ++)
    {
        long ipix = gsl_permutation_get(pix_perm, i);
        P[ipix] = -INFINITY;
    }

    /* Allocate space to store per-pixel, per-thread error value. */
    gsl_errnos = calloc(maxpix, sizeof(int));
    if (!gsl_errnos)
    {
        gsl_permutation_free(pix_perm);
        GSL_ERROR("failed to allocate space for pixel error status", GSL_ENOMEM);
    }

    /* Turn off error handler while in parallel section to avoid concurrent
     * calls to the GSL error handler, which if provided by the user may not
     * be threadsafe. */
    old_handler = gsl_set_error_handler_off();

    /* Compute posterior factor for amplitude consistency. */
    #pragma omp parallel for
    for (i = 0; i < maxpix; i ++)
    {
        long ipix = gsl_permutation_get(pix_perm, i);
        double F[nifos][2];
        double theta, phi;
        int itwopsi, iu, iifo;
        double accum = -INFINITY;

        /* Prepare workspace for adaptive integrator. */
        gsl_integration_workspace *workspace = gsl_integration_workspace_alloc(subdivision_limit);

        /* If the workspace could not be allocated, then record the GSL
         * error value for later reporting when we leave the parallel
         * section. Then, skip to the next loop iteration. */
        if (!workspace)
        {
            gsl_errnos[i] = GSL_ENOMEM;
            continue;
        }

        /* Look up polar coordinates of this pixel */
        pix2ang_ring(nside, ipix, &theta, &phi);

        /* Look up antenna factors */
        for (iifo = 0; iifo < nifos; iifo ++)
        {
            XLALComputeDetAMResponse(&F[iifo][0], &F[iifo][1], responses[iifo], phi, M_PI_2 - theta, 0, gmst);
            F[iifo][0] *= d1[iifo];
            F[iifo][1] *= d1[iifo];
        }

        /* Integrate over 2*psi */
        for (itwopsi = 0; itwopsi < ntwopsi; itwopsi++)
        {
            const double twopsi = (2 * M_PI / ntwopsi) * itwopsi;
            const double costwopsi = cos(twopsi);
            const double sintwopsi = sin(twopsi);
            int iu;

            /* Integrate over u; since integrand only depends on u^2 we only have to go fro u=0 to u=1. We want to include u=1, so the upper limit has to be <=*/
            for (iu = 0; iu <= nu; iu++)
            {
                const double u = (double)iu / nu;
                const double u2 = u * u;
                const double u4 = u2 * u2;

                /* A and B come from solving A/r^2 + B/r + C = ln(eta) */
                double A=0, B=0; /* A is SNR times the distance and is strictly negative */
                double breakpoints[5];
                int num_breakpoints = 0;

                /* Loop over detectors */
                for (iifo = 0; iifo < nifos; iifo++)
                {
                    const double Fp = F[iifo][0]; /* F plus antenna factor times r */
                    const double Fx = F[iifo][1]; /* F cross antenna factor times r */
                    const double FpFp = square(Fp);
                    const double FxFx = square(Fx);
                    const double FpFx = Fp * Fx;
                    const double rhotimesr2 = 0.125 * ((FpFp + FxFx) * (1 + 6*u2 + u4) + square(1 - u2) * ((FpFp - FxFx) * costwopsi + 2 * FpFx * sintwopsi));
                    const double rhotimesr = sqrt(rhotimesr2);

                    A += rhotimesr2;
                    B += rhotimesr * cabs(snrs[iifo]);
                }
                A *= -0.5;

                {
                    int i_breakpoint;
                    const double middle_breakpoint = -2 * A / B;
                    const double lower_breakpoint = 1 / (1 / middle_breakpoint + sqrt(log(eta) / A));
                    const double upper_breakpoint = 1 / (1 / middle_breakpoint - sqrt(log(eta) / A));
                    breakpoints[num_breakpoints++] = min_distance;
                    if(lower_breakpoint > breakpoints[num_breakpoints-1] && lower_breakpoint < max_distance)
                        breakpoints[num_breakpoints++] = lower_breakpoint;
                    if(middle_breakpoint > breakpoints[num_breakpoints-1] && middle_breakpoint < max_distance)
                        breakpoints[num_breakpoints++] = middle_breakpoint;
                    if(upper_breakpoint > breakpoints[num_breakpoints-1] && upper_breakpoint < max_distance)
                        breakpoints[num_breakpoints++] = upper_breakpoint;
                    breakpoints[num_breakpoints++] = max_distance;

                    for (i_breakpoint = 0; i_breakpoint < num_breakpoints; i_breakpoint ++)
                    {
                        breakpoints[i_breakpoint] = log(breakpoints[i_breakpoint]);
                    }
                }

                {
                    /* Perform adaptive integration. Stop when a relative
                     * accuracy of 0.05 has been reached. */
                    inner_integrand_params integrand_params = {A, B, -0.25 * square(B) / A};
                    const gsl_function func = {radial_integrand, &integrand_params};
                    double result, abserr;
                    int ret = gsl_integration_qagp(&func, &breakpoints[0], num_breakpoints, DBL_MIN, 0.05, subdivision_limit, workspace, &result, &abserr);

                    /* If the integrator failed, then record the GSL error
                     * value for later reporting when we leave the parallel
                     * section. Then, break out of the inner loop. */
                    if (ret != GSL_SUCCESS)
                    {
                        gsl_errnos[i] = ret;
                        break;
                    }

                    /* Take the logarithm and put the log-normalization back in. */
                    result = log(result) + integrand_params.log_offset;
                    if (result > -INFINITY) {
                        const double max_log_p = fmax(result, accum);
                        accum = log(exp(result - max_log_p) + exp(accum - max_log_p)) + max_log_p;
                    }
                }
            }
        }
        /* Discard workspace for adaptive integrator. */
        gsl_integration_workspace_free(workspace);

        /* Accumulate (log) posterior terms for SNR and TDOA. */
        P[ipix] += accum;
    }

    /* Restore old error handler. */
    gsl_set_error_handler(old_handler);

    /* Free permutation. */
    gsl_permutation_free(pix_perm);

    /* Check if there was an error in any thread evaluating any pixel. If there
     * was, raise the error and return. */
    for (i = 0; i < maxpix; i ++)
    {
        int gsl_errno = gsl_errnos[i];
        if (gsl_errno != GSL_SUCCESS)
        {
            free(gsl_errnos);
            GSL_ERROR(gsl_strerror(gsl_errno), gsl_errno);
        }
    }

    /* Discard array of GSL error values. */
    free(gsl_errnos);

    /* Exponentiate and normalize posterior. */
    return exp_normalize(npix, P);
}
