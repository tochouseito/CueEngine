"""Frame 区間と CSwitch / ReadyThread を QPC で照合する。元 ETL と生 CSV は保持する。"""

import argparse
import bisect
import csv
import json
import re
from collections import Counter, defaultdict
from fractions import Fraction
from pathlib import Path


def read_markers(path):
    """独自 Provider を Process ごとに分け、別起動の Frame ID を混ぜない。"""
    markers = defaultdict(list)
    clocks = defaultdict(list)
    with path.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            if None in row:
                raise ValueError("ETW CSV の列数が不正")
            if row["kind"] not in {"TimingSpan", "ClockSync"}:
                continue
            event = {key: value if key in {"kind", "name"} else int(value) for key, value in row.items()}
            (markers if row["kind"] == "TimingSpan" else clocks)[event["pid"]].append(event)
    return markers, clocks


def build_states(events, threads):
    """観測した状態だけで区間を作る。未観測の状態を Running や 0 ms と推定しない。"""
    current = {}
    result = defaultdict(list)

    def transition(thread, at, state, reason=0):
        previous = current.get(thread)
        if previous and at > previous[0]:
            # Ready 通知が観測できなければ Waiting と Ready の境界を推定しない
            previous_state = "Unknown" if state == "Running" and previous[1] == "Waiting" else previous[1]
            result[thread].append((previous[0], at, previous_state, previous[2]))
        current[thread] = (at, state, reason)

    last = 0
    for event in sorted(events, key=lambda value: value["qpc"]):
        at = event["qpc"]
        last = at
        if event["kind"] == "CSwitch":
            old, new = event["old_tid"], event["new_tid"]
            if old in threads:
                state = {1: "Ready", 3: "Ready", 7: "Ready", 5: "Waiting", 4: "Ended"}.get(event["old_state"], "Unknown")
                transition(old, at, state, event["wait_reason"])
            if new in threads:
                transition(new, at, "Running")
        elif event["ready_tid"] in threads:
            target = event["ready_tid"]
            previous = current.get(target)
            # Ready の再通知で最初の Ready 時刻を後ろへずらさない
            if not previous or previous[1] not in {"Ready", "Running"}:
                transition(target, at, "Ready", previous[2] if previous else 0)
    for thread, previous in current.items():
        if last > previous[0]:
            result[thread].append((previous[0], last, previous[1], previous[2]))
    return result


def duration_by_state(intervals, begin, end, ends=None):
    """境界を切り詰めて重複なく積算し、観測範囲外を Unknown として残す。"""
    totals = defaultdict(Fraction)
    if ends is None:
        ends = [interval[1] for interval in intervals]
    index = bisect.bisect_right(ends, begin)
    # islice(list, index, ...) は先頭から index 件を走査するため、添字から直接再開する
    while index < len(intervals):
        left, right, state, reason = intervals[index]
        if left >= end:
            break
        overlap = min(end, right) - max(begin, left)
        if overlap > 0:
            totals[state] += overlap
        index += 1
    uncovered = end - begin - sum(totals.values())
    if uncovered < 0:
        raise ValueError("Scheduling 区間が重複")
    totals["Unknown"] += uncovered
    return totals


def validate_csv(folder, markers):
    """ETW と間引いていない生 CSV の全区間・連番を照合し、欠落を検出する。"""
    cases = {}
    for path in folder.glob("*-spans.csv"):
        log = path.with_name(path.name.removesuffix("-spans.csv") + ".log").read_text(encoding="utf-8-sig")
        matched = re.search(r"\bpid=(\d+)\s+buffers=", log)
        if not matched:
            raise ValueError(f"Process ID がない: {path}")
        pid = int(matched[1])
        lookup = {(event["tid"], event["sequence"]): event for event in markers[pid]}
        if len(lookup) != len(markers[pid]):
            raise ValueError("ETW の Thread 連番が重複")
        sequences = Counter()
        offset = None
        count = 0
        with path.open(encoding="utf-8", newline="") as source:
            for row in csv.DictReader(source):
                tid = int(row["thread"])
                key = tid, sequences[tid]
                sequences[tid] += 1
                event = lookup.get(key)
                if not event or event["name"] != row["name"] or event["frame"] != int(row["frame"]):
                    raise ValueError(f"ETW / CSV の区間不一致: {path} {key}")
                candidate = event["begin_ns"] - int(row["begin_ns"])
                if offset is None:
                    offset = candidate
                if candidate != offset or event["end_ns"] - int(row["end_ns"]) != offset:
                    raise ValueError("ETW / CSV の時刻不一致")
                if event["detail"] != int(row["detail"]) or event["outcome"] != int(row["outcome"]):
                    raise ValueError("ETW / CSV の追加情報不一致")
                count += 1
        if count != len(lookup):
            raise ValueError("ETW / CSV の区間数不一致")
        cases[pid] = dict(name=path.name.removesuffix("-spans.csv"), spans=count, measurement_start_ns=offset)
    if set(cases) != set(markers):
        raise ValueError("対応する生 CSV のない ETW Process がある")
    return cases


