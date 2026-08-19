#!/usr/bin/env python3
"""Summarize one adaptive capacity stage or combine one measurement round."""

import argparse
import csv
import json
import pathlib
import statistics


def metric(metrics, name, field, default=None):
    value = metrics.get(name, {})
    return value.get("values", value).get(field, default)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[max(0, min(len(ordered) - 1, index))]


def number(row, name):
    try:
        return int(row[name])
    except (KeyError, TypeError, ValueError):
        return 0


def resource_summary(rows):
    cpu = [number(row, "cgroup_cpu_usec") for row in rows]
    pss = [number(row, "pss_kb") for row in rows if number(row, "pss_kb") > 0]
    rss = [number(row, "rss_kb") for row in rows if number(row, "rss_kb") > 0]
    memory = [number(row, "cgroup_memory_current") for row in rows]
    return {
        "samples": len(rows),
        "cpu_seconds": (max(cpu) - min(cpu)) / 1_000_000 if len(cpu) >= 2 else None,
        "pss_median_kb": statistics.median(pss) if pss else None,
        "pss_p95_kb": percentile(pss, 0.95),
        "rss_median_kb": statistics.median(rss) if rss else None,
        "memory_median_bytes": statistics.median(memory) if memory else None,
        "memory_peak_bytes": max(memory) if memory else None,
    }


def load_upstreams(snapshot_dir):
    upstreams = {}
    for name in "ab":
        path = snapshot_dir / f"upstream-{name}.json"
        if not path.exists():
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        upstreams[name.upper()] = {
            "data_requests": data.get("data_requests", data.get("requests", 0)),
            "health_requests": data.get("health_requests", 0),
            "tcp_accepts": data.get("tcp_accepts", 0),
            "reused_requests": data.get("reused_requests", 0),
            "responses": data.get("responses", {}),
        }
    return upstreams


