#!/usr/bin/env python3
"""Generate the four JWT variants used by the Bronx gateway demo."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import time
from typing import Any


SECRET = b"bronx-demo-secret-do-not-use-in-prod"


def encode(value: dict[str, Any]) -> str:
    raw = json.dumps(value, separators=(",", ":"), sort_keys=True).encode()
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def make_token(kind: str) -> str:
    now = int(time.time())
    header = {"alg": "HS256", "typ": "JWT"}
    payload: dict[str, Any] = {
        "iss": "bronx",
        "sub": "demo-user",
        "iat": now,
        "exp": now + 3600,
        "scope": "read",
    }

    if kind == "expired":
        payload["iat"] = now - 7200
        payload["exp"] = now - 3600
    elif kind == "alg-none":
        header["alg"] = "none"
    elif kind == "no-scope":
        payload["scope"] = "write"

    signing_input = f"{encode(header)}.{encode(payload)}"
    if kind == "alg-none":
        return f"{signing_input}."
    signature = hmac.new(SECRET, signing_input.encode(), hashlib.sha256).digest()
    encoded_signature = base64.urlsafe_b64encode(signature).rstrip(b"=").decode()
    return f"{signing_input}.{encoded_signature}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "kind",
        nargs="?",
        choices=("good", "expired", "alg-none", "no-scope"),
        help="print one token; without this argument print all shell assignments",
    )
    args = parser.parse_args()
    if args.kind:
        print(make_token(args.kind))
        return 0

    for name, kind in (
        ("GOOD", "good"),
        ("EXPIRED", "expired"),
        ("ALG_NONE", "alg-none"),
        ("NO_SCOPE", "no-scope"),
    ):
        print(f"{name}={make_token(kind)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
