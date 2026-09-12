"""Compile Application's actual TTS input prefix with ESP-IDF's real cJSON.

This is a production-fragment test, NOT an Application/RTOS/audio integration
test. The remainder of the callback is deliberately outside its proof scope.
"""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[2]
APPLICATION = PROJECT / "main/application.cc"


def input_prefix(source):
    anchor = 'if (strcmp(type->valuestring, "tts") == 0) {'
    end = 'if (strcmp(state->valuestring, "start") == 0) {'
    if source.count(anchor) != 1:
        raise AssertionError("TTS dispatch boundary changed; review test scope")
    remainder = source.split(anchor, 1)[1]
    if end not in remainder:
        raise AssertionError("TTS start boundary changed; review test scope")
    return remainder.split(end, 1)[0]


def require_guard(prefix):
    # Fail closed before running a deliberately unguarded pointer dereference
    # on the developer's PC. Runtime cases below test the guarded fragment.
    if "cJSON_IsString(state)" not in prefix or "return;" not in prefix:
        raise AssertionError("production TTS state is not guarded before strcmp")


HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <string>
#include "cJSON.h"
static int rejects = 0;
#define ESP_LOGW(...) (++rejects)
static void CheckPrefix(const cJSON* root, std::string& forwarded) {
/* PRODUCTION_PREFIX */
    forwarded = state->valuestring;
}
static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static void Case(const char* json, const char* expected) {
    cJSON* root = cJSON_Parse(json);
    Require(root != nullptr, "fixture must be valid JSON");
    const int before = rejects;
    std::string forwarded = "NOT_FORWARDED";
    CheckPrefix(root, forwarded);
    cJSON_Delete(root);
    if (expected) {
        Require(forwarded == expected, "valid state changed");
        Require(rejects == before, "valid state rejected");
    } else {
        Require(forwarded == "NOT_FORWARDED", "invalid state forwarded");
        Require(rejects == before + 1, "invalid state not rejected exactly once");
    }
}
int main() {
    Case(R"({"type":"tts"})", nullptr);
    Case(R"({"type":"tts","state":null})", nullptr);
    Case(R"({"type":"tts","state":1})", nullptr);
    Case(R"({"type":"tts","state":true})", nullptr);
    Case(R"({"type":"tts","state":false})", nullptr);
    Case(R"({"type":"tts","state":{}})", nullptr);
    Case(R"({"type":"tts","state":[]})", nullptr);
    Case(R"({"type":"tts","nested":{"state":"start"}})", nullptr);
    Case(R"({"type":"tts","state":"start"})", "start");
    Case(R"({"type":"tts","state":"stop"})", "stop");
    Case(R"({"type":"tts","state":"sentence_start"})", "sentence_start");
    Case(R"({"type":"tts","state":"unknown_future_state"})", "unknown_future_state");
    Case(R"({"type":"tts","state":""})", "");
    Case(R"({"type":"tts","STATE":"start"})", "start");
    Case(R"({"type":"tts","state":null,"state":"start"})", nullptr);
    Case(R"({"type":"tts","state":"start","state":null})", "start");
    // cJSON_Parse cannot create this malformed in-memory string node, but a
    // null payload must still be rejected without claiming heap-fault recovery.
    cJSON root{};
    cJSON state{};
    root.type = cJSON_Object;
    root.child = &state;
    state.type = cJSON_String;
    state.string = const_cast<char*>("state");
    std::string forwarded = "NOT_FORWARDED";
    const int before = rejects;
    CheckPrefix(&root, forwarded);
    Require(forwarded == "NOT_FORWARDED" && rejects == before + 1,
            "null string payload not rejected");
    std::puts("PASS: 17 cases; actual Application prefix + real cJSON; callback effects NOT tested");
}
'''


class TtsStateGuardTest(unittest.TestCase):
    def test_production_guard_precedes_first_state_dereference(self):
        prefix = input_prefix(APPLICATION.read_text(encoding="utf-8"))
        require_guard(prefix)

    def test_missing_guard_is_rejected_before_host_execution(self):
        with self.assertRaisesRegex(AssertionError, "not guarded"):
            require_guard('auto state = cJSON_GetObjectItem(root, "state");')

    def test_actual_prefix_with_real_idf_cjson(self):
        source = APPLICATION.read_text(encoding="utf-8")
        prefix = input_prefix(source)
        require_guard(prefix)
        idf = Path(os.environ.get("IDF_PATH", "C:/esp_alm/v5.4.1/esp-idf"))
        cjson = idf / "components/json/cJSON"
        self.assertTrue((cjson / "cJSON.c").is_file(), "real IDF cJSON is required")
        self.assertTrue((cjson / "cJSON.h").is_file(), "real IDF cJSON header is required")
        cc = shutil.which(os.environ.get("CC", "gcc"))
        cxx = shutil.which(os.environ.get("CXX", "g++"))
        self.assertIsNotNone(cc, "C compiler is required; do not skip this gate")
        self.assertIsNotNone(cxx, "C++ compiler is required; do not skip this gate")
        with tempfile.TemporaryDirectory(prefix="p4_tts_guard_") as temp:
            work = Path(temp)
            obj = work / "cjson.o"
            main = work / "test.cc"
            binary = work / ("test.exe" if os.name == "nt" else "test")
            main.write_text(HARNESS.replace("/* PRODUCTION_PREFIX */", prefix), encoding="utf-8")
            commands = [
                [cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)],
                [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(cjson),
                 str(main), str(obj), "-o", str(binary)],
                [str(binary)],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            print("production_prefix_sha256=" + hashlib.sha256(prefix.encode()).hexdigest())
            print("cjson_sha256=" + hashlib.sha256((cjson / "cJSON.c").read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
