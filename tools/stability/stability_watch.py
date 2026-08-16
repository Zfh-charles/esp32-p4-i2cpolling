"""Stability watch: alert on real reboot/fault + capture triage context."""
import time
import re
from pathlib import Path

LOG = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\serial_monitor.log")
ALERT = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\stability_alerts.log")
TRIAGE = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\stability_triage.log")
META = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\stability_watch.meta")

# Real reboot/fault only — do not match policy text "HP_WDT".
PAT = re.compile(
    r"rst:0x\d|HP_SYS_HP_WDT_RESET|Guru Meditation|REBOOT_AFTER|"
    r"reason=WDT|reason=PANIC|stack overflow|Brownout|assert failed|panic'ed|"
    r"PANIC_LAST \|",
    re.I,
)
KEY = re.compile(
    r"FW_MARKER|BOOT \||prev_phase|PANIC_LAST|CACHE |FACE_|MQTT_STATE|AFE_DIAG|"
    r"HEAP_INTEGRITY|IDLE_|rst:|Guru|REBOOT_AFTER|Saved PC|SAFE_MODE",
    re.I,
)

pos = LOG.stat().st_size if LOG.exists() else 0
META.write_text(f"running=1\nstarted={time.time()}\npos={pos}\n", encoding="utf-8")
with ALERT.open("a", encoding="utf-8") as af:
    af.write(f"\n===== STABILITY WATCH START {time.strftime('%Y-%m-%d %H:%M:%S')} (ctx) =====\n")

ring: list[str] = []
RING_MAX = 80
crash_n = 0


def triage_dump(trigger: str) -> None:
    global crash_n
    crash_n += 1
    keys = [l for l in ring if KEY.search(l)]
    with TRIAGE.open("a", encoding="utf-8") as tf:
        tf.write(f"\n======== CRASH #{crash_n} {time.strftime('%Y-%m-%d %H:%M:%S')} ========\n")
        tf.write(f"trigger: {trigger[:240]}\n")
        tf.write("--- key lines (ring) ---\n")
        for l in keys[-40:]:
            tf.write(l[:240] + "\n")
        tf.write("--- last 25 raw ---\n")
        for l in ring[-25:]:
            tf.write(l[:240] + "\n")
    with ALERT.open("a", encoding="utf-8") as af:
        af.write(f"{time.strftime('%H:%M:%S')} TRIAGE_WRITTEN n={crash_n}\n")


while True:
    try:
        if not LOG.exists():
            time.sleep(1)
            continue
        size = LOG.stat().st_size
        if size < pos:
            pos = 0
        if size > pos:
            with LOG.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
            for line in chunk.splitlines():
                ring.append(line)
                if len(ring) > RING_MAX:
                    ring = ring[-RING_MAX:]
                if PAT.search(line):
                    msg = f"{time.strftime('%H:%M:%S')} ALERT {line[:240]}\n"
                    with ALERT.open("a", encoding="utf-8") as af:
                        af.write(msg)
                    print(msg, end="", flush=True)
                    if re.search(r"REBOOT_AFTER|Guru Meditation|rst:0x", line, re.I):
                        triage_dump(line)
        META.write_text(
            f"running=1\nlast={time.time()}\npos={pos}\ncrashes={crash_n}\n",
            encoding="utf-8",
        )
        time.sleep(1.0)
    except Exception as e:
        with ALERT.open("a", encoding="utf-8") as af:
            af.write(f"WATCH_ERR {e}\n")
        time.sleep(2)
