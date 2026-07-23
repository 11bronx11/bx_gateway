import asyncio, sys, time

HOST, PORT = "127.0.0.1", 8090
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 20
CONCURRENCY = int(sys.argv[2]) if len(sys.argv) > 2 else 100
PATH = sys.argv[3] if len(sys.argv) > 3 else "/api/x"

stats = {"ok": 0, "non2xx": 0, "err": 0, "bytes": 0}
latencies = []
stop = False

REQ = (f"GET {PATH} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       "Connection: close\r\n\r\n").encode()

async def worker():
    global stop
    while not stop:
        t0 = time.monotonic()
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(HOST, PORT), timeout=5)
            w.write(REQ)
            await w.drain()
            data = await asyncio.wait_for(r.read(), timeout=5)
            w.close()
            try:
                await w.wait_closed()
            except Exception:
                pass
            latencies.append((time.monotonic() - t0) * 1000)
            stats["bytes"] += len(data)
            if b" 200 " in data[:20] or data[:12].endswith(b"200"):
                stats["ok"] += 1
            else:
                stats["non2xx"] += 1
        except Exception:
            stats["err"] += 1

async def main():
    global stop
    workers = [asyncio.create_task(worker()) for _ in range(CONCURRENCY)]
    await asyncio.sleep(DURATION)
    stop = True
    await asyncio.gather(*workers, return_exceptions=True)

t0 = time.monotonic()
asyncio.run(main())
elapsed = time.monotonic() - t0
total = stats["ok"] + stats["non2xx"] + stats["err"]
latencies.sort()
def pct(p):
    if not latencies: return 0
    return latencies[min(len(latencies)-1, int(len(latencies)*p))]
print(f"duration={elapsed:.1f}s concurrency={CONCURRENCY} path={PATH}")
print(f"total={total} ok={stats['ok']} non2xx={stats['non2xx']} err={stats['err']}")
print(f"RPS={total/elapsed:.0f}")
if latencies:
    print(f"lat ms: p50={pct(0.5):.1f} p90={pct(0.9):.1f} p99={pct(0.99):.1f} max={latencies[-1]:.1f}")
