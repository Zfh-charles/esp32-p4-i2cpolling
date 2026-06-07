"""
推理提醒 API — 固件 ReminderPoller 与 MCP backend_alert 共用队列。

启动:
  pip install -r requirements.txt
  python server.py

环境变量:
  INFERENCE_API_HOST   默认 0.0.0.0
  INFERENCE_API_PORT   默认 8765
  DEMO_DEVICE_ID       启动时自动插入演示提醒的 MAC（可留空跳过）
  SEED_DEMO_ON_START   默认 1，设为 0 则不自动 seed

接口:
  GET  /v1/devices/{device_id}/reminders/pending
  POST /v1/devices/{device_id}/reminders/push   MCP/推理服务写入队列
  POST /v1/devices/{device_id}/reminders/ack    固件播报后确认
"""
from __future__ import annotations

import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HOST = os.environ.get("INFERENCE_API_HOST", "0.0.0.0")
PORT = int(os.environ.get("INFERENCE_API_PORT", "8765"))
SEED_DEMO = os.environ.get("SEED_DEMO_ON_START", "1") != "0"

# device_id -> list of pending reminders
_PENDING: dict[str, list[dict]] = {}
_ACKED: set[str] = set()


def seed_demo_meeting(device_id: str) -> None:
    rid = f"meet-{int(time.time())}"
    _PENDING.setdefault(device_id, []).append(
        {
            "id": rid,
            "speak": True,
            "emotion": "neutral",
            "title": "会议提醒",
            "prompt": "请用简洁口语提醒用户：10分钟后有产品评审会，请提前进入会议室。",
            "expires_at": int(time.time()) + 3600,
        }
    )
    print(f"[seed] device={device_id} reminder={rid}")


def push_reminder(device_id: str, data: dict) -> dict:
    rid = data.get("id") or f"push-{int(time.time())}"
    speak = data.get("speak", True)
    delivery_mode = data.get("delivery_mode")
    if delivery_mode is None:
        delivery_mode = "mcp_wake" if speak else "alert_only"
    reminder = {
        "id": rid,
        "speak": speak,
        "delivery_mode": delivery_mode,
        "wake_text": data.get("wake_text", ""),
        "emotion": data.get("emotion", "neutral"),
        "title": data.get("title", "提醒"),
        "prompt": data.get("prompt") or data.get("message", ""),
        "expires_at": data.get("expires_at", int(time.time()) + 3600),
    }
    if not reminder["prompt"]:
        return {"success": False, "error": "prompt or message required"}
    queue = _PENDING.setdefault(device_id, [])
    queue = [r for r in queue if r["id"] != rid]
    queue.append(reminder)
    _PENDING[device_id] = queue
    _ACKED.discard(rid)
    print(f"[push] device={device_id} id={rid}")
    return {"success": True, "id": rid, "reminder": reminder}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[http] {self.address_string()} {fmt % args}")

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            return {}
        return json.loads(self.rfile.read(length))

    def _send(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _match_device_reminders(self, parts: list[str]) -> str | None:
        if (
            len(parts) == 5
            and parts[0] == "v1"
            and parts[1] == "devices"
            and parts[3] == "reminders"
        ):
            return parts[2]
        return None

    def do_GET(self):
        path = urlparse(self.path).path
        parts = path.strip("/").split("/")
        device_id = self._match_device_reminders(parts)
        if device_id and parts[4] == "pending":
            queue = _PENDING.get(device_id, [])
            queue = [r for r in queue if r["id"] not in _ACKED]
            now = int(time.time())
            queue = [r for r in queue if r.get("expires_at", now + 1) > now]
            _PENDING[device_id] = queue
            if not queue:
                return self._send(200, {"has_reminder": False})
            r = queue[0]
            speak = r.get("speak", True)
            delivery_mode = r.get("delivery_mode")
            if delivery_mode is None:
                delivery_mode = "mcp_wake" if speak else "alert_only"
            return self._send(
                200,
                {
                    "has_reminder": True,
                    "id": r["id"],
                    "speak": speak,
                    "delivery_mode": delivery_mode,
                    "wake_text": r.get("wake_text", ""),
                    "emotion": r.get("emotion", "neutral"),
                    "title": r.get("title", ""),
                    "prompt": r["prompt"],
                },
            )
        self._send(404, {"error": "not_found"})

    def do_POST(self):
        path = urlparse(self.path).path
        parts = path.strip("/").split("/")
        device_id = self._match_device_reminders(parts)
        if not device_id:
            return self._send(404, {"error": "not_found"})

        if parts[4] == "ack":
            data = self._read_json()
            rid = data.get("id", "")
            if rid:
                _ACKED.add(rid)
                _PENDING[device_id] = [
                    r for r in _PENDING.get(device_id, []) if r["id"] != rid
                ]
                print(f"[ack] device={device_id} id={rid}")
            return self._send(200, {"success": True})

        if parts[4] == "push":
            data = self._read_json()
            result = push_reminder(device_id, data)
            code = 200 if result.get("success") else 400
            return self._send(code, result)

        self._send(404, {"error": "not_found"})


def main():
    demo_device = os.environ.get("DEMO_DEVICE_ID", "")
    if SEED_DEMO and demo_device:
        seed_demo_meeting(demo_device)
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Inference API on http://{HOST}:{PORT}")
    print(f"  GET  /v1/devices/{{device_id}}/reminders/pending")
    print(f"  POST /v1/devices/{{device_id}}/reminders/push")
    print(f"  POST /v1/devices/{{device_id}}/reminders/ack")
    server.serve_forever()


if __name__ == "__main__":
    main()
