"""Compile the production VR extension parsers with mocked extension tables."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "vulkan.c"


class VRExtensionTests(unittest.TestCase):
    def check_parser(self, kind):
        source = SOURCE.read_text()
        match = re.search(r"static void parse_" + kind + r"_extensions\(.*?^\}", source, re.M | re.S)
        self.assertIsNotNone(match)
        harness = r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
struct vulkan_KIND_extensions { unsigned int seen; };
static void add_KIND_extension(const char *name, size_t length, struct vulkan_KIND_extensions *extensions)
{
    const char *expected[] = {"VK_KHR_surface", "VK_EXT_debug_utils"};
    if (!length) return;
    assert(extensions->seen < 2);
    assert(strlen(expected[extensions->seen]) == length);
    assert(!memcmp(name, expected[extensions->seen], length));
    ++extensions->seen;
}
ACTUAL_PARSER
int main(void)
{
    struct vulkan_KIND_extensions extensions = {0};
    parse_KIND_extensions(&extensions, NULL);
    parse_KIND_extensions(&extensions, "");
    parse_KIND_extensions(&extensions, "   ");
    assert(!extensions.seen);
    parse_KIND_extensions(&extensions, "VK_KHR_surface VK_EXT_debug_utils");
    assert(extensions.seen == 2);
    extensions.seen = 0;
    parse_KIND_extensions(&extensions, " VK_KHR_surface  VK_EXT_debug_utils ");
    assert(extensions.seen == 2);
}
""".replace("KIND", kind).replace("ACTUAL_PARSER", match.group())
        with tempfile.TemporaryDirectory(prefix="vr-extensions-") as temporary:
            directory = Path(temporary)
            generated, binary = directory / "test.c", directory / "test"
            generated.write_text(harness)
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("VR_EXTENSION_TEST_SANITIZERS"):
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command + [str(generated), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_instance_extensions(self):
        self.check_parser("instance")

    def test_device_extensions(self):
        self.check_parser("device")


if __name__ == "__main__":
    unittest.main()
