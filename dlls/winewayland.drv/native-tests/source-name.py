#!/usr/bin/env python3
"""Test source identity using Wine's registry, in a dedicated test prefix."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--wine", required=True)
    parser.add_argument("--cc", default="x86_64-w64-mingw32-gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parents[1] / "win32u/sysparams.c").read_text()
    helpers = []
    for name in ("read_source_name", "get_source_name"):
        match = re.search(r"^static BOOL " + name + r"\([^\n]*\)\n\{", source, re.MULTILINE)
        if not match:
            raise RuntimeError("missing production " + name)
        opening = source.index("{", match.start())
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        helpers.append(source[match.start():end])
    fixture = (here / "source-name.c.test").read_text().replace("/* PRODUCTION_NAME */", "\n\n".join(helpers))
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "source-name.c"
    binary = args.build_dir / "source-name.exe"
    generated.write_text(fixture)
    subprocess.run(shlex.split(args.cc) + ["-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-format-truncation", "-I" + str(here.parents[2] / "include"),
                   str(generated), "-ladvapi32", "-lntdll", "-o", str(binary)], check=True)
    env = dict(os.environ, WINEPREFIX=str(args.build_dir.resolve() / "prefix"),
               WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d", DISPLAY="", WAYLAND_DISPLAY="")
    subprocess.run([args.wine, str(binary.resolve())], env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
