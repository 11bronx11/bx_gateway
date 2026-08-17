# Bronx Gateway Demo

这套 Demo 用真实的 `gw`、`hub` 和 `banctl` 进程演示网关完整链路。它使用
`demo/` 作为独立运行根，业务配置、日志、SQLite、PID 和观测数据都与
`api_gw/bin/` 下的正式运行配置隔离。

`DEMO_PLAN.md` 是施工阶段的计划和调研记录。当前组件、命令和行为以本文及
`demo/*.sh` 为准。

## 组件与端口

| 组件 | 地址 | 作用 |
| --- | --- | --- |
| Nginx | `:80`、`:443` | HTTP 跳转、TLS/HTTP2、请求 ID、XFF、WSS 前置代理 |
| gw | `127.0.0.1:8090`、`:9090` | 网关业务口与 admin 口 |
| hub | `127.0.0.1:9091` + 3 个 Demo UDS | IP-ban 控制面、指标和 SQLite 持久化 |
| httpbin1/2 | `127.0.0.1:8081`、`:8082` | 加权负载均衡、延迟和故障上游 |
| json-server | `127.0.0.1:3002` | REST 业务上游 |
| ws-echo | `127.0.0.1:3003` | WebSocket 文本、二进制、ping/pong 回显上游 |
| Prometheus | `127.0.0.1:9099` | 抓取 gw 和 hub 的 `/metrics` |
| Loki | `127.0.0.1:3100` | 存储和查询 Demo 日志 |
| Alloy | `127.0.0.1:12345` | 采集 Nginx、gw 和 hub 日志并写入 Loki |
| Grafana | `127.0.0.1:3000` | 指标、熔断、WebSocket 和日志大盘 |

除 Nginx 外，所有 TCP 服务只绑定 loopback。Nginx 是唯一前置入口，使用本地
Demo CA 签发的证书，不应按生产证书使用。

## 准备

先构建产品二进制：

```bash
cmake -S . -B build-bx -DCMAKE_BUILD_TYPE=Debug
cmake --build build-bx --parallel 4
```

Demo 还需要 `curl`、`jq`、Python 3、Node.js、Nginx、Prometheus、Grafana、
Loki、Grafana Alloy 和 Vegeta。它们是演示环境依赖，不是 `gw`、`hub`、
`banctl` 的产品运行时依赖。当前主机验证过的主要版本是 Nginx 1.24、
Prometheus 2.45.3、Grafana 13.1.1、Loki 3.7.2、Alloy 1.18.0、
Node.js 22 和 Vegeta 12.13。

准备隔离的 Python 和 Node.js 上游依赖：

```bash
python3 -m venv demo/upstream/venv
demo/upstream/venv/bin/pip install httpbin==0.10.4 gunicorn==21.2.0
npm install --prefix demo/upstream
```

TLS 私钥故意不进 Git。启动前确认当前主机已经配置与证书匹配的本地私钥：

```bash
test -r demo/nginx/ca/ca.key
test -r demo/nginx/ca/server.key
```

## 启动

从仓库根目录执行：

```bash
./demo/up.sh
sudo ./demo/up.sh nginx
./demo/status.sh
```

第一次命令按依赖顺序启动普通用户管理的组件，并提示 Nginx 需要 root。第二条
只启动占用 80/443 的 Nginx。`status.sh` 应显示 11 个组件都是 `UP`。

也可以只管理指定组件，例如：

```bash
./demo/up.sh loki alloy grafana
./demo/status.sh loki alloy grafana
./demo/down.sh loki alloy grafana
```

调用方需要自己保证局部启动的依赖顺序，例如 Alloy 依赖 Loki，gw 依赖 hub。

## 执行演示

不带参数运行全部 16 项，传编号只运行一项：

```bash
./demo/demo.sh
./demo/demo.sh 15    # WSS 隧道和 WebSocket 帧往返
./demo/demo.sh 16    # Nginx/gw 日志采集和 request_id 关联
```

