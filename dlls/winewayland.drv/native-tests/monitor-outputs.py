#!/usr/bin/env python3
"""Run the production monitor membership and owner-thread reconciliation helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"^(?:static )?[^\n;{}]*\b" + name + r"\([^;{}]*\)\n\{", source, re.M)
    if not match:
        raise RuntimeError("missing production function " + name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror")
    parser.add_argument("--bits", choices=("native", "32", "64", "all"), default="native")
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    args.build_dir.mkdir(parents=True, exist_ok=True)
    names = (
        ("wayland_surface.c", (
            "wayland_surface_get_output", "wayland_surface_reset_presentation_output",
            "wayland_surface_clear_outputs", "wayland_surface_queue_output_update",
            "wayland_surface_update_output", "wayland_surface_handle_output",
            "wayland_client_surface_get_fullscreen_rect",
            "wayland_client_surface_refresh_fullscreen_targets",
            "wayland_client_surface_update_fullscreen_target",
            "wayland_surface_config_is_compatible", "wayland_surface_reconfigure_xdg")),
        ("window.c", (
            "WAYLAND_WindowPosChanging",
            "wayland_window_style_allows_fullscreen",
            "wayland_win_data_preserves_fullscreen",
            "wayland_win_data_get_requested_fullscreen_rect",
            "wayland_win_data_has_fixed_output", "wayland_win_data_get_fullscreen_rect",
            "wayland_win_data_has_fixed_fullscreen_size",
            "wayland_win_data_get_presentation_rect", "wayland_win_data_is_fullscreen",
            "wayland_win_data_retargets_fullscreen",
            "wayland_win_data_get_shm_source", "map_virtual_overlay_rect",
            "should_keep_toplevel_mapped",
            "wayland_win_data_get_config", "wayland_win_data_refresh_output_targets",
            "wayland_win_data_update_presentation_output",
            "wayland_window_update_output",
            "wayland_surface_has_pending_state", "wayland_surface_update_state_toplevel",
            "wayland_win_data_configure_surface_rect", "wayland_win_data_configure_window_rect",
            "wayland_configure_window")),
    )
    extracted = []
    hashes = {}
    for filename, functions in names:
        source = (here.parent / filename).read_text()
        hashes[filename] = hashlib.sha256(source.encode()).hexdigest()
        extracted.extend(function(source, name) for name in functions)
    generated = args.build_dir / "monitor-outputs.c"
    update = function((here.parent / "window.c").read_text(),
                      "wayland_surface_update_state_toplevel").split("{", 1)[0]
    parameters = update[update.index("(") + 1:update.rindex(")")].split(",")
    arguments = []
    for parameter in parameters:
        if "struct wayland_win_data" in parameter:
            arguments.append("&window")
        elif "struct wayland_surface" in parameter:
            arguments.append("&surface")
        else:
            raise RuntimeError("unsupported toplevel update parameter " + parameter)
    source = (here.parent / "window.c").read_text()
    create = function(source, "wayland_win_data_create_wayland_surface")
    visibility = create[create.index("    keep_toplevel_mapped ="):
                        create.index("    /* If the toplevel has no observable area")]
    changed = function(source, "WAYLAND_WindowPosChanged")
    fullscreen = changed[changed.index("    if ((data = wayland_win_data_get(hwnd)))"):
                         changed.index("    /* Infer application fullscreen")]
    target = changed[changed.index("    if (!preserve_fullscreen)\n"):
                     changed.index("    data->rects = *new_rects;")]
    client_target = changed[changed.index("    if (data->client_surface", changed.index("    data->rects = *new_rects;")):
                            changed.index("    wayland_win_data_update_restore_rect")]
    template = (here / "monitor-outputs.c.test").read_text()
    generated.write_text(template.replace("/* MONITOR_PRODUCTION_FUNCTIONS */", "\n\n".join(extracted))
        .replace("/* MONITOR_PRODUCTION_VISIBILITY */", visibility)
        .replace("/* MONITOR_PRODUCTION_FULLSCREEN_DECISION */", fullscreen)
        .replace("/* MONITOR_PRODUCTION_FULLSCREEN_TARGET */", target)
        .replace("/* MONITOR_PRODUCTION_CLIENT_TARGET */", client_target)
        .replace("/* MONITOR_TOPLEVEL_CALL */", "wayland_surface_update_state_toplevel(" +
                 ", ".join(arguments) + ")"))
    (args.build_dir / "sources.json").write_text(json.dumps(hashes, indent=2) + "\n")
    builds = [("native", [])] if args.bits == "native" else [
        (bits, ["-m" + bits]) for bits in (("64", "32") if args.bits == "all" else (args.bits,))]
    if args.sanitizers:
        builds.append(("native-sanitized", ["-O1", "-fsanitize=address,undefined",
                                            "-fno-omit-frame-pointer"]))
    for label, extra in builds:
        binary = args.build_dir / ("monitor-outputs-" + label)
        command = shlex.split(args.cc) + shlex.split(args.cflags) + extra + [
            "-std=gnu11", "-UNDEBUG", "-pthread", "-D__WINESRC__", "-I" + str(repo / "include"),
            str(generated), "-lwayland-client", "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary.resolve())], text=True, capture_output=True, timeout=10)
        (args.build_dir / (label + ".log")).write_text(result.stdout + result.stderr)
        print(label + ": " + result.stdout.strip(), flush=True)
        if result.stderr:
            print(result.stderr, end="", flush=True)
        result.check_returncode()


if __name__ == "__main__":
    main()
