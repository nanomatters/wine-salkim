#!/usr/bin/env python3
"""Exercise the production read-only display cache query and publication setup."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def extract_function(source, name):
    start = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", source.index(name, start))
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror")
    parser.add_argument("--source-file", type=Path, help="test a saved sysparams.c for a regression comparison")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (args.source_file or here.parents[1] / "win32u" / "sysparams.c").read_text()
    functions = ("get_display_device_init_mutex", "release_display_device_init_mutex",
                 "prepare_display_devices", "get_primary_name", "read_source_name", "get_source_name", "update_display_devices",
                 "get_display_layout")
    production = "\n\n".join(extract_function(source, name) for name in functions
                             if name != "get_source_name" or "static BOOL get_source_name(" in source)
    test = (here / "display-cache.c.test").read_text().replace("/* PRODUCTION_CACHE */", production)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "display-cache.c"
    binary = args.build_dir / "display-cache"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-pthread",
                    "-I" + str(here.parents[2] / "include"), "-Wno-unused-parameter", "-Wno-sign-compare",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
