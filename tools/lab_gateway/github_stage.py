"""Fetch an explicitly chosen successful Actions run; never flash or start a relay."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

from common import MAX_BLOB, MAX_JSON, digest, encoded, validate_job
from surface import node_lock, parse_segments, validate_p4_layout


def api(path):
    result = subprocess.run(["gh", "api", path], check=True, capture_output=True, timeout=45)
    return json.loads(result.stdout)


def verify_run(run, repo, sha):
    if (run.get("status") != "completed" or run.get("conclusion") != "success"
            or run.get("event") != "workflow_dispatch"
            or run.get("path") != ".github/workflows/p4-cloud-build.yml"
            or run.get("head_sha") != sha
            or run.get("head_repository", {}).get("full_name", "").lower() != repo.lower()):
        raise ValueError("run does not match reviewed workflow, source or success state")


def admit_archive(archive, target, sha, expected_id):
    with zipfile.ZipFile(archive) as bundle:
        entries = bundle.infolist()
        if len(entries) != 2 or {e.filename for e in entries} != {"job.json", "firmware.bin"}:
            raise ValueError("bundle must contain exactly firmware.bin and job.json")
        limits = {"job.json": MAX_JSON, "firmware.bin": 5 * 1024 * 1024}
        if any(e.file_size > limits[e.filename] or e.flag_bits & 1 for e in entries):
            raise ValueError("invalid archive size or encryption")
        job = validate_job(json.loads(bundle.read("job.json")))
        if job["commit"] != sha or job["id"] != expected_id or job["action"] != "stage":
            raise ValueError("bundle identity mismatch")
        payload = bundle.read("firmware.bin")
    if len(payload) != job["firmware"]["size"] or digest(payload) != job["firmware"]["sha256"]:
        raise ValueError("firmware integrity failure")
    validate_p4_layout(parse_segments(payload))
    if job["marker"].encode() not in payload:
        raise ValueError("firmware marker mismatch")
    if target.exists():
        if ((target / "job.json").read_bytes() != encoded(job)
                or (target / "firmware.bin").read_bytes() != payload):
            raise ValueError("existing staged bundle differs")
        return job
    with tempfile.TemporaryDirectory(dir=target.parent) as scratch:
        folder = Path(scratch) / "bundle"
        folder.mkdir()
        (folder / "firmware.bin").write_bytes(payload)
        (folder / "job.json").write_bytes(encoded(job))
        folder.rename(target)
    return job


def fetch(repo, run_id, sha, state):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("exact owner/repo and 40-character commit required")
    if run_id <= 0:
        raise ValueError("invalid run id")
    run = api(f"repos/{repo}/actions/runs/{run_id}")
    verify_run(run, repo, sha)
    attempt = int(run["run_attempt"])
    job_id = f"run-{run_id}-{attempt}"
    name = f"p4-firmware-{run_id}-{attempt}"
    artifacts = api(f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100")
    matches = [a for a in artifacts["artifacts"] if a["name"] == name and not a["expired"]]
    if len(matches) != 1 or not 0 < matches[0]["size_in_bytes"] <= MAX_BLOB:
        raise ValueError("one unexpired bounded firmware artifact required")
    artifact = matches[0]
    archive_hash = artifact.get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", archive_hash):
        raise ValueError("GitHub artifact SHA256 required")
    state.mkdir(parents=True, exist_ok=True)
    with node_lock(state / "github-stage.lock"), tempfile.TemporaryDirectory(dir=state) as scratch:
        archive = Path(scratch) / "artifact.zip"
        # gh handles GitHub's signed download redirect without exposing credentials.
        with archive.open("wb") as output:
            subprocess.run(["gh", "api", f"repos/{repo}/actions/artifacts/{int(artifact['id'])}/zip"],
                           stdout=output, check=True, timeout=180)
        if archive.stat().st_size > MAX_BLOB or digest(archive.read_bytes()) != archive_hash[7:]:
            raise ValueError("GitHub archive digest mismatch")
        target = state / job_id
        job = admit_archive(archive, target, sha, job_id)
        result = {"status": "STAGED", "id": job_id, "repo": repo, "commit": sha,
                  "artifact_id": artifact["id"], "sha256": job["firmware"]["sha256"],
                  "hardware_verified": False}
        (target / "stage-result.json").write_bytes(encoded(result))
        return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default="Zfh-charles/esp32-p4-i2cpolling")
    parser.add_argument("--run", type=int, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--state", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(fetch(args.repo, args.run, args.commit, args.state)))
