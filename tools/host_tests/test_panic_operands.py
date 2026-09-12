"""Exercise the actual panic C translation unit; hardware/cache/RTC are stubs.

Uses IDF's real RvExcFrame declaration. This is not a hardware crash test.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
IDF = Path(os.environ.get("IDF_PATH", "C:/esp_alm/v5.4.1/esp-idf"))


class PanicOperandsTest(unittest.TestCase):
    def test_production_capture_and_report(self):
        compiler = shutil.which("gcc") or "C:/Program Files/mingw64/bin/gcc.exe"
        self.assertTrue(Path(compiler).is_file(), "host GCC required")
        self.assertTrue((IDF / "components/riscv/include/riscv/rvruntime-frames.h").is_file())
        with tempfile.TemporaryDirectory(prefix="p4-panic-test-") as folder:
            temp = Path(folder)
            headers = {
                "esp_attr.h": "#define IRAM_ATTR\n#define RTC_NOINIT_ATTR\n",
                "esp_cpu.h": "static inline int esp_cpu_get_core_id(void) { return 0; }\n",
                "esp_private/cache_utils.h": "static inline int spi_flash_cache_enabled(void) { return 1; }\n",
                "soc/soc_caps.h": "#define SOC_CPU_COPROC_NUM 0\n",
                "esp_log.h": "void test_log(const char *, ...);\n#define ESP_LOGI(tag, ...) test_log(__VA_ARGS__)\n#define ESP_LOGW(tag, ...) test_log(__VA_ARGS__)\n#define ESP_LOGE(tag, ...) test_log(__VA_ARGS__)\n",
            }
            for name, content in headers.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "panic_capture.c"
static char output[4096];
void test_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    size_t used = strlen(output);
    vsnprintf(output + used, sizeof(output) - used, fmt, args);
    va_end(args);
}
void __real_panicHandler(void *p) { (void)p; }
void __real_xt_unhandled_exception(void *p) { (void)p; }
void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void) {}
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void) {}
void __real_spi_flash_disable_interrupts_caches_and_other_cpu_no_os(void) {}
void __real_spi_flash_enable_interrupts_caches_no_os(void) {}
int main(void) {
    RvExcFrame f = {0};
    f.mepc = 0x400adc2c; f.mtval = 0x49538a00; f.mcause = 5;
    f.s8 = 0x49538a00; f.s9 = 8; f.t0 = 3; f.t3 = 40; f.t4 = 12;
    f.a0 = 0x49538900; f.a1 = 11; f.a2 = 0x48000000; f.a3 = 13;
    __wrap_xt_unhandled_exception(&f);
    assert(g_rtc_panic_capture.s8 == (uint32_t)f.s8);
    assert(g_rtc_panic_capture.s9 == 8 && g_rtc_panic_capture.t0 == 3);
    assert(g_rtc_panic_capture.t3 == 40 && g_rtc_panic_capture.t4 == 12);
    assert(g_rtc_panic_capture.a0 == (uint32_t)f.a0);
    assert(g_rtc_panic_capture.a1 == 11 && g_rtc_panic_capture.a2 == (uint32_t)f.a2);
    assert(g_rtc_panic_capture.a3 == 13);
    f.mhartid = 1; f.s8 = 99;
    __wrap_panicHandler(&f);
    assert(g_rtc_panic_capture.s8 == 0x49538a00);
    assert(g_rtc_panic_capture.core2 == 1);
    PanicCaptureReport();
    assert(strstr(output, "PR1 49538a00,8,3,28,c,49538900,b,48000000,d"));
    assert(strstr(output, "DUAL_CORE_TRAP") && !g_rtc_panic_capture.magic);
    output[0] = 0;
    __wrap_panicHandler(NULL);
    assert(!g_rtc_panic_capture.s8 && !g_rtc_panic_capture.s9);
    assert(!g_rtc_panic_capture.a0 && !g_rtc_panic_capture.a1);
    assert(!g_rtc_panic_capture.a2 && !g_rtc_panic_capture.a3);
    assert(!g_rtc_panic_capture.t0 && !g_rtc_panic_capture.t3 && !g_rtc_panic_capture.t4);
    PanicCaptureReport();
    output[0] = 0;
    g_rtc_panic_capture.magic = PANIC_CAPTURE_MAGIC_LEGACY;
    g_rtc_panic_capture.mepc = 0x400adc2c;
    g_rtc_panic_capture.s8 = 0xdeadbeef; // not valid in the old layout
    PanicCaptureReport();
    assert(strstr(output, "mepc=0x400adc2c"));
    assert(!strstr(output, "PR1"));
    output[0] = 0;
    PanicCaptureReport();
    assert(strstr(output, "none armed=1"));
    puts("PASS: capture, first-frame preservation, null, legacy, report-clear");
    return 0;
}
'''
            (temp / "test.c").write_text(harness, encoding="utf-8")
            exe = temp / "test.exe"
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(temp),
                       "-I", str(ROOT / "main/debug"), "-I", str(IDF / "components/riscv/include"),
                       str(temp / "test.c"), "-o", str(exe)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
