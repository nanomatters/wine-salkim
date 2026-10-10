"""Exercise winmm joystick slot helpers without a Wine or HID installation."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "dlls/winmm/joystick.c"
STUBS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint32_t DWORD, ULONG, UINT;
typedef int32_t HRESULT;
typedef void *HWND;
typedef struct event { int id; } *HANDLE;
typedef struct device
{
    int id;
    HRESULT state_result, acquire_result;
} IDirectInputDevice8W;
typedef struct instance
{
    DWORD guidInstance, dwDevType;
    char name[8];
} DIDEVICEINSTANCEW;
struct joystick_state { int value; };
struct joystick
{
    DIDEVICEINSTANCEW instance;
    IDirectInputDevice8W *device;
    struct joystick_state state;
    HANDLE event;
    HWND capture;
    UINT timer, threshold;
    BOOL changed, disconnected;
};
static struct joystick joysticks[16];
static DIDEVICEINSTANCEW instances[16];
static int dinput;
static unsigned int failure_stage, created, released, events, closed, killed, acquires;
static HWND last_killed_window;
static UINT last_killed_timer;

#define TRUE 1
#define FALSE 0
#define CALLBACK
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define DIERR_INPUTLOST (-2)
#define DIERR_NOTACQUIRED (-3)
#define DIENUM_STOP 0
#define DIENUM_CONTINUE 1
#define DI8DEVTYPE_MOUSE 1
#define DI8DEVTYPE_KEYBOARD 2
#define DISCL_NONEXCLUSIVE 1
#define DISCL_BACKGROUND 2
#define WARN(...) ((void)0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)

static HANDLE CreateEventW(void *security, BOOL manual, BOOL initial, const void *name)
{
    (void)security; (void)manual; (void)initial; (void)name;
    if (failure_stage == 1) return NULL;
    HANDLE result = malloc(sizeof(*result));
    assert(result);
    result->id = ++events;
    return result;
}

static BOOL CloseHandle(HANDLE event)
{
    if (event) { ++closed; free(event); }
    return !!event;
}

static HRESULT IDirectInput8_CreateDevice(int input, const DWORD *id,
                                        IDirectInputDevice8W **output, void *outer)
{
    (void)input; (void)outer;
    if (failure_stage == 2) return -1;
    *output = calloc(1, sizeof(**output));
    assert(*output);
    (*output)->id = *id;
    ++created;
    return 0;
}

static void IDirectInputDevice8_Release(IDirectInputDevice8W *device)
{
    assert(device);
    ++released;
    free(device);
}

static HRESULT IDirectInputDevice8_SetEventNotification(IDirectInputDevice8W *device, HANDLE event)
{
    assert(device && event);
    return failure_stage == 3 ? -1 : 0;
}

static HRESULT IDirectInputDevice8_SetCooperativeLevel(IDirectInputDevice8W *device, HWND hwnd, DWORD flags)
{
    assert(device); (void)hwnd; (void)flags;
    return failure_stage == 4 ? -1 : 0;
}

static HRESULT set_data_format(IDirectInputDevice8W *device)
{
    assert(device);
    return failure_stage == 5 ? -1 : 0;
}

static HRESULT IDirectInputDevice8_Acquire(IDirectInputDevice8W *device)
{
    assert(device);
    ++acquires;
    if (!failure_stage && SUCCEEDED(device->acquire_result)) device->state_result = 0;
    return failure_stage == 6 ? -1 : device->acquire_result;
}

static HRESULT IDirectInputDevice8_GetDeviceState(IDirectInputDevice8W *device,
                                               DWORD size, struct joystick_state *state)
{
    assert(device && size == sizeof(*state));
    state->value = device->id;
    return device->state_result;
}

static BOOL KillTimer(HWND hwnd, UINT timer)
{
    ++killed;
    last_killed_window = hwnd;
    last_killed_timer = timer;
    return TRUE;
}

static DIDEVICEINSTANCEW instance(unsigned int id)
{
    DIDEVICEINSTANCEW result = {id, 0, "pad"};
    return result;
}

static void cleanup(void)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(joysticks); ++i)
    {
        if (joysticks[i].device) IDirectInputDevice8_Release(joysticks[i].device);
        if (joysticks[i].event) CloseHandle(joysticks[i].event);
    }
    memset(joysticks, 0, sizeof(joysticks));
    assert(created == released);
    assert(events == closed);
}
"""


