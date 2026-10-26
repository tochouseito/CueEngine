"""停止済み CueEngine の PresentMon CSV だけを集計する"""

import argparse
import collections
import csv
import json
import math
import pathlib
import re
import statistics


def numeric(value):
    """未取得値を 0 に置き換えず、有効な有限値だけを返す"""
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def main():
    """各 Capture の対象、ETW 欠落、表示経路を検証して集計する"""
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=pathlib.Path)
    args = parser.parse_args()
    root = args.directory.resolve()
    allowed = pathlib.Path(__file__).resolve().parents[3] / "out/validation/M06-PerformanceStudy/PresentationPaths"
    if root != allowed.resolve() and allowed.resolve() not in root.parents:
        raise ValueError("Capture directory is outside the CueEngine study boundary")
    reports = []
    metrics = []
    for capture_path in sorted(root.glob("*.capture.json")):
        capture = json.loads(capture_path.read_text(encoding="utf-8-sig"))
        stem = str(capture_path)[: -len(".capture.json")]
        engine = json.loads(pathlib.Path(stem + ".engine.csv.json").read_text(encoding="utf-8-sig"))
        with pathlib.Path(stem + ".presentmon.csv").open(encoding="utf-8-sig", newline="") as source:
            rows = list(csv.DictReader(source))
        if not rows:
            raise ValueError(f"PresentMon CSV has no rows: {stem}")
        targets = sorted({(r["Application"], r["ProcessID"]) for r in rows})
        # 起動した CueEngine の PID を指定して収集する。名前照会権限がないと Application は unknown になる
        target_valid = all(pid == str(capture["probe_pid"]) and name in ("CueEditorTimingProbe.exe", "<unknown>")
                           for name, pid in targets)
        loss = {}
        status_bytes = pathlib.Path(stem + ".presentmon.stdout.log").read_bytes()
        status_log = status_bytes.decode("utf-16" if status_bytes.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
        status_fields = {"EtwEventsLost": "EventsLost", "EtwBuffersLost": "BuffersLost",
                         "OverflowedPresents": "OverflowedPresents"}
        for field in ("EtwEventsLost", "EtwBuffersLost", "OverflowedPresents"):
            values = [numeric(row.get(field)) for row in rows]
            values = [v for v in values if v is not None]
            reported = [int(value) for value in re.findall(status_fields[field] + r"=(\d+)", status_log)]
            loss[field] = max(values) if len(values) == len(rows) else (max(reported) if reported else None)
        warmup = int(engine["warmup_frames"])
        frames = int(engine["measurement_frames"])
        selected = rows[warmup : warmup + frames]
        # PresentMon と Engine の FrameId は対応付けていないため、表示経路を調べる補助集計として扱う
        modes = dict(collections.Counter(r.get("PresentMode", "<missing>") for r in selected))
        displayed = sum(numeric(r.get("DisplayedTime", r.get("MsUntilDisplayed"))) is not None for r in selected)
        report = {
            "run": pathlib.Path(stem).name,
            "case": capture["case"],
            "repeat": capture["repeat"],
            "target_valid": target_valid,
            "targets": targets,
            "engine_status": engine["status"],
            "raw_rows": len(rows),
            "selected_rows": len(selected),
            "expected_measurement_frames": frames,
            "row_selection": "skip warmup rows, take up to measurement frames; ordinal only, not Engine FrameId aligned",
            "swapchains": sorted({r.get("SwapChainAddress", "<missing>") for r in rows}),
            "data_loss": loss,
            "loss_source": "CSV counters if present, otherwise maximum ETW status log counters reported during capture",
            "loss_counters_valid": all(v == 0 for v in loss.values()),
            "display_time_observed_rows": displayed,
            "display_time_unavailable_rows": len(selected) - displayed,
            "display_time_note": "NA is unavailable; it is not proof that the frame was never displayed",
            "present_modes": modes,
            "sync_intervals": dict(collections.Counter(r.get("SyncInterval", "<missing>") for r in selected)),
            "allows_tearing": dict(collections.Counter(r.get("AllowsTearing", "<missing>") for r in selected)),
        }
        reports.append(report)
        for field in ("MsInPresentAPI", "MsBetweenPresents", "MsUntilDisplayed", "DisplayedTime", "DisplayLatency", "MsGPUTime"):
            values = sorted(v for r in selected if (v := numeric(r.get(field))) is not None)
            if values:
                metrics.append({
                    "run": report["run"], "case": report["case"], "repeat": report["repeat"], "metric": field,
                    "count": len(values), "avg_ms": statistics.fmean(values),
                    "p95_ms": values[math.ceil(0.95 * len(values)) - 1], "max_ms": values[-1],
                })
    (root / "PresentationValidation.json").write_text(json.dumps(reports, indent=4), encoding="utf-8")
    with (root / "PresentationMetrics.csv").open("w", encoding="utf-8", newline="") as destination:
        fields = ["run", "case", "repeat", "metric", "count", "avg_ms", "p95_ms", "max_ms"]
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        writer.writerows(metrics)
    for report in reports:
        print(report["run"], "rows", report["raw_rows"], "loss", report["data_loss"], "modes", report["present_modes"])
    if not reports or any(not r["target_valid"] or not r["loss_counters_valid"] or r["engine_status"] != 0 for r in reports):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
