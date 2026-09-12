"""Submit one reviewed build or read its result; never upload ELF or raw logs."""
import argparse
import json
import os
from pathlib import Path
from common import Transport, digest, identifier, validate_job


def submit(transport, bundle):
    job = validate_job(json.loads((bundle / "job.json").read_text()))
    payload = (bundle / "firmware.bin").read_bytes()
    if len(payload) != job["firmware"]["size"] or digest(payload) != job["firmware"]["sha256"]:
        raise ValueError("bundle integrity failure")
    transport.request("/blobs/" + job["firmware"]["sha256"], payload, "PUT")
    return transport.json("/jobs", job)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--relay", required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--bundle", type=Path)
    group.add_argument("--result")
    args = parser.parse_args()
    transport = Transport(args.relay, os.environ["LAB_PRODUCER_TOKEN"])
    result = (submit(transport, args.bundle) if args.bundle else
              transport.json("/results/" + identifier(args.result)))
    print(json.dumps(result))
