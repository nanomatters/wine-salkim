#!/usr/bin/env python3
"""Exercise production keyboard defaults with injected server and registry calls."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    # Keep source offsets and line boundaries while hiding braces in comments
    # and literals. The returned function retains the original source text.
    tokens = r"""/\*.*?\*/|//(?:\\\n|[^\n])*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'"""
    code = re.sub(tokens, lambda match: re.sub(r"[^\n]", " ", match.group()),
                  source, flags=re.S)
    match = re.search(r"^(?:static )?[\w *]+\b" + re.escape(name) +
                      r"\([^)]*\)\n\{", code, re.M)
    if not match:
        raise RuntimeError("missing production function " + name)
    start = code.index("{", match.start())
    depth = 1
    end = start + 1
    while depth and end < len(code):
        depth += (code[end] == "{") - (code[end] == "}")
        end += 1
    if depth:
        raise RuntimeError("unterminated production function " + name)
    return source[match.start():end]


def handler(source, name):
    start = source.index("DECL_HANDLER(" + name + ")")
    source = source[start:].replace("DECL_HANDLER(" + name + ")",
                                   "static void server_" + name + "(void)", 1)
    return function(source, "server_" + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O2 -g -std=gnu11 -Wall -Wextra -Werror")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    input_source = (root / "dlls/win32u/input.c").read_text()
    imm_source = (root / "dlls/win32u/imm.c").read_text()
    server_source = (root / "server/winstation.c").read_text()
    keyboard_source = (here.parent / "wayland_keyboard.c").read_text()
    test = (here / "keyboard-default.c.test").read_text()
    for marker, source, names in (
        ("WIN32U_FUNCTIONS", input_source,
         ("get_locale_kbd_layout", "set_default_keyboard_layout", "NtUserGetKeyboardLayout",
          "NtUserActivateKeyboardLayout", "NtUserGetKeyboardLayoutName")),
        ("INPUT_CONTEXT_FUNCTIONS", imm_source,
         ("NtUserCreateInputContext", "NtUserDestroyInputContext", "NtUserUpdateInputContext")),
        ("WAYLAND_FUNCTIONS", keyboard_source,
         ("get_layout_hkl", "publish_keyboard_layout", "set_current_xkb_group", "reset_keyboard_layout",
          "wayland_keyboard_clear_layout", "wayland_keyboard_deinit",
          "keyboard_handle_modifiers", "WAYLAND_KbdLayerDescriptor",
          "WAYLAND_ReleaseKbdTables")),
        ("TRANSLATION_FUNCTIONS", input_source,
         ("kbd_tables_init_vsc2vk", "kbd_tables_init_vk2char", "kbd_tables_get_mod_num",
          "kbd_tables_vkey_to_wchar", "NtUserMapVirtualKeyEx", "NtUserGetKeyNameText", "NtUserToUnicodeEx")),
    ):
        test = test.replace("/* " + marker + " */",
                            "\n\n".join(function(source, name) for name in names))
    test = test.replace("/* SERVER_FUNCTIONS */", "\n\n".join(
        handler(server_source, name) for name in
        ("get_thread_layout", "set_thread_layout", "set_process_layout")))
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "keyboard-default.c"
    binary = args.build_dir / "keyboard-default"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-UNDEBUG", "-Wno-unused-parameter", "-Wno-sign-compare", "-pthread", "-I" + str(root / "include"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