| 编号 | 演示内容 |
| --- | --- |
| 1 | HTTP 到 HTTPS 跳转、TLS 和 HTTP/2 |
| 2 | `X-Request-Id` 在响应和网关访问日志中的传播 |
| 3 | 可信 Nginx XFF 与非可信直连 XFF 的边界 |
| 4 | 路由匹配、前缀改写和 route cache |
| 5 | JWT 缺失、有效、过期、`alg=none` 和 scope 不足 |
| 6 | 路由级令牌桶限流、`429` 和 `Retry-After` |
| 7 | XSS、路径穿越、扫描器 WAF 命中和风险上报 |
| 8 | WAF -> hub -> Guard 封禁闭环及 Hub 重启持久化 |
| 9 | 两个 httpbin endpoint 的 3:1 加权负载均衡 |
| 10 | endpoint 熔断 OPEN -> HALF_OPEN -> CLOSED 恢复 |
| 11 | SSE 流式响应与 Nginx 关闭代理缓冲 |
| 12 | Vegeta 开环负载、Prometheus target 和 Grafana 大盘 |
| 13 | 网关 trace 转换为 `demo/out/trace.json` |
| 14 | 在途流式请求期间热重载 |
| 15 | `wss://gw.local/ws/echo` 的 101、文本/二进制、ping/pong、关闭帧 |
| 16 | Alloy -> Loki 日志链路及 Nginx/gw 同一 `request_id` 关联 |

脚本最后输出 `completed with 0 failure(s)` 才代表本轮全部通过。单项演示也会返回
非零退出码表示失败。

## 图形界面

Ubuntu Server 不需要桌面环境。Grafana、Prometheus、Loki 和 Alloy 都是 Web
服务，页面由本地电脑浏览器渲染。可从本地电脑建立 SSH 隧道：

```bash
ssh -N \
  -L 8443:127.0.0.1:443 \
  -L 3000:127.0.0.1:3000 \
  -L 9099:127.0.0.1:9099 \
  -L 3100:127.0.0.1:3100 \
  -L 12345:127.0.0.1:12345 \
  <user>@<server>
```

然后访问：

- Gateway：本地 hosts 配置 `127.0.0.1 gw.local` 并信任 Demo CA 后访问
  `https://gw.local:8443`
- Grafana：`http://127.0.0.1:3000/d/bronx-gateway-demo/bronx-gateway-demo`
- Prometheus：`http://127.0.0.1:9099`
- Alloy：`http://127.0.0.1:12345`
- Loki ready：`http://127.0.0.1:3100/ready`

Grafana 已自动 provision Prometheus、Loki 和 `Bronx Gateway Demo` 大盘，允许
匿名 Viewer 访问；脚本访问管理 API 时使用 Demo 账号 `admin/admin`。

第 13 项生成的 `demo/out/trace.json` 可在浏览器的 Perfetto UI 或 Chromium
`chrome://tracing` 中打开，服务端本身不需要图形能力。

`demo.sh` 始终在服务器终端执行，SSH 隧道只用于从本地浏览器查看页面和入口。

## 日志

原始日志保留在以下位置：

- `demo/run/nginx_access.log`：Nginx JSON access log。
- `demo/run/nginx_error.log`：Nginx error log。
- `demo/logs/gw_system.log`：网关 JSON access/system log。
- `demo/logs/gw_trace.log`：网关按请求开启的高精度 trace。
- `demo/logs/hub_system.log`：Hub system log。
- `demo/logs/{component}.log`：脚本拉起外部组件时的标准输出和错误。

Alloy 持续 tail 这些文件并写入 Loki。Grafana 的 `Recent Gateway Logs` 用于查
明细，`Log Volume` 用于看日志速率。按单次请求定位时，先取得响应头中的
`X-Request-Id`，再在 Grafana/Loki 中查询该值；第 16 项自动验证 Nginx 和 gw
两个日志流都能查到同一个 ID。

## 停止与资源边界

```bash
./demo/down.sh
sudo ./demo/down.sh nginx
./demo/status.sh
```

停止顺序与启动依赖相反。最终状态应全部为 `stopped/DOWN`，80、443、3000、
3002、3003、3100、8081、8082、8090、9090、9091、9099、12345 均不再监听，
`/tmp/bronx_demo_ip_*.sock` 被移除。

`down.sh` 只回收进程、PID 和 Demo UDS，不删除依赖、证书、日志、SQLite、
Prometheus/Loki/Grafana 数据。运行数据位于 `demo/run/`、`demo/logs/`、
`demo/out/` 和 `demo/api_gw/bin/db/`，都已由 `.gitignore` 排除。需要删除历史
数据时应先确认 Demo 已全部停止，再单独审核这些目录，不要把
`api_gw/bin/gateway.yml` 或正式 Hub 数据库纳入清理。
