#!/usr/bin/env python3
"""Convert Bronx gateway trace logs to Chrome Trace Event JSON."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


PREFIX = re.compile(r"^\[c(\d+)(?:#(\d+))?\.(\d+)\]\s+(.*)$")
NUMBER = r"([0-9]+(?:\.[0-9]+)?)"


@dataclass
class Event:
    conn: int
    req: int | None
    seq: int
    kind: str
    data: dict[str, Any]
    raw: str
    thread_id: int
    thread: str
    fiber: int
    source: str


@dataclass
class ParseStats:
    skipped: int = 0
    shapes: Counter[str] = field(default_factory=Counter)

    def skip(self, text: str) -> None:
        self.skipped += 1
        shape = re.sub(r"[0-9]+(?:\.[0-9]+)?", "#", text.strip())
        self.shapes[shape[:160]] += 1


def as_int(value: str) -> int:
    try:
        return int(value)
    except ValueError:
        return 0


def parse_body(body: str) -> tuple[str, dict[str, Any]] | None:
    match = re.fullmatch(r"conn\s+open\s+peer=(\S+)\s+fd=(\d+)", body)
    if match:
        return "conn_open", {"peer": match.group(1), "fd": int(match.group(2))}

    match = re.fullmatch(r"conn\s+close\s+reqs=(\d+)", body)
    if match:
        return "conn_close", {"requests": int(match.group(1))}

    match = re.fullmatch(r"cli\s+recv\s+n=(\d+)\s+buf=(\d+)", body)
    if match:
        return "client_recv", {"bytes": int(match.group(1)), "buffer": int(match.group(2))}

    match = re.fullmatch(
        r"req\s+head\s+(\S+)\s+(\S+)\s+framing=(\S+)\s+clen=(\d+)\s+hdrs=(\d+)\s+ka=(\d+)",
        body,
    )
    if match:
        return "req_head", {
            "method": match.group(1),
            "path": match.group(2),
            "framing": match.group(3),
            "content_length": int(match.group(4)),
            "headers": int(match.group(5)),
            "keep_alive": bool(int(match.group(6))),
        }

    match = re.fullmatch(r"chain\s+start\s+mws=(\d+)", body)
    if match:
        return "chain_start", {"middlewares": int(match.group(1))}

    match = re.fullmatch(r"mw\s+(\d+)\s+(\S+)\s+->", body)
    if match:
        return "mw_enter", {"index": int(match.group(1)), "name": match.group(2)}

    match = re.fullmatch(
        rf"mw\s+(\d+)\s+(\S+)\s+(pass|SHORT|end)\s+status=(\d+)\s+self={NUMBER}ms\s+tot={NUMBER}ms",
        body,
    )
    if match:
        return "mw_exit", {
            "index": int(match.group(1)),
            "name": match.group(2),
            "verdict": match.group(3),
            "status": int(match.group(4)),
            "self_ms": float(match.group(5)),
            "total_ms": float(match.group(6)),
        }

    match = re.fullmatch(
        rf"chain\s+end\s+depth=(\d+)/(\d+)\s+status=(\d+)\s+state=(\S+)\s+cost={NUMBER}ms",
        body,
    )
    if match:
        return "chain_end", {
            "depth": int(match.group(1)),
            "middlewares": int(match.group(2)),
            "status": int(match.group(3)),
            "state": match.group(4),
            "cost_ms": float(match.group(5)),
        }

    match = re.fullmatch(rf"up\s+acquire\s+ep=(\S+)\s+reused=(\d+)\s+cost={NUMBER}ms", body)
    if match:
        return "up_acquire", {
            "endpoint": match.group(1),
            "reused": bool(int(match.group(2))),
            "cost_ms": float(match.group(3)),
        }

    match = re.fullmatch(rf"up\s+req\s+(.+?)\s+head=(\d+)B\s+cost={NUMBER}ms(?:\s+fresh_conn)?", body)
    if match:
        return "up_request", {
            "request": match.group(1),
            "header_bytes": int(match.group(2)),
            "cost_ms": float(match.group(3)),
            "fresh_connection": body.endswith(" fresh_conn"),
        }

    match = re.fullmatch(rf"up\s+rsp\s+(\d+)\s+ttfb={NUMBER}ms\s+(.+)", body)
    if match:
        return "up_response", {
            "status": int(match.group(1)),
            "cost_ms": float(match.group(2)),
            "detail": match.group(3),
        }

    match = re.fullmatch(rf"up\s+(?:finish|end)\s+(.+?)\s+cost={NUMBER}ms\s+(.+)", body)
    if match:
        return "up_finish", {
            "result": match.group(1),
            "cost_ms": float(match.group(2)),
            "detail": match.group(3),
        }

    match = re.fullmatch(r"cli\s+rsp\s+(\d+)\s+(.+)", body)
    if match:
        return "client_response", {"status": int(match.group(1)), "detail": match.group(2)}

    match = re.fullmatch(rf"cli\s+body#1\s+n=(\d+)\s+t={NUMBER}ms", body)
    if match:
        return "client_body_first", {"bytes": int(match.group(1)), "cost_ms": float(match.group(2))}

    match = re.fullmatch(rf"cli\s+body~\s+blk=(\d+)\s+bytes=(\d+)\s+t={NUMBER}ms", body)
    if match:
        return "client_body_progress", {
            "blocks": int(match.group(1)),
            "bytes": int(match.group(2)),
            "cost_ms": float(match.group(3)),
        }

    match = re.fullmatch(rf"cli\s+bodyend\s+blk=(\d+)\s+bytes=(\d+)\s+cost={NUMBER}ms\s+(\S+)", body)
    if match:
        return "client_body_end", {
            "blocks": int(match.group(1)),
            "bytes": int(match.group(2)),
            "cost_ms": float(match.group(3)),
            "result": match.group(4),
        }

    match = re.fullmatch(r"cli\s+body\s+recv\s+buf=(\d+)", body)
    if match:
        return "client_body_recv", {"buffer": int(match.group(1))}

    match = re.fullmatch(rf"req\s+done\s+status=(\d+)\s+cost={NUMBER}ms\s+cut=(\S+)", body)
    if match:
        return "req_done", {
            "status": int(match.group(1)),
            "cost_ms": float(match.group(2)),
            "cut": match.group(3),
        }

    return None


def parse_line(line: str, stats: ParseStats) -> Event | None:
    fields = line.rstrip("\n").split("\t", 7)
    if len(fields) != 8:
        stats.skip(line)
        return None
    if fields[5] != "[trace]":
        return None

    prefix = PREFIX.fullmatch(fields[7])
    if not prefix:
        stats.skip(fields[7])
        return None

    body = prefix.group(4)
    parsed = parse_body(body)
    if parsed is None:
        stats.skip(body)
        return None

    kind, data = parsed
    return Event(
        conn=int(prefix.group(1)),
        req=int(prefix.group(2)) if prefix.group(2) else None,
        seq=int(prefix.group(3)),
        kind=kind,
        data=data,
        raw=body,
        thread_id=as_int(fields[1]),
        thread=fields[2],
        fiber=as_int(fields[3]),
        source=fields[6],
    )


def micros(milliseconds: float) -> int:
    return max(1, round(milliseconds * 1000.0))


def event_args(event: Event) -> dict[str, Any]:
    args = dict(event.data)
    args.update({
        "src": event.source,
        "thread": event.thread,
        "thread_id": event.thread_id,
        "fiber": event.fiber,
        "seq": event.seq,
    })
    return args


def instant(event: Event, tid: int) -> dict[str, Any]:
    at_ms = float(event.data.get("cost_ms", 0.0))
    names = {
        "req_head": "request head",
        "up_acquire": "upstream acquire",
        "up_request": "upstream request",
        "up_response": "upstream response",
        "up_finish": "upstream finish",
        "client_recv": "client receive",
        "client_response": "client response",
        "client_body_first": "client first body",
        "client_body_progress": "client body progress",
        "client_body_end": "client body end",
        "client_body_recv": "client body receive",
        "req_done": "request done",
    }
    category = "up" if event.kind.startswith("up_") else "conn"
    return {
        "ph": "i",
        "s": "t",
        "pid": event.conn,
        "tid": tid,
        "ts": round(at_ms * 1000.0),
        "name": names.get(event.kind, event.kind),
        "cat": category,
        "args": event_args(event),
    }


def warn(conn: int, req: int, message: str) -> None:
    print(f"warning: c{conn}#{req}: {message}", file=sys.stderr)


def request_duration(events: list[Event]) -> float:
    chain = next((e for e in events if e.kind == "chain_end"), None)
    if chain:
        return float(chain.data["cost_ms"])
    totals = [float(e.data["total_ms"]) for e in events if e.kind == "mw_exit"]
    return max(totals, default=0.0)


def check_middlewares(conn: int, req: int, exits: list[Event]) -> tuple[int, int]:
    ordered = sorted(exits, key=lambda event: event.data["index"])
    checked = 0
    failures = 0
    for outer, inner in zip(ordered, ordered[1:]):
        actual = float(outer.data["total_ms"]) - float(inner.data["total_ms"])
        expected = float(outer.data["self_ms"])
        checked += 1
        if abs(actual - expected) > 0.01:
            failures += 1
            warn(conn, req, f"mw {outer.data['index']} identity failed: {actual:.6f} != {expected:.6f} ms")
    if ordered:
        inner = ordered[-1]
        checked += 1
        if abs(float(inner.data["total_ms"]) - float(inner.data["self_ms"])) > 0.01:
            failures += 1
            warn(conn, req, f"mw {inner.data['index']} innermost total != self")
    return checked, failures


def build_request(events: list[Event], do_check: bool) -> tuple[list[dict[str, Any]], int, int]:
    events = sorted(events, key=lambda event: event.seq)
    conn = events[0].conn
    req = events[0].req or 0
    starts = {e.data["index"]: e for e in events if e.kind == "mw_enter"}
    exits = [e for e in events if e.kind == "mw_exit"]
    chain_start = next((e for e in events if e.kind == "chain_start"), None)
    chain_end = next((e for e in events if e.kind == "chain_end"), None)

    stack: list[int] = []
    for event in events:
        if event.kind == "mw_enter":
            stack.append(event.data["index"])
        elif event.kind == "mw_exit" and stack:
            expected = stack.pop()
            if expected != event.data["index"]:
                warn(conn, req, f"middleware return {event.data['index']} did not match stack {expected}")
    if stack:
        warn(conn, req, f"{len(stack)} middleware enter event(s) have no return")

    duration_ms = request_duration(events)
    truncated = chain_end is None
    output: list[dict[str, Any]] = []
    if duration_ms > 0:
        chain_args: dict[str, Any] = {}
        if chain_start:
            chain_args["middlewares"] = chain_start.data["middlewares"]
        if chain_end:
            chain_args.update({
                "depth": f"{chain_end.data['depth']}/{chain_end.data['middlewares']}",
                "status": chain_end.data["status"],
                "state": chain_end.data["state"],
            })
        output.append({
            "ph": "X",
            "pid": conn,
            "tid": req,
            "ts": 0,
            "dur": micros(duration_ms),
            "name": "chain [truncated]" if truncated else "chain",
            "cat": "chain",
            "args": chain_args,
        })

    middleware_count = int(chain_start.data["middlewares"]) if chain_start else 0
    for event in sorted(exits, key=lambda item: item.data["index"]):
        data = event.data
        name = f"{data['index']} {data['name']}"
        if data["verdict"] == "SHORT" and middleware_count and data["index"] < middleware_count - 1:
            name += " [short]"
        args = event_args(event)
        start = starts.get(data["index"])
        if start:
            args["enter_src"] = start.source
            args["enter_thread"] = start.thread
        output.append({
            "ph": "X",
            "pid": conn,
            "tid": req,
            "ts": 0,
            "dur": micros(float(data["total_ms"])),
            "name": name,
            "cat": "mw",
            "args": args,
        })

    instant_kinds = {
        "req_head", "up_acquire", "up_request", "up_response", "up_finish",
        "client_recv", "client_response", "client_body_first", "client_body_progress",
        "client_body_end", "client_body_recv", "req_done",
    }
    output.extend(instant(event, req) for event in events if event.kind in instant_kinds)
    checked, failures = check_middlewares(conn, req, exits) if do_check else (0, 0)
    return output, checked, failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="gateway trace log")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Chrome Trace JSON output")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--conn", type=int, help="only emit one connection")
    selection.add_argument("--last", type=int, metavar="N", help="only emit the last N connections")
    parser.add_argument("--min-ms", type=float, default=0.0, help="only emit requests at least this slow")
    parser.add_argument("--check", action="store_true", help="verify total/self middleware identities")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.last is not None and args.last < 1:
        print("error: --last must be positive", file=sys.stderr)
        return 2
    if args.min_ms < 0:
        print("error: --min-ms cannot be negative", file=sys.stderr)
        return 2

    stats = ParseStats()
    events: list[Event] = []
    try:
        with args.input.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                event = parse_line(line, stats)
                if event:
                    events.append(event)
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    connections = sorted({event.conn for event in events})
    if args.conn is not None:
        selected = {args.conn}
    elif args.last is not None:
        selected = set(connections[-args.last:])
    else:
        selected = set(connections)

    by_request: dict[tuple[int, int], list[Event]] = defaultdict(list)
    by_connection: dict[int, list[Event]] = defaultdict(list)
    for event in events:
        if event.conn not in selected:
            continue
        if event.req is None:
            by_connection[event.conn].append(event)
        else:
            by_request[(event.conn, event.req)].append(event)

    by_request = {
        key: value for key, value in by_request.items()
        if request_duration(value) >= args.min_ms
    }
    selected = {conn for conn, _ in by_request} | ({conn for conn in selected if conn in by_connection} if args.min_ms == 0 else set())

    trace: list[dict[str, Any]] = []
    for conn in sorted(selected):
        conn_events = sorted(by_connection.get(conn, []), key=lambda event: event.seq)
        opened = next((event for event in conn_events if event.kind == "conn_open"), None)
        process_name = f"conn {conn}"
        if opened:
            process_name += f" peer={opened.data['peer']}"
        trace.append({"ph": "M", "name": "process_name", "pid": conn, "args": {"name": process_name}})
        trace.append({"ph": "M", "name": "thread_name", "pid": conn, "tid": 0, "args": {"name": "conn"}})

    checked = 0
    failures = 0
    request_count = 0
    for (conn, req), req_events in sorted(by_request.items()):
        request_count += 1
        head = next((event for event in req_events if event.kind == "req_head"), None)
        lane = f"req {req}"
        if head:
            lane += f" {head.data['method']} {head.data['path']}"
        trace.append({"ph": "M", "name": "thread_name", "pid": conn, "tid": req, "args": {"name": lane}})
        built, req_checked, req_failures = build_request(req_events, args.check)
        trace.extend(built)
        checked += req_checked
        failures += req_failures

    max_duration: dict[int, int] = defaultdict(int)
    for item in trace:
        if item.get("ph") == "X":
            max_duration[int(item["pid"])] = max(max_duration[int(item["pid"])], int(item["dur"]))
    for conn in sorted(selected):
        for event in sorted(by_connection.get(conn, []), key=lambda item: item.seq):
            if event.kind not in {"conn_open", "conn_close", "client_recv"}:
                continue
            at = max_duration[conn] if event.kind == "conn_close" else 0
            trace.append({
                "ph": "i",
                "s": "t",
                "pid": conn,
                "tid": 0,
                "ts": at,
                "name": event.kind.replace("_", " "),
                "cat": "conn",
                "args": event_args(event),
            })

    output = {"traceEvents": trace, "displayTimeUnit": "ms"}
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", encoding="utf-8") as stream:
            json.dump(output, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"wrote {len(trace)} events for {request_count} request(s) in {len(selected)} connection(s)", file=sys.stderr)
    print(f"skipped {stats.skipped} lines ({len(stats.shapes)} distinct shapes)", file=sys.stderr)
    if stats.shapes:
        for shape, count in stats.shapes.most_common(5):
            print(f"  {count}x {shape}", file=sys.stderr)
    if args.check:
        print(f"check: {checked} identities, {failures} failure(s)", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
