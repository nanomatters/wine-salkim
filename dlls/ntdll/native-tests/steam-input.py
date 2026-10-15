#!/usr/bin/env python3
# Copyright 2026 Erhan Bilgili
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Test the actual Steam Input path helpers without Wine or a controller."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(path, name):
    source = path.read_text()
    start = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\([^\n]*\)\n\{", source, re.M)
    if not start:
        raise RuntimeError(f"Could not find {name} in {path}")
    tokens = re.finditer(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[{}]",
                         source[start.start():], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start.start():start.start() + token.end()]
    raise RuntimeError(f"Unclosed function {name} in {path}")


root = Path(__file__).resolve().parents[3]
fixture = Path(__file__).with_suffix(".c.test").read_text()
helpers = function(root / "dlls/win32u/rawinput.c", "get_steam_input_path") + "\n"
helpers += function(root / "dlls/ntdll/unix/file.c", "replace_steam_input_path")
helpers += "\n" + function(root / "dlls/user32/input.c", "steam_input_devnotify")
helpers += "\n" + function(root / "dlls/windows.gaming.input/provider.c", "steam_input_non_roamable_id")
fixture = fixture.replace("/* PRODUCTION_HELPERS */", helpers)

with tempfile.TemporaryDirectory(prefix="wine-steam-input-") as temp:
    directory = Path(temp)
    source = directory / "test.c"
    binary = directory / "test"
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-fshort-wchar",
                    "-I" + str(root / "include"),
                    "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
