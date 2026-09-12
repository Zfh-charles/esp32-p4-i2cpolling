"""Exercise actual idle diagnostic calls/formats with ROM output disabled.

This is a logging-fragment test, not the animation control flow or ESP log/USB
implementation. A single-run source comparison separately checks behavior
preservation. Never treat a logged fence timeout as completed rendering.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "main/boards/ep-chat-p4-ml307/eezui_display_adapter.cc"
NAMES = ("Arm", "Fallback", "Drop", "Frame")


def fragments(source):
    declarations, calls = [], []
    for name in NAMES:
        symbol = "kIdleFlash" + name + "Log"
        declaration = re.findall(r'DRAM_ATTR const char ' + symbol + r'\[\] = "[^\n]+";', source)
        call = re.findall(r'(?:esp_log_write|esp_rom_printf)\([^;]*?\b' + symbol + r'\b[^;]*?\);', source)
        if len(declaration) != 1 or len(call) != 1:
            raise AssertionError("Idle diagnostic extraction changed: " + symbol)
        declarations.extend(declaration)
        calls.extend(call)
    return "\n".join(declarations), "\n".join(calls)


HARNESS = r'''
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#define DRAM_ATTR
#define TAG "EezuiDisplayAdapter"
enum { ESP_LOG_INFO = 3 };
static std::vector<std::string> output;
static unsigned esp_log_timestamp() { return 12345; }
static void esp_rom_printf(const char*, ...) {} // Actual console-none failure model.
static void esp_log_write(int level, const char* tag, const char* format, ...) {
    if (level != ESP_LOG_INFO || std::string(tag) != TAG) std::exit(2);
    char buffer[256];
    va_list args; va_start(args, format);
    const int count = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (count < 0 || count >= (int)sizeof(buffer)) std::exit(2);
    output.emplace_back(buffer);
}
/* DECLARATIONS */
int main() {
    const bool visual_ok = true, abort_to_base = true;
    const unsigned target = 3, delay = 5300;
    const int perr = 263; // Test preserves error value; does not certify a frame.
    /* CALLS */
    const std::vector<std::string> expected = {
        "I (12345) FaceIdle: arm rows=48 flash=1\n",
        "I (12345) FaceIdle: asset=0 fallback=1\n",
        "I (12345) FaceIdle: drop=1\n",
        "I (12345) FaceIdle: f=3 abort=1 err=263 next=5300\n"
    };
    if (output != expected) { std::fputs("FAIL: idle log route or values\n", stderr); return 1; }
    std::puts("PASS: four production log calls/formats with ROM disabled; hardware not exercised");
}
'''


class IdleLogRouteTest(unittest.TestCase):
    def test_actual_calls_and_rom_route_mutant(self):
        compiler = shutil.which(os.environ.get("CXX", "g++"))
        self.assertIsNotNone(compiler, "Host C++ compiler required; no SKIP")
        declarations, calls = fragments(SOURCE.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="p4_idle_log_") as temp:
            source = Path(temp) / "test.cc"
            exe = Path(temp) / ("test.exe" if os.name == "nt" else "test")
            for mutate in (False, True):
                selected = calls
                if mutate:
                    selected = selected.replace("esp_log_write(ESP_LOG_INFO, TAG,", "esp_rom_printf(")
                source.write_text(HARNESS.replace("/* DECLARATIONS */", declarations)
                                  .replace("/* CALLS */", selected), encoding="utf-8")
                compiled = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                           "-Wno-unused-function", str(source), "-o", str(exe)],
                                          capture_output=True, text=True, timeout=30)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1 if mutate else 0, result.stdout + result.stderr)
                if mutate:
                    self.assertIn("idle log route", result.stderr)

    def test_missing_or_duplicate_probe_is_not_silently_accepted(self):
        text = SOURCE.read_text(encoding="utf-8")
        for invalid in (text.replace("kIdleFlashArmLog", "removed"), text + text):
            with self.assertRaises(AssertionError):
                fragments(invalid)


if __name__ == "__main__":
    unittest.main()
