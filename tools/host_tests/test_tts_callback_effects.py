"""Execute the actual TTS branch with queued executor/device stubs and real cJSON.

This tests branch orchestration, not full Application, actual Schedule ordering,
SetDeviceState/audio drain, true wakeup, acoustics, concurrency or fault recovery.
No production source is edited. Mutations run only in temporary test translation units.
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
TEMPLATE = Path(__file__).with_name("tts_callback_effects_test.cc")


def branch(source):
    start = 'if (strcmp(type->valuestring, "tts") == 0) {'
    end = '} else if (strcmp(type->valuestring, "stt") == 0) {'
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError("TTS extraction boundary changed; review proof scope")
    left, right = source.index(start), source.index(end)
    if right <= left:
        raise AssertionError("TTS boundary order changed")
    return source[left:right] + "}"


class TtsCallbackEffectsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="p4_tts_effects_")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.code = branch(APPLICATION.read_text(encoding="utf-8"))
        cls.template = TEMPLATE.read_text(encoding="utf-8")
        cls.cjson = Path(os.environ.get("IDF_PATH", "C:/esp_alm/v5.4.1/esp-idf")) / "components/json/cJSON"
        cls.cc = shutil.which(os.environ.get("CC", "gcc"))
        cls.cxx = shutil.which(os.environ.get("CXX", "g++"))
        if not cls.cc or not cls.cxx or not (cls.cjson / "cJSON.c").is_file():
            raise RuntimeError("Real IDF cJSON and host C/C++ compilers required; no SKIP")
        cls.obj = cls.work / "cjson.o"
        result = subprocess.run([cls.cc, "-std=c99", "-c", str(cls.cjson / "cJSON.c"),
                                 "-o", str(cls.obj)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def execute(self, code, reminder=1, feedback=0):
        source = self.work / "effects.cc"
        executable = self.work / ("effects.exe" if os.name == "nt" else "effects")
        source.write_text(self.template.replace("/* PRODUCTION_TTS_BRANCH */", code), encoding="utf-8")
        compiled = subprocess.run([
            self.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-DCONFIG_BOARD_TYPE_EP_CHAT_P4_ML307=1", f"-DCONFIG_USE_REMINDER_POLL={reminder}",
            f"-DCONFIG_REMINDER_FEEDBACK_SEC={feedback}", "-I", str(self.cjson), str(source),
            str(self.obj), "-o", str(executable)
        ], capture_output=True, text=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        return subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)

    def test_actual_branch_in_three_reminder_configurations(self):
        for reminder, feedback in ((0, 0), (1, 0), (1, 10)):
            with self.subTest(reminder=reminder, feedback=feedback):
                result = self.execute(self.code, reminder, feedback)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print("production_tts_branch_sha256=" + hashlib.sha256(self.code.encode()).hexdigest())
        print("cjson_sha256=" + hashlib.sha256((self.cjson / "cJSON.c").read_bytes()).hexdigest())

    def test_behavior_mutations_are_rejected_at_runtime(self):
        mutations = (
            ('visual_tts_audio_started_.store(false, std::memory_order_release);',
             'visual_tts_audio_started_.store(true, std::memory_order_release);', "start resets"),
            ('xEventGroupSetBits(event_group_, MAIN_EVENT_TTS_UDP_PRIME);',
             'xEventGroupSetBits(event_group_, MAIN_EVENT_TTS_UDP_PRIME); VisualPort(display).NotifyTtsStart();',
             "sentences must not restart"),
            ('if (!proactive && device_state_ == kDeviceStateSpeaking)',
             'if (!proactive)', "late stop"),
            ('audio_service_.ReleasePlaybackPrebuffer();', '(void)0;', "release prebuffer"),
        )
        for old, new, expected_failure in mutations:
            with self.subTest(mutation=expected_failure):
                self.assertEqual(self.code.count(old), 1, "mutation boundary changed")
                result = self.execute(self.code.replace(old, new, 1))
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(expected_failure, result.stderr)

    def test_extraction_refuses_missing_or_duplicate_boundaries(self):
        source = APPLICATION.read_text(encoding="utf-8")
        for invalid in (source.replace('"tts"', '"changed"'), source + source):
            with self.assertRaises(AssertionError):
                branch(invalid)


if __name__ == "__main__":
    unittest.main()
