#!/usr/bin/env python3
# test_load_balancing.py — weighted / least-conn 分布验证
import sys, json, urllib.request, concurrent.futures, collections, time

G = "http://127.0.0.1:18090"

def get(path, headers=None, timeout=5):
    req = urllib.request.Request(G+path, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())

def test_weighted(n=1000):
    cnt = collections.Counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=20) as ex:
        futs = [ex.submit(get, "/weighted/x") for _ in range(n)]
        for f in futs:
            try: cnt[f.result()["endpoint"]] += 1
            except Exception as e: cnt["ERR"] += 1
    total = sum(v for k,v in cnt.items() if k!="ERR")
    print(f"=== Weighted 5:3:2, n={n} ===")
    exp = {"ep-A":0.5,"ep-B":0.3,"ep-C":0.2}
    for ep in ["ep-A","ep-B","ep-C"]:
        got = cnt[ep]; pct = got/total*100 if total else 0
        ep_exp = exp[ep]*100; dev = abs(pct-ep_exp)
        print(f"  {ep}: {got:4d} ({pct:5.1f}%) 期望{ep_exp:.0f}% 偏差{dev:.1f}pp")
    print(f"  ERR={cnt['ERR']}")
    # 判定: 偏差 < 5pp
    ok = all(abs(cnt[e]/total - exp[e]) < 0.05 for e in exp) if total else False
    print(f"  RESULT: {'PASS' if ok else 'FAIL'}")

def test_leastconn():
    # A 延迟2s, B 延迟0.5s, C 快速; 并发发, 新请求应优先 C(活跃连接少)
    print("=== Least-conn: A=2s B=0.5s C=fast, 并发30 ===")
    def slow(ep_delay):
        ep, d = ep_delay
        return get("/leastconn/x", headers={"X-Mock-Delay": str(d)})
    cnt = collections.Counter()
    # 先打满 A 和 B 的慢请求, 再发快请求看落点
    with concurrent.futures.ThreadPoolExecutor(max_workers=30) as ex:
        futs = []
        # 30 并发, 全部带不同延迟; least-conn 应让快的 C 承接更多
        for i in range(30):
            futs.append(ex.submit(get, "/leastconn/x", {"X-Mock-Delay":"100"}))
        for f in futs:
            try: cnt[f.result()["endpoint"]] += 1
            except Exception: cnt["ERR"]+=1
    print(f"  分布: {dict(cnt)}")
    print(f"  (least-conn 下应大致均衡, 不应全压一个; 故障端点不应被选)")

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv)>1 else "all"
    if which in ("weighted","all"): test_weighted(int(sys.argv[2]) if len(sys.argv)>2 else 1000)
    if which in ("leastconn","all"): test_leastconn()
