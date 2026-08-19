#!/usr/bin/env python3
"""Render Spring Cloud Gateway RouteDefinition payloads used by the adapter."""

import argparse
import json
import pathlib


def predicate(predicate_name, **args):
    return {"name": predicate_name, "args": {key: str(value) for key, value in args.items()}}


def filter_definition(filter_name, **args):
    return {"name": filter_name, "args": {key: str(value) for key, value in args.items()}}


def governed_route(route_id, path, strip_parts, uri="lb://bench-api"):
    paths = path if isinstance(path, list) else [path]
    return {
        "id": route_id,
        "uri": uri,
        "order": -100,
        "predicates": [predicate(
            "Path", **{f"_genkey_{index}": value for index, value in enumerate(paths)}
        )],
        "filters": [
            filter_definition("StripPrefix", parts=strip_parts),
            filter_definition(
                "CircuitBreaker",
                name="bench-api",
                fallbackUri="forward:/__bench/fallback",
                statusCodes="500,502,503,504",
            ),
            filter_definition(
                "RequestRateLimiter",
                **{
                    "rate-limiter": "#{@bucket4jRateLimiter}",
                    "key-resolver": "#{@clientIpKeyResolver}",
                    "status-code": "TOO_MANY_REQUESTS",
                },
            ),
            filter_definition("SetRequestHeader", name="X-Bench-Version", value="v2"),
            filter_definition("RemoveRequestHeader", name="X-Internal-Debug"),
        ],
    }


def simple_route(route_id, path, strip_parts, uri):
    return {
        "id": route_id,
        "uri": uri,
        "order": -100,
        "predicates": [predicate("Path", _genkey_0=path)],
        "filters": [
            filter_definition("StripPrefix", parts=strip_parts),
            filter_definition("SetRequestHeader", name="X-Bench-Version", value="v2"),
            filter_definition("RemoveRequestHeader", name="X-Internal-Debug"),
        ],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--upstream-a", required=True)
    parser.add_argument("--banned-ip", required=True)
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    routes = [
        governed_route("spring-v2-api", "/api/**", 1),
        governed_route("spring-v2-probes", ["/probe/rate/**", "/probe/observe/**"], 2),
        governed_route("spring-v2-exact", "/exact/**", 2),
        governed_route("spring-v2-route", "/route/**", 2),
        simple_route("spring-v2-ban", "/probe/ban/**", 2, "lb://bench-api"),
        simple_route("spring-v2-ws", "/ws/**", 1, f"ws://{args.upstream_a}"),
    ]
    ban = {
        "id": "spring-dynamic-ban",
        "uri": "no://op",
        "order": -1000,
        "predicates": [
            predicate("Path", _genkey_0="/probe/ban/**"),
            predicate(
                "XForwardedRemoteAddr",
                maxTrustedIndex=1,
                sources=f"{args.banned_ip}/32",
            ),
        ],
        "filters": [filter_definition("SetStatus", status="403")],
    }
    capacity_ip_policy = {
        "id": "spring-capacity-ip-policy",
        "uri": "no://op",
        "order": -1000,
        "predicates": [
            predicate("Path", _genkey_0="/**"),
            predicate(
                "XForwardedRemoteAddr",
                maxTrustedIndex=1,
                sources=f"{args.banned_ip}/32",
            ),
        ],
        "filters": [filter_definition("SetStatus", status="403")],
    }
    invalid = {
        "id": "spring-invalid-route",
        "uri": "lb://bench-api",
        "order": -2000,
        "predicates": [predicate("Path", _genkey_0="/api/**")],
        "filters": [filter_definition("DefinitelyNotARealGatewayFilter")],
    }

    for route in routes + [ban, capacity_ip_policy, invalid]:
        path = output / f"{route['id']}.json"
        path.write_text(json.dumps(route, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
