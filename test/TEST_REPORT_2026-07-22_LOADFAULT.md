# 2026-07-22 开环压测 + 故障注入测试报告

## 结论

用 vegeta 恒速率开环发压，拿到诚实的 p50/p99/p999（替掉 Python 闭环的协调遗漏假数），
再用 toxiproxy 在网关到上游那一跳注入确定性故障（延迟 / 断连 / 限带宽），观察熔断三态、
连接池驱逐和降级恢复。四个故障场景行为全部符合预期，baseline 成功率 100%。

这是本机 loopback 环境、最高 200 rps 的结果，未找系统饱和点，不替代生产网络条件或
7×24 长稳证明。

## 运行配置

```bash
# 正式跑(默认:baseline 60s, fault 30s, recovery 20s)
LF_GW_BIN=bin/gw bash test/loadfault/run_loadfault.sh

# 可调参数(全部可用 LF_* 环境变量覆盖)
# LF_BASELINE_RPS=200  LF_BASELINE_SECS=60
# LF_FAULT_RPS=100     LF_FAULT_SECS=30
# LF_RECOVERY_SECS=20
```

**拓扑**：
```
vegeta(恒速率开环) → 网关:GW_PORT → toxiproxy:TOXI_PORT → mock_upstream.py:MOCK_PORT
                                    ↑ 故障注在这一跳
```

**工具**：vegeta v12.13（`~/.local/bin/vegeta`）+ toxiproxy v2.12
（`~/.local/bin/toxiproxy-server/cli`），均 aarch64 静态二进制，免 sudo 免 Docker。

脚本在 `mktemp -d /tmp/loadfault.*` 创建独立运行根：把 `gw` 拷进 `run_dir/bin/gw`，
写最小 `bronx.yml` + `gateway.yml`，端口全部动态分配。anchorRoot 读 `/proc/self/exe`
上溯两级当 root，读隔离目录的配置，不使用 checkout 的运行时配置或数据库。

熔断配置：`slow_ms=1000`、`failure_threshold=5`、`min_requests=10`、`failure_rate=50`、
`open_timeout_ms=3000`、`half_open_max_requests=1`、`half_open_successes=1`、
`failure_statuses=[500,502,503,504]`。

## 四场景与结果

| 场景 | toxiproxy 故障 | 证明什么 |
|---|---|---|
| baseline | 无 | 诚实 p50/p99/p999,替掉闭环假数 |
| latency_inject | +1500ms 延迟(jitter 100) | 慢熔断触发(slow_ms=1000),`circuit_open` 计数 |
| upstream_down | timeout=0 立刻切断连接 | 熔断 OPEN→HALF_OPEN→CLOSED 全周期 |
| bandwidth_limit | 限速 1KB/s | 连接池驱逐 + 超时路径,降级后恢复 |

| 场景 | 成功率 | p50 | p99 | `circuit_open` |
|---|---:|---:|---:|---:|
| baseline（200rps） | 100% | 约 42ms | 约 45ms | 0 |
| latency_inject（+1500ms） | 100% | 1490ms | 1600ms | 93 |
| upstream_down | 0%（符合该故障预期） | 4000ms | 4008ms | 892 |
| bandwidth_limit | 96.4% | 180ms | 184ms | 910 |
| post_recovery（200rps） | 100% | 1.3ms | 3.3ms | — |

**读法**：
- baseline 是诚实开环基线，p99 约 45ms，成功率 100%（脚本对 baseline < 99% 会非零退出）。
- latency_inject 注 +1500ms 后 p99 抬到 1600ms，跨过 `slow_ms=1000` 触发慢熔断，
  `circuit_open` 累积 93。
- upstream_down 直接切断，成功率 0% 是预期——它证明的是熔断 OPEN→HALF_OPEN→CLOSED
  全周期能跑起来（`circuit_open` 累积 892），不是可用性指标。
- bandwidth_limit 限速 1KB/s 触发连接池驱逐和超时路径，成功率仍有 96.4%。
- post_recovery 故障全部解除后，p99 回落到 3.3ms、成功率 100%，证明降级后干净恢复。

## 证据产物

原始产物在脚本运行的 `/tmp/loadfault.*` 目录（脚本 trap 清理，需保留请在跑前改）：

- `*.bin` —— vegeta raw,可用 `vegeta report -type=hdrplot < baseline.bin` 出 HDR 直方图
- `*.txt` —— 每场景 text 报告
- `stats: circuit_open=N ...` —— 每场景后从 `/stats` 拉的熔断计数快照

## 已知边界

- 最高验证 200 rps，未找系统饱和点（极限吞吐未测）。
- 本机 loopback 环境；生产网络延迟、丢包和 TLS 前置层需独立数据。
- 故障注在网关↔上游那一跳；客户端↔网关那一跳的故障（慢客户端 / slowloris）由
  k6 的 `12_slow_attack.js`、`14_slowloris.js` 覆盖。
