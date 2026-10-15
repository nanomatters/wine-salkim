#!/usr/bin/env python3
# Copyright 2026 Erhan Bilgili
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the production HID input-report path without a controller or Wine."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    start = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\([^\n]*\)\n\{", source, re.M)
    if not start:
        raise RuntimeError(f"Could not find {name}")
    depth = 0
    for token in re.finditer(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[{}]",
                             source[start.start():], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start.start():start.start() + token.end()]
    raise RuntimeError(f"Unclosed function {name}")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-ref", help="Test the production functions at this Git revision")
parser.add_argument("cases", nargs="*", help="Run only the named test cases")
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
path = "dlls/winebus.sys/main.c"
source = (subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=root, text=True)
          if args.source_ref else (root / path).read_text())
helpers = "\n\n".join(extract(source, name) for name in ("deliver_next_report", "process_hid_report"))
fixture = Path(__file__).with_suffix(".c.test").read_text().replace("/* PRODUCTION_HELPERS */", helpers)

with tempfile.TemporaryDirectory(prefix="wine-hid-input-") as temp:
    directory = Path(temp)
    source = directory / "test.c"
    binary = directory / "test"
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-fno-pie", "-no-pie",
                    "-I" + str(root / "include"), "-fsanitize=address,undefined",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary), *args.cases], check=True, timeout=30)
