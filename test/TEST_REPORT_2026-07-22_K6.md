# 2026-07-22 k6 健康混合运行报告

测试日期：2026-07-22

## 结论

真实 `gw`、`hub` 和两个受控 upstream 连续运行 15 分钟。k6 完成 50,468 个 HTTP 请求和
41,912 次迭代，88,327 项检查全部通过，零请求失败、零中断迭代。网关没有 5xx、上游失败或熔断；
Hub 到网关的 IP-ban 同步链路保持 `open`。

这是增强健康混合运行的结果，不表示已完成 1 小时长稳，也不替代熔断故障、热重载或 Hub 重启专题。

## 运行对象

| 组件 | 本轮地址或版本 |
|---|---|
| Gateway | 真实 `bin/gw`，业务 `127.0.0.1:8090`，admin `127.0.0.1:9090` |
| Hub | 真实 `bin/hub`，HTTP `127.0.0.1:9091` |
| Upstream | 两个独立 `mock_rich.py`，`127.0.0.1:18080`、`127.0.0.1:18081` |
| 压测脚本 | `test/k6/soak.js`，`SOAK_DURATION=15m` |

运行时临时将 `gateway.yml` 的两个 api endpoint 指向 `18080` 和 `18081`，完成后已按开跑前备份
字节级恢复。用户原有的 `8080` mock 未参与本轮。

## 负载组成

| 场景 | 负载 |
|---|---|
| 正常 JWT | 恒定 40 req/s |
| 鉴权攻击 | 5 次迭代/s，每次验证 expired、bad_secret、alg:none 三种 token |
| WebSocket | 3 个持续 VU，握手后 ping/echo |
| Admin | 每 15 秒轮询一次 `/stats` |
| 限流探针 | 每 5 分钟独立 XFF 客户端突发，验证 429 后恢复 |
| WAF | 一次真实注入拦截 |

运行命令：

```bash
SOAK_DURATION=15m K6_GW_HOST=127.0.0.1 K6_GW_PORT=8090 \
K6_ADMIN_HOST=127.0.0.1 K6_ADMIN_PORT=9090 K6_HUB_PORT=9091 \
k6 run test/k6/soak.js
```

## k6 汇总

| 指标 | 结果 |
|---|---:|
| 实际运行时长 | 15m00.9s |
| HTTP 请求 | 50,468，56.02 req/s |
| 迭代 | 41,912，46.52 iter/s，0 中断 |
| Checks | 88,327 通过，0 失败，100.00% |
| `http_req_failed` | 0 / 50,468，0.00% |
| 全部 HTTP 延迟 | avg 14.62ms，median 13.81ms，p95 35.53ms，max 297.83ms |
| 正常 JWT 延迟 | avg 19.61ms，median 18.98ms，p95 37.40ms；p99 < 250ms 阈值通过 |
| Gateway 错误计数 | `gw_errors=0`，`gw_auth_rejected=0`，`gw_circuit_open=0` |

## 功能结果

| 路径 | 结果 |
|---|---|
| 正常业务 | 36,000 次 JWT 请求全部 200，响应来自受控 upstream。 |
| 鉴权攻击 | expired 4,501、bad_secret 4,501、alg:none 4,501，全部 401。 |
| WAF | 1 次注入请求返回 403，`gw_waf_blocked=1`。 |
| 限流 | 64 次 429；突发中没有其他状态，恢复请求全部 200。 |
| WebSocket | 1,347 次 101 握手，1,347 个 echo，2,694 帧收发。 |
| Admin | 120 次 `/stats` 轮询均为 200，返回熔断计数结构。 |

## 服务端快照

运行结束前的 Gateway `/stats`：

| 指标 | 结果 |
|---|---:|
| `requests` | 51,755 |
| 路由 `/api` 2xx / 4xx / 5xx | 36,839 / 13,567 / 0 |
| `upstream_ok` / `upstream_fail` | 38,186 / 0 |
| `circuit_open` | 0 |
| `rate_limited` / `waf_denied` | 64 / 1 |
| `ws_open` / `ws_close` / `ws_active` | 1,347 / 1,347 / 0 |
| IP-ban 同步 | `ipban_synced=1`，link state=`open`，帧错误和 ACK/PONG 超时均为 0 |

两个 endpoint 均为 healthy、circuit closed：`18080` 成功处理 19,068 请求，`18081` 成功处理
19,118 请求。测试结束后已停止本轮 gw、hub 和 mock，删除临时风险规则与 `/tmp` 运行目录。

## 覆盖边界

本文件证明健康多 upstream 条件下的持续业务、安全拒绝、限流恢复、WebSocket 和 admin 可用性。
熔断故障、上游断连、慢上游、热重载、Hub 重启和多 upstream 分布算法的逐节点断言由
`test/k6/scenarios/07_upstream_gov.js`、`08_lb_distribution.js` 及其他专题脚本负责。
