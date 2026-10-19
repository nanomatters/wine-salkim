#!/usr/bin/env python3
"""Exercise WASAPI notifications on an isolated PipeWire graph and Wine prefix.

Pass a private Wine runtime containing the modified winepipewire.so and
mmdevapi.dll, and a directory containing hotplug.exe.
"""
import argparse
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--app', type=Path, required=True)
parser.add_argument('--work', type=Path)
parser.add_argument('--lifetime-probe', type=Path,
                    help='optional notification-lifetime.exe of the same architecture; repeat both exit modes 10 times')
args = parser.parse_args()


def executable_machine(path):
    with path.open('rb') as executable:
        executable.seek(0x3c)
        offset, = struct.unpack('<I', executable.read(4))
        executable.seek(offset + 4)
        return struct.unpack('<H', executable.read(2))[0]


machine = executable_machine(args.app / 'hotplug.exe')
if args.lifetime_probe:
    args.lifetime_probe = args.lifetime_probe.resolve()
    assert executable_machine(args.lifetime_probe) == machine, 'Lifetime and hotplug probes must have the same architecture'
arch = {0x8664: 'x86_64', 0x14c: 'i386'}[machine]
wine = args.runtime / 'lib/wine' / (arch + '-unix') / 'wine'
work = args.work or Path(tempfile.mkdtemp(prefix='winepipewire-hotplug-runtime.'))
work.mkdir(parents=True, exist_ok=True)
server_dir = work / 'server'
server_dir.mkdir(mode=0o700, exist_ok=True)
env = os.environ.copy()
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WINELOADER', 'WINEDLLPATH', 'WINESERVER',
            'PIPEWIRE_CORE', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR', 'WINEARCH'):
    env.pop(key, None)
env.update(PIPEWIRE_RUNTIME_DIR=str(server_dir), XDG_RUNTIME_DIR=str(server_dir),
           PIPEWIRE_REMOTE='pipewire-0', WINEPREFIX=str(work / 'prefix'),
           WINE_AUDIO_DRIVER='pipewire',
           WINEDLLOVERRIDES='mmdevapi=b;winemenubuilder.exe=d',
           WINEDEBUG='-all,+mmdevapi,+loaddll,+pipewire,+seh')
server = probe = None
children = []
server_log = (work / 'pipewire.log').open('w')
wine_log = (work / 'wine.log').open('w')


def command(*argv):
    return subprocess.check_output(argv, env=env, text=True, stderr=subprocess.STDOUT, timeout=15)


def node_id(name):
    for entry in json.loads(command('pw-dump')):
        if entry.get('info', {}).get('props', {}).get('node.name') == name:
            return entry['id']
    raise AssertionError('No private node ' + name)


def add_node(name, description, media='Audio/Sink'):
    command('pw-cli', 'create-node', 'adapter', '{ factory.name=support.null-audio-sink '
            f'node.name={name} node.description={description} media.class={media} '
            'audio.position=[FL FR] object.linger=true }')
    return node_id(name)


def default(name):
    command('pw-metadata', '-n', 'default', '0', 'default.audio.sink',
            json.dumps({'name': name}), 'Spa:String:JSON')


def start_probe(*arguments):
    child = subprocess.Popen([str(wine), str(args.app / 'hotplug.exe'), *arguments],
                             cwd=args.app, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=wine_log, text=True, bufsize=1)
    children.append(child)
    if not arguments:
        assert select.select([child.stdout], [], [], 30)[0], 'No READY; see ' + str(work)
        assert child.stdout.readline().strip() == 'READY', 'Probe startup failed; see ' + str(work)
    return child


def wait_probe(child):
    code = child.wait(timeout=15)
    assert code == 0, ('Probe failed after its last response', child.pid, code, str(work / 'wine.log'))


def request(message, child=None):
    child = child or probe
    child.stdin.write(message + '\n')
    child.stdin.flush()
    if not select.select([child.stdout], [], [], 15)[0]:
        raise AssertionError('Timed out waiting for ' + message)
    line = child.stdout.readline().strip()
    if message != 'snapshot': print(line, flush=True)
    return line


def snapshot(render, capture, previous=None, minimums=None):
    deadline = time.monotonic() + 10
    while True:
        line = request('snapshot').split()
        assert line and line[0] == 'SNAPSHOT', line
        values = list(map(int, line[1:]))
        assert values[-1] == 0, values
        enough = not minimums or all(values[i] >= previous[i] + n for i, n in minimums.items())
        if values[:2] == [render, capture] and enough:
            print('SNAPSHOT', *values, flush=True)
            return values
        assert time.monotonic() < deadline, (render, capture, values, previous, minimums)
        time.sleep(0.05)


def wait_pnp(expected, child=None, endpoint=None):
    deadline = time.monotonic() + 10
    while True:
        response = request('pnp' + (' ' + endpoint if endpoint else ''), child).split()
        assert response[0] == 'PNP' and response[-1] == '0', response
        if list(map(int, response[1:3])) == [expected, expected]:
            return
        assert time.monotonic() < deadline, response
        time.sleep(0.05)


