These native Linux tests exercise the driver's actual helpers and entry points
without starting Wine or connecting to PipeWire. Unused functions are discarded
at link time. The tests replace only the runtime functions they need.

With PipeWire development headers installed and Wine headers generated:

Install the libudev development package as well if Wine was configured with
udev support.

```
make check WINE_BUILD_DIR=/path/to/configured/wine/build
```

Use `BUILD_DIR=/path/to/temporary/directory` to keep test binaries outside the
source tree. `CC` and `CFLAGS` can select another compiler or sanitizers.

Fixtures use `.c.test` names and are compiled explicitly as C, so Wine's
makefile generator cannot add them to the driver's production sources.

Hotplug coverage
----------------

`hotplug.c.test` exercises the production registry callbacks, blocking change
wait, transactional cache replacement, stream invalidation and monitor teardown.
It covers allocation failures, connection loss before and during a snapshot,
simultaneous format readers, repeated unplug/replug and shutdown while waiting.
It needs no PipeWire daemon or hardware.

For address-sanitizer builds with GCC, add `--param=asan-globals=0` to CFLAGS.
Otherwise sanitizer registration of the production dispatch tables retains
unrelated entry points which need ntdll.

`hotplug-runtime.c.test` and `hotplug-runtime.py` test the complete MMDevAPI path.
Build the probe, for example:

```
x86_64-w64-mingw32-gcc -x c -O2 -Wall hotplug-runtime.c.test \
    -o /path/to/test-app/hotplug.exe -lole32 -luuid -lpropsys -lsetupapi
```

Use a private installed Wine runtime rebuilt with both mmdevapi.dll and
winepipewire.so, not an in-place replacement of desktop Wine. Its Wine loaders
and ntdll.so must be real files, not symlinks into another installation, or
Wine's alternate-architecture loader can silently select that installation's
old Unix backend. The script checks loaded backend paths.

```
python3 hotplug-runtime.py --runtime /path/to/private/wine \
    --app /path/to/test-app
```

The script creates a disposable Wine prefix and starts PipeWire on a private
runtime socket using `hotplug-pipewire.conf`. There is no hardware discovery or
session manager; the virtual nodes never enter the desktop audio graph. It
requires `pipewire`, `pw-cli`, `pw-metadata` and `pw-dump`. Results and logs are
left in the printed temporary directory (or `--work /path/to/empty/directory`).

The checks cover output/capture arrivals and removals, all default roles,
callback re-entry, stable endpoint identity across reconnect, immutable old
collections, explicit Wine default overrides, and private-server restart.
It also checks that removed endpoints disappear from SetupAPI's present-device
enumeration, including after a fresh process starts, and return under the same
instance ID on reconnect. Eight simultaneous observers must agree on each new
endpoint's ID and with a subsequent process reading its persisted ID. A separate
probe releases its MMDevAPI references and exits its last application thread;
the device monitor must not keep the process alive, either with no notification
registration or after registering and unregistering a client. Another case
releases the enumerator while retaining an endpoint, audio client and property
store: device removal must still be observed. After releasing every reference,
reopening an enumerator must immediately return the current graph snapshot.
Repeat with an i686-w64-mingw32-built probe and the matching 32-bit Wine runtime.
Callbacks defer final COM releases to the main thread, as required by the
IMMNotificationClient contract. No physical USB/Bluetooth hotplug or live
playback migration is claimed by these virtual-device tests.

`notification-lifetime.c.test` separately stresses concurrent notification
registration/unregistration and worker restart. Build it with the matching
MinGW compiler and run in the same isolated runtime/prefix:

```
x86_64-w64-mingw32-gcc -x c -O2 -g notification-lifetime.c.test \
    -o /path/to/test-app/notification-lifetime.exe -lole32 -luuid
```

It performs 2,000 concurrent registration cycles and 100 retirement cycles,
checks real process handle counts through `SystemExtendedHandleInformation`,
then exits its last application thread, or performs normal process exit when
given `exitprocess`. Add the optional probe to the isolated integration runner:

```
python3 hotplug-runtime.py --runtime /path/to/private/wine \
    --app /path/to/test-app \
    --lifetime-probe /path/to/test-app/notification-lifetime.exe
```

The runner performs ten runs of each exit mode, checks callback references and
handle counts, and writes `notification-lifetime.log`. Each run has a 30-second
limit; a timeout or any nonzero exit (including a Unix signal) is a failure. Both
probes must be built for the same architecture.

Functional graph checks run before the strict last-thread-exit checks and have
their own success line. This Wine base also has an independently reproduced
last-thread exit-code race: a multi-threaded program using no audio or COM can
return status 1 even though every thread calls `ExitThread(0)`. The runner still
requires status 0; it does not suppress that failure or report a full-suite pass
when only the functional checks passed. Normal process-exit stress is checked
separately and also requires status 0.
