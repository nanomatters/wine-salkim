#!/usr/bin/env python3
"""Exercise production IME deactivation across host keyboard layout changes."""
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
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("keyboard_default", here / "keyboard-default.py")
    extractor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extractor)
    source = (here.parents[2] / "dlls/imm32/imm.c").read_text()
    test = (here / "keyboard-ime.c.test").read_text()
    functions = ("imc_release_ime", "imc_select_ime", "ImmLockIMC", "ImmUnlockIMC",
                 "enum_activate_layout", "ImmActivateLayout", "get_ime_ui_window", "ime_internal_msg")
    test = test.replace("/* IME_FUNCTIONS */", "\n\n".join(
        extractor.function(source, name) for name in functions))
    args.build_dir.mkdir(parents=True, exist_ok=True)
    generated = args.build_dir / "keyboard-ime.c"
    binary = args.build_dir / "keyboard-ime"
    generated.write_text(test)
    subprocess.run(shlex.split(args.cc) + shlex.split(args.cflags) +
                   ["-UNDEBUG", "-Wno-unused-parameter", str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)


if __name__ == "__main__":
    main()
