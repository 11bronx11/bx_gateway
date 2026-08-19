#!/usr/bin/env python3
"""Render the frozen Kong configuration for one benchmark run."""

import argparse
import pathlib

import yaml


WEIGHTS = (500, 300, 200, 100, 100)


def common_plugins(args, include_rate):
    plugins = [
        {
            "name": "jwt",
            "config": {
                "key_claim_name": "iss",
                "claims_to_verify": ["exp"],
                "secret_is_base64": False,
                "run_on_preflight": True,
            },
        },
        {"name": "bench-security", "config": {"required_scope": "read"}},
        {
            "name": "correlation-id",
            "config": {
                "header_name": "X-Request-ID",
                "generator": "uuid#counter",
                "echo_downstream": True,
            },
        },
        {
            "name": "request-transformer",
            "config": {
                "add": {"headers": [f"X-Bench-Version:{args.version}"]},
                "remove": {"headers": ["X-Internal-Debug"]},
            },
        },
        {"name": "file-log", "config": {"path": "/evidence/logs/kong-access.log"}},
    ]
    if args.main_ip_deny:
        plugins.insert(0, {"name": "ip-restriction", "config": {"deny": [args.banned_ip]}})
    if include_rate:
        plugins.insert(2, {
            "name": "rate-limiting",
            "config": {
                "second": args.rate,
                "limit_by": "header",
                "header_name": "X-Forwarded-For",
                "policy": "local",
                "fault_tolerant": True,
                "hide_client_headers": False,
            },
        })
    return plugins


def service(name, upstream, paths, plugins, strip_path=True):
    return {
        "name": name,
        "protocol": "http",
        "host": upstream,
        "port": 80,
        "connect_timeout": 300,
        "read_timeout": 1000,
        "write_timeout": 1000,
        "retries": 0,
        "routes": [{
            "name": name + "-routes",
            "paths": paths,
            "methods": ["GET", "POST"],
            "strip_path": strip_path,
            "path_handling": "v0",
            "preserve_host": False,
        }],
        "plugins": plugins,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    for name in "abcde":
        parser.add_argument(f"--upstream-{name}", required=True)
    parser.add_argument("--jwt-secret", required=True)
    parser.add_argument("--jwt-issuer", required=True)
    parser.add_argument("--banned-ip", required=True)
    parser.add_argument("--rate", required=True, type=int)
    parser.add_argument("--version", required=True)
    parser.add_argument("--upstream-count", type=int, choices=(2, 5), default=5)
    parser.add_argument("--main-ip-deny", action="store_true")
    parser.add_argument("--banned", action="store_true")
    args = parser.parse_args()

    upstreams = [getattr(args, f"upstream_{name}") for name in "abcde"][:args.upstream_count]
    weights = (100, 100) if args.upstream_count == 2 else WEIGHTS
    governed = {
        "name": "bench-upstream",
        "algorithm": "least-connections",
        "slots": 1000,
        "healthchecks": {
            "threshold": 0,
            "active": {
                "type": "http",
                "http_path": "/_soak/health",
                "timeout": 0.3,
                "concurrency": 1,
                "healthy": {
                    "interval": 1,
                    "successes": 1,
                    "http_statuses": [200, 302],
                },
                "unhealthy": {
                    "interval": 1,
                    "http_failures": 2,
                    "tcp_failures": 2,
                    "timeouts": 2,
                    "http_statuses": [429, 500, 502, 503, 504],
                },
            },
            "passive": {
                "type": "http",
                "healthy": {"successes": 1, "http_statuses": [200, 302]},
                "unhealthy": {
                    "http_failures": 3,
                    "tcp_failures": 2,
                    "timeouts": 2,
                    "http_statuses": [500, 502, 503, 504],
                },
            },
        },
        "targets": [
            {"target": target, "weight": weight}
            for target, weight in zip(upstreams, weights)
        ],
    }
    ws_upstream = {
        "name": "bench-ws-upstream",
        "algorithm": "round-robin",
        "targets": [{"target": upstreams[0], "weight": 100}],
    }

    ban_plugins = common_plugins(args, include_rate=False)
    if args.banned and not args.main_ip_deny:
        ban_plugins.insert(0, {
            "name": "ip-restriction",
            "config": {"deny": [args.banned_ip]},
        })

    config = {
        "_format_version": "3.0",
        "_transform": True,
        "upstreams": [governed, ws_upstream],
        "services": [
            service(
                "bench-service",
                "bench-upstream",
                ["~/api(?:/|$)", "~/exact/[1-4]$", "~/route/(?:10|[1-9])(?:/|$)",
                 "~/probe/(?:rate|observe)(?:/|$)"],
                common_plugins(args, include_rate=True),
            ),
            service(
                "bench-ban-service",
                "bench-upstream",
                ["~/probe/ban(?:/|$)"],
                ban_plugins,
            ),
            {
                **service(
                    "bench-ws-service",
                    "bench-ws-upstream",
                    ["~/ws(?:/|$)"],
                    common_plugins(args, include_rate=False),
                ),
                "read_timeout": 60000,
                "write_timeout": 60000,
            },
        ],
        "plugins": [{
            "name": "prometheus",
            "config": {
                "status_code_metrics": True,
                "latency_metrics": True,
                "upstream_health_metrics": True,
            },
        }],
        "consumers": [{
            "username": "bench-user",
            "jwt_secrets": [{
                "key": args.jwt_issuer,
                "secret": args.jwt_secret,
                "algorithm": "HS256",
            }],
        }],
    }
    path = pathlib.Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")


if __name__ == "__main__":
    main()