class JoystickSlotTests(unittest.TestCase):
    def run_native(self, helper, body):
        with tempfile.TemporaryDirectory(prefix="winmm-joysticks-") as temporary:
            directory = Path(temporary)
            source, binary = directory / "test.c", directory / "test"
            source.write_text(STUBS + helper + body)
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", "-g"]
            if os.environ.get("WINMM_JOYSTICK_SANITIZERS"):
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command + [str(source), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_duplicate_and_failure_cleanup(self):
        source = SOURCE.read_text()
        helper = source[source.index("static void add_joystick("):
                        source.index("static void find_joysticks(")]
        self.run_native(helper, r"""
int main(void)
{
    unsigned int i, stage;
    DIDEVICEINSTANCEW first = instance(1), second = instance(2);
    add_joystick(&first);
    add_joystick(&second);
    add_joystick(&second);
    add_joystick(&first);
    assert(created == 2 && events == 2);
    assert(joysticks[0].device->id == 1 && joysticks[1].device->id == 2);
    cleanup();

    for (stage = 1; stage <= 6; ++stage)
    {
        failure_stage = stage;
        add_joystick(&first);
        assert(!joysticks[0].device && !joysticks[0].event);
        cleanup();
    }
    failure_stage = 0;
    for (i = 0; i < ARRAY_SIZE(joysticks); ++i)
    {
        DIDEVICEINSTANCEW value = instance(i + 1);
        add_joystick(&value);
        assert(joysticks[i].device && joysticks[i].device->id == (int)i + 1);
    }
    first = instance(17);
    i = created;
    add_joystick(&first);
    assert(created == i);
    cleanup();
    return 0;
}
""")

    def test_reconnect_and_capture_cleanup(self):
        source = SOURCE.read_text()
        helper = source[source.index("static void update_connected_state("):
                        source.index("static void find_joysticks(")]
        self.run_native(helper, r"""
int main(void)
{
    DIDEVICEINSTANCEW value = instance(1), replacement = instance(99);
    unsigned int i, stage, before, acquire_count;
    IDirectInputDevice8W *old_device;
    HANDLE old_event;

    add_joystick(&value);
    old_device = joysticks[0].device;
    old_event = joysticks[0].event;
    joysticks[0].capture = (HWND)(uintptr_t)123;
    joysticks[0].timer = 456;
    joysticks[0].device->state_result = DIERR_INPUTLOST;
    joysticks[0].device->acquire_result = -1;
    acquire_count = acquires;
    update_connected_state();
    assert(acquires == acquire_count + 1);
    assert(joysticks[0].disconnected && !killed);
    add_joystick(&value);
    assert(joysticks[0].device == old_device && joysticks[0].event == old_event);
    joysticks[0].device->acquire_result = 0;
    update_connected_state();
    assert(!joysticks[0].disconnected && !killed);
    assert(joysticks[0].capture == (HWND)(uintptr_t)123 && joysticks[0].timer == 456);

    joysticks[0].device->state_result = DIERR_NOTACQUIRED;
    update_connected_state();
    assert(!joysticks[0].disconnected);
    assert(joysticks[0].device == old_device && joysticks[0].event == old_event);

    joysticks[0].device->state_result = -100;
    update_connected_state();
    assert(killed == 1 && last_killed_window == (HWND)(uintptr_t)123 && last_killed_timer == 456);
    assert(!joysticks[0].device && !joysticks[0].event && !joysticks[0].capture && !joysticks[0].timer);
    cleanup();

    for (i = 0; i < ARRAY_SIZE(joysticks); ++i)
    {
        value = instance(i + 1);
        add_joystick(&value);
    }
    old_device = joysticks[0].device;
    old_event = joysticks[0].event;
    joysticks[0].disconnected = TRUE;
    joysticks[0].capture = (HWND)(uintptr_t)234;
    joysticks[0].timer = 567;
    joysticks[0].threshold = 42;
    for (stage = 1; stage <= 6; ++stage)
    {
        failure_stage = stage;
        before = killed;
        add_joystick(&replacement);
        assert(joysticks[0].device == old_device && joysticks[0].event == old_event);
        assert(joysticks[0].instance.guidInstance == 1 && joysticks[0].disconnected);
        assert(joysticks[0].capture == (HWND)(uintptr_t)234 && joysticks[0].timer == 567);
        assert(joysticks[0].threshold == 42 && killed == before);
        assert(created - released == ARRAY_SIZE(joysticks));
        assert(events - closed == ARRAY_SIZE(joysticks));
    }
    failure_stage = 0;
    add_joystick(&replacement);
    assert(joysticks[0].device->id == 99 && !joysticks[0].disconnected);
    assert(!joysticks[0].capture && !joysticks[0].timer);
    assert(killed == 2 && last_killed_window == (HWND)(uintptr_t)234 && last_killed_timer == 567);
    cleanup();
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main()
