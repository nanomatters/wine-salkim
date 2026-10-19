#!/usr/bin/env python3
"""Run the production endpoint identity and synthetic-device removal helpers."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"^static [^\n]+\b" + re.escape(name) + r"\([^;]*?\n\{", source, re.M)
    if not match:
        raise RuntimeError("Function not found: " + name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -std=gnu11 -Wall -Wextra -Werror")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here / "../devenum.c").read_text()
    test = (here / "endpoint-identity.c.test").read_text()
    test = test.replace("/* PRODUCTION_FUNCTIONS */", "\n\n".join(function(source, name) for name in
        ("unregister_endpoint_device", "MMDevice_Unregister", "MMDevice_Create",
         "set_endpoint_state", "get_endpoint_identity_changes")))
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "endpoint-identity.c"
    binary = args.build_dir / "endpoint-identity"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-UNDEBUG", "-Wno-unused-parameter", str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
