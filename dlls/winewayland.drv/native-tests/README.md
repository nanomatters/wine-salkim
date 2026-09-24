# DMA-BUF notification tests

Run on Linux with a native C compiler:

```sh
make -C dlls/winewayland.drv/native-tests check
```

`BUILD_DIR=/path/to/build` selects another output directory. For sanitizers:

```sh
make -C dlls/winewayland.drv/native-tests check BUILD_DIR=/tmp/dmabuf-asan \
    CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined'
```

The tests compile the production event-coalescing and receive helpers directly. They use real Unix `SOCK_SEQPACKET` sockets, `SCM_RIGHTS`, and
edge-triggered `epoll`. Only interrupted/fatal receive errors are injected, to
exercise those branches deterministically. No sleeps, compositor, GPU, Wine
installation or full Proton build are needed. No test binaries are installed.

Coverage includes descriptor ownership on malformed/truncated messages,
drain-to-EAGAIN, late watch registration, subsequent readiness, stale surface
identities and deferred-fence readiness. These are transport/policy unit tests, not an end-to-end
Wayland window-lifecycle or game performance test.
