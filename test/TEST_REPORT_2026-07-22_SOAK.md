# 2026-07-22 API Gateway 五分钟浸泡测试报告

## 结论

完整流程通过。测试在隔离运行根目录 `/tmp/bronx_api_gw_soak.Pq9zPB` 中启动真实
`gw`、`hub`、`banctl` 和三台 mock upstream；300 秒基线完成 23,773 次请求，传输错误为 0。
多上游均衡、上游故障熔断、故障恢复、限流/IP-ban、热重载、日志轮转、优雅停机和资源采样均执行。

这是本机 loopback 环境的 5 分钟结果，不替代生产网络条件、容量上限或 7x24 稳定性证明。

## 运行配置

```bash
API_GW_SOAK_GW_BIN="$PWD/bin/gw" \
API_GW_SOAK_HUB_BIN="$PWD/bin/hub" \
API_GW_SOAK_BANCTL_BIN="$PWD/bin/banctl" \
SOAK_PROBE_ONLY=0 \
SOAK_SECONDS=300 \
SOAK_FAULT_SECONDS=10 \
SOAK_RATE_SECONDS=5 \
SOAK_RECOVERY_SECONDS=5 \
SOAK_MULTI_UPSTREAM_SECONDS=15 \
SOAK_RELOAD_SECONDS=3 \
SOAK_LOG_RELOAD_SECONDS=5 \
SOAK_MONITOR_SECONDS=2 \
bash test/api_gw/run_soak.sh
```

网关 `api` upstream group 含 A、B、C 三个等权 endpoint，并开启主动健康检查。运行时 YAML、UDS、
端口、日志和 SQLite 都由脚本在隔离目录创建；正式 `api_gw/bin/gateway.yml` 没有被测试修改。

## 基线结果

| 指标 | 结果 |
|---|---:|
| 时长 | 300.00s |
| worker | 20 |
| 目标 QPS / 实际 QPS | 80 / 79.24 |
| 总请求 | 23,773 |
| 传输错误 | 0 |
| p50 / p90 / p99 | 15.05ms / 31.11ms / 46.27ms |
| 最大响应时间 | 162.43ms |

| 流量类型 | 数量和结果 |
|---|---|
| JWT 正常请求 | 14,365 个 200 |
| 缺少 Bearer | 3,474 个 401 |
| JWT POST | 2,339 个 200 |
| 未命中路由 | 1,928 个 404 |
| 伪造 JWT | 959 个 401 |
| 主动断连 | 708 次请求已发送 |

## 多上游和故障恢复

| 阶段 | 结果 |
|---|---|
| 多上游预检 | 15.00s、20 worker、目标 90 QPS；1,350/1,350 为 200，A/B/C 分别处理 450/450/450 请求，p99 44.28ms，传输错误 0。 |
| 突发负载 | 10.01s 内 1,600 个 JWT 请求全部 200，159.82 QPS，p99 42.71ms，传输错误 0。 |
| A 故障注入 | 10.00s 内返回 380 个 200、20 个预期 503；A 熔断器进入 open，B/C 分别承接 180/179 请求。 |
| 熔断恢复 | 5.00s 内 400/400 为 200；A/B/C 熔断器均恢复为 closed。 |
| 传输故障 | 10.01s 内返回 380 个 200、4 个 502、16 个 504，均为预期网关响应；benchmark 传输错误为 0。 |
| 传输恢复 | 5.01s 内 400/400 为 200，传输错误为 0。 |

## 控制面和资源

| 检查 | 结果 |
|---|---|
| `gateway-features.json` | 67/67，失败 0 |
| `ipban-rate.json` | 8/8，失败 0 |
| `ipban-persist-seed.json` | 4/4，失败 0 |
| `ipban-persist-verify.json` | 6/6，失败 0 |
| 探针合计 | 85/85，失败 0 |
| gw FD | 18 -> 24，峰值 58 |
| gw RSS | 13,692KB -> 14,560KB，峰值 15,028KB |
| hub FD / RSS | 17 -> 17；7,900KB -> 7,936KB |
| 线程数 | gw 11，hub 8，未变化 |

主动断连场景使 mock 记录 `ConnectionResetError`：A/B/C 分别为 96/68/89 条。这些记录与客户端
提前关闭连接一致，不是 gateway 崩溃或 benchmark 传输失败。

## 配置、清理和证据

热重载期间仅改写
`/tmp/bronx_api_gw_soak.Pq9zPB/root/api_gw/bin/gateway.yml`，其备份
`/tmp/bronx_api_gw_soak.Pq9zPB/gateway.yml.bak` 已在测试结束前恢复。正式配置未被修改，因此无需
对正式 YAML 进行备份或还原。

本轮启动的 gw、hub 和三台 mock 均已退出；不属于本轮的 `test/api_gw/mock_rich.py --port 8080`
进程保持运行。

原始产物目录：`/tmp/bronx_api_gw_soak.Pq9zPB`。关键文件包括：

- `bench-baseline.json`、`bench-multi-upstream.json`
- `bench-circuit-fault.json`、`bench-circuit-recovery.json`
- `stats-after-circuit.json`、`stats-circuit-recovered.json`
- `gateway-features.json`、`ipban-rate.json`、`ipban-persist-seed.json`、`ipban-persist-verify.json`
- `resources.csv`、`gw.log`、`hub.log`
