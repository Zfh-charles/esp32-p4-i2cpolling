"""Long soak: poll stability meta/alerts; write heartbeat status for agent."""
from pathlib import Path
import time

ROOT = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109")
ALERT = ROOT / "stability_alerts.log"
TRIAGE = ROOT / "stability_triage.log"
META = ROOT / "stability_watch.meta"
STATUS = ROOT / "stability_status.txt"
LOG = ROOT / "serial_monitor.log"

last_alert_size = ALERT.stat().st_size if ALERT.exists() else 0
last_triage_size = TRIAGE.stat().st_size if TRIAGE.exists() else 0
n = 0
while True:
    n += 1
    now = time.strftime("%Y-%m-%d %H:%M:%S")
    meta = META.read_text(encoding="utf-8", errors="replace") if META.exists() else "missing"
    log_mtime = time.strftime("%H:%M:%S", time.localtime(LOG.stat().st_mtime)) if LOG.exists() else "?"
    new_alert = False
    new_triage = False
    if ALERT.exists() and ALERT.stat().st_size > last_alert_size:
        new_alert = True
        last_alert_size = ALERT.stat().st_size
    if TRIAGE.exists() and TRIAGE.stat().st_size > last_triage_size:
        new_triage = True
        last_triage_size = TRIAGE.stat().st_size

    # last FW / REBOOT from serial tail
    tail = ""
    try:
        data = LOG.read_bytes()[-60000:].decode("utf-8", "replace")
        lines = data.splitlines()
        fw = [l for l in lines if "FW_MARKER" in l]
        reb = [l for l in lines if "REBOOT_AFTER" in l]
        face = [l for l in lines if "FACE_ARC" in l or "FACE_BREATHE tick" in l]
        tail = f"fw={fw[-1][-60:] if fw else '-'}\nlast_reboot={reb[-1][:120] if reb else 'none'}\nlast_face={face[-1][:120] if face else '-'}\n"
    except Exception as e:
        tail = f"tail_err={e}\n"

    flag = ""
    if new_alert or new_triage:
        flag = "NEW_CRASH_EVENT\n"
    STATUS.write_text(
        f"updated={now}\nloop={n}\nlog_mtime={log_mtime}\n{flag}meta:\n{meta}\n{tail}",
        encoding="utf-8",
    )
    if new_alert or new_triage:
        print(f"{now} NEW_CRASH_EVENT", flush=True)
    elif n % 30 == 0:
        print(f"{now} heartbeat ok loop={n}", flush=True)
    time.sleep(20)
