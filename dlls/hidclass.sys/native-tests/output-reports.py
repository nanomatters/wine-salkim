#!/usr/bin/env python3
# Copyright 2026 Erhan Bilgili
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Test actual HID transfer dispatch and completion without a device or Wine."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    start = re.search(r"^(?:static )?[^\n]*\b" + re.escape(name) + r"\([^\n]*\)\n\{", source, re.M)
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
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
path = "dlls/hidclass.sys/device.c"
source = (subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=root, text=True)
          if args.source_ref else (root / path).read_text())
params = re.search(r"^struct completion_params\n\{.*?^\};", source, re.S | re.M)
if not params:
    raise RuntimeError("Could not find completion_params")
helpers = params.group() + "\n\n" + "\n\n".join(extract(source, name) for name in
    ("find_report_with_type_and_id", "xfer_completion", "hid_device_xfer_report"))
fixture = Path(__file__).with_suffix(".c.test").read_text().replace("/* PRODUCTION_HELPERS */", helpers)

with tempfile.TemporaryDirectory(prefix="wine-hid-output-") as temp:
    directory = Path(temp)
    source = directory / "test.c"
    binary = directory / "test"
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g",
                    "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
