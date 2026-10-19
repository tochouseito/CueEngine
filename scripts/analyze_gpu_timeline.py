"""Frame ごとの CPU 提出と完了 GPU Query を生 Clock 値から照合する手動解析。"""

import argparse
import bisect
import csv
import json
import math
import re
from collections import defaultdict
from fractions import Fraction
from pathlib import Path


def project(tick, calibration, qpc_frequency):
    """巨大な絶対 GPU Tick を浮動小数点へ丸めず QPC へ写す。"""
    return Fraction(calibration['cpu_tick']) + Fraction(
        (tick - calibration['begin_tick']) * qpc_frequency, calibration['frequency'])


def analyze(prefix):
    text = Path(str(prefix) + '.log').read_text(encoding='utf-8-sig')
    clock_line = next(line for line in text.splitlines() if line.startswith('etw_clock '))
    clock = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', clock_line)}
    hz = clock['qpc_frequency']
    start_mid = Fraction(clock['start_qpc_before'] + clock['start_qpc_after'], 2)
    offset = clock['start_steady_ns'] - start_mid * 1_000_000_000 / hz
    origin = clock['measurement_start_ns']
    stop_mid = Fraction(clock['stop_qpc_before'] + clock['stop_qpc_after'], 2)
    stop_offset = clock['stop_steady_ns'] - stop_mid * 1_000_000_000 / hz

    def ns(qpc):
        return qpc * Fraction(1_000_000_000, hz) + offset - origin

    with Path(str(prefix) + '-gpu.csv').open(newline='', encoding='utf-8') as file:
        gpu = [{k: v if k == 'name' else int(v) for k, v in row.items()}
               for row in csv.DictReader(file)]
    with Path(str(prefix) + '-spans.csv').open(newline='', encoding='utf-8') as file:
        cpu = [{k: v if k == 'name' else int(v) for k, v in row.items()}
               for row in csv.DictReader(file)]
    calibrations = defaultdict(list)
    before = {}
    submissions = defaultdict(list)
    passes = defaultdict(list)
    invalid = []
    for row in gpu:
        key = (row['frame'], row['queue'])
        if row['status'] != 0:
            invalid.append(row)
            continue
        if row['kind'] == 0 and row['frequency']:
            calibrations[row['queue']].append(row)
            if row['name'] == 'BeforeSubmit':
                before.setdefault(key, []).append(row)
        elif row['kind'] == 1:
            submissions[key].append(row)
        elif row['kind'] == 2:
            passes[key].append(row)
    calibration_ticks = {}
    for queue, rows in calibrations.items():
        rows.sort(key=lambda r: r['cpu_tick'])
        ticks = [r['begin_tick'] for r in rows]
        if ticks != sorted(ticks):
            raise ValueError('GPU Clock が逆行した Queue は自動対応を中止する')
        calibration_ticks[queue] = ticks

    mapped = {}
    unavailable = []
    sensitivities = []
    for key, rows in passes.items():
        submitted = submissions.get(key, [])
        anchors = before.get(key, [])
        # 現在の単一 Queue Probe では一 Frame 一提出。複数 Batch を誤って結合しない
        if len(submitted) != 1 or len(anchors) != 1:
            unavailable.append({'frame': key[0], 'queue': key[1], 'reason': 'submission_count',
                                'submissions': len(submitted), 'passes': len(rows)})
            continue
        anchor = anchors[0]
        end_tick = max(row['end_tick'] for row in rows)
        pos = bisect.bisect_left(calibration_ticks[key[1]], end_tick)
        candidates = calibrations[key[1]]
        if pos == len(candidates):
            unavailable.append({'frame': key[0], 'queue': key[1], 'reason': 'no_later_calibration'})
            continue
        later = candidates[pos]
        if later['cpu_tick'] <= anchor['cpu_tick']:
            unavailable.append({'frame': key[0], 'queue': key[1], 'reason': 'no_clock_bracket'})
            continue
        converted = []
        for row in rows:
            if row['frequency'] != anchor['frequency'] or row['frequency'] != later['frequency']:
                raise ValueError('Queue の Timestamp 周波数が変化した')
            begin = project(row['begin_tick'], anchor, hz)
            end = project(row['end_tick'], anchor, hz)
            alternate_begin = project(row['begin_tick'], later, hz)
            alternate_end = project(row['end_tick'], later, hz)
            sensitivity = max(abs(begin - alternate_begin), abs(end - alternate_end)) * 1_000_000_000 / hz
            sensitivities.append(float(sensitivity))
            converted.append({'name': row['name'], 'index': row['detail'],
                              'begin_ns': float(ns(begin)), 'end_ns': float(ns(end)),
                              'duration_ns': float(Fraction((row['end_tick'] - row['begin_tick']) *
                                                           1_000_000_000, row['frequency'])),
                              'calibration_sensitivity_ns': float(sensitivity)})
        first = min(row['begin_ns'] for row in converted)
        last = max(row['end_ns'] for row in converted)
        submission = submitted[0]
        mapped[key] = {'frame': key[0], 'queue': key[1], 'fence': submission['detail'],
                       'submit_begin_ns': float(ns(submission['qpc_before'])),
                       'submit_end_ns': float(ns(submission['qpc_after'])),
                       'first_gpu_ns': first, 'last_gpu_ns': last,
                       'gpu_envelope_ns': last - first, 'passes': converted,
                       'calibration_sensitivity_ns': max(p['calibration_sensitivity_ns'] for p in converted)}
    render = [row for row in cpu if row['name'] == 'Frame.Render']
    by_frame = defaultdict(list)
    for row in cpu:
        by_frame[row['frame']].append(row)
    mapped_by_frame = defaultdict(list)
    by_fence = defaultdict(list)
    ordered_submissions = sorted(mapped.values(), key=lambda r: r['submit_end_ns'])
    submit_ends = [r['submit_end_ns'] for r in ordered_submissions]
    for row in mapped.values():
        mapped_by_frame[row['frame']].append(row)
        by_fence[row['fence']].append(row)

    def stage_context(row):
        stage = dict(row)
        if row['name'].startswith('Fence.Event'):
            # CPU Span に Queue ID がない旧形式は候補を列挙し、一意と仮定しない
            stage['fence_candidates'] = by_fence.get(row['detail'], [])
        elif row['name'] == 'DXGI.Event':
            index = bisect.bisect_right(submit_ends, row['begin_ns']) - 1
            if index >= 0:
                stage['latest_prior_submission'] = ordered_submissions[index]
                stage['prior_last_gpu_to_wait_return_ns'] = row['end_ns'] - ordered_submissions[index]['last_gpu_ns']
        return stage

    def statistics(values):
        ordered = sorted(values)
        if not ordered:
            return {'count': 0}
        return {'count': len(ordered), 'average_ns': sum(ordered) / len(ordered),
                'p95_ns': ordered[math.ceil(len(ordered) * .95) - 1], 'max_ns': ordered[-1]}

    top = []
    for row in sorted(render, key=lambda r: r['duration_ns'], reverse=True)[:15]:
        top.append({'frame': row['frame'], 'render': row,
                    'stages': [stage_context(r) for r in by_frame[row['frame']]
                               if r['name'] in ('DXGI.Event', 'DXGI.Present', 'Fence.Event', 'Fence.Event1ms')],
                    'gpu': mapped_by_frame[row['frame']]})
    frame_timings = sorted(mapped.values(), key=lambda r: r['gpu_envelope_ns'], reverse=True)
    result = {'quality': {'raw_gpu_records': len(gpu), 'invalid_gpu_records': len(invalid),
                          'calibrations': sum(map(len, calibrations.values())),
                          'submissions': sum(map(len, submissions.values())),
                          'raw_passes': sum(map(len, passes.values())), 'mapped_frames': len(mapped),
                          'mapped_passes': sum(len(row['passes']) for row in mapped.values()),
                          'unavailable': unavailable, 'clock_offset_drift_ns': float(stop_offset - offset),
                          'max_calibration_sensitivity_ns': max(sensitivities, default=None),
                          'clock_start_bracket_ns': (clock['start_qpc_after'] - clock['start_qpc_before']) * 1e9 / hz,
                          'clock_stop_bracket_ns': (clock['stop_qpc_after'] - clock['stop_qpc_before']) * 1e9 / hz},
              'statistics': {'render': statistics([r['duration_ns'] for r in render]),
                             'gpu_envelope': statistics([r['gpu_envelope_ns'] for r in mapped.values()]),
                             'calibration_sensitivity': statistics(sensitivities),
                             'submit_to_first_gpu_from_begin': statistics(
                                 [r['first_gpu_ns'] - r['submit_begin_ns'] for r in mapped.values()]),
                             'submit_to_first_gpu_from_end': statistics(
                                 [r['first_gpu_ns'] - r['submit_end_ns'] for r in mapped.values()])},
              'top_render': top, 'top_gpu_envelope': frame_timings[:15], 'invalid': invalid}
    output = Path(str(prefix) + '-gpu-analysis.json')
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    # 全 Frame の変換結果も残し、上位 Frame だけの記録にしない
    with Path(str(prefix) + '-gpu-frames.jsonl').open('w', encoding='utf-8') as file:
        for row in mapped.values():
            file.write(json.dumps(row, ensure_ascii=False) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    args = parser.parse_args()
    result = analyze(args.prefix)
    print(json.dumps(result['quality'], ensure_ascii=False, indent=2))
    for row in result['top_render'][:5]:
        print('frame', row['frame'], 'render_ms', row['render']['duration_ns'] / 1e6,
              'gpu_envelope_ms', [r['gpu_envelope_ns'] / 1e6 for r in row['gpu']])
