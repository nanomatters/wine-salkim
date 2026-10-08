#!/usr/bin/env python3
"""Exercise production Vulkan fullscreen requests with injected output topology."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def definition(source, name, kind="function"):
    if kind == "function":
        pattern = r"^(?:static )?[\w *]+\b" + re.escape(name) + r"\([^)]*\)\n\{"
    else:
        pattern = r"^" + kind + r" " + re.escape(name) + r"\n\{"
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError("missing production " + kind + " " + name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + (";" if kind != "function" else "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -Wall -Wextra -Werror -Wno-unused-parameter")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    header = (repo / "include/wine/vulkan_driver.h").read_text()
    wayland_header = (here.parent / "waylanddrv.h").read_text()
    surface = (here.parent / "wayland_surface.c").read_text()
    vulkan = (here.parent / "vulkan.c").read_text()
    win32u = (repo / "dlls/win32u/vulkan.c").read_text()
    types = "\n\n".join([
        definition(header, "vulkan_surface_fullscreen_action", "enum"),
        definition(header, "vulkan_surface_fullscreen_target", "enum"),
        definition(header, "vulkan_surface_fullscreen_info", "struct"),
        definition(wayland_header, "wayland_fullscreen_request", "struct"),
    ])
    helpers = "\n\n".join(definition(surface, name) for name in (
        "wayland_client_surface_get_fullscreen_rect",
        "wayland_client_surface_refresh_fullscreen_targets",
        "wayland_client_surface_update_fullscreen_target",
        "wayland_client_surface_clear_fullscreen_requests",
    ))
    functions = "\n\n".join(definition(vulkan, name) for name in (
        "find_fullscreen_request", "resolve_fullscreen_output",
        "wayland_vulkan_surface_fullscreen_supported", "wayland_vulkan_surface_fullscreen",
    ))
    destroy = definition(surface, "wayland_client_surface_destroy")
    assert "wayland_client_surface_clear_fullscreen_requests(surface)" in destroy
    source = (here / "fullscreen-outputs.c.test").read_text()
    source = source.replace("/* PRODUCTION_TYPES */", types)
    source = source.replace("/* PRODUCTION_HELPERS */", helpers)
    source = source.replace("/* PRODUCTION_VULKAN */", functions)
    source = source.replace("/* PRODUCTION_INVALIDATION */", "\n\n".join(
        definition(win32u, name) for name in ("surface_is_cloaked", "swapchain_is_out_of_date")))
    assert "/* PRODUCTION_" not in source
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "fullscreen-outputs.c"
    binary = args.build_dir / "fullscreen-outputs"
    generated.write_text(source)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-UNDEBUG", "-D__WINESRC__", "-I" + str(repo / "include"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
