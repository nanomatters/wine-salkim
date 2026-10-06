#!/usr/bin/env python3
"""Compile the production reader functions with injected platform calls."""
import argparse
from pathlib import Path
import platform
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\([^)]*\)\n\{", source, re.M)
    if not match:
        raise RuntimeError("missing production function " + name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror -Wno-unused-parameter")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    args.build_dir.mkdir(parents=True, exist_ok=True)
    for label, filename, marker, names in (
        ("reader-startup", "waylanddrv_main.c", "READER_FUNCTIONS",
         ("WAYLAND_SetDesktopWindow", "waylanddrv_unix_init", "wayland_read_events_thread")),
        ("reader-handoff", "wayland_data_device.c", "HANDOFF_FUNCTIONS",
         ("discard_clipboard_events", "clear_clipboard_owner", "wayland_clipboard_destroy_window",
          "wayland_clipboard_dispatch_timeout", "wayland_clipboard_cleanup_thread", "queue_clipboard_event",
          "process_clipboard_event", "data_control_source_send", "ext_data_control_source_send",
          "data_source_send", "handle_selection")),
        ("reader-signals", "../ntdll/unix/signal_x86_64.c", "SIGNAL_FUNCTIONS",
         ("init_handler", "leave_handler")),
    ):
        if label == "reader-signals" and platform.machine().lower() not in ("x86_64", "amd64"):
            print("reader signal TLS: skipped outside x86-64")
            continue
        source = (here.parent / filename).read_text()
        functions = "\n\n".join(function(source, name) for name in names)
        test = (here / (label + ".c.test")).read_text().replace("/* " + marker + " */", functions)
        generated = args.build_dir / (label + ".c")
        binary = args.build_dir / label
        generated.write_text(test)
        subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                       ["-UNDEBUG", "-pthread", str(generated), "-o", str(binary)], check=True)
        subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
