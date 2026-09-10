"""Real loopback HTTP integration plus failure-injection; no serial device touched."""
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from common import BOARD, Transport, database, digest, encoded, validate_job
from relay import server
from surface import Node, node_lock


def image():
    header = bytearray(24)
    header[0], header[1] = 0xE9, 3
    marker = b"boot_trace_v10_test_lab"
    return (bytes(header) + struct.pack("<II", 0x48000020, 65496) + bytes(65496)
            + struct.pack("<II", 0x4FF00000, 24) + bytes(24)
            + struct.pack("<II", 0x40000020, len(marker)) + marker)


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.server = server(root / "relay", "p" * 32, "n" * 32)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        base = "http://127.0.0.1:" + str(self.server.server_port)
        self.producer = Transport(base, "p" * 32)
        self.transport = Transport(base, "n" * 32)
        self.node = Node({"state_dir": str(root / "node"), "port": "COM7"}, self.transport)
        self.payload = image()
        self.job = {"version": 1, "id": "test-1", "board": BOARD, "action": "stage",
                    "marker": "boot_trace_v10_test_lab", "commit": "a" * 40,
                    "firmware": {"sha256": digest(self.payload), "size": len(self.payload)},
                    "provenance": {"release": "same_graph"}}

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def enqueue(self):
        self.producer.request("/blobs/" + digest(self.payload), self.payload, "PUT")
        self.producer.json("/jobs", self.job)

    def test_real_http_stage_roundtrip_and_duplicate_submission(self):
        self.enqueue()
        self.enqueue()
        self.assertEqual(self.node.tick()["status"], "STAGED")
        self.assertIsNone(self.node.tick())
        result = self.producer.json("/results/test-1")
        self.assertFalse(result["hardware_verified"])
        self.assertEqual(result["sha256"], digest(self.payload))

    def test_upload_failure_retries_result_not_execution(self):
        self.enqueue()
        original = self.transport.json
        def fail_result(path, value=None):
            if value is not None:
                raise ConnectionError("offline")
            return original(path, value)
        with patch.object(self.node, "execute", wraps=self.node.execute) as execute:
            with patch.object(self.transport, "json", side_effect=fail_result):
                with self.assertRaises(ConnectionError):
                    self.node.tick()
            self.node.tick()
            self.assertEqual(execute.call_count, 1)

    def test_interrupted_process_is_not_reexecuted(self):
        self.enqueue()
        with database(self.node.db) as db:
            db.execute("INSERT INTO runs VALUES (?,?,NULL)", (self.job["id"], digest(encoded(self.job))))
        with patch.object(self.node, "execute") as execute:
            self.assertEqual(self.node.tick()["status"], "EXECUTION_INTERRUPTED_DO_NOT_RETRY")
            execute.assert_not_called()

    def test_corrupt_download_never_reaches_executor(self):
        self.enqueue()
        (self.server.store.root / digest(self.payload)).write_bytes(b"broken")
        with patch.object(self.node, "execute") as execute:
            with self.assertRaises(ValueError):
                self.node.tick()
            execute.assert_not_called()

    def test_flash_disabled_by_default(self):
        self.job["action"] = "flash"
        self.enqueue()
        with patch("surface.subprocess.run") as run:
            self.assertEqual(self.node.tick()["status"], "BLOCKED_LOCAL_FLASH_POLICY")
            run.assert_not_called()

    def test_fixed_adapter_only_for_locally_approved_hash(self):
        self.node.config.update(allow_flash=True, flash_adapter=["local-adapter"],
                                monitor_lock=str(self.node.root / "monitor.lock"),
                                approved_flash_sha256=[digest(self.payload)])
        self.job["action"] = "flash"
        self.enqueue()
        with patch("surface.subprocess.run") as run:
            run.return_value.returncode = 0
            self.assertEqual(self.node.tick()["status"], "ADAPTER_COMPLETED_DEVICE_VALIDATION_PENDING")
            self.assertFalse(run.call_args.kwargs["shell"])
            self.assertEqual(run.call_args.args[0], ["local-adapter"])

    def test_live_monitor_blocks_adapter(self):
        lock = self.node.root / "monitor.lock"
        self.node.config.update(allow_flash=True, flash_adapter=["local-adapter"],
                                monitor_lock=str(lock), approved_flash_sha256=[digest(self.payload)])
        self.job["action"] = "flash"
        self.enqueue()
        with node_lock(lock), patch("surface.subprocess.run") as run:
            self.assertEqual(self.node.tick()["status"], "BLOCKED_MONITOR_BUSY_OR_LOCK_UNAVAILABLE")
            run.assert_not_called()

    def test_oversized_firmware_rejected(self):
        self.job["firmware"]["size"] = 5 * 1024 * 1024 + 1
        with self.assertRaises(ValueError):
            validate_job(self.job)

    def test_download_cache_avoids_network(self):
        self.enqueue()
        path = self.node.root / "cached.bin"
        path.write_bytes(self.payload)
        with patch.object(self.transport.opener, "open") as open_request:
            self.transport.download(self.job["firmware"], path)
            open_request.assert_not_called()

    def test_node_lock_excludes_second_owner(self):
        with node_lock(self.node.root / "node.lock"):
            with self.assertRaises(OSError):
                with node_lock(self.node.root / "node.lock"):
                    self.fail("second owner acquired lock")

    def test_roles_and_tokens(self):
        with self.assertRaises(HTTPError) as error:
            self.producer.json("/next")
        self.assertEqual(error.exception.code, 403)
        error.exception.close()
        with self.assertRaises(HTTPError) as error:
            Transport(self.producer.base, "wrong").json("/next")
        self.assertEqual(error.exception.code, 401)
        error.exception.close()

    def test_changed_job_id_rejected(self):
        self.enqueue()
        self.job["action"] = "flash"
        with self.assertRaises(HTTPError) as error:
            self.producer.json("/jobs", self.job)
        error.exception.close()

    def test_remote_paths_and_commands_rejected(self):
        for field in ("command", "port", "offset", "firmware_path"):
            with self.assertRaises(ValueError):
                validate_job(dict(self.job, **{field: "arbitrary"}))
        self.job["id"] = "../escape"
        with self.assertRaises(ValueError):
            validate_job(self.job)

    def test_https_required_outside_loopback(self):
        with self.assertRaises(ValueError):
            Transport("http://example.com", "token")

    def test_marker_mismatch_blocks_execution(self):
        self.job["marker"] = "boot_trace_v10_wrong"
        self.enqueue()
        with patch.object(self.node, "execute") as execute:
            with self.assertRaises(ValueError):
                self.node.tick()
            execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
