# Parser read cache tests

These native Linux tests compile the production `read_thread()` by including
`quartz_parser.c`. Mock IAsyncReader and WG callbacks supply deterministic
requests and validate every returned byte and upstream read. No GStreamer
pipeline, Wine installation, game, or graphics device is required.

Wine's generated headers must already exist in a configured build's
`include` directory, including `strmif.h`, `amvideo.h`, and `mfidl.h`:

```sh
make -C dlls/winegstreamer/native-tests check \
    WINE_BUILD_DIR=/path/to/configured-wine-build
```

For an in-tree Wine build, omit `WINE_BUILD_DIR`. Set
`GENERATED_INCLUDE_DIR=/path/to/generated/include` when generated headers
are stored separately. `BUILD_DIR=/path/to/test-output` selects another
output directory. The tests do not regenerate or write Wine build resources,
and no test binaries are installed.

For GCC AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -C dlls/winegstreamer/native-tests check \
    WINE_BUILD_DIR=/path/to/configured-wine-build BUILD_DIR=/tmp/parser-asan \
    CFLAGS='-O1 -g -fsanitize=address,undefined --param=asan-globals=0 -fno-omit-frame-pointer'
```

Disabling ASan global instrumentation lets ELF section garbage collection
discard unrelated Wine functions and their external dependencies. Heap and
stack address checks remain enabled. If LeakSanitizer cannot run in a traced
or restricted environment, prefix the command with
`ASAN_OPTIONS=detect_leaks=0`.

For a 32-bit native compiler and runtime:

```sh
make -C dlls/winegstreamer/native-tests check \
    WINE_BUILD_DIR=/path/to/32-bit-wine-build BUILD_DIR=/tmp/parser-32 \
    CFLAGS='-O2 -g -m32' LDFLAGS=-m32
```

Coverage includes repeated and overlapping cache hits, extending an existing
read, backward and disjoint seeks, buffer growth, EOF clipping, failed
extensions, and partial reads with and without cached data.
[`IAsyncReader::SyncRead`](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-iasyncreader-syncread)
returns `S_FALSE` for a partial read without exposing the actual byte count.
Those responses must neither reach WG as a complete buffer nor enter the
cache. These are parser read-thread unit tests, not playback or performance
tests.
