# Wayland driver native tests

Run on Linux with a native C compiler:

```sh
make -C dlls/winewayland.drv/native-tests check
```

`BUILD_DIR=/path/to/build` selects another output directory. For sanitizers:

```sh
make -C dlls/winewayland.drv/native-tests check BUILD_DIR="$HOME/tmp/dmabuf-asan" \
    CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined'
```

The tests compile the production event-coalescing, receive, update-mode and
consumer-state helpers directly. They use real Unix `SOCK_SEQPACKET` sockets,
`SCM_RIGHTS`, and edge-triggered `epoll`. Only interrupted/fatal receive errors
are injected, to exercise those branches deterministically. No sleeps,
compositor, GPU, Wine installation or full Proton build are needed. No test
binaries are installed.

Coverage includes descriptor ownership on malformed/truncated messages,
drain-to-EAGAIN, late watch registration, subsequent readiness, stale surface
identities, deferred-fence readiness, and legacy versus readiness-driven
consumer states. Teardown tests retain server-like socket duplicates and fill
the reply queue, checking hangup/EOF both before the frame queue fills and
under send backpressure. Successful suspension leaves the socket reusable;
replacement sockets have a distinct identity even while old handles survive.
Update-mode tests cover initial mapping, retained child
presentation, direct-parent buffer restrictions and resuming normal updates.
A socket test exercises delivery and replies while reconfiguration is pending;
it does not simulate a compositor or assert that a frame was displayed. These
are transport/policy unit tests, not an end-to-end Wayland window-lifecycle or
game performance test.

## Monitor placement and fullscreen targets

`make monitor-check BUILD_DIR=/path/to/build` runs seven suites that compile
the production C functions with injected platform calls:

- `display-cache.py` tests win32u's accepted physical layout query and publication
  locking. Its cached sources use Video registry aliases such as `0000`, whose
  opened keys resolve to connector names. It covers failed identity queries,
  bootstrap, snapshot capacity and physical versus emulated modes.
- `display-publication.py` tests win32u's publication and cache-refresh control
  flow. A pending replacement must skip committing devices, release the
  publication lock and GPU probe data, and reload the previous accepted display
  snapshot. Startup without a snapshot still fails rather than inventing a
  monitor. A later completed update publishes normally.
- `display-notification.py` tests Wayland's opt-in asynchronous application
  notification after publication. It compares attached connector identities,
  placement, primary selection and current/physical modes rather than union
  bounds. Identical events and enumeration-only changes do not notify again.
  Removal, restoration, in-place rearrangement, mode changes, invalid identities,
  allocation failures and source reference cleanup are covered. Existing
  non-Wayland host and application-initiated mode change paths are unchanged.
  Message transport and registry publication are injected calls, so this does
  not replace a Wine window message integration test.
- `monitor-layout.py` tests incremental startup announcements in both monitor
  orders, delayed geometry, hotplug and mixed scaling. Startup coordinates
  remain provisional until a complete desktop layout has been accepted.
- `output-layout.py` tests primary selection and accepted layout reconciliation.
  It covers missing and partial snapshots, monitor removal and re-enabling a
  monitor while the previous coordinate map remains authoritative. Delayed
  replacement modes defer publication before any device callback. Repeated
  pending updates, startup, recovery, removal of a pending output and a ready
  survivor with an unrelated pending output are covered.
- `fullscreen-outputs.py` tests retained output identity, explicit versus
  window-following targets, removal, presentation invalidation and reference
  cleanup.
- `monitor-outputs.py` tests enter/leave delivery, owner-thread reconciliation,
  ambiguous memberships, virtual desktops, window positioning and separation
  of fullscreen intent from compositor placement. It also runs the configure,
  accepted geometry and acknowledgement chain in both configure/enter orders.
  Repeated hotplug checks disable the initial output, restore it with a new
  identity and remove the other output while presentation remains on the
  survivor. They verify origin rebasing without a fresh enter event, unchanged
  state updates preserving Win32u and inferred fullscreen, and real geometry,
  style, hide or minimize changes leaving the preservation path.
  The output-removal regression also runs the production toplevel visibility
  decision while the old HWND is outside the surviving desktop. The replacement
  fullscreen configure must be acknowledged before the survivor's enter arrives.
  Explicit hiding, cloaking, minimize capabilities and fullscreen exit retain
  their existing visibility behavior. The visibility test injects platform calls
  and does not run native presentation or compositor frame callbacks.
  Fullscreen preservation tests also exercise the production driver decision
  before mode inference. They retain win32u's newly detected fullscreen state,
  reject hide, minimize and style transitions, and consume the internal geometry
  waiver before subsequent application position changes.
  Application retargeting tests request A, accept compositor placement on B,
  then request A again through the production window-position decisions.
  They verify one renewed request, preservation through pending configures and
  stale membership reconciliation, and no repeated requests from passive
  updates. Size-only changes retain the chosen monitor, including Vulkan WINDOW
  targets. Settled application moves on the requested and observed output do
  not renew fullscreen. Unknown or ambiguous membership, pending configures,
  unconfirmed fullscreen and an already deferred request retain renewal.
  Moving to another output still sends one request. Virtual desktops, fixed
  Vulkan targets and fullscreen style guards
  retain their separate selection rules. These tests extract the target and
  preservation blocks and supply mode-query results and Win32 calls with mocks.