def summarize_stage(args):
    case = json.loads(pathlib.Path(args.case).read_text(encoding="utf-8"))
    metrics = json.loads(pathlib.Path(args.k6_summary).read_text(encoding="utf-8")).get("metrics", {})
    resource_path = pathlib.Path(args.resources)
    rows = list(csv.DictReader(resource_path.open(encoding="utf-8"))) if resource_path.exists() else []
    tag = f"load_stage:s{args.stage_index}"
    requests = metric(metrics, f"http_reqs{{{tag}}}", "count", 0) or 0
    dropped = metric(metrics, f"dropped_iterations{{{tag}}}", "count", 0) or 0
    contract = metric(metrics, f"bench_contract_ok{{{tag}}}", "value", None)
    unexpected = metric(metrics, f"bench_unexpected{{{tag}}}", "count", 0) or 0
    rate_limited = metric(metrics, f"bench_rate_limited{{{tag}}}", "count", 0) or 0
    p99 = metric(metrics, f"http_req_duration{{{tag}}}", "p(99)", None)
    delivered_rps = requests / float(case["stage_seconds"])
    delivery_ratio = delivered_rps / args.target_rps
    scheduled = requests + dropped
    dropped_ratio = dropped / scheduled if scheduled else 1.0
    upstream_contract_path = pathlib.Path(args.upstream_contract)
    upstream_contract = (
        json.loads(upstream_contract_path.read_text(encoding="utf-8"))
        if upstream_contract_path.exists()
        else {"passed": False}
    )

    gates = {
        "delivery": delivery_ratio >= float(case["delivery_min"]),
        "semantic": contract is not None and contract >= float(case["semantic_min"]),
        "unexpected": unexpected == 0,
        "rate_limit": rate_limited == 0,
        "dropped": dropped_ratio <= float(case["dropped_max_ratio"]),
        "p99": p99 is not None and p99 <= float(case["p99_max_ms"]),
        "upstream_contract": upstream_contract.get("passed") is True,
    }
    output = {
        "stage": f"s{args.stage_index}",
        "target_rps": args.target_rps,
        "stage_seconds": case["stage_seconds"],
        "http_requests": requests,
        "delivered_rps": delivered_rps,
        "delivery_ratio": delivery_ratio,
        "dropped_iterations": dropped,
        "dropped_ratio": dropped_ratio,
        "semantic_ok_rate": contract,
        "unexpected": unexpected,
        "rate_limited": rate_limited,
        "latency_ms": {
            "p50": metric(metrics, f"http_req_duration{{{tag}}}", "med", None),
            "p95": metric(metrics, f"http_req_duration{{{tag}}}", "p(95)", None),
            "p99": p99,
            "max": metric(metrics, f"http_req_duration{{{tag}}}", "max", None),
        },
        "resources": resource_summary(rows),
        "upstreams": load_upstreams(pathlib.Path(args.snapshot_dir)),
        "upstream_contract": upstream_contract,
        "gates": gates,
        "failure_reasons": [name for name, passed in gates.items() if not passed],
        "stable": all(gates.values()),
    }
    pathlib.Path(args.output).write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def combine_round(args):
    case = json.loads(pathlib.Path(args.case).read_text(encoding="utf-8"))
    round_dir = pathlib.Path(args.round_dir)
    stages = []
    for target in case["stage_rps"]:
        path = round_dir / "stages" / str(target) / "summary.json"
        if path.exists():
            stages.append(json.loads(path.read_text(encoding="utf-8")))
    failed = next((stage for stage in stages if not stage["stable"]), None)
    calibration_path = round_dir / "calibration" / "summary.json"
    calibration = (
        json.loads(calibration_path.read_text(encoding="utf-8")) if calibration_path.exists() else None
    )
    resources = [stage["resources"] for stage in stages]
    memory_peaks = [item["memory_peak_bytes"] for item in resources if item["memory_peak_bytes"] is not None]
    cpu_seconds = [item["cpu_seconds"] for item in resources if item["cpu_seconds"] is not None]
    output = {
        "product": args.product,
        "round": args.round,
        "configured_stage_rps": case["stage_rps"],
        "attempted_stage_rps": [stage["target_rps"] for stage in stages],
        "stages": stages,
        "highest_stable_rps": max((stage["target_rps"] for stage in stages if stage["stable"]), default=0),
        "first_failed_rps": failed["target_rps"] if failed else None,
        "first_failure_reasons": failed["failure_reasons"] if failed else [],
        "stopped_early": failed is not None and len(stages) < len(case["stage_rps"]),
        "reached_ceiling": failed is None and len(stages) == len(case["stage_rps"]),
        "failure_owner": calibration.get("failure_owner") if calibration else None,
        "direct_calibration": calibration,
        "overall_resources": {
            "samples": sum(item["samples"] for item in resources),
            "cpu_seconds": sum(cpu_seconds) if cpu_seconds else None,
            "memory_peak_bytes": max(memory_peaks) if memory_peaks else None,
        },
    }
    pathlib.Path(args.output).write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    stage = subparsers.add_parser("stage")
    stage.add_argument("--case", required=True)
    stage.add_argument("--stage-index", type=int, required=True)
    stage.add_argument("--target-rps", type=int, required=True)
    stage.add_argument("--k6-summary", required=True)
    stage.add_argument("--resources", required=True)
    stage.add_argument("--snapshot-dir", required=True)
    stage.add_argument("--upstream-contract", required=True)
    stage.add_argument("--output", required=True)
    stage.set_defaults(handler=summarize_stage)

    round_parser = subparsers.add_parser("round")
    round_parser.add_argument("--product", required=True)
    round_parser.add_argument("--round", type=int, required=True)
    round_parser.add_argument("--case", required=True)
    round_parser.add_argument("--round-dir", required=True)
    round_parser.add_argument("--output", required=True)
    round_parser.set_defaults(handler=combine_round)
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
