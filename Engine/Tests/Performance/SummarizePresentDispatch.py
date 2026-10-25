"""Present の Dispatch Counter を集計する。時間と CPU Cycle は換算しない。"""

import argparse
import bisect
import collections
import csv
import json
import math
import pathlib
import statistics


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def statistics_of(values):
    ordered = sorted(values)
    if not ordered:
        return {"count": 0}
    return {"count": len(ordered), "avg": statistics.mean(ordered),
            "median": statistics.median(ordered),
            "p95": ordered[math.ceil(len(ordered) * .95) - 1], "max": ordered[-1]}


def overlap(intervals, starts, start, end):
    # 同じ Thread の同じ Metric は重複しない。隣接する二つ以上の区間にも対応する
    first = max(0, bisect.bisect_right(starts, start) - 1)
    total = 0
    for left, right in intervals[first:]:
        if left >= end:
            break
        total += max(0, min(end, right) - max(start, left))
    return total


def summarize(directory):
    records = []
    calibrations = collections.defaultdict(list)
    runs = []
    baseline = []
    for path in sorted(directory.glob("*.csv")):
        if path.name.endswith((".dispatch.csv", ".timeline.csv")) or path.name == "Summary.csv":
            continue
        metadata_path = pathlib.Path(str(path) + ".json")
        if not metadata_path.exists():
            continue
        metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
        if metadata["status"] or metadata["dropped_samples"]:
            raise ValueError(f"Invalid run: {path}")
        raw = read_csv(path)
        metrics = collections.defaultdict(list)
        native = {}
        for row in raw:
            metrics[row["metric"]].append(int(row["nanoseconds"]) / 1e6)
            if row["metric"] == "PresentNative":
                native[int(row["frame"])] = int(row["nanoseconds"]) / 1e6
        runs.append({"run": path.stem, "metrics_ms": {k: statistics_of(v) for k, v in metrics.items()},
                     "status": metadata["status"], "dropped": metadata["dropped_samples"]})
        case = raw[0]["case"] if raw else path.stem
        if case == "new-baseline":
            baseline.extend(metrics["PresentNative"])
            continue
        dispatch_path = pathlib.Path(str(path) + ".dispatch.csv")
        if not dispatch_path.exists():
            continue
        readiness_path = pathlib.Path(str(path) + ".readiness.csv")
        readiness = {int(r["frame"]): r for r in read_csv(readiness_path)} if readiness_path.exists() else {}
        timeline = read_csv(pathlib.Path(str(path) + ".timeline.csv"))
        intervals = {}
        starts = {}
        for metric in ("UiBuild", "Main", "MainPump"):
            intervals[metric] = sorted((int(r["start_ns"]), int(r["end_ns"])) for r in timeline
                                       if r["metric"] == metric)
            starts[metric] = [left for left, _ in intervals[metric]]
        for row in read_csv(dispatch_path):
            sample = {key: int(value) for key, value in row.items() if key != "kind"}
            if row["kind"] != "Present":
                calibrations[row["kind"]].append(sample)
                continue
            sample["case"] = case
            sample["run"] = path.stem
            sample["native_ms"] = native.get(sample["frame"])
            sample["dispatch_ms"] = (sample["end_ns"] - sample["start_ns"]) / 1e6
            sample["was_gpu_ready"] = int(readiness[sample["frame"]]["was_ready"]) if sample["frame"] in readiness else -1
            sample["is_gpu_ready"] = int(readiness[sample["frame"]]["is_ready"]) if sample["frame"] in readiness else -1
            for metric in intervals:
                sample[metric + "_overlap_ms"] = overlap(intervals[metric], starts[metric],
                                                         sample["start_ns"], sample["end_ns"]) / 1e6
            records.append(sample)
    cases = {}
    for case in sorted({r["case"] for r in records}):
        rows = [r for r in records if r["case"] == case]
        valid = [r for r in rows if r["status"] == 0 and r["native_ms"] is not None]
        switched = [r for r in valid if r["context_switches"] > 0]
        no_switch = [r for r in valid if r["context_switches"] == 0]
        reasons = collections.Counter()
        for r in valid:
            for bit in range(64):
                if r["wait_reasons"] & (1 << bit):
                    reasons[str(bit)] += 1
        groups = {}
        for name, subset in (("switched", switched), ("no_switch", no_switch),
                             ("ui_overlap", [r for r in valid if r["UiBuild_overlap_ms"] > 0]),
                             ("no_ui_overlap", [r for r in valid if r["UiBuild_overlap_ms"] == 0]),
                             ("pump_overlap", [r for r in valid if r["MainPump_overlap_ms"] > 0]),
                             ("no_pump_overlap", [r for r in valid if r["MainPump_overlap_ms"] == 0])):
            groups[name] = statistics_of([r["native_ms"] for r in subset])
        cases[case] = {"count": len(rows), "invalid": len(rows) - len(valid),
                       "gpu_ready_before_count": sum(r["was_gpu_ready"] == 1 for r in valid),
                       "gpu_ready_after_count": sum(r["is_gpu_ready"] == 1 for r in valid),
                       "gpu_readiness_unknown_count": sum(r["is_gpu_ready"] == -1 for r in valid),
                       "native_ms": statistics_of([r["native_ms"] for r in valid]),
                       "dispatch_ms": statistics_of([r["dispatch_ms"] for r in valid]),
                       "cycles": statistics_of([r["cycles"] for r in valid]),
                       "profile_cycles": statistics_of([r["profile_cycles"] for r in valid]),
                       "switched_count": len(switched), "reason_bit_counts": dict(reasons),
                       "groups_ms": groups,
                       "ui_overlap_total_ms": sum(r["UiBuild_overlap_ms"] for r in valid),
                       "pump_overlap_total_ms": sum(r["MainPump_overlap_ms"] for r in valid)}
    calibration_summary = {}
    for kind, rows in calibrations.items():
        valid = [r for r in rows if r["status"] == 0]
        calibration_summary[kind] = {
            "count": len(rows), "invalid": len(rows) - len(valid),
            "switched_count": sum(r["context_switches"] > 0 for r in valid),
            "elapsed_ms": statistics_of([(r["end_ns"] - r["start_ns"]) / 1e6 for r in valid]),
            "hook_ms": statistics_of([r.get("hook_ns", 0) / 1e6 for r in valid]),
            "cycles": statistics_of([r["cycles"] for r in valid]),
            "profile_cycles": statistics_of([r["profile_cycles"] for r in valid]),
            "wait_reason_maps": dict(collections.Counter(hex(r["wait_reasons"]) for r in valid))}
    return {"baseline_native_ms": statistics_of(baseline), "cases": cases,
            "calibration": calibration_summary, "runs": runs}, records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    summary, records = summarize(args.directory)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if records:
        with pathlib.Path(str(args.output) + ".frames.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=records[0].keys())
            writer.writeheader()
            writer.writerows(records)
    print(json.dumps({"baseline": summary["baseline_native_ms"], "cases": summary["cases"],
                      "calibration": summary["calibration"]}, indent=2))


if __name__ == "__main__":
    main()
