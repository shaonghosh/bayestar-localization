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
Functions that support the command line interface.
"""
__author__ = "Leo Singer <leo.singer@ligo.org>"


from optparse import IndentedHelpFormatter


class NewlinePreservingHelpFormatter(IndentedHelpFormatter):
    """A help formatter for optparse that preserves paragraphs and bulleted
    lists whose lines start with a whitespace character."""

    def _format_text(self, text):
        __doc__ = IndentedHelpFormatter._format_text
        return "\n\n".join(
            t if len(t) == 0 or t[0].isspace()
            else IndentedHelpFormatter._format_text(self, t)
            for t in text.split("\n\n")
        )