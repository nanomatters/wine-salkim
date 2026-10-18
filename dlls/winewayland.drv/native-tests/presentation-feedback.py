#!/usr/bin/env python3
"""Exercise production presentation callbacks with native client and socket fixtures."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import xml.etree.ElementTree as ET


def definition(source, name, kind="function"):
    if kind == "function":
        pattern = r"^(?:static )?[\w *]+\b" + re.escape(name) + r"\([^)]*\)\n\{"
    else:
        pattern = r"^" + kind + r" " + re.escape(name) + r"\n\{"
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError("missing production " + kind + " " + name)
    opening = source.index("{", match.start())
    end = block_end(source, opening)
    return source[match.start():end] + (";" if kind != "function" else "")


def block_end(source, opening):
    end = opening + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return end


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -std=gnu11 -Wall -Wextra -Werror")
    parser.add_argument("--surface-source", type=Path,
                        help="saved callbacks for a regression negative control")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    surface = (args.surface_source or here.parent / "wayland_surface.c").read_text()
    header = (repo / "include/wine/gdi_driver.h").read_text()
    types = [definition(header, "client_surface_presentation_flags", "enum"),
             definition(header, "client_surface_presentation_timing", "struct")]
    types += [definition(surface, name, "struct") for name in (
        "wayland_hwnd_dmabuf_presentation_part", "wayland_hwnd_dmabuf_presentation")]
    names = (
        "wayland_hwnd_dmabuf_send_presentation_result",
        "wayland_hwnd_dmabuf_presentation_complete",
        "hwnd_dmabuf_presentation_handle_sync_output",
        "hwnd_dmabuf_presentation_handle_presented",
        "hwnd_dmabuf_presentation_handle_discarded",
        "wayland_presentation_feedback_claim",
        "presentation_feedback_handle_presented",
        "presentation_feedback_handle_discarded",
    )
    protocol = ET.parse(here.parent / "presentation-time.xml").getroot()
    feedback = protocol.find("interface[@name='wp_presentation_feedback']")
    constants = "\n".join("#define WP_PRESENTATION_FEEDBACK_KIND_" + entry.attrib["name"].upper()
                           + " " + entry.attrib["value"] for entry in feedback.findall("enum[@name='kind']/entry"))
    registry = (here.parent / "wayland.c").read_text()
    start = registry.index('    else if (strcmp(interface, "wp_presentation") == 0)')
    branch = registry[start:block_end(registry, registry.index("{", start))]
    source = (here / "presentation-feedback.c.test").read_text()
    source = source.replace("/* PRODUCTION_TYPES */", "\n\n".join(types))
    source = source.replace("/* PROTOCOL_FLAGS */", constants)
    source = source.replace("/* PRODUCTION_CALLBACKS */", "\n\n".join(definition(surface, name) for name in names))
    source = source.replace("/* PRODUCTION_REGISTRY */", branch)
    assert "/* PRODUCTION_" not in source
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "presentation-feedback.c"
    binary = args.build_dir / "presentation-feedback"
    generated.write_text(source)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) + [
        "-D_GNU_SOURCE", "-D__WINESRC__", "-UNDEBUG", "-Wno-unused-parameter",
        "-I" + str(repo / "include"), "-I" + str(here.parent),
        str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