def wait_endpoint_id(name, child=None):
    deadline = time.monotonic() + 10
    while True:
        response = request('id ' + name, child).split()
        assert response[0] == 'ID' and response[-1] == '0', response
        if response[1] == '1':
            assert response[2] != '-', response
            return response[2]
        assert response[1] == '0' and time.monotonic() < deadline, response
        time.sleep(0.05)


def wait_retained(state):
    deadline = time.monotonic() + 10
    result = 0 if state == 1 else 0x88890004  # AUDCLNT_E_DEVICE_INVALIDATED
    while True:
        response = request('retained').split()
        assert response[0] == 'RETAINED' and response[-1] == '0', response
        if int(response[1]) == state and int(response[2], 0) == result:
            return
        assert time.monotonic() < deadline, response
        time.sleep(0.05)


def check_concurrent_ids():
    # All eight processes watch the same prefix before creating a previously
    # unseen endpoint. Every process must publish the one persisted GUID.
    peers = [start_probe() for _ in range(8)]
    try:
        for iteration in range(4):
            name = 'HotplugRace' + str(iteration)
            node = add_node('hotplug.race.' + str(iteration), name)
            ids = [wait_endpoint_id(name, child) for child in peers]
            assert len(set(ids)) == 1, ('Processes assigned different endpoint IDs', ids)
            # Also test a fresh process: its registry lookup must agree with
            # every process which raced to discover the device.
            newcomer = start_probe()
            try:
                assert wait_endpoint_id(name, newcomer) == ids[0]
            finally:
                assert request('quit', newcomer) == 'DONE 0'
                wait_probe(newcomer)
            command('pw-cli', 'destroy', str(node))
    finally:
        for child in peers:
            if child.poll() is None:
                assert request('quit', child) == 'DONE 0'
                wait_probe(child)


def check_lifetime_stress():
    if not args.lifetime_probe:
        return
    with (work / 'notification-lifetime.log').open('w') as output_log:
        for mode in ('exitthread', 'exitprocess'):
            for iteration in range(10):
                argv = [str(wine), str(args.lifetime_probe)]
                if mode == 'exitprocess':
                    argv.append(mode)
                child = subprocess.Popen(argv, cwd=args.lifetime_probe.parent, env=env,
                                         stdout=subprocess.PIPE, stderr=wine_log, text=True)
                children.append(child)
                output, _ = child.communicate(timeout=30)
                label = f'LIFETIME {mode} {iteration + 1}/10 exit={child.returncode}'
                print(label, output.rstrip(), sep='\n', flush=True)
                output_log.write(label + '\n' + output)
                output_log.flush()
                assert child.returncode == 0, (label, output, str(work / 'wine.log'))
                lines = output.splitlines()
                assert len(lines) == 2 and lines[1] == ('EXIT_PROCESS' if mode == 'exitprocess' else 'EXIT_THREAD'), lines
                result = re.fullmatch(r'REGISTRY_LIFETIME failures=(\d+) refs=(\d+) handles=(\d+)->(\d+)', lines[0])
                assert result, lines
                failures, refs, before, after = map(int, result.groups())
                assert failures == 0 and refs == 1 and after <= before + 16, (label, result.groups())


def check_probe_lifetime():
    for mode in ('exitthread', 'exitthread-notify'):
        lifetime = start_probe(mode)
        output, _ = lifetime.communicate(timeout=15)
        assert lifetime.returncode == 0 and output.strip() == 'EXIT_THREAD 0', (mode, lifetime.returncode, output)
        print('PASS: releasing MMDevAPI references permits last-thread exit:', mode, flush=True)


