#!/usr/bin/env python
#
# Copyright (C) 2012  Leo Singer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""
LIGO-LW convenience functions.
"""
__author__ = "Leo Singer <leo.singer@ligo.org>"


# LIGO-LW XML imports.
from pylal import ligolw_inspinjfind
from glue.ligolw.utils import process as ligolw_process
from glue.ligolw import table as ligolw_table
from glue.ligolw import lsctables


def get_temlate_bank_f_low(xmldoc):
    """Determine the low frequency cutoff from a template bank file,
    whether the template bank was produced by lalapps_tmpltbank or
    lalapps_cbc_sbank. bayestar_sim_to_tmpltbank does not have a command
    line for the low-frequency cutoff; instead, it is recorded in a row
    of the search_summvars table."""
    try:
        template_bank_f_low, = ligolw_process.get_process_params(xmldoc,
            'tmpltbank', '--low-frequency-cutoff')
    except ValueError:
        try:
            template_bank_f_low, = ligolw_process.get_process_params(xmldoc,
                'lalapps_cbc_sbank', '--flow')
        except ValueError:
            try:
                search_summvars_table = ligolw_table.get_table(xmldoc,
                    lsctables.SearchSummVarsTable.tableName)
                template_bank_f_low, = (search_summvars.value
                    for search_summvars in search_summvars_table
                    if search_summvars.name == 'low-frequency cutoff')
            except ValueError:
                raise ValueError("Could not determine low-frequency cutoff")
    return template_bank_f_low


def sim_and_sngl_inspirals_for_xmldoc(xmldoc):
    """Retrieve (as a generator) all of the
    (sim_inspiral, (sngl_inspiral, sngl_inspiral, ... sngl_inspiral) tuples from
    found coincidences in a LIGO-LW XML document."""

    # Look up necessary tables.
    coinc_table = ligolw_table.get_table(xmldoc, lsctables.CoincTable.tableName)
    coinc_def_table = ligolw_table.get_table(xmldoc, lsctables.CoincDefTable.tableName)
    coinc_map_table = ligolw_table.get_table(xmldoc, lsctables.CoincMapTable.tableName)

    # Look up coinc_def ids.
    sim_coinc_def_id = coinc_def_table.get_coinc_def_id(
        ligolw_inspinjfind.InspiralSCExactCoincDef.search,
        ligolw_inspinjfind.InspiralSCExactCoincDef.search_coinc_type,
        create_new=False)

    def events_for_coinc_event_id(coinc_event_id):
        for coinc_map in coinc_map_table:
            if coinc_map.coinc_event_id == coinc_event_id:
                for row in ligolw_table.get_table(xmldoc, coinc_map.table_name):
                    column_name = coinc_map.event_id.column_name
                    if getattr(row, column_name) == coinc_map.event_id:
                        yield coinc_map.event_id, row

    # Loop over all coinc_event <-> sim_inspiral coincs.
    for sim_coinc in coinc_table:

        # If this is not a coinc_event <-> sim_inspiral coinc, skip it.
        if sim_coinc.coinc_def_id != sim_coinc_def_id:
            continue

        # Locate the sim_inspiral and coinc events.
        sim_inspiral = None
        coinc = None
        for event_id, event in events_for_coinc_event_id(sim_coinc.coinc_event_id):
            if event_id.table_name == ligolw_table.StripTableName(lsctables.SimInspiralTable.tableName):
                if sim_inspiral is not None:
                    raise RuntimeError("Found more than one matching sim_inspiral entry")
                sim_inspiral = event
            elif event_id.table_name == ligolw_table.StripTableName(lsctables.CoincTable.tableName):
                if coinc is not None:
                    raise RuntimeError("Found more than one matching coinc entry")
                coinc = event
            else:
                raise RuntimeError("Did not expect coincidence to contain an event of type '%s'" % event_id.table_name)

        sngl_inspirals = tuple(event
            for event_id, event in events_for_coinc_event_id(coinc.coinc_event_id))

        yield sim_inspiral, sngl_inspirals
