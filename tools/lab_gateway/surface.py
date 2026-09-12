"""Outbound polling node. Stage by default; flash only through a local fixed adapter."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from common import Transport, database, digest, encoded, validate_job

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "code_health"))
from check_p4_image_layout import parse_segments, validate_p4_layout


@contextmanager
def node_lock(path):
    """Kernel lock released on process exit; never delete a possibly live lock."""
    with Path(path).open("a+b") as stream:
        stream.seek(0)
        if not stream.read(1):
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


class Node:
    def __init__(self, config, transport):
        self.config, self.transport = config, transport
        self.root = Path(config["state_dir"]).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.db = self.root / "node.sqlite"
        with database(self.db) as db:
            db.execute("CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, job_hash TEXT, result TEXT)")

    def execute(self, job, firmware):
        if job["action"] == "stage":
            return "STAGED"
        command = self.config.get("flash_adapter", [])
        approved = self.config.get("approved_flash_sha256", [])
        if not self.config.get("allow_flash", False) or job["firmware"]["sha256"] not in approved:
            return "BLOCKED_LOCAL_FLASH_POLICY"
        if not command or not all(isinstance(part, str) for part in command):
            return "BLOCKED_NO_FLASH_ADAPTER"
        monitor_lock = self.config.get("monitor_lock")
        if not monitor_lock:
            return "BLOCKED_NO_MONITOR_LOCK"
        # Remote jobs cannot specify shell commands, COM ports, addresses or paths.
        # Adapter owns monitor handoff, physical-port exclusion and known-good policy.
        request = {"job": job, "firmware_path": str(firmware), "port": self.config["port"]}
        try:
            # Shares the existing COM7 monitor byte lock. Never kill a logger or
            # steal its port; an active monitor makes this job explicitly blocked.
            with node_lock(monitor_lock):
                with (self.root / (job["id"] + ".adapter.log")).open("wb") as log:
                    try:
                        completed = subprocess.run(command, input=encoded(request), stdout=log,
                                                   stderr=log, shell=False, timeout=600)
                    except (OSError, subprocess.TimeoutExpired):
                        return "FLASH_OUTCOME_UNKNOWN_DO_NOT_RETRY"
        except OSError:
            return "BLOCKED_MONITOR_BUSY_OR_LOCK_UNAVAILABLE"
        return ("ADAPTER_COMPLETED_DEVICE_VALIDATION_PENDING" if completed.returncode == 0
                else "FLASH_FAILED_DO_NOT_RETRY")

    def tick(self):
        with node_lock(self.root / "node.lock"):
            job = self.transport.json("/next")
            if job is None:
                return None
            validate_job(job)
            fingerprint = digest(encoded(job))
            with database(self.db) as db:
                previous = db.execute("SELECT job_hash,result FROM runs WHERE id=?", (job["id"],)).fetchone()
            if previous:
                if previous[0] != fingerprint:
                    raise ValueError("job id changed after admission")
                result = (json.loads(previous[1]) if previous[1] else
                          {"id": job["id"], "status": "EXECUTION_INTERRUPTED_DO_NOT_RETRY"})
            else:
                firmware = self.root / (job["firmware"]["sha256"] + ".bin")
                self.transport.download(job["firmware"], firmware)
                payload = firmware.read_bytes()
                validate_p4_layout(parse_segments(payload))
                if job["marker"].encode() not in payload:
                    raise ValueError("marker not present in firmware")
                # Durable intent BEFORE side effects: a crash is unknown, never auto-repeat.
                with database(self.db) as db:
                    db.execute("INSERT INTO runs VALUES (?,?,NULL)", (job["id"], fingerprint))
                status = self.execute(job, firmware)
                result = {"id": job["id"], "status": status, "sha256": job["firmware"]["sha256"],
                          "expected_marker": job["marker"], "hardware_verified": False}
            with database(self.db) as db:
                db.execute("UPDATE runs SET result=? WHERE id=?", (encoded(result).decode(), job["id"]))
            # If upload fails, the durable outcome is retried; execute() is not.
            self.transport.json("/results/" + job["id"], result)
            return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8-sig"))
    node = Node(config, Transport(config["relay"], os.environ["LAB_NODE_TOKEN"]))
    delay = 5
    while True:
        try:
            result = node.tick()
            if result:
                print(json.dumps(result), flush=True)
            delay = 5
        except Exception as exc:
            print("Gateway retry: " + type(exc).__name__, file=sys.stderr, flush=True)
            if args.once:
                return 1
            delay = min(delay * 2, 120)
        if args.once:
            return 0
        time.sleep(delay)


if __name__ == "__main__":
    raise SystemExit(main())
