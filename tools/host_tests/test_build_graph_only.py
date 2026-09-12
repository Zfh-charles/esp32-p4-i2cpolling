"""Exercise the real Windows wrapper and Ninja using isolated tiny graphs."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


WRAPPER = Path(__file__).resolve().parents[1] / "build-idf.ps1"


class BuildGraphOnlyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p4_graph_gate_")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "main").mkdir()
        (self.root / "build").mkdir()
        for name in ("sdkconfig", "dependencies.lock", "CMakeLists.txt", "main/CMakeLists.txt"):
            (self.root / name).write_text("unchanged\n", encoding="utf-8")
        self.shell = shutil.which("pwsh") or shutil.which("powershell")
        self.assertIsNotNone(self.shell, "Windows PowerShell required; this gate is not optional")

    def graph(self, action="pass"):
        script = self.root / "prepare.py"
        script.write_text(
            "from pathlib import Path\n"
            "import sys\n"
            "root = Path(__file__).parent\n"
            + ("sys.exit(7)\n" if action == "fail" else "")
            + ("(root / 'sdkconfig').write_text('drift')\n" if action == "drift" else "")
            + "(root / 'build/build.ninja').touch()\n",
            encoding="utf-8",
        )
        # The default target would run another command; graph-only MUST NOT.
        app = self.root / "app.py"
        app.write_text(
            "from pathlib import Path\n"
            "(Path(__file__).parent / 'unexpected_application_build').touch()\n",
            encoding="utf-8",
        )
        ninja = self.root / "build/build.ninja"
        python = sys.executable.replace("$", "$$")
        ninja.write_text(
            "rule prepare\n"
            f'  command = "{python}" "{script}"\n'
            "  generator = 1\n"
            "  restat = 1\n"
            "rule app\n"
            f'  command = "{python}" "{app}"\n'
            "build build.ninja: prepare trigger\n"
            "build forbidden_app: app\n"
            "default forbidden_app\n",
            encoding="utf-8",
        )
        trigger = self.root / "build/trigger"
        trigger.touch()
        now = time.time()
        os.utime(ninja, (now - 20, now - 20))
        os.utime(trigger, (now - 10, now - 10))

    def run_wrapper(self, *extra):
        result = subprocess.run(
            [self.shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(WRAPPER),
             "-ProjectRoot", str(self.root), "-PrepareGraphOnly", *extra],
            capture_output=True, text=True, timeout=30,
        )
        self.assertFalse((self.root / "unexpected_application_build").exists(),
                         result.stdout + result.stderr)
        return result

    def test_prepares_real_ninja_graph_without_application_build(self):
        self.graph()
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BUILD_GRAPH_PREPARED_ONLY", result.stdout)

    def test_drift_is_failure_and_is_not_silently_reverted(self):
        self.graph("drift")
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Build graph input drift", result.stdout + result.stderr)
        self.assertEqual((self.root / "sdkconfig").read_text(), "drift")

    def test_graph_command_failure_does_not_request_application(self):
        self.graph("fail")
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("BUILD_GRAPH_PREPARED_ONLY", result.stdout)

    def test_ambiguous_flags_rejected(self):
        for flag in ("-DryRun", "-Reconfigure"):
            with self.subTest(flag=flag):
                result = self.run_wrapper(flag)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("cannot be combined", result.stdout + result.stderr)

    def test_probe_arguments_cannot_fall_through_to_application_build(self):
        for flags, expected in (
            (("-ProbeSource", "main/test.cc"), "must be supplied together"),
            (("-ProbeOutput", "build/probe/test.o"), "must be supplied together"),
            (("-ProbeSource", "main/test.cc", "-ProbeOutput", "build/probe/test.o"),
             "cannot be combined"),
        ):
            with self.subTest(flags=flags):
                result = self.run_wrapper(*flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
