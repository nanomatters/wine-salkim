#!/usr/bin/env python3
"""Exercise managed presentation feedback through the production Vulkan timing query."""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -std=gnu11 -Wall -Wextra -Werror")
    parser.add_argument("--source-file", type=Path, help="saved Vulkan source for regression controls")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("presentation_fixture", here / "presentation-feedback.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    source = (args.source_file or repo / "dlls/win32u/vulkan.c").read_text()
    names = ("managed_handle_presentation_feedback", "managed_swapchain_get_status",
             "win32u_vkGetSwapchainTimingPropertiesEXT")
    bodies = "\n\n".join(fixture.definition(source, name) for name in names)
    template = (here / "managed-present-timing.c.test").read_text()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "managed-present-timing.c"
    binary = args.build_dir / "managed-present-timing"
    generated.write_text(template.replace("/* PRODUCTION_TIMING */", bodies))
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) + [
        "-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-Wno-unused-parameter",
        "-I" + str(repo / "include"), str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
