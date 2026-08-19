#!/usr/bin/env python3
"""Aggregate adaptive capacity repetitions and write JSON/Markdown."""

import argparse
import json
import pathlib
import statistics


def stats(values):
    values = [value for value in values if value is not None]
    if not values:
        return {"mean": None, "min": None, "max": None}
    return {"mean": statistics.mean(values), "min": min(values), "max": max(values)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    case = json.loads(pathlib.Path(args.case).read_text(encoding="utf-8"))

    products = {}
    for product in case["products"]:
        runs = [
            json.loads(path.read_text(encoding="utf-8"))
            for path in sorted(root.glob(f"round-*/products/{product}/summary.json"))
        ]
        stages = []
        for index, target in enumerate(case["stage_rps"], start=1):
            samples = [
                stage
                for run in runs
                for stage in run["stages"]
                if stage["target_rps"] == target
            ]
            all_stable = len(samples) == case["repeats"] and all(sample["stable"] for sample in samples)
            stages.append({
                "stage": f"s{index}",
                "target_rps": target,
                "attempted_rounds": len(samples),
                "stable_rounds": sum(sample["stable"] for sample in samples),
                "all_rounds_stable": all_stable,
                "delivered_rps": stats([sample["delivered_rps"] for sample in samples]),
                "semantic_ok_rate": stats([sample["semantic_ok_rate"] for sample in samples]),
                "p99_ms": stats([sample["latency_ms"]["p99"] for sample in samples]),
                "dropped_ratio": stats([sample["dropped_ratio"] for sample in samples]),
                "cpu_seconds": stats([sample["resources"]["cpu_seconds"] for sample in samples]),
                "memory_peak_bytes": stats([sample["resources"]["memory_peak_bytes"] for sample in samples]),
                "rate_limited_total": sum(sample["rate_limited"] for sample in samples),
                "unexpected_total": sum(sample["unexpected"] for sample in samples),
                "dropped_iterations_total": sum(sample["dropped_iterations"] for sample in samples),
            })
        highest = max((stage["target_rps"] for stage in stages if stage["all_rounds_stable"]), default=0)
        ceiling = case["stage_rps"][-1]
        reached_ceiling = highest == ceiling
        products[product] = {
            "completed_rounds": len(runs),
            "highest_stable_rps": highest,
            "capacity_display": f">={ceiling}" if reached_ceiling else str(highest),
            "reached_test_ceiling": reached_ceiling,
            "first_failed_rps_by_round": [run["first_failed_rps"] for run in runs],
            "failure_owner_by_round": [run["failure_owner"] for run in runs],
            "stages": stages,
            "overall_memory_peak_bytes": stats([
                run["overall_resources"]["memory_peak_bytes"] for run in runs
            ]),
        }

    output = {
        "case": case["name"],
        "repeats": case["repeats"],
        "test_ceiling_rps": case["stage_rps"][-1],
        "products": products,
    }
    pathlib.Path(args.output).write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [
        "# 2 CPU / 2 GiB Adaptive Healthy Capacity",
        "",
        "A target is stable only when every configured repetition attempted and passed it.",
        "The test stops each repetition at its first failed target.",
        "",
        "| Product | Stable capacity RPS | Completed rounds | First failed RPS by round | Failure owner by round |",
        "|---|---:|---:|---|---|",
    ]
    for product in case["products"]:
        row = products[product]
        failures = ", ".join("-" if value is None else str(value) for value in row["first_failed_rps_by_round"])
        owners = ", ".join("-" if value is None else str(value) for value in row["failure_owner_by_round"])
        lines.append(f"| {product} | {row['capacity_display']} | {row['completed_rounds']} | {failures} | {owners} |")
    lines.extend([
        "", "## Stage averages", "",
        f"| Product | Target RPS | Attempted | Stable {case['repeats']}/{case['repeats']} | Mean delivered RPS | Mean P99 ms | Mean drop ratio | Mean CPU s | 429 | Unexpected |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])

    def shown(value):
        return "-" if value is None else f"{value:.3f}"

    for product in case["products"]:
        for stage in products[product]["stages"]:
            lines.append(
                f"| {product} | {stage['target_rps']} | {stage['attempted_rounds']} | "
                f"{'yes' if stage['all_rounds_stable'] else 'no'} | {shown(stage['delivered_rps']['mean'])} | "
                f"{shown(stage['p99_ms']['mean'])} | {shown(stage['dropped_ratio']['mean'])} | "
                f"{shown(stage['cpu_seconds']['mean'])} | {stage['rate_limited_total']} | {stage['unexpected_total']} |"
            )
    pathlib.Path(args.report).write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
