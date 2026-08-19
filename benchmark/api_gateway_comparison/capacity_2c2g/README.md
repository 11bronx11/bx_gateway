# 2 CPU / 2 GiB 健康容量复现套件

本目录是综合故障测试之外的独立容量补充。它只回答：在单机 2 CPU、2 GiB 总预算下，两台
健康上游和完整成功请求链始终开启时，各网关持续升压后能稳定交付到哪个 RPS 档位。它不替代
`../repro/` 的安全拒绝、热更新、故障恢复和观测性证据。

## 一键执行

```bash
# 约几十秒，只验证脚本和公共响应合同，不作为性能证据
./run.sh --case smoke

# 验证同一产品连续三次独立测量和平均汇总，不作为性能证据
./run.sh --case smoke-3x --products bronx

# 强制在首档触发 P99 门槛，验证早停和直连校准，不作为性能证据
./run.sh --case smoke-stop

# 正式测试：每产品最多 56 秒 x 3 次，四产品严格串行，约 15 分钟
./run.sh --case formal

# 只跑 Bronx 与 Spring 性能主线
./run.sh --case formal --products bronx,spring
```

默认 CPU 拓扑是：k6/Vegeta 使用 CPU 0，被测网关方案只允许使用 CPU 1、2，两台上游共同使用
CPU 3。目标机器 CPU 编号不同时，使用 `--load-cpu`、`--target-cpus` 和 `--upstream-cpu` 修改；
三组 CPU 不允许重叠。

## 冻结负载

- 两台上游 A/B 始终健康、等权，延迟基线为 1-2 ms；无故障、慢请求或控制面事件。
- 每轮依次运行 500、1,000、2,000、4,000、8,000、16,000、32,000 RPS，每档固定 8 秒；
  第一个不稳定档结束后不再执行该轮更高档位。
- 每个产品只启动并预热 10 秒一次，随后顺序执行 3 个相互独立的自适应测量。全部达到测试
  上限时正式流量为每产品 168 秒、四产品 11.2 分钟，外加启动、预热和封存约 15 分钟；较早
  达到容量边界时会更短。
- 所有请求都是 `GET /api/items`，携带合法 HS256 JWT、issuer、read scope、可信 XFF、唯一
  request-id 和待删除的内部头。
- 持续检查 200、A/B 上游名、GET、改写后 `/items`、`X-Bench-Version: v1`、内部头删除和
  request-id 请求/上游/响应一致性。
- 限流没有关闭：所有方案都配置为每秒 1,000,000，远高于最高档。结果要求 429 为 0，既经过
  限流中间件，又不让限流阈值成为容量上限。

请求实际经过路由匹配、可信客户端 IP、IP 策略、WAF 扫描、JWT 签名/过期/issuer/scope、限流、
请求头改写、负载均衡、健康上游选择、连接池和转发。访问日志与指标开启，高精度 trace 关闭。

## 测试场景设计

### 请求链路

每个请求从 k6 发出，经过被测网关的完整中间件链，最终到达上游并返回：

```
k6 (CPU 0)
  → 网关 (CPU 1-2): ClientAddr 解析 → 结构化日志 → 安全头 → IP 策略 (通过) 
                    → WAF 扫描 (通过) → 路由匹配 (/api/items)
                    → JWT 认证 (HS256, iss/scope 验证) → 限流 (1M/s, 通过)
                    → 请求头改写 (删除内部头, 添加 X-Forwarded-*)
  → 负载均衡 (加权最少连接 / least-connections)
  → 连接池 (复用 HTTP/1.1 keep-alive)
  → 上游 A/B (CPU 3): 基线延迟 1-2ms, 256B JSON 响应
  ← 网关: 响应日志 + 指标更新
  ← k6: 验证 200 + 上游名 + 路径改写 + request-id 一致性
```

**设计目标**：在不触发拒绝逻辑（IP 封禁、WAF 拦截、限流、JWT 失败）的前提下，让请求经过网关所有治理中间件，测量网关在"正常通过"路径上的最大吞吐和延迟表现。

### 测试上游

使用与综合测试相同的 `test/api_gw/mock_rich.py`，但配置为**健康基线模式**：

**上游 A 配置**：
```bash
python3 mock_rich.py --port 9001 --name bench-a --seed 100 \
  --delay-p50-ms 1.0 --delay-p99-ms 3.0 --max-normal-delay-ms 10 \
  --body-sizes 256 --healthy
```

