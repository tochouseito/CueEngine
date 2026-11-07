"""停止済み Probe の生 CSV を集計し、反復前後の基準と比較する調査用 Tool"""

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def summarize(values):
    """平均の平均を使わず、生 Sample から nearest-rank p95 を計算する"""
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "avg_ms": statistics.mean(ordered) / 1_000_000,
        "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1] / 1_000_000,
        "max_ms": ordered[-1] / 1_000_000,
    }


def write_csv(path, rows):
    """測定 Process 停止後だけ集計結果を書き出す"""
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    """指定 Phase 内の成功した実行を検証し、別 Phase の Sample は混ぜない"""
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--baseline", default="baseline")
    args = parser.parse_args()
    pooled = defaultdict(list)
    by_repeat = defaultdict(list)
    run_rows = []
    validations = []
    cycles_pooled = defaultdict(list)
    for metadata_path in sorted(args.directory.glob("*.csv.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
        path = metadata_path.with_suffix("")
        if metadata["status"] != 0 or metadata["dropped_samples"] != 0:
            raise ValueError(f"Failed or incomplete run: {path}")
        case = metadata["case"]
        repeat = metadata["repeat"]
        metrics = defaultdict(list)
        with path.open(newline="", encoding="utf-8-sig") as source:
            for row in csv.DictReader(source):
                if row["case"] != case or int(row["repeat"]) != repeat:
                    raise ValueError(f"Run label mismatch: {path}")
                frame = int(row["frame"])
                if not metadata["warmup_frames"] <= frame < metadata["warmup_frames"] + metadata["measurement_frames"]:
                    raise ValueError(f"Warmup or excess sample: {path}")
                metrics[row["metric"]].append(int(row["nanoseconds"]))
        if len(metrics["Render"]) != metadata["measurement_frames"]:
            raise ValueError(f"Render sample count mismatch: {path}")
        if metadata.get("cycle_failures", 0):
            raise ValueError(f"CPU cycle collection failed: {path}")
        if metadata["study"].get("native_cycles", False):
            cycles = defaultdict(list)
            with Path(str(path) + ".cycles.csv").open(newline="", encoding="utf-8") as source:
                for row in csv.DictReader(source):
                    frame = int(row["frame"])
                    if row["case"] != case or int(row["repeat"]) != repeat or not metadata["warmup_frames"] <= frame < metadata["warmup_frames"] + metadata["measurement_frames"]:
                        raise ValueError(f"Cycle label mismatch: {path}")
                    cycles[row["metric"]].append(int(row["cycles"]))
            for metric in ("CycleReadOverhead", "AllocatorReset", "ListReset", "CommandClose", "ExecuteLists", "Signal", "PresentNative"):
                if len(cycles[metric]) != metadata["measurement_frames"]:
                    raise ValueError(f"Cycle count mismatch: {path}, {metric}")
                cycles_pooled[case, metric].extend(cycles[metric])
        for metric, values in metrics.items():
            pooled[case, metric].extend(values)
            if metric == "Render":
                by_repeat[case, repeat].extend(values)
            run_rows.append({"run": path.stem, "case": case, "repeat": repeat, "metric": metric, **summarize(values)})
        validations.append({"run": path.stem, "render_samples": len(metrics["Render"]),
                            "dropped": metadata["dropped_samples"], "ui_vertices": metadata["ui"]["vertices"]})
    if not validations:
        raise ValueError("No completed runs")
    rows = [{"case": case, "metric": metric, **summarize(values)} for (case, metric), values in sorted(pooled.items())]
    paired = []
    for (case, repeat), values in sorted(by_repeat.items()):
        if case == args.baseline:
            continue
        baseline = summarize(by_repeat[args.baseline, repeat])
        candidate = summarize(values)
        paired.append({"case": case, "repeat": repeat, "baseline_avg_ms": baseline["avg_ms"],
                       "avg_ms": candidate["avg_ms"], "delta_avg_ms": candidate["avg_ms"] - baseline["avg_ms"],
                       "delta_avg_percent": (candidate["avg_ms"] / baseline["avg_ms"] - 1) * 100,
                       "baseline_p95_ms": baseline["p95_ms"], "p95_ms": candidate["p95_ms"]})
    write_csv(args.directory / "PooledMetrics.csv", rows)
    write_csv(args.directory / "RunMetrics.csv", run_rows)
    write_csv(args.directory / "PairedMetrics.csv", paired)
    cycle_rows = []
    for (case, metric), values in sorted(cycles_pooled.items()):
        ordered = sorted(values)
        cycle_rows.append({"case": case, "metric": metric, "count": len(values), "avg_cycles": statistics.mean(values),
                           "p95_cycles": ordered[math.ceil(0.95 * len(values)) - 1], "max_cycles": ordered[-1]})
    write_csv(args.directory / "CycleMetrics.csv", cycle_rows)
    (args.directory / "Validation.json").write_text(json.dumps(validations, indent=4), encoding="utf-8")
    for row in rows:
        if row["metric"] == "Render":
            print(f'{row["case"]:30} {row["count"]:5d} avg={row["avg_ms"]:.4f} p95={row["p95_ms"]:.4f} max={row["max_ms"]:.4f}')


if __name__ == "__main__":
    main()
