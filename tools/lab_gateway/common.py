"""Bounded HTTPS transport and the v1 single-board lab contract (stdlib only)."""
import hashlib
from contextlib import contextmanager
import json
import re
import sqlite3
import urllib.parse
import urllib.request
from pathlib import Path

MAX_BLOB = 8 * 1024 * 1024
MAX_JSON = 64 * 1024
BOARD = "ep-chat-p4-ml307"


@contextmanager
def database(path):
    connection = sqlite3.connect(path)
    try:
        with connection:
            yield connection
    finally:
        connection.close()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", value):
        raise ValueError("invalid identifier")
    return value


def validate_job(job):
    if not isinstance(job, dict) or set(job) != {
        "version", "id", "board", "action", "marker", "commit", "firmware", "provenance"
    }:
        raise ValueError("invalid job fields")
    identifier(job["id"])
    if job["version"] != 1 or job["board"] != BOARD or job["action"] not in ("stage", "flash"):
        raise ValueError("unsupported job")
    identifier(job["marker"])
    if not re.fullmatch(r"[0-9a-f]{40,64}", job["commit"]):
        raise ValueError("source commit required")
    fw = job["firmware"]
    if not isinstance(fw, dict) or set(fw) != {"sha256", "size"}:
        raise ValueError("invalid firmware descriptor")
    if not re.fullmatch(r"[0-9a-f]{64}", fw["sha256"]):
        raise ValueError("invalid hash")
    if type(fw["size"]) is not int or not 24 <= fw["size"] <= 5 * 1024 * 1024:
        raise ValueError("firmware exceeds current 5 MiB app partition")
    if not isinstance(job["provenance"], dict) or job["provenance"].get("release") != "same_graph":
        raise ValueError("same-graph release provenance required")
    if len(encoded(job)) > MAX_JSON:
        raise ValueError("job too large")
    return job


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("redirect rejected; use the final trusted relay URL")


class Transport:
    def __init__(self, base, token):
        parsed = urllib.parse.urlsplit(base)
        if (parsed.scheme != "https" and not
                (parsed.scheme == "http" and parsed.hostname in ("127.0.0.1", "::1", "localhost"))):
            raise ValueError("HTTPS required outside loopback")
        if parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/"):
            raise ValueError("relay URL must be an origin")
        if not token:
            raise ValueError("missing relay token")
        self.base, self.token = base.rstrip("/"), token
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, path, data=None, method=None, limit=MAX_JSON):
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers={"Authorization": "Bearer " + self.token})
        with self.opener.open(req, timeout=30) as response:
            body = response.read(limit + 1)
        if len(body) > limit:
            raise ValueError("response size limit exceeded")
        return body

    def json(self, path, value=None):
        return json.loads(self.request(path, None if value is None else encoded(value)))

    def download(self, descriptor, destination):
        """Content-addressed cache; partial files are never admitted for flashing."""
        path = Path(destination)
        if path.exists() and path.stat().st_size == descriptor["size"] and digest(path.read_bytes()) == descriptor["sha256"]:
            return path
        temporary = path.with_suffix(".partial")
        req = urllib.request.Request(self.base + "/blobs/" + descriptor["sha256"],
                                     headers={"Authorization": "Bearer " + self.token})
        total, checksum = 0, hashlib.sha256()
        with self.opener.open(req, timeout=30) as response, temporary.open("wb") as output:
            while chunk := response.read(64 * 1024):
                total += len(chunk)
                if total > descriptor["size"]:
                    raise ValueError("download too large")
                checksum.update(chunk)
                output.write(chunk)
        if total != descriptor["size"] or checksum.hexdigest() != descriptor["sha256"]:
            raise ValueError("download integrity failure")
        temporary.replace(path)
        return path
