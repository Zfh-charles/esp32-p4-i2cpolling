# stability scripts

Host-side soak / reboot audit. Do **not** paste raw `serial_monitor.log` into an agent context.

- `reboot_audit.py` — per-marker mean lifetime (`prev_t` is milliseconds)
- `stability_soak_status.py` / `stability_evidence_collector.py` / `stability_watch.py`

`n<4` completed boots: do not declare stable.
