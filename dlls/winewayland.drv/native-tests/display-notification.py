#!/usr/bin/env python3
"""Test production display-change publication and application notification."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror")
    parser.add_argument("--source-file", type=Path, help="test a saved sysparams.c")
    parser.add_argument("--revision", help="test sysparams.c from a Git revision")
    args = parser.parse_args()
    if args.source_file and args.revision:
        parser.error("--source-file and --revision are mutually exclusive")
    here = Path(__file__).resolve().parent
    if args.revision:
        source = subprocess.check_output(
            ["git", "show", args.revision + ":dlls/win32u/sysparams.c"],
            cwd=here.parents[2], text=True)
    else:
        source = (args.source_file or here.parents[1] / "win32u/sysparams.c").read_text()
    functions = []
    for name in ("display_modes_equal", "display_sources_changed", "display_mode_changed"):
        match = re.search(r"^static (?:BOOL|void) " + name + r"\([^\n]*\)\n\{", source, re.MULTILINE)
        if not match:
            raise RuntimeError("missing production " + name)
        opening = source.index("{", match.start())
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        functions.append(source[match.start():end])
    production = "\n\n".join(functions)
    fixture = (here / "display-notification.c.test").read_text()
    fixture = fixture.replace("/* PRODUCTION_NOTIFICATION */", production)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "display-notification.c"
    binary = args.build_dir / "display-notification"
    generated.write_text(fixture)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) + [
        "-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-pthread",
        "-I" + str(here.parents[2] / "include"), str(generated),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
