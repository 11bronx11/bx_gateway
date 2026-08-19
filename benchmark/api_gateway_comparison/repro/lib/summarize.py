#!/usr/bin/env python3
"""Create a compact reproducible product summary from k6 and resource evidence."""

import argparse
import csv
import json
import pathlib
import statistics


def metric_value(metrics, name, field, default=None):
    metric = metrics.get(name, {})
    values = metric.get("values", metric)
    return values.get(field, default)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((len(ordered) - 1) * fraction))))
    return ordered[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--product", required=True)
    parser.add_argument("--k6-summary", required=True)
    parser.add_argument("--resources", required=True)
    parser.add_argument("--events", required=True)
    parser.add_argument("--duration-s", type=float, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.duration_s <= 0:
        parser.error("--duration-s must be greater than zero")

    summary_path = pathlib.Path(args.k6_summary)
    metrics = json.loads(summary_path.read_text(encoding="utf-8")).get("metrics", {}) \
        if summary_path.exists() else {}
    resource_path = pathlib.Path(args.resources)
    rows = list(csv.DictReader(resource_path.open(encoding="utf-8"))) if resource_path.exists() else []

    def ints(name):
        output = []
        for row in rows:
            try:
                output.append(int(row[name]))
            except (KeyError, TypeError, ValueError):
                pass
        return output

    pss = [value for value in ints("pss_kb") if value > 0]
    rss = [value for value in ints("rss_kb") if value > 0]
    memory = ints("cgroup_memory_current")
    cpu = ints("cgroup_cpu_usec")
    event_failures = 0
    event_path = pathlib.Path(args.events)
    if event_path.exists():
        for line in event_path.read_text(encoding="utf-8").splitlines():
            try:
                event_failures += json.loads(line).get("ok") is False
            except json.JSONDecodeError:
                event_failures += 1

    product_dir = pathlib.Path(args.output).parent
    upstreams = {}
    for name in "abcde":
        path = product_dir / "snapshots" / "after" / f"upstream-{name}.json"
        if path.exists():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                accepts = data.get("tcp_accepts", 0)
                requests = data.get("data_requests", data.get("requests", 0))
                reused = data.get("reused_requests", 0)
                upstreams[name.upper()] = {
                    "data_requests": requests,
                    "health_requests": data.get("health_requests", 0),
                    "health_failures": data.get("health_failures", 0),
                    "tcp_accepts": accepts,
                    "connection_closes": data.get("connection_closes", 0),
                    "max_active_connections": data.get("max_active_connections", 0),
                    "reused_requests": reused,
                    "reuse_rate": reused / requests if requests else None,
                    "responses": data.get("responses", {}),
                    "drops": data.get("drops", 0),
                    "slow_requests": data.get("slow_requests", 0),
                }
            except (json.JSONDecodeError, OSError):
                upstreams[name.upper()] = {"error": "invalid snapshot"}

    stage_p99 = {}
    for index in range(1, 6):
        name = f"http_req_duration{{traffic:normal,load_stage:s{index}}}"
        stage_p99[f"s{index}"] = metric_value(metrics, name, "p(99)", None)

    http_requests = metric_value(metrics, "http_reqs", "count", 0)
    output = {
        "product": args.product,
        "measurement_duration_s": args.duration_s,
        "http_requests": http_requests,
        "http_rps": http_requests / args.duration_s,
        "k6_reported_http_rps": metric_value(metrics, "http_reqs", "rate", 0),
        "semantic_ok_rate": metric_value(metrics, "bench_semantic_ok", "value", None),
        "normal_p50_ms": metric_value(metrics, "http_req_duration{traffic:normal}", "med", None),
        "normal_p95_ms": metric_value(metrics, "http_req_duration{traffic:normal}", "p(95)", None),
        "normal_p99_ms": metric_value(metrics, "http_req_duration{traffic:normal}", "p(99)", None),
        "normal_stage_p99_ms": stage_p99,
        "contracts": {
            "normal": metric_value(metrics, "bench_normal_contract_ok", "value", None),
            "auth": metric_value(metrics, "bench_auth_contract_ok", "value", None),
            "waf": metric_value(metrics, "bench_waf_contract_ok", "value", None),
            "ban": metric_value(metrics, "bench_ban_contract_ok", "value", None),
            "rate": metric_value(metrics, "bench_rate_contract_ok", "value", None),
            "routing": metric_value(metrics, "bench_routing_contract_ok", "value", None),
            "websocket": metric_value(metrics, "bench_websocket_contract_ok", "value", None),
            "upstream_response": metric_value(metrics, "bench_upstream_contract_ok", "value", None),
            "forwarded_headers": metric_value(metrics, "bench_header_contract_ok", "value", None),
            "request_id": metric_value(metrics, "bench_request_id_contract_ok", "value", None),
        },
        "fault_available_rate": metric_value(metrics, "bench_fault_available", "value", None),
        "recovery_available_rate": metric_value(metrics, "bench_recovery_available", "value", None),
        "rate_limited": metric_value(metrics, "bench_rate_limited", "count", 0),
        "waf_blocked": metric_value(metrics, "bench_waf_blocked", "count", 0),
        "ban_blocked": metric_value(metrics, "bench_ban_blocked", "count", 0),
        "event_failures": event_failures,
        "resource_samples": len(rows),
        "pss_median_kb": statistics.median(pss) if pss else None,
        "pss_p95_kb": percentile(pss, 0.95),
        "rss_median_kb": statistics.median(rss) if rss else None,
        "rss_p95_kb": percentile(rss, 0.95),
        "memory_median_bytes": statistics.median(memory) if memory else None,
        "memory_peak_bytes": max(memory) if memory else None,
        "cgroup_cpu_seconds": ((max(cpu) - min(cpu)) / 1_000_000) if len(cpu) >= 2 else None,
        "upstreams": upstreams,
        "fault_evidence": {
            "c_503_responses": upstreams.get("C", {}).get("responses", {}).get("503", 0),
            "d_slow_requests": upstreams.get("D", {}).get("slow_requests", 0),
            "e_health_failures": upstreams.get("E", {}).get("health_failures", 0),
            "e_drops": upstreams.get("E", {}).get("drops", 0),
        },
    }
    pathlib.Path(args.output).write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
