"""One-shot audit over serial_monitor.log: per-marker lifetime + panic fingerprints.

Read-only. Groups every REBOOT_AFTER/rst by the FW_MARKER that was live at the
time, so regressions are judged by mean lifetime per marker (s1bz-audit method)
rather than by the last few crashes.
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOG = ROOT / "serial_monitor.log"

RE_MARKER = re.compile(r"FW_MARKER[^\w]*([\w.]+)")
RE_REBOOT = re.compile(r"REBOOT_AFTER\s*\|?\s*(.*)")
RE_RST = re.compile(r"rst:0x([0-9a-fA-F]+)\s*\(([^)]*)\)")
RE_PREV_T = re.compile(r"prev_t=(\d+)")
RE_PREV_PH = re.compile(r"prev_phase=(\S+)")
RE_REASON = re.compile(r"reason=(\S+)")
RE_MEPC = re.compile(r"mepc=(0x[0-9a-fA-F]+)")
RE_RA = re.compile(r"\bra=(0x[0-9a-fA-F]+)")
RE_MTVAL = re.compile(r"mtval=(0x[0-9a-fA-F]+)")
RE_MCAUSE = re.compile(r"mcause=(\d+)")
RE_SP = re.compile(r"\bsp=(0x[0-9a-fA-F]+)")
RE_CACHE = re.compile(r"cache_on=(\d)")
RE_FLASHOP = re.compile(r"flashop_depth=(\d+)")


def main() -> None:
    marker = "?"
    per_marker_life: dict[str, list[int]] = defaultdict(list)
    per_marker_boots: dict[str, int] = defaultdict(int)
    events: list[dict] = []
    panics: list[dict] = []
    cur: dict | None = None
    tail_ctx: list[str] = []

    with LOG.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "FW_MARKER" in line:
                m = RE_MARKER.search(line)
                if m:
                    marker = m.group(1)
                    per_marker_boots[marker] += 1
            if "REBOOT_AFTER" in line:
                ev = {"marker": marker, "line": line.strip()}
                t = RE_PREV_T.search(line)
                if t:
                    ev["prev_t"] = int(t.group(1))
                    per_marker_life[marker].append(int(t.group(1)))
                r = RE_REASON.search(line)
                ev["reason"] = r.group(1) if r else "?"
                p = RE_PREV_PH.search(line)
                ev["prev_phase"] = p.group(1) if p else "?"
                events.append(ev)
            if "PANIC_LAST" in line and "none" not in line:
                if RE_MEPC.search(line) or RE_MCAUSE.search(line):
                    d = {"marker": marker}
                    for key, rx in (
                        ("mcause", RE_MCAUSE),
                        ("mepc", RE_MEPC),
                        ("ra", RE_RA),
                        ("mtval", RE_MTVAL),
                        ("sp", RE_SP),
                        ("cache_on", RE_CACHE),
                        ("flashop", RE_FLASHOP),
                    ):
                        mm = rx.search(line)
                        if mm:
                            d[key] = mm.group(1)
                    if cur != d:
                        panics.append(d)
                        cur = d
            tail_ctx.append(line)
            if len(tail_ctx) > 400:
                tail_ctx.pop(0)

    print("=== per-marker lifetime (mean/min/max seconds, n crashes, boots) ===")
    for mk in sorted(per_marker_life, key=lambda k: -len(per_marker_life[k])):
        v = per_marker_life[mk]
        print(
            f"{mk:38s} n={len(v):4d} boots={per_marker_boots.get(mk,0):4d} "
            f"mean={sum(v)/len(v):8.1f} min={min(v):6d} max={max(v):7d}"
        )
    print(f"\ntotal REBOOT_AFTER={len(events)}")

    print("\n=== reason x prev_phase (all) ===")
    combo: dict[tuple[str, str], int] = defaultdict(int)
    for e in events:
        combo[(e["reason"], e["prev_phase"])] += 1
    for (rsn, ph), n in sorted(combo.items(), key=lambda kv: -kv[1])[:25]:
        print(f"{n:5d}  reason={rsn:22s} prev_phase={ph}")

    print("\n=== last 25 REBOOT_AFTER ===")
    for e in events[-25:]:
        print(
            f"[{e['marker']}] prev_t={e.get('prev_t','?')}s reason={e['reason']} "
            f"prev_phase={e['prev_phase']}"
        )

    print(f"\n=== distinct PANIC_LAST frames (n={len(panics)}), last 30 ===")
    for d in panics[-30:]:
        print(
            f"[{d.get('marker','?')}] mcause={d.get('mcause','?')} "
            f"mepc={d.get('mepc','?')} ra={d.get('ra','?')} mtval={d.get('mtval','?')} "
            f"sp={d.get('sp','?')} cache_on={d.get('cache_on','?')} flashop={d.get('flashop','?')}"
        )

    print("\n=== mtval region histogram (all panics) ===")
    reg: dict[str, int] = defaultdict(int)
    for d in panics:
        mv = d.get("mtval")
        if not mv:
            continue
        v = int(mv, 16)
        if 0x48000000 <= v < 0x4C000000:
            reg["PSRAM 0x48-0x4B"] += 1
        elif 0x4FF00000 <= v < 0x50000000:
            reg["internal SRAM 0x4FFx"] += 1
        elif v < 0x1000:
            reg["null-ish <0x1000"] += 1
        elif v >= 0xF0000000:
            reg["0xFxxxxxxx"] += 1
        else:
            reg[f"other {mv[:6]}xxxx"] += 1
    for k, n in sorted(reg.items(), key=lambda kv: -kv[1]):
        print(f"{n:5d}  {k}")

    if "--tail" in sys.argv:
        print("\n=== last boot context (grep) ===")
        keep = re.compile(
            r"FW_MARKER|BOOT \||REBOOT_AFTER|PANIC_LAST|CACHE |rst:0x|Guru|"
            r"SAFE_MODE|Saved PC|HEAP_INTEGRITY|SD_MOUNT|seed_stills|FACE_ASSET",
            re.I,
        )
        for line in tail_ctx:
            if keep.search(line):
                print(line.rstrip())


if __name__ == "__main__":
    main()
