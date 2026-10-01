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