**上游 B 配置**：
```bash
python3 mock_rich.py --port 9002 --name bench-b --seed 101 \
  --delay-p50-ms 2.0 --delay-p99-ms 4.0 --max-normal-delay-ms 10 \
  --body-sizes 256 --healthy
```

**关键特性**：
- **故障概率全关闭**：`error_probability=0`、`drop_probability=0`、`slow_probability=0`
- **固定响应体**：256 字节 JSON，包含上游名、请求方法、路径、透传头（X-Request-ID/X-Bench-Version），减少序列化开销和带宽干扰
- **HTTP/1.1 keep-alive**：持续开启，验证连接池复用能力（统计端点 `reused_requests` 计数必须 >0）
- **健康检查端点**：`/_soak/health` 始终返回 200，网关主动健康检查每 2-5 秒探测一次
- **统计端点**：`/_soak/stats` 提供数据请求计数、健康检查计数、连接复用统计、响应码分布，用于验证负载均衡是否均匀和连接池是否生效

**上游容量验证**：每轮首个不稳定档位后，runner 自动对 A/B 两台上游并发直连校准（绕过网关，k6 直接连上游，相同 RPS）。若直连也失败，标记 `load_generator_or_upstream`，说明瓶颈在上游或 k6；若直连通过，标记 `gateway_path_or_k6`，排除上游容量不足。

## 2 核配置

| 产品 | 数据面并行度 | CPU/内存总限制 | 上游与连接配置 |
|---|---:|---:|---|
| Bronx | 2 IO worker、2 CPU worker；Hub 1 worker | `gw + hub` 共 2 CPU/2 GiB | A/B 等权 weighted least-conn，连接池和主动健康检查开启 |
| Spring | Reactor Netty 2 worker | 2 CPU/2 GiB，Xmx 1280 MiB，direct 384 MiB | A/B 等权官方 LoadBalancer，fixed pool 320，健康检查、Bucket4j 限流和 Resilience4j route circuit breaker 开启 |
| Kong DB-less | 2 worker | 2 CPU/2 GiB | A/B 等权 least-connections，连接复用和主动/被动健康检查开启 |
| Kong + PostgreSQL | 2 worker | Kong 1.75 CPU/1.75 GiB + PG 0.25 CPU/0.25 GiB | 与 DB-less 相同数据面治理；数据库资源计入总量 |

这些是该资源档位的预先冻结配置，不允许在看到结果后只为某个产品追加 CPU、关闭日志/插件或
修改请求语义。Spring/Kong/PG 使用各发行版当前默认值的字段保持默认，只有表内与公共协议所需
字段显式修改。

## 通过和统计

高压档容量不足不是 runner 故障。runner 保存该档证据，按相同总 RPS 对 A/B 两台上游并发
直连校准，然后停止该轮后续档位。某档只有同时满足以下条件，才记为一次 `stable`：

- 实际 HTTP RPS 不低于目标的 99%。
- 完整响应合同正确率不低于 99.9%。
- 非预期响应为 0，429 为 0。
- `dropped_iterations` 不超过计划请求的 1%。
- 客户端 P99 不高于 100 ms。
- A/B 均收到数据和健康检查，并观察到连接复用。

最终 `highest_stable_rps` 要求三个测量全部 stable，不取单次最好值。`summary.json` 和 `report.md`
给出三轮平均、最小、最大交付 RPS，以及平均 P99、CPU 秒、内存峰值、drop、429 和非预期数。
若三轮都通过 32,000 档，只能报告 `>=32000 RPS`。失败档直连也失败时标记
`load_generator_or_upstream`，不能把边界归因给网关；直连通过时标记 `gateway_path_or_k6`，它排除
了受控上游容量不足，但仍不能仅凭这一项区分网关数据面和 k6 负载发生器。

## 结果目录

每次命令生成新的 `results/<run-id>/`，不会覆盖旧结果。产品级渲染配置和进程日志保存在
`products/<product>/`；每个 `round-N/products/<product>/stages/<rps>/` 保存该档 k6 summary、
每秒资源样本、A/B 上游统计、快照、判定和状态；首个失败档的直连证据位于该轮
`calibration/`。根目录保存三次聚合、源码快照、
主机信息和 `manifest.sha256`。

默认不保存体积很大的 k6 逐样本流；需要时加 `--save-raw`。校验结果：

```bash
cd results/<run-id>
sha256sum -c manifest.sha256
```