try:
    server = subprocess.Popen(['pipewire', '-c', str(Path(__file__).with_name('hotplug-pipewire.conf'))],
                              env=env, stdout=server_log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 10
    while not (server_dir / 'pipewire-0').exists():
        assert server.poll() is None, 'private PipeWire failed; see ' + str(work)
        assert time.monotonic() < deadline
        time.sleep(0.05)
    add_node('hotplug.base', 'HotplugBase')
    default('hotplug.base')
    # The prefix is disposable; select only PipeWire through Wine's usual key.
    command(str(wine), 'reg', 'add', r'HKCU\Software\Wine\Drivers',
            '/v', 'Audio', '/d', 'pipewire', '/f')
    probe = start_probe()
    pids = [probe.pid]
    maps = ''
    for pid in pids:
        try:
            maps += Path(f'/proc/{pid}/maps').read_text()
            pids += list(map(int, Path(f'/proc/{pid}/task/{pid}/children').read_text().split()))
        except FileNotFoundError:
            pass
    # Wine may create the other-architecture process through wineserver, not
    # as a Unix child of its launcher. Match only our unique private prefix.
    marker = ('WINEPREFIX=' + env['WINEPREFIX']).encode()
    for process in Path('/proc').glob('[0-9]*'):
        try:
            if marker in (process / 'environ').read_bytes().split(b'\0'):
                maps += (process / 'maps').read_text()
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    (work / 'probe-maps.txt').write_text(maps)
    assert str(args.runtime / 'lib/wine') in maps, 'Probe is not using the private runtime'
    assert str(args.runtime / 'lib/wine/x86_64-unix/winepipewire.so') in maps or \
           str(args.runtime / 'lib/wine/i386-unix/winepipewire.so') in maps, 'Wrong Unix backend'
    before = snapshot(2, 1)
    added = add_node('hotplug.new', 'HotplugNew')
    after = snapshot(3, 1, before, {2: 1, 5: 1})
    assert request('hold') == 'HOLD 0'
    assert request('state') == 'STATE 1 1 3 0'
    endpoint_id = wait_endpoint_id('HotplugNew')
    wait_pnp(1)
    default('hotplug.new')
    switched = snapshot(3, 1, after, {4: 3})
    command('pw-cli', 'destroy', str(added))
    removed = snapshot(2, 1, switched, {3: 1, 5: 1})
    assert request('state') == 'STATE 4 1 3 0'
    wait_pnp(0)
    newcomer = start_probe()
    wait_pnp(0, newcomer, endpoint_id)
    assert request('quit', newcomer) == 'DONE 0'
    wait_probe(newcomer)
    # Starting another MMDevAPI process must not revive stale PnP entries.
    wait_pnp(0)
    add_node('hotplug.new', 'HotplugNew')
    replugged = snapshot(3, 1, removed, {2: 1, 5: 1})
    assert request('state') == 'STATE 1 1 3 0'
    assert wait_endpoint_id('HotplugNew') == endpoint_id
    wait_pnp(1)
    capture_id = add_node('hotplug.capture', 'HotplugCapture', 'Audio/Source/Virtual')
    captured = snapshot(3, 2, replugged, {2: 1, 5: 1})
    command('pw-cli', 'destroy', str(capture_id))
    snapshot(3, 1, captured, {3: 1, 5: 1})
    assert request('override') == 'OVERRIDE 0'
    time.sleep(0.15)  # Let the independent Wine registry watcher deliver its changes.
    assert request('selected') == 'SELECTED 1 0'
    overridden = snapshot(3, 1)
    default('hotplug.base')
    time.sleep(0.15)
    unchanged = snapshot(3, 1)
    assert unchanged[4] == overridden[4], 'Host default must not override a named Wine selection'
    assert request('selected') == 'SELECTED 1 0'
    assert request('clear') == 'CLEAR 0'
    time.sleep(0.15)
    assert request('selected') == 'SELECTED 0 0'
    before_restart = snapshot(3, 1)
    server.terminate()
    server.wait(timeout=10)
    snapshot(1, 1, before_restart, {3: 2, 5: 2})
    assert request('state') == 'STATE 4 1 3 0'
    wait_pnp(0)
    server = subprocess.Popen(['pipewire', '-c', str(Path(__file__).with_name('hotplug-pipewire.conf'))],
                              env=env, stdout=server_log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 10
    while not (server_dir / 'pipewire-0').exists():
        assert server.poll() is None and time.monotonic() < deadline
        time.sleep(0.05)
    add_node('hotplug.base', 'HotplugBase')
    add_node('hotplug.new', 'HotplugNew')
    default('hotplug.base')
    snapshot(3, 1)
    assert request('state') == 'STATE 1 1 3 0'
    assert wait_endpoint_id('HotplugNew') == endpoint_id
    wait_pnp(1)
    # The enumerator is not the sole owner of hotplug monitoring. Existing
    # endpoint/client/property-store references must keep observing removals.
    assert request('pause-enumerator') == 'PAUSED 0'
    wait_retained(1)
    command('pw-cli', 'destroy', str(node_id('hotplug.new')))
    wait_retained(4)
    assert request('resume-enumerator') == 'RESUMED 0'
    snapshot(2, 1)
    add_node('hotplug.new', 'HotplugNew')
    snapshot(3, 1)
    wait_retained(1)
    # With every MMDevAPI reference gone the monitor may stop. Reopening must
    # synchronously reconcile the graph before returning an enumerator.
    assert request('stop-enumerator') == 'STOPPED 0'
    command('pw-cli', 'destroy', str(node_id('hotplug.new')))
    assert request('resume-enumerator') == 'RESUMED 0'
    values = request('snapshot').split()
    assert values[0] == 'SNAPSHOT' and values[1:3] == ['2', '1'] and values[-1] == '0', values
    assert request('quit') == 'DONE 0'
    wait_probe(probe)
    check_concurrent_ids()
    print('PASS: hotplug functionality: arrival, removal, reconnect identity, PnP presence, collections, '
          'defaults, overrides, server restart, callback re-entry, retained references and cross-process IDs;',
          work, flush=True)
    check_probe_lifetime()
    check_lifetime_stress()
    print('PASS: full hotplug integration and requested lifetime checks;', work, flush=True)
finally:
    for child in children:
        if child.poll() is None:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    # This wineserver command targets only the disposable prefix in env.
    subprocess.run([str(args.runtime / 'bin/wineserver'), '-k'], env=env,
                   stdout=wine_log, stderr=subprocess.STDOUT, timeout=10)
    if server and server.poll() is None:
        server.terminate()
        server.wait(timeout=10)
    server_log.close()
    wine_log.close()
