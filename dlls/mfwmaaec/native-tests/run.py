#!/usr/bin/env python3
"""Run the production capture routines with deterministic WASAPI providers."""
import argparse
import os
import shlex
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=ROOT.parent / 'mfwmaaec.c')
    parser.add_argument('--output', type=Path, default=ROOT / 'build')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text()
    start = source.index('static const WAVEFORMATEX default_output_format')
    end = source.index('\n};', source.index('struct aaec\n', start)) + 3
    declarations = source[start:end]
    functions = '\n'.join(function(source, signature) for signature in (
        'static HRESULT get_capture_device(',
        'static HRESULT start_audio_capture(',
        'static void stop_audio_capture(',
        'static struct aaec *impl_from_IMediaObject(',
        'static HRESULT WINAPI media_object_ProcessOutput('))
    generated = (ROOT / 'capture.c.in').read_text().replace('@DECLARATIONS@', declarations)
    generated = generated.replace('@FUNCTIONS@', functions)
    path = args.output / 'capture.c'
    path.write_text(generated)
    binary = args.output / 'capture'
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra',
               '-Werror', '-Wno-unused-parameter', '-Wno-unused-const-variable', '-UNDEBUG']
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
