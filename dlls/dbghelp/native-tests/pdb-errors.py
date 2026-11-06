#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the production PDB record walkers with malformed input and failing I/O."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, name):
    start = re.search(r"^static [^;{]*\b" + re.escape(name) + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--wine-build-dir", type=Path, required=True)
parser.add_argument("--source", type=Path, help="PDB source from another revision")
parser.add_argument("--case", action="append", help="Run only the named case")
parser.add_argument("--build-dir", type=Path, help="Keep generated source and executable here")
args = parser.parse_args()
directory = Path(__file__).resolve().parent
root = directory.parents[2]
source = (args.source or directory.parent / "pdb.c").read_text()
names = ["pdb_reader_push_action_and_filler", "pdb_reader_TPI_fillin_enumlist",
         "pdb_reader_TPI_fillin_UDTlist", "pdb_reader_TPI_field_request"]
functions = "\n\n".join(function(source, name) for name in names)
harness = (directory / "pdb-errors.c.in").read_text().replace("@FUNCTIONS@", functions)

with tempfile.TemporaryDirectory(prefix="dbghelp-pdb-errors-") as temporary:
    output = args.build_dir or Path(temporary)
    output.mkdir(parents=True, exist_ok=True)
    test = output / "pdb-errors.c"
    binary = output / "pdb-errors"
    test.write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-fshort-wchar", "-D__WINESRC__", "-fsanitize=address,undefined", "-g",
                "-isystem", str(args.wine_build_dir / "include"), "-isystem", str(root / "include")]
    command += shlex.split(os.environ.get("CFLAGS", ""))
    subprocess.run(command + [str(test), "-o", str(binary)], check=True)
    cases = args.case or subprocess.check_output([str(binary), "list"], text=True).splitlines()
    for case in cases:
        subprocess.run([str(binary), case], check=True, timeout=10)
