#!/usr/bin/env python3
#
# Copyright 2026 Erhan
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#
# Extract the actual desktop handler and reuse the graph dependency stubs.
import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from graph import HERE, compile_fixture, function, graph_fixture


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path, help='Wine source tree')
    parser.add_argument('--output', type=Path, required=True, help='Executable path prefix')
    args = parser.parse_args()
    source = (args.root / 'server/winstation.c').read_text()
    source = source.replace('DECL_HANDLER(set_thread_desktop)', 'static void test_set_thread_desktop(void)')
    actual = function(source, 'test_set_thread_desktop')
    desktop = (HERE / 'desktop.c').read_text().replace('/* ACTUAL_DESKTOP_HANDLER */', actual)
    fixture = '#define main graph_fixture_main\n' + graph_fixture(args.root)
    fixture += '\n#undef main\n' + desktop
    compile_fixture(args.root, fixture, args.output)
