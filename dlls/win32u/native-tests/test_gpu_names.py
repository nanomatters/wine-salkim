"""Compile production GPU naming and revision discovery with mocked device data."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
TYPES = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t UINT16;
typedef unsigned int UINT;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define MAKELONG(low, high) ((uint32_t)(low) | ((uint32_t)(high) << 16))
"""
PCI_HEADER = (ROOT / "include/wine/gdi_driver.h").read_text()
PCI_ID = re.search(r"^#define PCI_REVISION_UNKNOWN .*", PCI_HEADER, re.M).group() + "\n"
PCI_ID += re.search(r"struct pci_id\n\{.*?^\};", PCI_HEADER, re.M | re.S).group()

def function(path, name):
    source = (ROOT / path).read_text()
    return re.search(r"^(?:const char \*|void |BOOL )" + name + r"\(.*?^\}", source, re.M | re.S).group()


class GpuNameTests(unittest.TestCase):
    def compile_and_run(self, harness):
        with tempfile.TemporaryDirectory(prefix="gpu-names-") as temporary:
            source, binary = Path(temporary) / "test.c", Path(temporary) / "test"
            source.write_text(TYPES + PCI_ID + "\n" + harness)
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("GPU_NAME_TEST_SANITIZERS"):
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command + [str(source), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_wayland_revision_and_old_libdrm(self):
        harness = r"""
#define HAVE_XF86DRM_H 1
#define DRM_DEVICE_GET_PCI_REVISION 1
#define DRM_BUS_PCI 0
struct wayland_dmabuf_feedback { BOOL has_main_device; uint64_t main_device; };
static struct { struct wayland_dmabuf_feedback dmabuf_default_feedback; } process_wayland;
struct drm_pci_device_info { UINT16 vendor_id, device_id; uint8_t revision_id; };
struct drm_device { int bustype; struct { struct drm_pci_device_info *pci; } deviceinfo; };
typedef struct drm_device *drmDevicePtr;
static struct drm_pci_device_info pci = {0x1002, 0x73df, 0xc0};
static struct drm_device device = {DRM_BUS_PCI, {&pci}};
static int calls, frees, errors[2];
static uint32_t call_flags[2];
static int drmGetDeviceFromDevId(uint64_t dev, uint32_t flags, drmDevicePtr *out)
{
    assert(dev == 42 && calls < 2 && flags <= 1);
    call_flags[calls++] = flags;
    if (errors[flags]) return -1;
    *out = &device;
    return 0;
}
static void drmFreeDevice(drmDevicePtr *out)
{
    assert(*out == &device);
    *out = NULL;
    ++frees;
}
ACTUAL
int main(void)
{
    struct pci_id id = {1, 2, 3, 4};
    assert(!wayland_dmabuf_get_main_device_pci_id(&id));
    assert(!id.vendor && !id.device && !id.subsystem && id.revision == PCI_REVISION_UNKNOWN && !calls && !frees);
    process_wayland.dmabuf_default_feedback = (struct wayland_dmabuf_feedback){TRUE, 42};
    assert(wayland_dmabuf_get_main_device_pci_id(&id));
    assert(id.vendor == 0x1002 && id.device == 0x73df && id.revision == 0xc0 && !id.subsystem);
    assert(calls == 1 && call_flags[0] == 1 && frees == 1);
    calls = frees = 0;
    pci.device_id = 0x7480;
    pci.revision_id = 0;
    assert(wayland_dmabuf_get_main_device_pci_id(&id));
    assert(id.device == 0x7480 && !id.revision && calls == 1 && frees == 1);
    calls = frees = 0;
    pci.device_id = 0x73df;
    pci.revision_id = 0xff;
    assert(wayland_dmabuf_get_main_device_pci_id(&id));
    assert(id.revision == 0xff && calls == 1 && frees == 1);
    calls = frees = 0;
    errors[1] = -1;
    assert(wayland_dmabuf_get_main_device_pci_id(&id));
    assert(id.vendor == 0x1002 && id.device == 0x73df && id.revision == PCI_REVISION_UNKNOWN);
    assert(calls == 2 && call_flags[0] == 1 && !call_flags[1] && frees == 1);
    calls = frees = 0;
    errors[0] = -1;
    assert(!wayland_dmabuf_get_main_device_pci_id(&id));
    assert(!id.vendor && !id.device && id.revision == PCI_REVISION_UNKNOWN && calls == 2 && !frees);
    calls = frees = errors[0] = errors[1] = 0;
    device.bustype = 1;
    assert(!wayland_dmabuf_get_main_device_pci_id(&id));
    assert(!id.vendor && !id.device && id.revision == PCI_REVISION_UNKNOWN && calls == 1 && frees == 1);
    calls = frees = 0;
    device.bustype = DRM_BUS_PCI;
    device.deviceinfo.pci = NULL;
    assert(!wayland_dmabuf_get_main_device_pci_id(&id));
    assert(!id.vendor && !id.device && id.revision == PCI_REVISION_UNKNOWN && calls == 1 && frees == 1);
}
""".replace("ACTUAL", function("dlls/winewayland.drv/wayland.c", "wayland_dmabuf_get_main_device_pci_id"))
        self.compile_and_run(harness)
        self.compile_and_run(function("dlls/winewayland.drv/wayland.c", "wayland_dmabuf_get_main_device_pci_id") + r"""
int main(void)
{
    struct pci_id id = {1, 2, 3, 4};
    assert(!wayland_dmabuf_get_main_device_pci_id(&id));
    assert(!id.vendor && !id.device && !id.subsystem && id.revision == PCI_REVISION_UNKNOWN);
}
""")

    def test_egl_revision(self):
        source = (ROOT / "dlls/win32u/opengl.c").read_text()
        block = source[source.index("    egl->revision_id = PCI_REVISION_UNKNOWN;"):source.index('    TRACE( "  - device_id:')]
        self.compile_and_run(r"""
#define EGL_DRM_DEVICE_FILE_EXT 1
struct egl_platform { UINT vendor_id, device_id, revision_id; void *device; };
static UINT vendor = 0x1002, device_id = 0x73df, revision = 0xc0;
static int reads;
static const char *device_path = "/dev/dri/renderD128";
static BOOL has_extension(const char *extensions, const char *name)
{
    assert(!strcmp(name, "EGL_EXT_device_drm"));
    return !!*extensions;
}
static const char *query_device(void *device, int prop)
{
    assert(!device && prop == EGL_DRM_DEVICE_FILE_EXT);
    return device_path;
}
static const struct { const char *(*p_eglQueryDeviceStringEXT)(void *, int); } functions = {query_device}, *funcs = &functions;
static UINT read_drm_device_prop(const char *path, const char *prop)
{
    assert(!strcmp(path, "/renderD128"));
    ++reads;
    if (!strcmp(prop, "vendor")) return vendor;
    if (!strcmp(prop, "device")) return device_id;
    assert(!strcmp(prop, "revision"));
    return revision;
}
static void read_identity(struct egl_platform *egl, const char *extensions)
{
    const char *str;
ACTUAL
}
int main(void)
{
    struct egl_platform egl = {0};
    read_identity(&egl, "yes");
    assert(egl.vendor_id == vendor && egl.device_id == device_id && egl.revision_id == 0xc0 && reads == 3);
    revision = 0xff;
    read_identity(&egl, "yes");
    assert(egl.revision_id == 0xff);
    revision = 0;
    read_identity(&egl, "yes");
    assert(!egl.revision_id);
    revision = (UINT)-1;
    read_identity(&egl, "yes");
    assert(egl.revision_id == PCI_REVISION_UNKNOWN);
    revision = 0x100;
    read_identity(&egl, "yes");
    assert(egl.revision_id == PCI_REVISION_UNKNOWN);
    revision = 0xc0;
    device_id = 0x73bf;
    reads = 0;
    read_identity(&egl, "yes");
    assert(egl.revision_id == 0xc0 && reads == 3);
    vendor = 0x10de;
    device_id = 0x73df;
    reads = 0;
    read_identity(&egl, "yes");
    assert(egl.revision_id == 0xc0 && reads == 3);
    vendor = 0x8086;
    device_id = 0x9a49;
    revision = 1;
    reads = 0;
    read_identity(&egl, "yes");
    assert(egl.revision_id == 1 && reads == 3);
    read_identity(&egl, "");
    assert(egl.vendor_id == (UINT)-1 && egl.device_id == (UINT)-1 && egl.revision_id == PCI_REVISION_UNKNOWN);
    device_path = NULL;
    egl.revision_id = 0xc0;
    read_identity(&egl, "yes");
    assert(egl.revision_id == PCI_REVISION_UNKNOWN);
}
""".replace("ACTUAL", block))

    def test_vulkan_revision_and_identity_match(self):
        source = (ROOT / "dlls/win32u/d3dkmt.c").read_text()
        start = source.index("        devinfo[i].revision = PCI_REVISION_UNKNOWN;")
        discovery = source[start:source.index("\n    }\n    qsort", start)]
        source = (ROOT / "dlls/win32u/sysparams.c").read_text()
        start = source.index("    if (pci_id->revision == PCI_REVISION_UNKNOWN && uuid")
        enrichment = source[start:source.index("    fixup_device_id", start)]
        start = source.index("    /* Keep the unknown revision marker internal;")
        normalization = source[start:source.index("    snprintf( gpu->path", start)]
        self.compile_and_run(r"""
#include <errno.h>
static const char *sysfs_value;
static int reads;
static BOOL read_sysfs_line(const char *path, char *buffer, size_t size)
{
    assert(!strcmp(path, "/sys/bus/pci/devices/0001:02:03.1/revision"));
    ++reads;
    if (!sysfs_value) return FALSE;
    snprintf(buffer, size, "%s", sysfs_value);
    return TRUE;
}
static UINT16 discover(BOOL extension, UINT vendor, UINT device)
{
    struct { struct { BOOL has_VK_EXT_pci_bus_info; } extensions; } physical = {{extension}}, *physical_device = &physical;
    struct { struct { struct { UINT vendorID, deviceID; } properties; } properties2; UINT16 revision; }
        devinfo[1] = {{{{vendor, device}}, 0}};
    const struct { UINT pciDomain, pciBus, pciDevice, pciFunction; } pci = {1, 2, 3, 1};
    const unsigned int i = 0;
DISCOVERY
    return devinfo[0].revision;
}
typedef uint32_t GUID;
#define IsEqualGUID(a, b) (*(a) == *(b))
struct gpu_info { GUID uuid; struct pci_id pci_id; };
static UINT16 enrich(struct pci_id original, const GUID *uuid, struct gpu_info *vulkan_gpu)
{
    const struct pci_id *pci_id = &original;
    struct pci_id id;
    static const GUID empty_uuid;
ENRICHMENT
    return pci_id->revision;
}
static UINT16 registry_revision(struct pci_id original)
{
    const struct pci_id *pci_id = &original;
    struct pci_id id;
NORMALIZATION
    return pci_id->revision;
}
int main(void)
{
    static const char *invalid[] = {NULL, "", "garbage", "0xc0junk", "0x100", "-1", "999999999999999999999999999999999"};
    struct pci_id id = {0x1002, 0x73df, 0, PCI_REVISION_UNKNOWN};
    struct gpu_info gpu = {1, {0x1002, 0x73df, 0, 0xc0}};
    const GUID uuid = 1, different = 2, empty = 0;
    size_t i;
    sysfs_value = "0xc0";
    assert(discover(TRUE, 0x1002, 0x73df) == 0xc0);
    sysfs_value = "0xff";
    assert(discover(TRUE, 0x1002, 0x73df) == 0xff);
    sysfs_value = "0x00";
    assert(discover(TRUE, 0x1002, 0x7480) == 0);
    for (i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
    {
        sysfs_value = invalid[i];
        assert(discover(TRUE, 0x1002, 0x73df) == PCI_REVISION_UNKNOWN);
    }
    reads = 0;
    assert(discover(FALSE, 0x1002, 0x73df) == PCI_REVISION_UNKNOWN);
    assert(!reads);
    sysfs_value = "0xc0";
    assert(discover(TRUE, 0x10de, 0x73df) == 0xc0);
    assert(discover(TRUE, 0x1002, 0x73bf) == 0xc0);
    sysfs_value = "0x01";
    assert(discover(TRUE, 0x8086, 0x9a49) == 1);
    assert(reads == 3);
    assert(enrich(id, &uuid, &gpu) == 0xc0);
    assert(enrich(id, NULL, &gpu) == PCI_REVISION_UNKNOWN);
    assert(enrich(id, &empty, &gpu) == PCI_REVISION_UNKNOWN);
    assert(enrich(id, &different, &gpu) == PCI_REVISION_UNKNOWN);
    assert(enrich(id, &uuid, NULL) == PCI_REVISION_UNKNOWN);
    assert(registry_revision(id) == 0);
    id.revision = 0xc1;
    assert(enrich(id, &uuid, &gpu) == 0xc1);
    assert(registry_revision(id) == 0xc1);
    id.revision = 0xff;
    assert(registry_revision(id) == 0xff);
    id.revision = 0;
    assert(enrich(id, &uuid, &gpu) == 0);
    assert(registry_revision(id) == 0);
    id.revision = PCI_REVISION_UNKNOWN;
    gpu.pci_id.revision = 0;
    assert(enrich(id, &uuid, &gpu) == 0);
    gpu.pci_id.revision = PCI_REVISION_UNKNOWN;
    assert(enrich(id, &uuid, &gpu) == PCI_REVISION_UNKNOWN);
    gpu.pci_id.revision = 0xc0;
    id.vendor = 0x10de;
    assert(enrich(id, &uuid, &gpu) == PCI_REVISION_UNKNOWN);
    id.vendor = 0x1002;
    id.device = 0x73bf;
    assert(enrich(id, &uuid, &gpu) == PCI_REVISION_UNKNOWN);
}
""".replace("DISCOVERY", discovery).replace("ENRICHMENT", enrichment).replace("NORMALIZATION", normalization))


if __name__ == "__main__":
    unittest.main()
