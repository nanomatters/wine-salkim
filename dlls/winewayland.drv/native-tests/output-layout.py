#!/usr/bin/env python3
"""Exercise production output layout selection against accepted win32u snapshots."""
import argparse
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror")
    parser.add_argument("--publisher-source", type=Path,
                        help="saved source containing the publisher, for regression negative controls")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parent / "display.c").read_text()
    start = source.index("static int output_info_cmp_primary_x_y(")
    end = source.index("static void wayland_add_device_gpu(", start)
    test = (here / "output-layout.c.test").read_text()
    test = test.replace("/* PRODUCTION_LAYOUT */", source[start:end])
    start = source.index("static void output_info_array_update_layout(")
    end = source.index("static int scale_output_coordinate(", start)
    test = test.replace("/* PRODUCTION_UPDATE */", source[start:end])
    publisher_source = args.publisher_source.read_text() if args.publisher_source else source
    start = publisher_source.index("UINT WAYLAND_UpdateDisplayDevices(")
    end = publisher_source.index("\n}", start) + len("\n}")
    publisher = publisher_source[start:end]
    test = test.replace("/* PRODUCTION_PUBLISHER */", publisher)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "output-layout.c"
    binary = args.build_dir / "output-layout"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-I" + str(here.parents[2] / "include"),
                    "-Wno-unused-parameter", str(generated), "-lwayland-client", "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
