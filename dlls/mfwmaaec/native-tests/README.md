Run `make check` for deterministic capture tests or `make sanitize` for ASan and
UBSan. Use `BUILD_DIR=/tmp/aaec-tests` to keep generated files outside the tree.

The runner compiles the production capture routines and state declaration with
mocked WASAPI providers and allocation failures. It checks output bounds, packet
handling and capture lifetime without opening a microphone. These host tests do
not validate COM ABI layout. The DMO and MFT conformance tests live in
`dlls/mf/tests/transform.c` and run against the built DLL.
