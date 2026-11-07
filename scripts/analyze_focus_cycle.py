"""前面切替の要求・実測を検証し、全生記録を安定区間と切替区間へ分ける手動解析。"""

import argparse
import csv
import json
import math
import re
from pathlib import Path


def statistics(values):
    """全値から nearest-rank p95 と最大を求める。空区間は未測定として返す。"""
    ordered = sorted(values)
    if not ordered:
        return {'count': 0}
    return {'count': len(ordered), 'average_ms': sum(ordered) / len(ordered) / 1e6,
            'p95_ms': ordered[math.ceil(len(ordered) * .95) - 1] / 1e6,
            'max_ms': ordered[-1] / 1e6}


def stable_phase(begin, end):
    """各 4 秒の先頭・末尾 250 ms を比較対象から分離し、跨ぐ区間も切替側へ残す。"""
    phase = int(begin // 4_000_000_000)
    if 0 <= phase < 5 and begin >= phase * 4_000_000_000 + 250_000_000 and end <= (phase + 1) * 4_000_000_000 - 250_000_000:
        return phase
    return None


def validate_window_shape(spans):
    """前面 Bit と分けて、可視・非最小化・要求 Client Size の全観測変化を検証する。"""
    states = [s['detail'] for s in spans if s['name'] == 'Probe.WindowState']
    if not states:
        return ['missing_window_shape']
    return ['window_shape'] if any(state & 14 != 10 for state in states) else []


def validate_focus(rows):
    """要求成功だけで条件成立とせず、各安定区間中の実測 PID と照合する。"""
    errors = []
    requests = [r for r in rows if r['kind'] == 'request']
    observations = sorted([r for r in rows if r['kind'] == 'observed'], key=lambda r: r['time_ns'])
    if [r['phase'] for r in requests] != list(range(5)):
        errors.append('phase_requests')
        return errors
    if len({r['expected_pid'] for r in requests[::2]}) != 1 or len({r['expected_pid'] for r in requests[1::2]}) != 1:
        errors.append('expected_process_changed')
    if requests[0]['expected_pid'] == requests[1]['expected_pid']:
        errors.append('same_process')
    for request in requests:
        phase = request['phase']
        begin, end = phase * 4_000_000_000 + 250_000_000, (phase + 1) * 4_000_000_000 - 250_000_000
        if not request['success'] or not phase * 4_000_000_000 <= request['time_ns'] < begin:
            errors.append(f'phase_{phase}_request')
        prior = [r for r in observations if r['time_ns'] <= begin]
        active = prior[-1:] + [r for r in observations if begin < r['time_ns'] <= end]
        if not prior or any(r['foreground_pid'] != request['expected_pid'] for r in active):
            errors.append(f'phase_{phase}_observed_process')
    return errors


def analyze(prefix):
    """生 CSV と全 GPU Frame の対応済み結果を用い、前面比較の成立とデータ品質を分ける。"""
    with Path(str(prefix) + '-focus.csv').open(newline='', encoding='utf-8') as source:
        focus = [{k: v if k == 'kind' else int(v) for k, v in row.items()} for row in csv.DictReader(source)]
    focus_errors = validate_focus(focus)
    with Path(str(prefix) + '-spans.csv').open(newline='', encoding='utf-8') as source:
        spans = [{k: v if k == 'name' else int(v) for k, v in row.items()} for row in csv.DictReader(source)]
    with Path(str(prefix) + '-gpu-frames.jsonl').open(encoding='utf-8') as source:
        gpu = [json.loads(line) for line in source]
    render = [r for r in spans if r['name'] == 'Frame.Render']
    gpu_by_frame = {r['frame']: r for r in gpu}
    states_path = Path(str(prefix) + '-cpu-states.csv')
    states = {}
    if states_path.exists():
        with states_path.open(newline='', encoding='utf-8') as source:
            states = {int(r['frame']): r for r in csv.DictReader(source) if r['name'] == 'Frame.Render'}
    groups = []
    for phase in range(5):
        cpu_rows = [r for r in render if stable_phase(r['begin_ns'], r['end_ns']) == phase]
        gpu_rows = [r for r in gpu if stable_phase(r['submit_begin_ns'], r['last_gpu_ns']) == phase]
        groups.append({'phase': phase, 'foreground': phase % 2 == 0,
                       'stable_begin_ns': phase * 4_000_000_000 + 250_000_000,
                       'stable_end_ns': (phase + 1) * 4_000_000_000 - 250_000_000,
                       'render': statistics([r['duration_ns'] for r in cpu_rows]),
                       'gpu_envelope': statistics([r['gpu_envelope_ns'] for r in gpu_rows]),
                       'submit_to_first_gpu': statistics([r['first_gpu_ns'] - r['submit_end_ns'] for r in gpu_rows]),
                       'dxgi_wait': statistics([r['duration_ns'] for r in spans if r['name'] == 'DXGI.Event' and stable_phase(r['begin_ns'], r['end_ns']) == phase]),
                       'fence_wait': statistics([r['duration_ns'] for r in spans if r['name'].startswith('Fence.Event') and stable_phase(r['begin_ns'], r['end_ns']) == phase]),
                       'top_render': [{'cpu': r, 'gpu': gpu_by_frame.get(r['frame']), 'states': states.get(r['frame'])}
                                      for r in sorted(cpu_rows, key=lambda r: r['duration_ns'], reverse=True)[:5]]})
    log = Path(str(prefix) + '.log').read_text(encoding='utf-8-sig')
    errors = validate_window_shape(spans)
    if not re.search(r'trace_spans=\d+ dropped=0\b', log) or not re.search(r'gpu_records=\d+ dropped=0 overwritten=0\b', log):
        errors.append('raw_loss_or_overwrite')
    if not re.search(r'focus_cycle .*dropped=0 stopped=1 failed_requests=0', log):
        errors.append('focus_controller_status')
    done = json.loads((prefix.parent / 'Done.json').read_text(encoding='utf-8-sig'))
    if done.get('Error') or not all(done.get(k) for k in ('Merged', 'KernelStopped', 'UserStopped')):
        errors.append('collector_status')
    if any(g['render']['count'] == 0 or g['gpu_envelope']['count'] == 0 for g in groups):
        errors.append('missing_phase_data')
    scheduling_path = prefix.parent / 'CpuScheduling.json'
    scheduling = json.loads(scheduling_path.read_text(encoding='utf-8')) if scheduling_path.exists() else []
    if not any(r.get('name') == prefix.name and r.get('spans') == len(spans) for r in scheduling) or not states:
        errors.append('cpu_marker_validation')
    gpu_quality = json.loads(Path(str(prefix) + '-gpu-analysis.json').read_text(encoding='utf-8'))['quality']
    if gpu_quality['invalid_gpu_records'] or gpu_quality['mapped_frames'] != gpu_quality['submissions']:
        errors.append('gpu_mapping')
    export_log = prefix.parent / 'Export.log'
    if not export_log.exists() or not re.search(r'errors=0.*events_lost=0 buffers_lost=0', export_log.read_text(encoding='utf-8-sig')):
        errors.append('etw_export_quality')
    transition = [r for r in render if stable_phase(r['begin_ns'], r['end_ns']) is None]
    result = {'focus_condition_succeeded': not focus_errors, 'focus_errors': focus_errors,
              'quality_errors': errors, 'succeeded': not focus_errors and not errors,
              'gpu_quality': gpu_quality,
              'note': 'CPU Marker 全件照合は CpuScheduling.json と元の解析ログも保持する',
              'focus': focus, 'phases': groups,
              'all_render': statistics([r['duration_ns'] for r in render]),
              'transition_render': statistics([r['duration_ns'] for r in transition]),
              'top_all_render': [{'cpu': r, 'gpu': gpu_by_frame.get(r['frame']), 'states': states.get(r['frame'])}
                                 for r in sorted(render, key=lambda r: r['duration_ns'], reverse=True)[:10]]}
    Path(str(prefix) + '-focus-analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    result = analyze(parser.parse_args().prefix)
    print('succeeded', result['succeeded'], 'focus_errors', result['focus_errors'], 'quality_errors', result['quality_errors'])
    for phase in result['phases']:
        print(phase['phase'], 'foreground', phase['foreground'], 'render', phase['render'], 'gpu_lead', phase['submit_to_first_gpu'])
    raise SystemExit(0 if result['succeeded'] else 1)
