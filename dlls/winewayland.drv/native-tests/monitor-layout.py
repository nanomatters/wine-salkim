#!/usr/bin/env python3
"""Exercise production monitor layout with incremental output announcements."""
import argparse
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror")
    parser.add_argument("--source-file", type=Path, help="test a saved display.c for a regression comparison")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (args.source_file or here.parent / "display.c").read_text()
    start = source.index("static int output_info_cmp_primary_x_y(")
    end = source.index("static void wayland_add_device_gpu(", start)
    layout = source[start:end]
    # Also accepts the withdrawn implementation to demonstrate the regression.
    start = source.find("static void output_info_array_update_layout(")
    if start < 0:
        start = source.index("void output_info_array_update(")
    end = source.index("static int scale_output_coordinate(", start)
    fixture = (here / "monitor-layout.c.test").read_text()
    fixture = fixture.replace("/* PRODUCTION_LAYOUT */", layout + source[start:end])
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "monitor-layout.c"
    binary = args.build_dir / "monitor-layout"
    generated.write_text(fixture)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) + [
        "-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-Wno-sign-compare",
        "-I" + str(here.parents[2] / "include"), str(generated),
        "-lwayland-client", "-pthread", "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
