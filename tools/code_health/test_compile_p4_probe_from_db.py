import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import compile_p4_probe_from_db as probe


class ProbePlanTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p4_probe_plan_")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / "build").mkdir()
        (self.root / "main").mkdir()
        self.source = self.root / "main/test.cc"
        self.source.write_text("int probe;\n")
        self.output = self.root / "build/new_probe/test.o"
        command = f'compiler -o original.o -c {self.source}'
        (self.root / "build/compile_commands.json").write_text(json.dumps([
            {"file": str(self.source), "output": "original.o", "command": command}
        ]))

    def run_probe(self, *extra):
        argv = ["probe", "--repo-root", str(self.root), "--template", "main/test.cc",
                "--source", "main/test.cc", "--output", str(self.output), *extra]
        with patch("sys.argv", argv), contextlib.redirect_stdout(io.StringIO()) as log:
            result = probe.main()
        return result, log.getvalue()

    def test_dry_run_creates_no_artifacts_and_never_invokes_compiler(self):
        with patch.object(probe.subprocess, "run") as run:
            result, output = self.run_probe("--dry-run")
        self.assertEqual(result, 0)
        self.assertIn("NON_FLASHABLE", output)
        run.assert_not_called()
        self.assertFalse(self.output.parent.exists())

    def test_existing_object_is_never_overwritten(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b"keep")
        with self.assertRaisesRegex(SystemExit, "already exists"):
            self.run_probe()
        self.assertEqual(self.output.read_bytes(), b"keep")

    def test_existing_response_is_never_overwritten(self):
        self.output.parent.mkdir()
        response = self.output.with_suffix(".o.rsp")
        response.write_bytes(b"keep")
        with self.assertRaisesRegex(SystemExit, "already exists"):
            self.run_probe()
        self.assertEqual(response.read_bytes(), b"keep")

    def test_output_outside_build_is_rejected(self):
        self.output = self.root / "main/test.o"
        with self.assertRaisesRegex(SystemExit, "inside build"):
            self.run_probe("--dry-run")


if __name__ == "__main__":
    unittest.main()
