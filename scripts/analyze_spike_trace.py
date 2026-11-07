"""SpikeTimingProbe の全区間を集計し、最大値と同一 Frame の内訳を保存する。"""

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def percentile(values, percent):
    """nearest-rank で実在する標本を選び、近似値へ置き換えない。"""
    return values[math.ceil(len(values) * percent / 100) - 1]


def analyze(path):
    """欠落件数は Probe の Log と別途照合し、異なる Frame の Max を足し合わせない。"""
    with path.open(encoding="utf-8", newline="") as source:
        spans = list(csv.DictReader(source))
    for span in spans:
        for key in span.keys() - {"name"}:
            span[key] = int(span[key])
        if span["end_ns"] < span["begin_ns"]:
            raise ValueError("区間の終了が開始より前")
    groups = defaultdict(list)
    by_frame = defaultdict(list)
    for span in spans:
        groups[span["name"]].append(span)
        by_frame[span["frame"]].append(span)
    renders = sorted(groups["Frame.Render"], key=lambda value: value["end_ns"])
    groups["Frame.Interval"] = [
        dict(current, name="Frame.Interval", begin_ns=previous["end_ns"],
             duration_ns=current["end_ns"] - previous["end_ns"], cycles_valid=0, cycles=0)
        for previous, current in zip(renders, renders[1:])
    ]
    summaries = []
    for name, events in sorted(groups.items()):
        if not events:
            continue
        durations = sorted(event["duration_ns"] for event in events)
        maximum = max(events, key=lambda event: event["duration_ns"])
        cycles = [event["cycles"] for event in events if event["cycles_valid"]]
        summaries.append(dict(
            name=name, count=len(events), average_ms=statistics.mean(durations) / 1e6,
            p95_ms=percentile(durations, 95) / 1e6, p99_ms=percentile(durations, 99) / 1e6,
            max_ms=durations[-1] / 1e6, over_1ms=sum(value > 1_000_000 for value in durations),
            max_frame=maximum["frame"], max_thread=maximum["thread"],
            max_begin_ms=maximum["begin_ns"] / 1e6,
            median_ms=statistics.median(durations) / 1e6,
            median_cycles=statistics.median(cycles) if cycles else "",
            cycles_at_max=maximum["cycles"] if maximum["cycles_valid"] else "",
            signaled=sum(event.get("outcome") == 0 for event in events),
            timed_out=sum(event.get("outcome") == 258 for event in events),
            timeout_over_2ms=sum(event.get("outcome") == 258 and event["duration_ns"] > 2_000_000 for event in events),
        ))
    destination = path.with_name(path.stem.removesuffix("-spans") + "-statistics.csv")
    if summaries:
        with destination.open("w", encoding="utf-8", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=summaries[0].keys())
            writer.writeheader()
            writer.writerows(summaries)
    # 包含区間の総和は二重計上になるため、内訳は区間のまま保存する
    worst = []
    for name in ("Frame.Render", "Frame.Main", "Frame.Interval", "ImGui.Lock", "ImGui.Callback"):
        for event in sorted(groups[name], key=lambda value: value["duration_ns"], reverse=True)[:5]:
            children = [candidate for candidate in by_frame[event["frame"]]
                        if candidate["thread"] == event["thread"]
                        and candidate is not event
                        and candidate["begin_ns"] >= event["begin_ns"]
                        and candidate["end_ns"] <= event["end_ns"]]
            overlaps = [candidate for candidate in spans
                        if candidate["thread"] != event["thread"]
                        and candidate["name"] in {"ImGui.Callback", "ImGui.GpuRecord", "Fence.Event", "DXGI.Present"}
                        and candidate["end_ns"] > event["begin_ns"]
                        and candidate["begin_ns"] < event["end_ns"]]
            parents = [candidate for candidate in by_frame[event["frame"]]
                       if candidate["thread"] == event["thread"]
                       and candidate is not event
                       and candidate["begin_ns"] <= event["begin_ns"]
                       and candidate["end_ns"] >= event["end_ns"]]
            worst.append(dict(event=event, children=children, parents=parents, overlapping_other_thread=overlaps))
    path.with_name(path.stem.removesuffix("-spans") + "-worst.json").write_text(
        json.dumps(worst, ensure_ascii=False, indent=2), encoding="utf-8")
    for summary in summaries:
        if summary["name"] in {"Frame.Main", "Frame.Render", "Frame.Interval", "ImGui.Lock", "DXGI.Ready", "Fence.Event", "Fence.Event1ms"}:
            print(f"{path.stem}: {summary['name']} count={summary['count']} "
                  f"avg={summary['average_ms']:.3f} p95={summary['p95_ms']:.3f} "
                  f"max={summary['max_ms']:.3f} frame={summary['max_frame']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    arguments = parser.parse_args()
    for trace in arguments.files:
        analyze(trace)
