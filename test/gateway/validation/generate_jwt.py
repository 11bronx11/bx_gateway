#!/usr/bin/env python3
# generate_jwt.py — 生成各类测试 JWT（HS256 + 攻击变体），无第三方依赖。
# 用法: python3 generate_jwt.py <kind> [secret]
#   kind: valid | wrongsecret | tampered_payload | tampered_header | algnone |
#         emptysig | rs256 | expired | nbf_future | malformed | nosub | customclaims
import sys, json, hmac, hashlib, base64, time

SECRET = sys.argv[2] if len(sys.argv) > 2 else "test-secret-do-not-use-in-prod"

def b64(d):
    if isinstance(d, (dict, list)): d = json.dumps(d, separators=(",",":")).encode()
    elif isinstance(d, str): d = d.encode()
    return base64.urlsafe_b64encode(d).rstrip(b"=").decode()

def sign(header, payload, secret):
    h, p = b64(header), b64(payload)
    sig = hmac.new(secret.encode(), f"{h}.{p}".encode(), hashlib.sha256).digest()
    return f"{h}.{p}.{b64(sig)}"

now = int(time.time())
kind = sys.argv[1] if len(sys.argv) > 1 else "valid"

if kind == "valid":
    print(sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","iat":now,"exp":now+3600}, SECRET))
elif kind == "wrongsecret":
    print(sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","exp":now+3600}, "wrong-secret"))
elif kind == "tampered_payload":
    t = sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","exp":now+3600}, SECRET)
    h,p,s = t.split(".");
    p2 = b64({"sub":"admin","exp":now+3600})   # 改 payload 不重签
    print(f"{h}.{p2}.{s}")
elif kind == "tampered_header":
    t = sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","exp":now+3600}, SECRET)
    h,p,s = t.split(".")
    h2 = b64({"alg":"HS256","typ":"JWT","x":"tampered"})
    print(f"{h2}.{p}.{s}")
elif kind == "algnone":
    h = b64({"alg":"none","typ":"JWT"}); p = b64({"sub":"admin","exp":now+3600})
    print(f"{h}.{p}.")            # alg=none 空签名
elif kind == "emptysig":
    h = b64({"alg":"HS256","typ":"JWT"}); p = b64({"sub":"user-a","exp":now+3600})
    print(f"{h}.{p}.")
elif kind == "rs256":
    print(sign({"alg":"RS256","typ":"JWT"}, {"sub":"user-a","exp":now+3600}, SECRET))
elif kind == "expired":
    print(sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","iat":now-7200,"exp":now-3600}, SECRET))
elif kind == "nbf_future":
    print(sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-a","nbf":now+3600,"exp":now+7200}, SECRET))
elif kind == "malformed":
    print("not.a.valid.jwt.token")
elif kind == "nosub":
    print(sign({"alg":"HS256","typ":"JWT"}, {"iat":now,"exp":now+3600}, SECRET))
elif kind == "customclaims":
    print(sign({"alg":"HS256","typ":"JWT"}, {"sub":"user-b","role":"admin","tier":"gold","exp":now+3600}, SECRET))
else:
    print(f"unknown kind: {kind}", file=sys.stderr); sys.exit(1)
