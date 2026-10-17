# Process keyboard default runtime probe

`keyboard-default-runtime.c.test` exercises the real win32u and wineserver
implementation. It publishes synthetic host layouts through Wine's private
`NtUserCallOneParam_SetDefaultKeyboardLayout` call. Compile against the same
source tree as the Wine runtime being tested.

The probe checks an English user locale with German and French keyboards:

- Layout names queried before the first publication must update afterwards.
- Existing workers that have not selected a layout must see the process default.
- A worker's own query and another thread's query for it must agree.
- Replacing or clearing the default must leave explicitly activated layouts alone.
- Activating the current default explicitly must also pin that thread's layout.
- Activation must return the previous effective default layout.
- Creating an IME client context before the first host publication must leave
  its owner following the default, including later default changes.
- A worker attaching the client to another thread's placeholder context through
  `ImmLockIMC` must not select a layout for either thread.
- Explicit activation must still retain a thread's layout when it owns contexts.
- Queries for another process must use that process's default, even when the
  calling process changes or clears its own default.

These checks do not validate Wayland keymap construction, scancode conversion,
compositor input events or IME window notifications. They test publication,
layout naming, context creation and thread/process isolation. The
program refuses to run on native Windows because
the private call numbering is specific to this Wine tree.

Build from the Wine source root with MinGW:

```sh
keyboard_build=$(mktemp -d /tmp/wine-keyboard-default.XXXXXX)
x86_64-w64-mingw32-gcc -O2 -g -Wall -Wextra -Werror -Iinclude -x c \
    dlls/winewayland.drv/native-tests/keyboard-default-runtime.c.test -x none \
    -o "$keyboard_build/keyboard-default-64.exe" -luser32 -limm32
i686-w64-mingw32-gcc -O2 -g -Wall -Wextra -Werror -Iinclude -x c \
    dlls/winewayland.drv/native-tests/keyboard-default-runtime.c.test -x none \
    -o "$keyboard_build/keyboard-default-32.exe" -luser32 -limm32
```

Set `keyboard_wine` to the matching rebuilt Wine loader. Use a fresh disposable
prefix with an English US locale. Unset both display variables so a real driver
does not overwrite the synthetic publications. The test does not create windows.

```sh
keyboard_wine=/absolute/path/to/matching/wine
keyboard_prefix=$(mktemp -d /tmp/wine-keyboard-prefix.XXXXXX)
env -u DISPLAY -u WAYLAND_DISPLAY LC_ALL=en_US.UTF-8 \
    WINEPREFIX="$keyboard_prefix" WINEDEBUG=-all \
    "$keyboard_wine" "$keyboard_build/keyboard-default-64.exe"
env -u DISPLAY -u WAYLAND_DISPLAY LC_ALL=en_US.UTF-8 \
    WINEPREFIX="$keyboard_prefix" WINEDEBUG=-all \
    "$keyboard_wine" "$keyboard_build/keyboard-default-32.exe"
```

For a native 32-bit Wine build, use a separate 32-bit prefix and its matching
loader for the second command. To test new WoW64 with this runtime, create a
fresh prefix and add `WINEARCH=wow64` to the 32-bit command. Exit code 0 means
all checks passed. Exit code 1 means a check failed. Exit code 77 means
Wine-private calls are unavailable or the prefix does not have the required
locale. The child process is part of the
cross-process test and is launched automatically.
