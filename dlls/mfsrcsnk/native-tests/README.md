Run `make check` from this directory to test descriptor validation in the production
`media_source_start()` function with controlled providers. No Wine installation or
media codec is needed. The test covers descriptor-count failure, shutdown, invalid
zero stream identifiers and a valid identifier. It checks that a failed descriptor
does not modify stream state, seek position or emit startup events.

The build extracts the function from `media_source.c` into a generated header.
The harness supplies simplified objects and providers. It tests control flow,
not COM ABI compatibility or asynchronous media playback.

For sanitizer checking, run:

```sh
make check BUILD_DIR=/tmp/mfsrcsnk-source-tests CFLAGS='-O1 -g -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'
```