`WAYLANDDRV_PRIMARY_MONITOR` selects the primary when a Wine session first
publishes its desktop. Processes joining that session reuse its shared Windows
coordinate map rather than applying a conflicting local origin. A compositor
move updates the window's observed monitor without turning the observation into
a new fullscreen request. Wayland has no global window-position event, so
windowed placement remains synthetic and enter/leave does not move or clamp
windowed dialogs. Only settled fullscreen membership adjusts the Win32 origin.
An explicit Vulkan monitor stays fixed. Configures change the accepted size
before owner-thread output reconciliation changes the fullscreen origin.
An application fullscreen move renews its monitor request even if the same
monitor was requested before the compositor relocated the window. Its output
identity survives configure processing. Passive reconciliation and resizing
alone do not renew that request.
Each new window synchronizes its process's layout before its initial Win32
rectangle is interpreted. This query runs without the driver window lock.
Connector names are resolved once when win32u loads each source, including
registry aliases. Repeated layout reads use those cached names.
Wayland explicitly requests asynchronous `WM_DISPLAYCHANGE` application
notifications after an accepted attached topology or display mode changes.
Repeated identical output events still refresh the desktop but do not emit
another application broadcast. Other drivers retain their existing host-change
notification policy.
When the last initialized output disappears but a replacement has already been
announced, Wine retains the published Win32 display snapshot until the output's
initial mode arrives. The existing output events retry publication without a
timer or blocking wait. Removed Wayland outputs are still released immediately.
Ready surviving outputs and genuinely empty output lists keep their existing
publication policy.

`source-name.py` additionally tests the production connector-name helper through
real registry links in a dedicated Wine prefix below its build directory:

```sh
python3 dlls/winewayland.drv/native-tests/source-name.py \
    --build-dir "$HOME/tmp/wayland-monitor-tests/registry-link" \
    --wine /path/to/wine
```

It requires a MinGW compiler and leaves its own test keys in that isolated
prefix. It neither launches a game nor tests compositor window placement.

The default compiler target is native. For the 32-bit and 64-bit reconciliation
matrix, including sanitizers:

```sh
python3 dlls/winewayland.drv/native-tests/monitor-outputs.py \
    --bits all --sanitizers --build-dir "$HOME/tmp/wayland-monitor-tests/monitor"
```

The other suites accept `--cc` and `--cflags` for a 32-bit or sanitized run.
`check-driver-build.py` additionally compiles the live changed translation
units with an existing Wine build's configuration, rather than its staged
source copies. Add `--all` to compile all driver translation units and check
consumers of the changed shared structures. `--win32u` includes the display
cache translation unit:

```sh
python3 dlls/winewayland.drv/native-tests/check-driver-build.py \
    --all --win32u \
    --wine-build /opt/devel/wineland/build \
    --output-dir "$HOME/tmp/wayland-monitor-tests/driver"
```

These are implementation-level regression tests. They do not prove actual
compositor placement or game behaviour. Runtime checks should cover moving a
window between unequal-resolution and mixed-scale outputs, fullscreen and
virtual-desktop presentation, monitor rearrangement and unplugging a targeted
output. Check `+waylanddrv` traces for the requested and observed output and
compare the game's reported monitor after the owning thread applies placement.

## Event reader lifetime

`make reader-check BUILD_DIR=/path/to/build` tests startup failure, publication
ordering, fatal display errors and clipboard callback handoff. The runner
compiles the production functions with injected platform calls. A real pthread
starts before publication and must wait for both the driver and its desktop.
Clipboard tests cover FIFO delivery, invalid and replayed messages, the wrong
receiving thread, allocation and posting failures, window destruction and
undelivered work after owner-thread termination. These tests do not create Wine
threads or simulate PE callbacks.

The signal tests also cover the ntdll prerequisite. Unix-only readers must not
install PE TLS when a signal arrives in a WoW64 process. Ordinary PE threads
must retain their TLS and syscall-dispatch transitions. These tests inject the
TLS operation rather than changing the test runner's real FS base.

