import asyncio, sys

# 串包检测：每个请求带唯一 ID，上游回显该 ID。
# 复用连接下若定界错误，会读到别的请求的响应 → ID 不匹配。
HOST, PORT = "127.0.0.1", 8090
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
CONC = int(sys.argv[2]) if len(sys.argv) > 2 else 50

mismatch = 0
ok = 0
err = 0
lock = asyncio.Lock()
counter = 0

async def one(rid):
    global mismatch, ok, err
    path = f"/api/echo?id={rid}"
    req = (f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           f"X-Echo-Id: {rid}\r\nConnection: close\r\n\r\n").encode()
    try:
        r, w = await asyncio.wait_for(asyncio.open_connection(HOST, PORT), timeout=5)
        w.write(req); await w.drain()
        data = await asyncio.wait_for(r.read(), timeout=5)
        w.close()
        try: await w.wait_closed()
        except Exception: pass
        body = data.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in data else b""
        # 上游回显 X-Echo-Id 的值在 body 里
        if str(rid).encode() in body:
            ok += 1
        else:
            mismatch += 1
            if mismatch <= 5:
                print(f"MISMATCH rid={rid} body={body[:60]!r}")
    except Exception as e:
        err += 1

async def worker():
    global counter
    while True:
        async with lock:
            if counter >= N: return
            rid = counter; counter += 1
        await one(rid)

async def main():
    await asyncio.gather(*[worker() for _ in range(CONC)])

asyncio.run(main())
print(f"total={N} ok={ok} mismatch={mismatch} err={err}")
print("RESULT:", "PASS (no串包)" if mismatch == 0 else f"FAIL ({mismatch} 串包!)")
