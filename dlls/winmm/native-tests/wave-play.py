#!/usr/bin/env python3
"""Test waveOut playback, Reset and rerouting on a private PipeWire graph."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, required=True, help='private installed Wine runtime')
parser.add_argument('--arch', choices=('x86_64', 'i386'), default='x86_64')
parser.add_argument('--cc', help='MinGW compiler; defaults to the selected architecture')
parser.add_argument('--work', type=Path, help='new output directory; defaults to a temporary directory')
args = parser.parse_args()
source = Path(__file__).resolve()
runtime = args.runtime.resolve()
wine = runtime / 'lib/wine' / (args.arch + '-unix') / 'wine'
compiler = args.cc or {'x86_64': 'x86_64-w64-mingw32-gcc', 'i386': 'i686-w64-mingw32-gcc'}[args.arch]
work = args.work.resolve() if args.work else Path(tempfile.mkdtemp(prefix='winmm-wave-play.'))
if args.work:
    work.mkdir(parents=True)  # Refuse to reuse a prefix or an audio socket.
server_dir = work / 'server'
server_dir.mkdir(mode=0o700)
print('Results:', work, flush=True)

subprocess.run([compiler, '-x', 'c', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
                str(source.with_suffix('.c.test')), '-o', str(work / 'probe.exe'), '-lwinmm'], check=True)
env = os.environ.copy()
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WINELOADER', 'WINEDLLPATH', 'WINESERVER',
            'WINEARCH', 'PIPEWIRE_CORE', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR',
            'DBUS_SESSION_BUS_ADDRESS'):
    env.pop(key, None)
env.update(PIPEWIRE_RUNTIME_DIR=str(server_dir), XDG_RUNTIME_DIR=str(server_dir),
           PIPEWIRE_REMOTE='pipewire-0', WINEPREFIX=str(work / 'prefix'),
           XDG_CONFIG_HOME=str(work / 'config'), XDG_STATE_HOME=str(work / 'state'),
           XDG_CACHE_HOME=str(work / 'cache'), GIO_USE_VFS='local',
           DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + str(server_dir / 'no-system-bus'),
           WINE_AUDIO_DRIVER='pipewire',
           WINEDLLOVERRIDES='mmdevapi,winmm=b;winemenubuilder.exe=d;winevulkan=d',
           WINEDEBUG='-all,+winmm')

# The notification-only graph needs linking support for real playback.
config = (source.parents[2] / 'winepipewire.drv/native-tests/hotplug-pipewire.conf').read_text()
config = config.replace('    { name = libpipewire-module-client-node }',
                        '    { name = libpipewire-module-client-node }\n'
                        '    { name = libpipewire-module-link-factory }\n'
                        '    { name = libpipewire-module-spa-node-factory }')
(work / 'pipewire.conf').write_text(config)
children = []
logs = []
probe = None


def start(name, argv):
    log = (work / (name + '.log')).open('w')
    logs.append(log)
    child = subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    children.append(child)
    return child


def command(*argv):
    return subprocess.check_output(argv, env=env, text=True, stderr=subprocess.STDOUT, timeout=30)


def graph():
    return json.loads(command('pw-dump'))


def playback_link(entries, sink):
    nodes = {entry['id']: entry.get('info', {}).get('props', {}) for entry in entries
             if entry['type'] == 'PipeWire:Interface:Node'}
    for entry in entries:
        info = entry.get('info', {})
        if entry['type'] != 'PipeWire:Interface:Link' or info.get('state') != 'active':
            continue
        node = nodes.get(info.get('output-node-id'), {})
        if info.get('input-node-id') == sink and node.get('media.class') == 'Stream/Output/Audio':
            return node['object.serial']
    return None


try:
    server = start('pipewire', ['pipewire', '-c', str(work / 'pipewire.conf')])
    deadline = time.monotonic() + 10
    while not (server_dir / 'pipewire-0').exists():
        if server.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError('Private PipeWire failed to start; see pipewire.log')
        time.sleep(0.05)
    # The policy profile links virtual sinks without discovering audio hardware.
    start('wireplumber', ['dbus-run-session', '--', 'wireplumber', '-p', 'policy'])
    for name in ('first', 'second'):
        command('pw-cli', 'create-node', 'adapter', '{ factory.name=support.null-audio-sink '
                f'node.name=winmm.{name} node.description=Winmm{name} media.class=Audio/Sink '
                'audio.position=[FL FR] node.driver=true object.linger=true }')
    nodes = {entry.get('info', {}).get('props', {}).get('node.name'): entry['id'] for entry in graph()}
    first, second = nodes['winmm.first'], nodes['winmm.second']
    command('wpctl', 'set-default', str(first))
    command(str(wine), 'reg', 'add', r'HKCU\Software\Wine\Drivers',
            '/v', 'Audio', '/d', 'pipewire', '/f')
    with (work / 'wine.log').open('w') as wine_log, (work / 'probe.log').open('w') as output:
        probe = subprocess.Popen([str(wine), str(work / 'probe.exe')], cwd=work, env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=wine_log)
        buffer = b''
        switched = False
        deadline = time.monotonic() + 40
        while True:
            if time.monotonic() >= deadline:
                raise RuntimeError('Playback probe timed out')
            if not select.select([probe.stdout], [], [], 0.1)[0]:
                continue
            data = os.read(probe.stdout.fileno(), 4096)
            if not data:
                break
            buffer += data
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                line = line.decode().strip()
                print(line, flush=True)
                print(line, file=output, flush=True)
                if line != 'READY reroute':
                    continue
                before = graph()
                old_stream = playback_link(before, first)
                assert old_stream is not None, 'No active playback on the first sink'
                (work / 'before-switch.json').write_text(json.dumps(before, indent=2))
                command('wpctl', 'set-default', str(second))
                switch_deadline = time.monotonic() + 10
                while True:
                    after = graph()
                    new_stream = playback_link(after, second)
                    if new_stream is not None and new_stream != old_stream:
                        break
                    assert time.monotonic() < switch_deadline, 'No replacement stream on the second sink'
                    time.sleep(0.02)
                (work / 'after-switch.json').write_text(json.dumps(after, indent=2))
                print(f'Private reroute: stream {old_stream} -> {new_stream}', flush=True)
                probe.stdin.write(b'switched\n')
                probe.stdin.flush()
                switched = True
        result = probe.wait(timeout=10)
        assert result == 0, f'Playback probe failed with exit {result}'
        assert switched, 'No reroute was tested'
    assert 'reroute_mapper_device' in (work / 'wine.log').read_text(), 'No WinMM reroute trace'
finally:
    if probe and probe.poll() is None:
        probe.kill()
        probe.wait()
    try:
        subprocess.run([str(runtime / 'bin/wineserver'), '-k'], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    finally:
        for child in reversed(children):
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
        for log in logs:
            log.close()
