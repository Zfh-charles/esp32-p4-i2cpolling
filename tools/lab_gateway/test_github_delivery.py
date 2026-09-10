import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from common import BOARD, digest, encoded
from github_stage import admit_archive, verify_run
from prepare_inputs import prepare
from test_gateway import image


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.payload = image()
        self.job = {"version": 1, "id": "run-12-1", "board": BOARD, "action": "stage",
                    "marker": "boot_trace_v10_test_lab", "commit": "a" * 40,
                    "firmware": {"sha256": digest(self.payload), "size": len(self.payload)},
                    "provenance": {"release": "same_graph"}}

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, extra=None):
        path = self.root / "bundle.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("job.json", encoded(self.job))
            archive.writestr("firmware.bin", self.payload)
            if extra:
                archive.writestr(extra, b"unexpected")
        return path

    def test_stage_and_repeat(self):
        path = self.archive()
        for _ in range(2):
            result = admit_archive(path, self.root / "stage", "a" * 40, "run-12-1")
            self.assertEqual(result, self.job)

    def test_traversal_and_extra_files_rejected(self):
        for extra in ("../escape", "firmware.elf"):
            with self.assertRaises(ValueError):
                admit_archive(self.archive(extra), self.root / "stage", "a" * 40, "run-12-1")
        self.assertFalse((self.root / "stage").exists())

    def test_wrong_commit_rejected(self):
        with self.assertRaises(ValueError):
            admit_archive(self.archive(), self.root / "stage", "b" * 40, "run-12-1")

    def test_wrong_attempt_rejected(self):
        with self.assertRaises(ValueError):
            admit_archive(self.archive(), self.root / "stage", "a" * 40, "run-12-2")

    def test_corrupt_firmware_rejected(self):
        self.payload += b"changed"
        with self.assertRaises(ValueError):
            admit_archive(self.archive(), self.root / "stage", "a" * 40, "run-12-1")

    def test_run_provenance(self):
        run = {"status": "completed", "conclusion": "success", "event": "workflow_dispatch",
               "head_sha": "a" * 40, "head_repository": {"full_name": "owner/repo"},
               "path": ".github/workflows/p4-cloud-build.yml"}
        verify_run(run, "owner/repo", "a" * 40)
        for key, value in (("conclusion", "failure"), ("event", "pull_request"),
                           ("path", "different.yml"), ("head_sha", "b" * 40)):
            with self.assertRaises(ValueError):
                verify_run(dict(run, **{key: value}), "owner/repo", "a" * 40)

    def test_private_inputs_prevalidated_before_copy(self):
        source, project = self.root / "inputs", self.root / "project"
        source.mkdir()
        (project / ".git/info").mkdir(parents=True)
        values = {"sdkconfig": b"configuration", "dependencies.lock": b"locked"}
        for name, value in values.items():
            (source / name).write_bytes(value)
        manifest = {name: digest(value) for name, value in values.items()}
        manifest["dependencies.lock"] = "0" * 64
        (source / "build-inputs.sha256.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            prepare(source, project)
        self.assertFalse((project / "sdkconfig").exists())
        manifest["dependencies.lock"] = digest(b"locked")
        (source / "build-inputs.sha256.json").write_text(json.dumps(manifest))
        prepare(source, project)
        self.assertEqual((project / "sdkconfig").read_bytes(), b"configuration")


if __name__ == "__main__":
    unittest.main()
