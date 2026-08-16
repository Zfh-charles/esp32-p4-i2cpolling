"""
Collect reboot/panic evidence into a dated retrospective file.
Backfills from serial_monitor.log, then tails for new events.
"""
from __future__ import annotations

import re
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109")
LOG = ROOT / "serial_monitor.log"
DAY = datetime.now().strftime("%Y%m%d")
EVIDENCE = ROOT / f"retrospective_evidence_{DAY}.log"
META = ROOT / "stability_evidence.meta"
SUMMARY = ROOT / f"retrospective_summary_{DAY}.md"

# Real reboot/fault only — never match policy text "HP_WDT" alone.
TRIGGER = re.compile(
    r"REBOOT_AFTER|Guru Meditation Error|rst:0x\d+\s*\(|HP_SYS_HP_WDT_RESET|"
    r"PANIC_LAST \| (?!none)",
    re.I,
)
BOOT_MARK = re.compile(r"FW_MARKER|BOOT \| reason=|SAFE_MODE|REBOOT_AFTER|PANIC_LAST", re.I)
CTX_KEY = re.compile(
    r"FW_MARKER|BOOT \| |REBOOT_AFTER|PANIC_LAST|CACHE |prev_phase|Saved PC|"
    r"FACE_BREATHE|FACE_BUS|FACE_ROWS|FACE_ASSET|FACE_SLICE|HEAP_INTEGRITY|"
    r"MQTT_|AFE_|rst:0x|Guru|SAFE_MODE|Encode wake|mcp_wake|SD_MOUNT",
    re.I,
)


def append(text: str) -> None:
    with EVIDENCE.open("a", encoding="utf-8") as f:
        f.write(text)
        if not text.endswith("\n"):
            f.write("\n")


def write_summary(events: list[dict]) -> None:
    lines = [
        f"# 稳定性复盘证据摘要 {DAY}",
        "",
        f"- 生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        f"- 证据全文: `{EVIDENCE.name}`",
        f"- 事件数: {len(events)}",
        "",
        "| # | 本地时间 | reason | prev_t | prev_phase | marker 线索 |",
        "|---|----------|--------|--------|------------|-------------|",
    ]
    for i, e in enumerate(events, 1):
        lines.append(
            f"| {i} | {e.get('wall','-')} | {e.get('reason','-')} | "
            f"{e.get('prev_t','-')} | {e.get('prev_phase','-')} | {e.get('marker','-')} |"
        )
    lines.extend(
        [
            "",
            "## 验收关键字（串口搜）",
            "`FW_MARKER` / `REBOOT_AFTER` / `PANIC_LAST` / `prev_phase` / `FACE_ASSET` / `SAFE_MODE`",
            "",
            "## 备注",
            "- 邻接 `prev_phase=FACE_PRESENT` ≠ 脸有罪；须看 `PANIC_LAST` mepc/ra/mtval。",
            "- 本文件由 `stability_evidence_collector.py` 自动维护。",
            "",
        ]
    )
    SUMMARY.write_text("\n".join(lines), encoding="utf-8")


def parse_reboot_fields(line: str) -> dict:
    out = {"reason": "?", "prev_t": "?", "prev_phase": "?"}
    m = re.search(r"reason=(\S+)", line)
    if m:
        out["reason"] = m.group(1)
    m = re.search(r"prev_t=(\d+)ms", line)
    if m:
        sec = int(m.group(1)) / 1000.0
        out["prev_t"] = f"{sec:.0f}s"
    m = re.search(r"prev_phase=(\S+)", line)
    if m:
        out["prev_phase"] = m.group(1)
    return out


def dump_event(idx: int, trigger: str, before: list[str], after: list[str], events: list[dict]) -> None:
    wall = datetime.now().strftime("%H:%M:%S")
    fields = parse_reboot_fields(trigger)
    marker = "-"
    for l in reversed(before + [trigger] + after):
        if "FW_MARKER" in l:
            m = re.search(r"FW_MARKER\s+(\S+)", l)
            if m:
                marker = m.group(1)
            break
    fields.update({"wall": wall, "marker": marker})
    events.append(fields)

    append(f"\n{'=' * 72}")
    append(f"EVENT #{idx} wall={wall} marker={marker}")
    append(f"trigger: {trigger.strip()[:300]}")
    append("--- context before (key) ---")
    keys = [l for l in before if CTX_KEY.search(l)]
    for l in keys[-30:]:
        append(l.rstrip()[:280])
    append("--- trigger+after (key) ---")
    for l in ([trigger] + after):
        if CTX_KEY.search(l) or TRIGGER.search(l):
            append(l.rstrip()[:280])
    append("")
    write_summary(events)


