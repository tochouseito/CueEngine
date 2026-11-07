"""PresentMon v1/QPC 出力と生の Present API 区間を一意に照合する手動解析。"""

import argparse
import bisect
import csv
import json
import re
from collections import Counter
from fractions import Fraction
from pathlib import Path


def match_present(begin, end, times, uncertainty):
    """時刻幅内に一件だけある場合に採用し、最も近い別 Frame への推定対応をしない。"""
    first = bisect.bisect_left(times, begin - uncertainty)
    last = bisect.bisect_right(times, end + uncertainty)
    return first if last - first == 1 else None


def analyze(prefix, presentmon, output):
    """ClockSync の生値を用い、Warmup と未記録境界を Frame 対応済み結果へ混ぜない。"""
    log = Path(str(prefix) + '.log').read_text(encoding='utf-8-sig')
    clock = {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', next(
        line for line in log.splitlines() if line.startswith('etw_clock')))}
    pid = int(re.search(r'^pid=(\d+)', log, re.M)[1])
    frequency = clock['qpc_frequency']
    if frequency <= 0:
        raise ValueError('QPC Frequency が不正')
    offset = Fraction(clock['start_qpc_before'] + clock['start_qpc_after'], 2) - Fraction(
        clock['start_steady_ns'] * frequency, 10**9)
    stop_offset = Fraction(clock['stop_qpc_before'] + clock['stop_qpc_after'], 2) - Fraction(
        clock['stop_steady_ns'] * frequency, 10**9)
    uncertainty = Fraction(max(clock['start_qpc_after'] - clock['start_qpc_before'],
                               clock['stop_qpc_after'] - clock['stop_qpc_before']), 2)
    with presentmon.open(encoding='utf-8-sig', newline='') as source:
        presents = list(csv.DictReader(source))
    if not presents or any(int(row['ProcessID']) != pid for row in presents):
        raise ValueError('対象 Process の PresentMon 出力ではない')
    if len({row['SwapChainAddress'] for row in presents}) != 1:
        raise ValueError('この解析は単一 SwapChain の Probe に限定する')
    times = [int(row['QPCTime']) for row in presents]
    if times != sorted(times) or len(set(times)) != len(times):
        raise ValueError('Present の QPC が逆行または重複')
    spans = []
    with Path(str(prefix) + '-spans.csv').open(encoding='utf-8', newline='') as source:
        for row in csv.DictReader(source):
            if row['name'] in {'DXGI.Present', 'Frame.Render'}:
                spans.append({key: value if key == 'name' else int(value) for key, value in row.items()})
    mapped, failed, used = [], [], set()
    for span in spans:
        if span['name'] != 'DXGI.Present':
            continue
        begin = Fraction((span['begin_ns'] + clock['measurement_start_ns']) * frequency, 10**9) + offset
        end = Fraction((span['end_ns'] + clock['measurement_start_ns']) * frequency, 10**9) + offset
        index = match_present(begin, end, times, uncertainty)
        if index is None or index in used:
            failed.append({'span': span, 'reason': 'missing_ambiguous_or_reused_present'})
            continue
        used.add(index)
        row = dict(presents[index])
        row.update(frame=span['frame'], thread=span['thread'], begin_ns=span['begin_ns'], end_ns=span['end_ns'],
                   qpc=times[index], time_ns=float((Fraction(times[index]) - offset) * 10**9 / frequency - clock['measurement_start_ns']))
        mapped.append(row)
    mapped.sort(key=lambda row: row['qpc'])
    transitions = []
    previous = None
    for row in mapped:
        if row['PresentMode'] != previous:
            transitions.append(row)
            previous = row['PresentMode']
    by_frame = {row['frame']: row for row in mapped}
    renders = sorted([s for s in spans if s['name'] == 'Frame.Render'], key=lambda s: s['duration_ns'], reverse=True)
    result = {'pid': pid, 'presentmon_rows': len(presents), 'recorded_present_spans': len(mapped) + len(failed),
              'mapped': len(mapped), 'unassigned_presentmon_rows': len(presents) - len(used), 'failed': failed,
              'qpc_bracket_ticks': float(uncertainty * 2), 'clock_offset_drift_ns': float((stop_offset - offset) * 10**9 / frequency),
              'mode_counts': dict(Counter(row['PresentMode'] for row in mapped)), 'transitions': transitions,
              'top_render': [{'cpu': row, 'present': by_frame.get(row['frame'])} for row in renders[:10]],
              'succeeded': bool(mapped) and not failed}
    Path(str(output) + '-modes.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    with Path(str(output) + '-frames.jsonl').open('w', encoding='utf-8') as target:
        for row in mapped:
            target.write(json.dumps(row, ensure_ascii=False) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('presentmon', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = analyze(args.prefix, args.presentmon, args.output)
    print('mapped', result['mapped'], 'failed', len(result['failed']), 'modes', result['mode_counts'])
    raise SystemExit(0 if result['succeeded'] else 1)
