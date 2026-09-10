"""Single Surface relay. Bind loopback behind an authenticated HTTPS reverse proxy."""
import argparse
import hmac
import json
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from common import MAX_BLOB, MAX_JSON, database, digest, encoded, identifier, validate_job


class Store:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.db = self.root / "queue.sqlite"
        self.lock = threading.Lock()
        with database(self.db) as db:
            db.executescript("CREATE TABLE IF NOT EXISTS jobs (id TEXT PRIMARY KEY, body TEXT);"
                             "CREATE TABLE IF NOT EXISTS results (id TEXT PRIMARY KEY, body TEXT);")

    def add(self, table, key, value):
        body = encoded(value).decode()
        with self.lock, database(self.db) as db:
            previous = db.execute(f"SELECT body FROM {table} WHERE id=?", (key,)).fetchone()
            if previous and previous[0] != body:
                raise ValueError("immutable id reused with different content")
            db.execute(f"INSERT OR IGNORE INTO {table} VALUES (?,?)", (key, body))

    def next(self):
        with database(self.db) as db:
            row = db.execute("SELECT body FROM jobs WHERE id NOT IN (SELECT id FROM results) ORDER BY rowid LIMIT 1").fetchone()
        return json.loads(row[0]) if row else None

    def get(self, table, key):
        with database(self.db) as db:
            row = db.execute(f"SELECT body FROM {table} WHERE id=?", (key,)).fetchone()
        return json.loads(row[0]) if row else None


def server(root, producer_token, node_token, port=0):
    if min(len(producer_token), len(node_token)) < 32 or producer_token == node_token:
        raise ValueError("two distinct tokens of at least 32 characters required")
    store = Store(root)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # Do not log tokens or job payloads.

        def setup(self):
            super().setup()
            self.connection.settimeout(30)

        def reply(self, status, value):
            body = encoded(value)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def handle_request(self):
            auth = self.headers.get("Authorization", "")
            producer = hmac.compare_digest(auth, "Bearer " + producer_token)
            node = hmac.compare_digest(auth, "Bearer " + node_token)
            if not (producer or node):
                return self.reply(401, {"error": "unauthorized"})
            try:
                path = self.path
                if self.command == "GET" and path == "/next" and node:
                    return self.reply(200, store.next())
                if re.fullmatch(r"/blobs/[0-9a-f]{64}", path):
                    sha = path.rsplit("/", 1)[1]
                    artifact = store.root / sha
                    if self.command == "PUT" and producer:
                        body = self.read_body(MAX_BLOB)
                        if digest(body) != sha:
                            raise ValueError("artifact hash mismatch")
                        with store.lock:
                            temp = artifact.with_suffix(".partial")
                            temp.write_bytes(body)
                            temp.replace(artifact)
                        return self.reply(200, {"sha256": sha})
                    if self.command == "GET" and node:
                        size = artifact.stat().st_size
                        self.send_response(200)
                        self.send_header("Content-Length", str(size))
                        self.end_headers()
                        with artifact.open("rb") as stream:
                            while chunk := stream.read(64 * 1024):
                                self.wfile.write(chunk)
                        return
                if self.command == "POST" and path == "/jobs" and producer:
                    job = validate_job(json.loads(self.read_body(MAX_JSON)))
                    artifact = store.root / job["firmware"]["sha256"]
                    if artifact.stat().st_size != job["firmware"]["size"]:
                        raise ValueError("upload artifact before job")
                    store.add("jobs", job["id"], job)
                    return self.reply(200, {"id": job["id"]})
                if path.startswith("/results/"):
                    key = identifier(path[len("/results/"):])
                    if self.command == "GET" and producer:
                        return self.reply(200, store.get("results", key))
                    if self.command == "POST" and node:
                        if not store.get("jobs", key):
                            raise ValueError("unknown job")
                        result = json.loads(self.read_body(MAX_JSON))
                        if not isinstance(result, dict) or result.get("id") != key:
                            raise ValueError("result identity mismatch")
                        store.add("results", key, result)
                        return self.reply(200, {"id": key})
                return self.reply(403, {"error": "operation forbidden"})
            except (ValueError, TypeError, KeyError, OSError) as exc:
                return self.reply(400, {"error": type(exc).__name__})

        def read_body(self, limit):
            length = int(self.headers.get("Content-Length", "-1"))
            if not 0 <= length <= limit:
                raise ValueError("invalid content length")
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError("truncated request")
            return data

        do_GET = do_POST = do_PUT = handle_request

    instance = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    instance.store = store
    return instance


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--port", type=int, default=8768)
    args = parser.parse_args()
    server(args.state, os.environ["LAB_PRODUCER_TOKEN"], os.environ["LAB_NODE_TOKEN"], args.port).serve_forever()
