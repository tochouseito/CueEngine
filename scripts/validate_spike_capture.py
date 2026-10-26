"""Trigger 前後の保持範囲と生 CSV の連続性を検証する。古い履歴の上書きは欠落と区別する。"""

import argparse
import csv
import json
import re
from fractions import Fraction
from pathlib import Path


def validate(folder):
    prefix = folder / 'on-q2'
    log = prefix.with_suffix('.log').read_text(encoding='utf-8-sig')
    trigger_line = next(s for s in log.splitlines() if s.startswith('trigger '))
    trigger = {k: int(v) for k, v in re.findall(r'(kind|frame|duration_ns|at_ns)=(-?\d+)', trigger_line)}
    if trigger['kind'] == 0:
        result = {'trigger': trigger, 'status': 'not_triggered', 'succeeded': False}
        (folder / 'Coverage.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        return result
    clock_line = next(s for s in log.splitlines() if s.startswith('etw_clock '))
    clock = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', clock_line)}
    hz = clock['qpc_frequency']
    sync = Fraction(clock['start_qpc_before'] + clock['start_qpc_after'], 2)

    def relative_ns(qpc):
        return float((Fraction(qpc) - sync) * 1_000_000_000 / hz +
                     clock['start_steady_ns'] - clock['measurement_start_ns'])

    sequences = {}
    cpu = {}
    event_start = trigger['at_ns']
    with Path(str(prefix) + '-spans.csv').open(newline='', encoding='utf-8') as file:
        for row in csv.DictReader(file):
            tid, seq = int(row['thread']), int(row['source_sequence'])
            if tid in sequences and seq != sequences[tid] + 1:
                raise ValueError('保持範囲の CPU 連番に欠落がある')
            sequences[tid] = seq
            bounds = cpu.setdefault(tid, [int(row['begin_ns']), int(row['end_ns']), seq, seq])
            bounds[0] = min(bounds[0], int(row['begin_ns']))
            bounds[1] = max(bounds[1], int(row['end_ns']))
            bounds[3] = seq
            if trigger['kind'] == 1 and row['name'] == 'Frame.Render' and int(row['frame']) == trigger['frame']:
                event_start = int(row['begin_ns'])
    if trigger['kind'] == 2:
        # GPU の開始位置は Offline の整数 Tick 変換を使い、検出時刻と区別する
        frames = [json.loads(line) for line in Path(str(prefix) + '-gpu-frames.jsonl').read_text().splitlines()]
        matches = [r for r in frames if r['frame'] == trigger['frame']]
        if len(matches) != 1:
            raise ValueError('Trigger の GPU Frame を一意に照合できない')
        event_start = min(matches[0]['submit_begin_ns'], matches[0]['first_gpu_ns'])
    desired_begin = event_start - 5_000_000_000
    # Provider の Event 周期を考慮し、末端 50 ms の有無も数値として残す
    desired_end = trigger['at_ns'] + 1_950_000_000
    ranges = {}
    kernel = {}
    kernel_cpus = {}
    with (folder / 'Events.csv').open(newline='', encoding='utf-8') as file:
        for row in csv.DictReader(file):
            if row['kind'] == 'ProviderRange':
                ranges[row['name']] = [relative_ns(int(row['qpc'])), relative_ns(int(row['detail'])),
                                       int(row['sequence'])]
            elif row['kind'] == 'CSwitch':
                at = relative_ns(int(row['qpc']))
                bounds = kernel_cpus.setdefault(int(row.get('cpu', 0)), [at, at])
                bounds[0], bounds[1] = min(bounds[0], at), max(bounds[1], at)
                for tid in (int(row['old_tid']), int(row['new_tid'])):
                    if tid in cpu:
                        bounds = kernel.setdefault(tid, [at, at])
                        bounds[0], bounds[1] = min(bounds[0], at), max(bounds[1], at)
    checks = {}
    checks['kernel_cpu_history'] = bool(kernel_cpus) and max(b[0] for b in kernel_cpus.values()) <= desired_begin
    for name in ('DxgKrnl', 'DXGI', 'DWM'):
        bounds = ranges.get(name)
        checks[name] = bool(bounds and bounds[0] <= desired_begin and bounds[1] >= desired_end)
    for tid, bounds in cpu.items():
        # 測定途中に生まれた Thread を無理に過去へ延ばさない。今回の Main / Render は開始時から存在する
        checks[f'cpu_{tid}'] = bounds[0] <= desired_begin and bounds[1] >= desired_end
        native = kernel.get(tid)
        checks[f'kernel_{tid}'] = bool(native and native[0] <= desired_begin and native[1] >= desired_end)
    done = json.loads((folder / 'Done.json').read_text(encoding='utf-8-sig'))
    checks['collector'] = all(done.get(k) for k in ('KernelSaved', 'UserSaved', 'KernelStopped',
                                                   'UserStopped', 'MarkersStopped', 'Merged')) and not done['Error']
    checks['foreground'] = bool(re.search(r'window_checks=\d+ mismatches=0 changes=0 final_state=11', log))
    checks['raw_cpu_loss'] = bool(re.search(r'trace_spans=\d+ dropped=0', log))
    checks['raw_gpu_loss'] = bool(re.search(r'gpu_records=\d+ dropped=0', log))
    export = (folder / 'Export.log').read_text(encoding='utf-8-sig')
    checks['etw_loss'] = all(token in export for token in ('process_trace=0 ', 'decode_errors=0 ',
                                                          'events_lost=0 ', 'buffers_lost=0 '))
    coverage_succeeded = all(v for k, v in checks.items() if k != 'foreground')
    result = {'trigger': trigger, 'event_start_ns': event_start, 'desired_begin_ns': desired_begin,
              'desired_end_ns': desired_end, 'provider_ranges_ns': ranges, 'cpu_ranges_ns': cpu,
              'kernel_ranges_ns': kernel, 'kernel_cpu_ranges_ns': kernel_cpus, 'checks': checks,
              'data_coverage_succeeded': coverage_succeeded,
              'status': 'valid' if all(checks.values()) else ('condition_changed' if coverage_succeeded else 'incomplete'),
              'succeeded': all(checks.values())}
    (folder / 'Coverage.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    result = validate(parser.parse_args().folder)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(0 if result['succeeded'] else (2 if result.get('status') == 'not_triggered' else 1))