def backfill(events: list[dict]) -> int:
    """Scan last ~4MB for REBOOT_AFTER and capture packets. Skip if already in evidence."""
    if not LOG.exists():
        return 0
    existing = EVIDENCE.read_text(encoding="utf-8", errors="replace") if EVIDENCE.exists() else ""
    data = LOG.read_bytes()[-4_000_000:].decode("utf-8", errors="replace")
    lines = data.splitlines()
    n = 0
    for i, line in enumerate(lines):
        if "REBOOT_AFTER" not in line:
            continue
        # dedupe by unique snippet
        key = line.strip()[:160]
        if key in existing:
            continue
        before = lines[max(0, i - 40) : i]
        after = lines[i : min(len(lines), i + 25)]
        n += 1
        dump_event(len(events) + 1, line, before, after, events)
        existing = EVIDENCE.read_text(encoding="utf-8", errors="replace")
    return n


def load_events_from_evidence() -> list[dict]:
    """Rebuild summary rows from existing evidence file (survive collector restart)."""
    events: list[dict] = []
    if not EVIDENCE.exists():
        return events
    text = EVIDENCE.read_text(encoding="utf-8", errors="replace")
    for block in re.split(r"\n={10,}\n", text):
        if "EVENT #" not in block and "trigger:" not in block:
            continue
        wall = "-"
        marker = "-"
        m = re.search(r"wall=(\d{2}:\d{2}:\d{2})", block)
        if m:
            wall = m.group(1)
        m = re.search(r"marker=(\S+)", block)
        if m:
            marker = m.group(1)
        trig = ""
        m = re.search(r"trigger:\s*(.+)", block)
        if m:
            trig = m.group(1)
        fields = parse_reboot_fields(trig)
        fields.update({"wall": wall, "marker": marker})
        if "REBOOT_AFTER" in trig or fields.get("reason") not in ("?",):
            events.append(fields)
    return events


def main() -> None:
    events: list[dict] = load_events_from_evidence()
    if not EVIDENCE.exists():
        append(f"===== EVIDENCE START {datetime.now().isoformat(timespec='seconds')} =====")
        append(f"source={LOG}")
        append("")

    bf = backfill(events)
    append(f"[{datetime.now().strftime('%H:%M:%S')}] backfill_new={bf} total_events={len(events)}")
    write_summary(events)
    print(f"evidence={EVIDENCE} backfill={bf} events={len(events)}", flush=True)

    pos = LOG.stat().st_size if LOG.exists() else 0
    META.write_text(
        f"running=1\nstarted={time.time()}\npos={pos}\nevents={len(events)}\n",
        encoding="utf-8",
    )

    ring: list[str] = []
    RING = 100
    # After a REBOOT_AFTER, keep collecting a few more lines for PANIC_LAST.
    pending_after = 0
    pending_before: list[str] = []
    pending_trig = ""

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
                    if len(ring) > RING:
                        ring = ring[-RING:]

                    if pending_after > 0:
                        pending_after -= 1
                        if pending_after == 0:
                            dump_event(
                                len(events) + 1,
                                pending_trig,
                                pending_before,
                                ring[-30:],
                                events,
                            )
                            print(
                                f"EVENT#{len(events)} {events[-1].get('reason')} "
                                f"up={events[-1].get('prev_t')} phase={events[-1].get('prev_phase')}",
                                flush=True,
                            )
                        continue

                    if re.search(r"REBOOT_AFTER", line):
                        pending_trig = line
                        pending_before = list(ring[:-1])
                        pending_after = 20  # wait for PANIC_LAST / FW_MARKER
                    elif TRIGGER.search(line) and "REBOOT_AFTER" not in line:
                        # Guru/rst without immediate REBOOT_AFTER — still archive
                        dump_event(len(events) + 1, line, list(ring[:-1]), ring[-5:], events)
                        print(f"EVENT#{len(events)} soft-trigger", flush=True)

            META.write_text(
                f"running=1\nlast={time.time()}\npos={pos}\nevents={len(events)}\n",
                encoding="utf-8",
            )
            time.sleep(1.0)
        except Exception as e:
            append(f"[{datetime.now().strftime('%H:%M:%S')}] collector_err={e!r}")
            time.sleep(2.0)


if __name__ == "__main__":
    main()
