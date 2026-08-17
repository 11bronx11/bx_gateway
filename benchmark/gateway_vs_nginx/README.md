# Gateway vs Nginx benchmark

这里保存 Bronx 与 Nginx HTTP/1.1 反向代理的可复现容量测试。正式结论、方法和
证据边界见 [2026-08-17 压测报告](REPORT_2026-08-17.md)。

## 测量内容

- Vegeta 开环固定速率，记录实际 QPS、成功率、P50/P95/P99 和最大延迟。
- `pidstat` 同时记录代理、静态上游和负载发生器 CPU，避免把外部瓶颈归因给代理。
- 单核对比将负载发生器、代理、上游分配到不同 CPU；每个正式档位运行 12 秒、重复 3 轮。
- 结果目录保存配置快照、构建信息、二进制 SHA-256、逐轮 JSON/CPU 采样和全目录校验清单。

“确认稳定交付”要求同一档位三轮都满足：HTTP 成功率 100%，实际发送率不低于目标的
99%。P99 单独报告，不用成功率掩盖排队和长尾。

## 复现

依赖 CMake、GCC、Nginx、Vegeta、`jq`、`pidstat`、`taskset`、`curl` 和至少 4 个在线 CPU。
脚本从 Git `HEAD` 导出临时源码并独立构建，不读取或修改 checkout 中的
`api_gw/bin/*.yml`。

```bash
./benchmark/gateway_vs_nginx/run.sh
```

缩小或扩大阶梯：

```bash
ROUNDS=3 DURATION_SECONDS=12 \
BRONX_RATES="10000 12000 14000 16000" \
NGINX_RATES="40000 42000 44000 46000" \
OUT_DIR=benchmark/gateway_vs_nginx/runs/manual \
./benchmark/gateway_vs_nginx/run.sh
```

常用参数见 `./benchmark/gateway_vs_nginx/run.sh --help`。默认不保留体积很大的 Vegeta
二进制流；设置 `KEEP_RAW=1` 可保存压缩后的 `.bin.gz`。JSON 报告已经包含请求数、
实际速率、状态码、延迟分位和错误类型。

## 证据

- [单核宽阶梯](results/2026-08-17-one-core/summary.csv)
- [单核边界复测](results/2026-08-17-one-core-refine/summary.csv)
- [双核正式对比](results/2026-08-17-two-core/summary.csv)

每个新脚本结果目录都包含 `manifest.sha256`，可在对应目录执行
`sha256sum -c manifest.sha256` 验证。双核证据是早先同日测试的精简归档，只保留报告
引用的 JSON、CPU 采样和配置，不保留 3.84 GiB 原始 `.bin`。
