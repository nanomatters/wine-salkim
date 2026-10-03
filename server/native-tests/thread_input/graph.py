#!/usr/bin/env python3
#
# Copyright 2026 Erhan
#
# This library is free software. You can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation. Either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY. Without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#
# Compile native tests using the actual queue.c functions, not copies.
# Run the produced ASan/UBSan and i386 executables separately.
import argparse
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent


def function(source, name):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\n\{', source, re.M)
    assert match, name
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[match.start():index + 1]
    raise AssertionError(name)


def graph_fixture(root):
    source = (root / 'server/queue.c').read_text()
    actual = '\n\n'.join(function(source, name) for name in (
        'create_thread_input', 'assign_thread_input', 'mark_input_component',
        'split_thread_input', 'repair_thread_input', 'remove_queue_input',
        'attach_thread_input', 'detach_thread_input'))
    fixture = (HERE / 'graph.c').read_text().replace('/* ACTUAL_FUNCTIONS */', actual)
    teardown = function(source, 'msg_queue_destroy')
    start = teardown.index('SHARED_WRITE_BEGIN( input_shm, input_shm_t )')
    end = teardown.index('remove_queue_input( queue );', start) + len('remove_queue_input( queue );')
    return fixture.replace('/* ACTUAL_TEARDOWN_CORE */', teardown[start:end])


def compile_fixture(root, fixture, output):
    for suffix, flags in [('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']),
                          ('32', ['-m32'])]:
        subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Werror', '-I' + str(root / 'include'),
                        *flags, '-x', 'c', '-', '-o', str(output) + '-' + suffix],
                       input=fixture, text=True, check=True)
    print('Compiled extracted production functions with ASan/UBSan and for i386')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='Wine source tree containing server/queue.c')
    parser.add_argument('--output', type=Path, required=True, help='Executable path prefix')
    args = parser.parse_args()
    compile_fixture(args.root, graph_fixture(args.root), args.output)
