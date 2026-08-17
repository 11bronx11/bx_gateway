# Bronx vs Nginx Gateway Benchmark

Date: 2026-08-17 UTC

## Build and host

- Bronx commit: `d3a3f73fe049ca7af7dc7c1e8288303abbdf6766`
- Bronx build: `RelWithDebInfo`, `-O2 -g -DNDEBUG`, GCC 13.3.0
- Bronx binary SHA-256: `b9bd2011098b7bc9ec342126a9da2572ac5d270fd095c4438405e388b1a77c6b`
- Nginx: 1.24.0 (Ubuntu)
- Load generator: Vegeta 12.13.0, open-loop fixed rate
- Host: Linux 6.8.0, AArch64, 4 vCPU, 5.2 GiB RAM

## Method

Topology: `Vegeta -> proxy under test -> Nginx static upstream`, all over loopback HTTP/1.1 keep-alive with a 29-byte response.

- Vegeta pinned to CPU 0.
- Bronx or Nginx proxy pinned to CPUs 1-2; both configured with two I/O workers.
- Static Nginx upstream pinned to CPU 3.
- Access-log writes and Bronx trace were disabled.
- Bronx retained its current fixed middleware chain, including request ID, client address, structured access accounting, security headers, routing, connection pooling, and proxying.
- Equal-load test: 30,000 target QPS, 256 clients, 12 seconds, three alternating rounds.
- Capacity test: 40,000 target QPS. Bronx used 512 clients to avoid a 256-client scheduling ceiling; Nginx needed only 256. Each ran for 12 seconds in three rounds.
- Stable delivery gate used during the staircase: 100% HTTP success, no transport errors, and actual rate at least 99% of target.

## Equal load: 30,000 QPS

Three-round medians:

| Proxy | Actual QPS | Success | P50 | P95 | P99 | Proxy CPU |
|---|---:|---:|---:|---:|---:|---:|
| Bronx | 30,000.67 | 100% | 2.99 ms | 9.65 ms | 14.64 ms | 156.75% |
| Nginx | 30,000.80 | 100% | 0.19 ms | 4.51 ms | 9.27 ms | 70.67% |

At the same throughput, Bronx P99 was 57.99% higher and proxy CPU was 2.22x Nginx.

## Capacity boundary

Three-round medians at a 40,000 QPS target:

| Proxy | Actual QPS | Success | P50 | P95 | P99 | Proxy CPU | Interpretation |
|---|---:|---:|---:|---:|---:|---:|---|
| Bronx | 36,954.12 | 100% | 10.54 ms | 17.88 ms | 22.11 ms | 178.75% | Saturation plateau around 37k QPS |
| Nginx | 40,000.69 | 100% | 0.94 ms | 4.83 ms | 8.08 ms | 83.42% | Still followed target; lower bound only |

The single-core generator and static upstream plateaued around 40k QPS, so this run cannot identify Nginx's exact maximum. It proves Nginx is at least 40k QPS in this setup. Nginx delivered about 8.24% more observed throughput than Bronx at the test ceiling; Bronx had 2.74x the P99, but these two P99 values are at different achieved loads.

## Evidence boundaries

- This is a same-host loopback HTTP/1.1 benchmark, not production capacity sizing.
- TLS, WAN latency, large bodies, WebSocket, authentication, WAF, rate limiting, Hub/IP-ban, reload, and failure recovery are outside this comparison.
- The host was shared with VS Code, clangd, Codex, Docker, and MySQL processes; alternating rounds and medians reduce but do not eliminate scheduler noise.
- An exploratory batch using up to 4096 client connections exhausted the local ephemeral port range. Those runs are invalid and excluded from every reported median.
- Raw Vegeta, JSON, and pidstat outputs are in `results/`; benchmark configs are under `src/api_gw/bin/`, `nginx-proxy/`, and `nginx-upstream/`.