def analyze(path):
    """Clock 対応の不確かさと状態の観測不足も出力し、因果関係を自動で断定しない。"""
    markers, clocks = read_markers(path)
    if not markers:
        raise ValueError("Frame Marker がない")
    cases = validate_csv(path.parent, markers)
    targets = {event["tid"] for events in markers.values() for event in events}
    scheduling = []
    with path.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            is_switch = row["kind"] == "CSwitch" and bool(
                {int(row["old_tid"]), int(row["new_tid"])} & targets
            )
            is_ready = row["kind"] == "ReadyThread" and int(row["ready_tid"]) in targets
            if is_switch or is_ready:
                scheduling.append({key: value if key in {"kind", "name"} else int(value) for key, value in row.items()})
    states = build_states(scheduling, targets)
    results = []
    for pid, spans in markers.items():
        syncs = clocks[pid]
        if {sync["name"] for sync in syncs} != {"Start", "Stop"} or len(syncs) != 2:
            raise ValueError("開始 / 終了 Clock Marker が揃っていない")
        frequency = syncs[0]["frequency"]
        if not frequency or any(sync["frequency"] != frequency for sync in syncs):
            raise ValueError("Clock Frequency が不正")
        offsets = [Fraction(sync["qpc_before"] + sync["qpc_after"], 2) - Fraction(sync["steady_ns"] * frequency, 1_000_000_000) for sync in syncs]
        clock_uncertainty = max(sync["qpc_after"] - sync["qpc_before"] for sync in syncs) * 1e9 / frequency / 2
        case = dict(cases[pid], pid=pid, frequency=frequency, clock_uncertainty_ns=clock_uncertainty,
                    clock_offset_drift_ns=float(abs(offsets[1] - offsets[0]) * 1_000_000_000 / frequency))
        interval_ends = {tid: [interval[1] for interval in values] for tid, values in states.items()}
        # 長い区間の原因調査を行う。全区間自体は ETW と生 CSV の照合を済ませて保持する
        analyzed = []
        for event in spans:
            if event["name"] not in {
                "Frame.Render", "Frame.Main", "Fence.Event1ms", "Fence.Event",
                "DXGI.Event", "DXGI.Present", "ImGui.Lock", "ImGui.PlatformNewFrame",
                "Graph.Record", "Graph.Execute",
                "Queue.Submit", "Queue.SubmitLock", "Queue.Signal",
                "CommandPool.Submit", "CommandPool.SubmitLock", "Graph.MarkSubmitted",
            }:
                continue
            sync = min(syncs, key=lambda value: abs(value["steady_ns"] - event["begin_ns"]))
            origin = Fraction(sync["qpc_before"] + sync["qpc_after"], 2)
            begin = origin + Fraction((event["begin_ns"] - sync["steady_ns"]) * frequency, 1_000_000_000)
            end = origin + Fraction((event["end_ns"] - sync["steady_ns"]) * frequency, 1_000_000_000)
            totals = duration_by_state(states[event["tid"]], begin, end, interval_ends[event["tid"]])
            analyzed.append(dict(name=event["name"], frame=event["frame"], tid=event["tid"], outcome=event["outcome"],
                                 begin_qpc=float(begin), end_qpc=float(end), duration_ms=(event["end_ns"]-event["begin_ns"])/1e6,
                                 **{state.lower()+"_ms": float(totals[state]*1000/frequency) for state in ("Running","Waiting","Ready","Unknown","Ended")}))
        worst = []
        for name in sorted({event["name"] for event in analyzed}):
            worst.extend(sorted((event for event in analyzed if event["name"] == name), key=lambda value: value["duration_ms"], reverse=True)[:5])
        case["worst"] = worst
        results.append(case)
        print(case["name"], "spans=", case["spans"], "clock_uncertainty_ns=", clock_uncertainty)
        for event in worst:
            if event["name"] == "Frame.Render":
                print(
                    " frame", event["frame"], "duration", round(event["duration_ms"], 4),
                    "running", round(event["running_ms"], 4),
                    "waiting", round(event["waiting_ms"], 4),
                    "ready", round(event["ready_ms"], 4),
                    "unknown", round(event["unknown_ms"], 6),
                )
        with path.with_name(case["name"] + "-cpu-states.csv").open("w", encoding="utf-8", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=analyzed[0].keys())
            writer.writeheader()
            writer.writerows(analyzed)
    destination = path.with_name("CpuScheduling.json")
    destination.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("events", type=Path)
    analyze(parser.parse_args().events)
