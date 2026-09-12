"""Observe production face logs without running a replacement policy model.

Counts transaction *begins*, not DMA completion. Silence never proves safety.
Historical log tokens are an explicit supported dialect, not a universal ABI.
"""
import argparse
import json
from pathlib import Path
import re

GEN = re.compile(r'ScreenPresenter: s1es speech_generation=(\d+)\b')
BEGIN = re.compile(r'FaceRouteV2: s1cn-c begin why=(\w+)\b')
FIRST = re.compile(r'ReminderTrace: LC \| owner=User phase=tts_audio_first ok=1\b')
SEED = re.compile(r'EezuiDisplayAdapter: SAD_DIAG lvgl_face seed emo=(\w+) full=(\d+)x(\d+)\b')
COST = re.compile(r'ScreenPresenter: SAD_DIAG PRESENT play_done emo=(\w+) cost_ms=(\d+)\b')
RESET = re.compile(r'^rst:0x[0-9a-f]+\b', re.I)


def observe(text, *, max_presenter_ms):
    if max_presenter_ms <= 0:
        raise ValueError('explicit positive presenter budget required')
    windows, current = [], None
    for number, raw in enumerate(text.split('\n'), 1):
        line = raw.strip('\r')
        match = GEN.search(line)
        if match:
            current = {'generation': int(match[1]), 'line': number,
                       'begins': 0, 'begins_after_audio': 0, 'full_seed_records': 0,
                       'audio_first_observed': False, 'max_presenter_ms': 0,
                       'reset_observed': False, 'evidence_lines': []}
            windows.append(current)
            continue
        if current is None:
            continue
        recognized = False
        if FIRST.search(line):
            current['audio_first_observed'] = True
            recognized = True
        if BEGIN.search(line):
            current['begins'] += 1
            current['begins_after_audio'] += int(current['audio_first_observed'])
            recognized = True
        match = SEED.search(line)
        if match:
            current['full_seed_records'] += int(int(match[2]) >= 480 and int(match[3]) >= 480)
            recognized = True
        match = COST.search(line)
        if match:
            current['max_presenter_ms'] = max(current['max_presenter_ms'], int(match[2]))
            recognized = True
        if recognized:
            current['evidence_lines'].append(number)
        if RESET.search(line):
            current['reset_observed'] = True
            current = None
    violations = []
    for index, window in enumerate(windows):
        reasons = []
        if window['begins'] > 1:
            reasons.append('multiple_transaction_begins')
        if window['begins_after_audio']:
            reasons.append('transaction_begin_after_audio_first')
        if window['max_presenter_ms'] > max_presenter_ms:
            reasons.append('presenter_budget_exceeded')
        if reasons:
            violations.append({'window': index, 'generation': window['generation'],
                               'reasons': reasons})
    return {'status': 'OBSERVED_VIOLATION' if violations else 'INSUFFICIENT_FOR_PASS',
            'scope': 's1es/s1cn-c production log observations only',
            'max_presenter_ms': max_presenter_ms, 'windows': windows,
            'violations': violations, 'completion_or_cause_proven': False,
            'runtime_fix_proven': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--max-presenter-ms', type=int, required=True)
    args = parser.parse_args()
    if args.log.stat().st_size > 4 * 1024 * 1024:
        parser.error('use a bounded, independently identified capture <=4MiB')
    result = observe(args.log.read_bytes().decode('utf-8', errors='replace'),
                     max_presenter_ms=args.max_presenter_ms)
    print(json.dumps(result, indent=2))
    return 1 if result['violations'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