`reader-lifetime.c.test` is a separate PE integration probe. Build it with MinGW
and run it only in a disposable Wine prefix on an isolated compositor. Its TLS
callback loads the display driver and terminates application-visible threads
whose start address is in the PE Wayland driver. The old reader is found and
terminated. A Unix system reader is not in the thread snapshot. The probe also
checks that ordinary application threads remain terminable and exercises
window creation, painting, maximizing and restoring. Use `+waylanddrv` traces
to check configure dispatch. Painting alone does not prove host visibility.

`reader-callbacks.c.test` and `reader-hook.c.test` check the callback handoffs in a real
Wine runtime. Build the latter as a DLL exporting undecorated names. Pass its
path and the value of `WM_WAYLAND_NOTIFY_REORDER` to the probe. It posts the
driver message and verifies that an in-context WinEvent hook runs on its UI
thread. This does not simulate real pointer exposure or stacking.

The probe then offers `CF_TEXT`. In the isolated Wayland session, request
`text/plain;charset=utf-8` with `wl-paste` and check for
`reader-clipboard-source`. This exercises Unicode synthesis on clipboard
export. Copy `reader-clipboard-replacement` with `wl-copy` to verify selection
delivery and clipboard import. Run the probes as 64-bit, native 32-bit and
WoW64 processes. Keep generated binaries and logs under a persistent build
directory such as `$HOME/tmp/wayland-reader-tests`.

The `.c.test` suffix keeps these standalone programs out of the driver's
automatically generated source list. Compile them with an explicit `-x c`.
For example, from this directory:

```sh
reader_build="$HOME/tmp/wayland-reader-tests"
mkdir -p "$reader_build"
x86_64-w64-mingw32-gcc -O2 -g -x c reader-lifetime.c.test -x none \
    -o "$reader_build/reader-lifetime.exe" -luser32 -lgdi32
x86_64-w64-mingw32-gcc -O2 -g -shared -Wl,--kill-at -x c reader-hook.c.test -x none \
    -o "$reader_build/reader-hook.dll"
x86_64-w64-mingw32-gcc -O2 -g -x c reader-callbacks.c.test -x none \
    -o "$reader_build/reader-callbacks.exe" -luser32
```

Use `i686-w64-mingw32-gcc` to build 32-bit probes for native 32-bit and WoW64 runs.

## System-thread exceptions

`reader-exceptions.c.test` tests real faults on a thread created with
`PsCreateSystemThread`. Build it both as a Unix library and as a PE executable.
It requires a matching Wine build and a private runtime containing the ntdll
changes. Use a disposable prefix. No compositor or GPU is required.

For a 64-bit build, set `reader_source` to the Wine source tree,
`reader_wine_build` to its configured build directory, and `reader_runtime` to
the private runtime. Keep test outputs in a persistent directory:

```sh
reader_build="$HOME/tmp/wayland-reader-exceptions"
mkdir -p "$reader_build"
cc -shared -fPIC -O2 -g -D__WINESRC__ -DWINE_UNIX_LIB \
    -I"$reader_wine_build/include" -I"$reader_source/include" \
    -x c reader-exceptions.c.test -x none \
    "$reader_runtime/lib/wine/x86_64-unix/ntdll.so" -Wl,-z,defs \
    -o "$reader_runtime/lib/wine/x86_64-unix/reader-exceptions.so"
x86_64-w64-mingw32-gcc -O2 -g -x c reader-exceptions.c.test -x none \
    -o "$reader_build/reader-exceptions.exe"
WINEPREFIX="$reader_build/prefix" "$reader_runtime/bin/wine" \
    "$reader_build/reader-exceptions.exe" guarded
WINEPREFIX="$reader_build/prefix" WINEDEBUG=-all,+seh "$reader_runtime/bin/wine" \
    "$reader_build/reader-exceptions.exe" fatal
```

The guarded run must exit with status 0. It checks invalid source and destination
buffers in `NtReadVirtualMemory`, direct Unix `__TRY` recovery, successful work
after a recovered fault, and delivery of a suspend signal. The fatal run must
exit with status 5, the Unix exit status for `STATUS_ACCESS_VIOLATION`, and log
`Exception c0000005 in system thread`. Neither run may invoke the probe's PE
vectored exception handler.

For native 32-bit Wine, build the Unix helper with `cc -m32`, the matching
32-bit build headers and ntdll, and use the `i386-unix` runtime directory.
Build the PE launcher with `i686-w64-mingw32-gcc`. For WoW64, use that 32-bit
launcher with the 64-bit Unix helper and a separate `WINEARCH=wow64` prefix.
