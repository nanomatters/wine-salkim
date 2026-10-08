#!/usr/bin/env python3
"""Compile live driver sources with an existing Wine build's configuration."""

import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine-build", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--all", action="store_true", help="compile all driver translation units")
    parser.add_argument("--win32u", action="store_true", help="also compile the display-cache implementation")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for arch in ("x86_64", "i386"):
        obj = args.wine_build / ("obj-wine-" + arch)
        database = json.loads((obj / "compile_commands.json").read_text())
        filenames = ("display.c", "vulkan.c", "wayland_output.c", "wayland_surface.c", "window.c")
        if args.all:
            filenames = tuple(Path(item["file"]).name for item in database
                              if "/dlls/winewayland.drv/" in item["file"])
        paths = tuple("dlls/winewayland.drv/" + filename for filename in filenames)
        if args.win32u:
            paths += ("dlls/win32u/sysparams.c",)
        for path in paths:
            filename = Path(path).name
            entry = next(item for item in database if item["file"].endswith("/" + path))
            command = shlex.split(entry["command"])
            if arch == "i386" and not shutil.which(command[0]):
                command[:1] = ["gcc", "-m32"]
            stem = filename.removesuffix(".c") + "-" + arch
            command[command.index("-o") + 1] = str(args.output_dir / (stem + ".o"))
            staged = str(args.wine_build / "src-wine")
            live = []
            index = 0
            while index < len(command):
                arg = command[index]
                if arg in ("-MF", "-MT", "-MQ"):
                    index += 2
                    continue
                if arg in ("-MD", "-MMD", "-MP"):
                    index += 1
                    continue
                if arg.startswith("-I" + staged):
                    live.append(arg.replace(staged, str(repo), 1))
                    live.append(arg)
                elif arg == entry["file"]:
                    live.append(str(repo / path))
                else:
                    live.append(arg)
                index += 1
            (args.output_dir / (stem + ".command")).write_text(shlex.join(live) + "\n")
            result = subprocess.run(live, cwd=entry["directory"], capture_output=True, text=True)
            (args.output_dir / (stem + ".log")).write_text(result.stdout + result.stderr)
            print(stem + (": PASS" if not result.returncode else ": FAIL"), flush=True)
            if result.returncode:
                print(result.stdout + result.stderr)
            result.check_returncode()


if __name__ == "__main__":
    main()
