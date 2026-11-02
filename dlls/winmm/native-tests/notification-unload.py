#!/usr/bin/env python3
"""Test winmm notifications under the loader lock, using disposable audio state."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, required=True, help='private installed Wine runtime')
parser.add_argument('--arch', choices=('x86_64', 'i386'), default='x86_64')
parser.add_argument('--cc', help='MinGW compiler; defaults to the selected architecture')
parser.add_argument('--work', type=Path, help='new output directory; defaults to a temporary directory')
parser.add_argument('--cycles', type=int, default=200, help='supplementary unload/reload cycles')
args = parser.parse_args()
if args.cycles < 0:
    parser.error('--cycles must be nonnegative')
source = Path(__file__).resolve()
runtime = args.runtime.resolve()
wine = runtime / 'lib/wine' / (args.arch + '-unix') / 'wine'
compiler = args.cc or {'x86_64': 'x86_64-w64-mingw32-gcc', 'i386': 'i686-w64-mingw32-gcc'}[args.arch]
if args.work:
    work = args.work.resolve()
    work.mkdir(parents=True)  # Refuse to reuse a prefix or somebody else's audio socket.
else:
    work = Path(tempfile.mkdtemp(prefix='winmm-notification-unload.'))
server_dir = work / 'server'
server_dir.mkdir(mode=0o700)
print('Results:', work, flush=True)

subprocess.run([compiler, '-x', 'c', '-O2', '-Wall', '-Wextra',
                str(source.with_suffix('.c.test')), '-o', str(work / 'probe.exe'),
                '-lole32', '-luuid', '-ladvapi32'], check=True)
env = os.environ.copy()
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WINELOADER', 'WINEDLLPATH', 'WINESERVER',
            'WINEARCH', 'PIPEWIRE_CORE', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR'):
    env.pop(key, None)
env.update(PIPEWIRE_RUNTIME_DIR=str(server_dir), XDG_RUNTIME_DIR=str(server_dir),
           PIPEWIRE_REMOTE='pipewire-0', WINEPREFIX=str(work / 'prefix'),
           WINE_AUDIO_DRIVER='pipewire',
           WINEDLLOVERRIDES='mmdevapi,winmm=b;winemenubuilder.exe=d;winevulkan=d',
           WINEDEBUG='-all,+mmdevapi,+winmm,+seh')


def command(*argv):
    return subprocess.check_output(argv, env=env, text=True, stderr=subprocess.STDOUT, timeout=30)


server = None
with (work / 'pipewire.log').open('w') as server_log, (work / 'wine.log').open('w') as wine_log:
    try:
        config = source.parents[2] / 'winepipewire.drv/native-tests/hotplug-pipewire.conf'
        server = subprocess.Popen(['pipewire', '-c', str(config)], env=env,
                                  stdout=server_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while not (server_dir / 'pipewire-0').exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Private PipeWire failed to start; see pipewire.log')
            time.sleep(0.05)
        for name, description in (('winmm.first', 'WinmmFirst'), ('winmm.second', 'WinmmSecond')):
            command('pw-cli', 'create-node', 'adapter', '{ factory.name=support.null-audio-sink '
                    f'node.name={name} node.description={description} media.class=Audio/Sink '
                    'audio.position=[FL FR] object.linger=true }')
        command('pw-metadata', '-n', 'default', '0', 'default.audio.sink',
                json.dumps({'name': 'winmm.first'}), 'Spa:String:JSON')
        command(str(wine), 'reg', 'add', r'HKCU\Software\Wine\Drivers',
                '/v', 'Audio', '/d', 'pipewire', '/f')
        with (work / 'probe.log').open('w') as output:
            result = subprocess.run([str(wine), str(work / 'probe.exe'), str(args.cycles)],
                                    cwd=work, env=env, stdout=output, stderr=wine_log, timeout=120)
        print((work / 'probe.log').read_text(), end='', flush=True)
        result.check_returncode()
    finally:
        try:
            subprocess.run([str(runtime / 'bin/wineserver'), '-k'], env=env,
                           stdout=wine_log, stderr=subprocess.STDOUT, timeout=15)
        finally:
            if server and server.poll() is None:
                server.terminate()
                server.wait(timeout=10)
