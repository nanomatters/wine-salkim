#!/usr/bin/env python3
"""Exercise production notification registration and dispatch with pthreads."""
import argparse
from pathlib import Path
import shlex
import subprocess
import runpy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g")
    parser.add_argument("--wine-build-dir", type=Path, default=Path("../../.."))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    function = runpy.run_path(str(here / "endpoint-identity.py"))["function"]
    source = (here / "../devenum.c").read_text()
    declarations = source[source.index("struct NotificationClientWrapper {"):
                          source.index("static struct NotificationClientWrapper **snapshot_notification_clients")]
    names = ("snapshot_notification_clients", "release_notification_clients",
             "begin_notification", "end_notification", "notification_in_progress",
             "notify_default_clients", "dispatch_endpoint_events", "MMDevEnum_RegisterEndpointNotificationCallback",
             "MMDevEnum_UnregisterEndpointNotificationCallback")
    declarations += source[source.index("enum endpoint_event_type {"):
                           source.index("struct endpoint_update\n")]
    test = (here / "notification-lifetime.c.test").read_text().replace(
        "/* PRODUCTION */", declarations + "\n" + "\n\n".join(function(source, n) for n in names))
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "notification-lifetime.c"
    binary = args.build_dir / "notification-lifetime"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", "-Wno-missing-field-initializers", "-UNDEBUG",
                    "-D_GNU_SOURCE", "-D__WINESRC__", "-DWINE_UNIX_LIB",
                    "-I" + str(here / "../../../include"), "-I" + str(args.wine_build_dir / "include"),
                    str(generated), "-pthread", "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary.resolve())], check=True, timeout=30)


if __name__ == "__main__":
    main()
