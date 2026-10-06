# DMA-BUF notification tests

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
