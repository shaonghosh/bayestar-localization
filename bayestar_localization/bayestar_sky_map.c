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

    for (i = 0; i < nifos; i ++)
        w[i] = 1 / s2_toas[i];
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


#define INTEGRAND_COUNT_NODES 15
#define INTEGRAND_COUNT_SAMPLES (INTEGRAND_COUNT_NODES * INTEGRAND_COUNT_NODES)


typedef struct {
    double a;
    double log_offset;
} inner_integrand_params;


/* Radial integrand for uniform-in-log-distance prior. */
static double radial_integrand_uniform_in_log_distance(double log_x, void *params)
{
    const inner_integrand_params *integrand_params = (const inner_integrand_params *) params;

    /* We'll need to divide by 1/x and also 1/x^2. We can save one division by
     * computing 1/x and then squaring it. */
    const double onebyx = exp(-log_x);
    const double onebyx2 = onebyx * onebyx;
    const double I0_arg = onebyx / integrand_params->a;
    return exp(I0_arg - 0.5 * onebyx2 - integrand_params->log_offset) * gsl_sf_bessel_I0_scaled(I0_arg);
}


/* Radial integrand for uniform-in-volume prior. */
static double radial_integrand_uniform_in_volume(double log_x, void *params)
{
    const inner_integrand_params *integrand_params = (const inner_integrand_params *) params;

    /* We'll need to divide by 1/x and also 1/x^2. We can save one division by
     * computing 1/x and then squaring it. */
    const double onebyx = exp(-log_x);
    const double onebyx2 = onebyx * onebyx;
    const double x3 = 1 / (onebyx2 * onebyx);
    const double I0_arg = onebyx / integrand_params->a;
    return exp(I0_arg - 0.5 * onebyx2 - integrand_params->log_offset) * gsl_sf_bessel_I0_scaled(I0_arg) * x3;
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

    /* Constants that determine how much of the peak in the likelihood to enclose in integration break points. */
    static const double y1 = 0.01, y2 = 0.005;
    const double upper_breakpoint_default = (log(y2) - sqrt(log(y1) * log(y2))) / (sqrt(-2 * log(y2)) * (log(y2) - log(y1)));

    /* Precalculate trigonometric that occur in the integrand. */
    double u4_6u2_1[INTEGRAND_COUNT_NODES];
    double u4_2u2_1[INTEGRAND_COUNT_NODES];
    double u3_u[INTEGRAND_COUNT_NODES];
    double cosines[INTEGRAND_COUNT_NODES];
    double sines[INTEGRAND_COUNT_NODES];
    for (i = -(INTEGRAND_COUNT_NODES / 2); i <= (INTEGRAND_COUNT_NODES / 2); i ++)
    {
        const double u = (double) i / (INTEGRAND_COUNT_NODES / 2);
        const double u2 = u * u;
        const double u3 = u2 * u;
        const double u4 = u3 * u;
        const double angle = M_PI * u;
        u4_6u2_1[i + (INTEGRAND_COUNT_NODES / 2)] = u4 + 6 * u2 + 1;
        u4_2u2_1[i + (INTEGRAND_COUNT_NODES / 2)] = u4 - 2 * u2 + 1;
        u3_u[i + (INTEGRAND_COUNT_NODES / 2)] = u3 + u;
        cosines[i + (INTEGRAND_COUNT_NODES / 2)] = cos(angle);
        sines[i + (INTEGRAND_COUNT_NODES / 2)] = sin(angle);
    }

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

        /* Pre-compute some coefficients in the integrand that are determined by antenna factors and SNR. */
        double e, f, g;
        double c1, c2, c3, c4;
        {
            int j;
            double theta, phi;
            double a, b, c, d;
            pix2ang_ring(nside, ipix, &theta, &phi);
            for (j = 0, a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0; j < nifos; j ++)
            {
                double Fp, Fx;
                XLALComputeDetAMResponse(&Fp, &Fx, responses[j], phi, M_PI_2 - theta, 0, gmst);
                Fp *= d1[j];
                Fx *= d1[j];
                a += Fp * creal(snrs[j]);
                b += Fx * cimag(snrs[j]);
                c += Fx * creal(snrs[j]);
                d += Fp * cimag(snrs[j]);
                g += Fp * Fx;
                Fp *= Fp;
                Fx *= Fx;
                e += Fp + Fx;
                f += Fp - Fx;
            }
            c1 = a * b - c * d;
            c4 = (a * c + b * d) / 4;
            a *= a;
            b *= b;
            c *= c;
            d *= d;
            c2 = (a + b + c + d) / 8;
            c3 = (a - b - c + d) / 8;
        }
        e /= 8;
        f /= 8;
        g /= 4;

        /* Evaluate integral on a regular lattice in psi and cos(i) and using
         * adaptive quadrature over log(distance). */
        {
            int j;
            double accum;

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

            /* Loop over cos(i). */
            for (accum = -INFINITY, j = 0; j < INTEGRAND_COUNT_NODES; j ++)
            {
                /* Coefficients in integrand that are determined by antenna factors, SNR, and cos(i), but not psi. */
                const double args0 = u4_6u2_1[j] * e;
                const double args1 = u3_u[j] * c1 + u4_6u2_1[j] * c2;

                int k;

                /* Loop over psi. */
                for (k = 0; k < INTEGRAND_COUNT_NODES; k ++)
                {
                    /* Variables to store output from integrator. */
                    double result = NAN, abserr = NAN;

                    /* Coefficients in integrand that are determined by antenna factors, SNR, cos(i), and psi. */
                    const double num = args1 + u4_2u2_1[j] * (c3 * cosines[k] + c4 * sines[k]);
                    const double den = args0 + u4_2u2_1[j] * (f * cosines[k] + g * sines[k]);
                    const double a2 = den / num;
                    const double a = sqrt(a2);
                    const double sqrt_den = sqrt(den);

                    /* Create data structures for integrand callback. */
                    inner_integrand_params integrand_params = {a, 0.5 / a2};
                    const gsl_function func = {radial_integrand, &integrand_params};

                    /* Limits of integration. */
                    const double x1 = min_distance / sqrt_den;
                    const double x2 = max_distance / sqrt_den;

                    /* Find break points in integration interval that enclose the peak in the likelihood function. */
                    const double lower_breakpoint = (a - a2 * sqrt(-2 * log(y1))) / (1 + 2 * a2 * log(y1));
                    const double upper_breakpoint = (a < 1 / sqrt(-2 * log(y2)))
                        ? ((a + a2 * sqrt(-2 * log(y1))) / (1 + 2 * a2 * log(y1)))
                        : upper_breakpoint_default;

                    /* Create list of integration break points. */
                    double breakpoints[4];
                    int num_breakpoints = 0;
                    /* Always start with lower limit of integration. */
                    breakpoints[num_breakpoints++] = log(x1);
                    /* If integration interval contains lower break point, add it. */
                    if (lower_breakpoint > x1 && lower_breakpoint < x2)
                        breakpoints[num_breakpoints++] = log(lower_breakpoint);
                    /* If integration interval contains upper break point, add it too. */
                    if (upper_breakpoint > x1 && upper_breakpoint < x2)
                        breakpoints[num_breakpoints++] = log(upper_breakpoint);
                    /* Always end with upper limit of integration. */
                    breakpoints[num_breakpoints++] = log(x2);

                    if (prior == BAYESTAR_PRIOR_UNIFORM_IN_VOLUME)
                        integrand_params.log_offset += 3 * log(a);

                    {
                        /* Perform adaptive integration. Stop when a relative
                         * accuracy of 0.05 has been reached. */
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
                    }

                    /* If the radial integral was nonzero ,then accumulate the
                     * log posterior for this cos(i) and psi. */
                    if (result > -INFINITY) {
                        const double max_log_p = fmax(result, accum);
                        accum = log(exp(result - max_log_p) + exp(accum - max_log_p)) + max_log_p;
                    }
                }
            }

            /* Discard workspace for adaptive integrator. */
            gsl_integration_workspace_free(workspace);

            /* Accumulate (log) posterior terms for SNR and TDOA. */
            P[ipix] += accum;
        }
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
