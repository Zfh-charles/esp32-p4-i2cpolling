"""Linux same-graph release builder. Run from the pinned IDF export environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from common import BOARD, digest, encoded, identifier


def run(command, cwd=None):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def build(root, out, job_id, execute=False):
    root, out = root.resolve(), out.resolve()
    identifier(job_id)
    idf = Path(os.environ["IDF_PATH"])
    version = run(["git", "describe", "--tags", "--exact-match"], idf)
    if version != "v5.4.1" or run(["git", "status", "--porcelain", "--untracked-files=no"], idf):
        raise ValueError("require pristine ESP-IDF v5.4.1")
    compiler = run(["riscv32-esp-elf-gcc", "--version"]).splitlines()[0]
    if "esp-14.2.0_20241119" not in compiler:
        raise ValueError("compiler differs from reviewed local toolchain")
    if run(["git", "rev-parse", "--show-toplevel"], root).replace("\\", "/") != root.as_posix():
        raise ValueError("project must be repository root")
    if run(["git", "status", "--porcelain"], root):
        raise ValueError("commit reviewed source before release; use an ignored output path")
    inputs = [root / "sdkconfig", root / "dependencies.lock", root / "partitions/v2/32m.csv"]
    before = {str(p.relative_to(root)): digest(p.read_bytes()) for p in inputs}
    if b'CONFIG_IDF_TARGET="esp32p4"' not in inputs[0].read_bytes():
        raise ValueError("wrong IDF target")
    # Pin all locally patched/vendor source explicitly. Never silently use stock LVGL.
    inventory = root / "build-inputs.sha256.json"
    expected = json.loads(inventory.read_text())
    if not isinstance(expected, dict) or not expected:
        raise ValueError("private build-inputs inventory required")
    for relative, sha in expected.items():
        path = (root / relative).resolve()
        if not path.is_relative_to(root) or digest(path.read_bytes()) != sha:
            raise ValueError("private build input mismatch: " + relative)
    commit = run(["git", "rev-parse", "HEAD"], root)
    destination = out / job_id
    if destination.exists():
        raise ValueError("release id already exists; use a new id")
    command = [sys.executable, str(idf / "tools/idf.py"), "-C", str(root),
               "-B", str(out / "build"), "build"]
    print(json.dumps({"plan": command, "idf": version, "commit": commit,
                      "private_input_count": len(expected), "full_build_possible": True}), flush=True)
    if not execute:
        return
    out.mkdir(parents=True, exist_ok=True)
    with (out / (job_id + ".build.log")).open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError("build failed; see build log")
    after = {str(p.relative_to(root)): digest(p.read_bytes()) for p in inputs}
    if before != after:
        raise ValueError("build changed sdkconfig/dependency inputs; review before publishing")
    firmware = out / "build/xiaozhi.bin"
    elf = out / "build/xiaozhi.elf"
    subprocess.run([sys.executable, str(root / "tools/code_health/check_p4_image_layout.py"),
                    str(firmware)], check=True)
    payload = firmware.read_bytes()
    markers = set(re.findall(rb"boot_trace_v10_[A-Za-z0-9_]+", payload))
    if len(markers) != 1 or not elf.is_file():
        raise ValueError("unambiguous marker and matching ELF required")
    provenance = {"release": "same_graph", "idf": version,
                  "idf_commit": run(["git", "rev-parse", "HEAD"], idf),
                  "compiler": compiler,
                  "inputs": before, "private_inventory_sha256": digest(inventory.read_bytes()),
                  "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest()}
    job = {"version": 1, "id": job_id, "board": BOARD, "action": "stage",
           "marker": next(iter(markers)).decode(), "commit": commit,
           "firmware": {"sha256": digest(payload), "size": len(payload)}, "provenance": provenance}
    from common import validate_job
    validate_job(job)
    destination.mkdir()
    shutil.copyfile(firmware, destination / "firmware.bin")
    shutil.copyfile(elf, destination / "firmware.elf")
    (destination / "job.json").write_bytes(encoded(job))
    print("RELEASE_READY " + str(destination), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--id", required=True)
    parser.add_argument("--execute", action="store_true", help="otherwise inspect inputs and print build plan")
    args = parser.parse_args()
    build(args.project, args.output, args.id, args.execute)
