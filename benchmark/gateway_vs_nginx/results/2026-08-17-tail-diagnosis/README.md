# 尾延迟控制组

日期：2026-08-17 UTC  
Commit：`9bfa8d6e12cd689ec237c7b4bb72dc1c77e05a5e`

本目录保存报告“尾延迟复查”使用的紧凑汇总。测试继续使用 AArch64 4 vCPU 主机、
`RelWithDebInfo`、Vegeta 12.13.0、HTTP/1.1 keep-alive、256 initial workers 和 1024
connections。所有路径均为 loopback，目标为 29 字节静态响应。

## 测试命令

分离 Bronx 业务与辅助线程的 20 秒测试：

```bash
OUT_DIR=/tmp/bronx-tail-affinity-20260817 \
PROXIES=bronx BRONX_RATES=8000 ROUNDS=3 DURATION_SECONDS=20 \
WARMUP_SECONDS=3 RUN_DIRECT=0 KEEP_RAW=1 KEEP_WORK=1 \
LOAD_CPUS=0 PROXY_CPU=1-2 UPSTREAM_CPU=3 IO_WORKERS=1 \
./benchmark/gateway_vs_nginx/run.sh
```

网关启动后，将 `iom_0` 固定到 CPU 2，其余 Bronx 线程固定到 CPU 1。第一轮发生在
调整完成前，因此 `summary.csv` 只保留有效的第二、三轮。

60 秒 Bronx 样本使用相同负载参数，进程允许在 CPU 1-2 上运行。直打与 Nginx
控制组命令为：

```bash
OUT_DIR=/tmp/bronx-tail-controls-20260817 \
PROXIES=nginx NGINX_RATES=8000 ROUNDS=1 DURATION_SECONDS=60 \
WARMUP_SECONDS=3 RUN_DIRECT=1 DIRECT_RATE=8000 KEEP_RAW=1 \
LOAD_CPUS=0 PROXY_CPU=2 UPSTREAM_CPU=3 IO_WORKERS=1 \
./benchmark/gateway_vs_nginx/run.sh
```

## 解释边界

- `thread-pidstat.txt` 是分核后第三轮中连续 10 秒的逐线程 CPU、调度等待和上下文切换
  采样；`system-journal.txt` 保留了与 18:18:10 长尾窗口重合的系统定时采集日志。
- 三条 60 秒路径是顺序执行，不是同一时间的配对样本，不能据此计算严格的代理间
  P99 差值。
- 直打与 Nginx 控制组均出现 200ms 以上最大延迟，足以否定“所有长尾都来自 Bronx”。
- 原始 Vegeta 数据保留在测试机 `/tmp/bronx-tail-*`；本目录只保留适合 Git 的紧凑
  汇总。报告中的容量阶梯仍以原结果目录为准。
- `perf stat`/`perf sched` 因 `kernel.perf_event_paranoid=4` 被拒绝，本轮没有内核调度
  trace。Bronx 内部队列放大仍是待埋点验证的假设，不是已确认根因。
