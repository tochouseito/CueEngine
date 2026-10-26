"""SwapChain 比較の実 DXGI 条件を検証し、前後の基準と各時間区間を対応付ける。"""

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

from SummarizeEditorTimingStudy import summarize, write_csv


def main():
    """成功した記録だけを扱い、表示待機の移動と Render 全体の短縮を分ける。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--baseline", default="new-baseline")
    args = parser.parse_args()
    grouped = defaultdict(list)
    validations = []
    for path in sorted(args.directory.glob("*.csv.json")):
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        study = data["study"]
        actual = study["swap_chain_created"]
        expected_flags = (64 if study["waitable_swap_chain"] else 0) | (
            2048 if data["presentation"]["actual_tearing"] else 0)
        if (data["status"] or data["dropped_samples"] or
                actual["buffers"] != data["presentation"]["buffers"] or
                actual["maximum_latency"] != study["maximum_presentation_latency"] or
                actual["scaling"] != int(study["no_presentation_scaling"]) or
                actual["flags"] != expected_flags or
                data["frame"]["frames_in_flight"] != 2):
            raise ValueError(f"Failed run or actual DXGI setting mismatch: {path}")
        # 通常全画面寸法の基準は起動直後に OS 補正を戻すため、生成時の寸法とは別に扱う
        if data["case"] != "new-screen-windowed" and (
                actual["width"] != data["window"]["width"] or
                actual["height"] != data["window"]["height"]):
            raise ValueError(f"Window / buffer size mismatch: {path}")
        metrics = defaultdict(list)
        with path.with_suffix("").open(newline="", encoding="utf-8-sig") as source:
            for row in csv.DictReader(source):
                frame = int(row["frame"])
                if (row["case"] != data["case"] or int(row["repeat"]) != data["repeat"] or
                        not data["warmup_frames"] <= frame < data["warmup_frames"] + data["measurement_frames"]):
                    raise ValueError(f"Sample label or frame range mismatch: {path}")
                metrics[row["metric"]].append(int(row["nanoseconds"]))
        for metric in ("Render", "PresentNative"):
            if len(metrics[metric]) != data["measurement_frames"]:
                raise ValueError(f"Missing {metric} sample: {path}")
        if study["waitable_swap_chain"] and len(metrics["PresentationWait"]) != data["measurement_frames"]:
            raise ValueError(f"Missing presentation wait: {path}")
        for metric, values in metrics.items():
            grouped[data["case"], data["repeat"], metric].extend(values)
        validations.append({"run": path.stem, "actual": actual, "render_samples": len(metrics["Render"]),
                            "wall_seconds": data["wall_seconds"], "window": data["window"],
                            "frame": data["frame"], "presentation": data["presentation"]})
    rows = []
    for (case, repeat, metric), values in sorted(grouped.items()):
        if case == args.baseline:
            continue
        baseline_values = grouped[args.baseline, repeat, metric]
        if not baseline_values:
            continue
        baseline = summarize(baseline_values)
        candidate = summarize(values)
        rows.append({"case": case, "repeat": repeat, "metric": metric, "baseline_avg_ms": baseline["avg_ms"],
                     "avg_ms": candidate["avg_ms"], "delta_avg_ms": candidate["avg_ms"] - baseline["avg_ms"],
                     "delta_avg_percent": (candidate["avg_ms"] / baseline["avg_ms"] - 1) * 100,
                     "baseline_p95_ms": baseline["p95_ms"], "p95_ms": candidate["p95_ms"]})
    if not validations or not rows:
        raise ValueError("No completed paired runs")
    write_csv(args.directory / "SwapChainPairedMetrics.csv", rows)
    (args.directory / "SwapChainValidation.json").write_text(
        json.dumps(validations, ensure_ascii=False, indent=4), encoding="utf-8")
    print(f"Validated {len(validations)} runs, {sum(r['render_samples'] for r in validations)} Render samples")


if __name__ == "__main__":
    main()
