# 推理提醒 API Demo

与固件 `ReminderPoller`、MCP `backend_alert.py` 使用同一 HTTP 接口。

## 启动

```bash
cd tools/inference-api-demo
python server.py
```

默认 `http://0.0.0.0:8765`，启动时为 `DEMO_DEVICE_ID`（默认 `00:00:00:00:00:00`）插入一条会议提醒。

## 固件配置

`menuconfig` → 启用 **推理提醒 HTTP 轮询**，烧录后在 NVS 写入（可用代码或后续配网工具）：

| 键 | 示例 |
|----|------|
| `reminder_poll.poll_url` | `http://192.168.1.100:8765/v1/devices/{device_id}/reminders/pending` |
| `reminder_poll.ack_url` | （可选）`http://192.168.1.100:8765/v1/devices/{device_id}/reminders/ack` |

`{device_id}` 会替换为设备 MAC。

## 接口

见 [docs/architecture-reminder-poll-mcp.md](../../docs/architecture-reminder-poll-mcp.md)。
