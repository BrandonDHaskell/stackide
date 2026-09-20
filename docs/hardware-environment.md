# Hardware and Environment Criteria

Status: accepted
Applies to: all of `stackide`
Authority: this document is the source of truth for the derived constants below.
Code that hardcodes a tier value instead of deriving it from here is a defect.

## Tiers

| | Floor | Reference | Headroom |
|---|---|---|---|
| **RAM** | 16 GB | 32 GB | 64 to 128 GB |
| **Cores** | 4 physical | 8 physical | 16+ physical |
| **Storage** | SATA SSD | NVMe | NVMe |
| **Repo ceiling** | medium | large | large |
| **Workload** | editor, 1 LSP, 1 agent | editor, LSP pool, remote API, 2 to 3 agents | 5 agents, multiple large repos |

**Reference is the tuning target.** Floor must remain usable, not fast.
Headroom must scale without redesign.

Tiers are disjoint. A machine sits in exactly one.

### Repo ceiling definitions

| Class | Translation units | Lines | Anchor |
|---|---|---|---|
| small | < 500 | < 50k | SDL3 |
| medium | < 5,000 | < 1M | CPython, Godot |
| large | < 50,000 | < 10M | LLVM monorepo |
| out of scope | beyond | beyond | Chromium |

Out of scope means stackide is not expected to index it interactively.
It should degrade to a usable editor, not fail.

## Platform

- **Primary:** Linux x86-64, Debian 13 and equivalents
- **Secondary:** macOS ARM64
- **Deferred:** Windows, stated publicly rather than half-supported
- **Toolchain:** GCC 16 or Clang 21, C++26, with shims where library support lags
- **SIMD baseline:** AVX2 and NEON. Not AVX-512: absent on most consumer parts
  and it downclocks on several server ones. Prefer `std::simd` where the
  library supports it, intrinsics only where it does not.
- **Rendering:** GPU-accelerated via SDL_GPU (Vulkan on Linux, Metal on macOS).
  Integrated graphics sufficient.

### Cache line

- x86-64: 64 bytes
- ARM64 (Apple Silicon): 128 bytes

Do not static-assert a single value. Use a per-architecture compile-time
constant, `stackide::kCacheLineBytes`, selected by target macro and
static-asserted against `std::hardware_destructive_interference_size` where the
implementation provides a usable one. Detection at runtime remains rejected.

## Derived constants

### LSP pool size

```
lsp_pool = clamp(floor(0.4 * RAM_bytes / lsp_rss_estimate), 1, 4)
```

`lsp_rss_estimate` starts from a per-language bootstrap default and is replaced
by a measured rolling maximum once a server has indexed once.

Bootstrap defaults for clangd, to be replaced by measurement:

| Repo class | Estimate |
|---|---|
| small | 0.5 GB |
| medium | 1.5 GB |
| large | 4.0 GB |

The formula saturates at the clamp ceiling by 32 GB on a large repo. This is
intentional and is why Reference is 32 GB.

### Worker concurrency

Each worker owns an isolated git worktree and therefore its own LSP instance.
Instances are not shared across workers.

    max_workers = clamp(
        floor((0.4 * RAM_bytes - orchestrator_reserve) / lsp_rss_estimate),
        1,
        floor(physical_cores / 2))

`orchestrator_reserve` covers the orchestrator's own view of the base tree:
one LSP instance plus editor.

`lsp_rss_estimate` assumes a shared background index. Without one, use the
cold-start figures and expect roughly a third of the worker count.

| Repo class | With shared index | Cold |
|---|---|---|
| small | 0.3 GB | 0.5 GB |
| medium | 0.8 GB | 1.5 GB |
| large | 1.5 GB | 4.0 GB |

### Task partitioning

At most one worker may hold write access to a given header at a time.
Implementation files partition freely. A worker's result must be reproducible
from its base commit and task alone; in-flight state is never propagated
between worker trees.

### Memory floors

- Editor alone, no LSP, no agents: under 1 GB resident. The editor runs anywhere.
- Editor plus one LSP on a medium repo: under 3 GB resident at Floor tier.

### Frame budget

- Working budget: 16 ms, the 60 Hz refresh interval, treated as the floor
  assumption. On a 120 Hz display the budget is 8.3 ms and the same code path
  must fit it.
- Input to paint: under 8 ms, measured independently of refresh rate.

## How these are validated

Each constant above needs a way to be proven wrong. Until a check exists, the
constant is an assumption, not a criterion.

| Constant | Check | Status |
|---|---|---|
| Editor-only floor | RSS assertion in a smoke test | not built |
| Frame budget | frame-time histogram, p99 gate | not built |
| Input to paint | instrumented event-to-present timestamp | not built |
| `lsp_rss_estimate` | measured at runtime, logged | not built |

## Decision log

- **2026-09-20:** Reference RAM set to 32 GB. The prior draft named 64 GB in the
  table and 32 GB in the prose. 32 GB is correct: it is the smallest tier at
  which the LSP pool formula reaches its clamp ceiling, so 64 GB buys nothing
  under the current formula, and 32 GB matches the common developer machine.
- **2026-09-20:** Reference cores set to 8, not a range. A tuning target is a
  single number.
- **2026-09-20:** Headroom RAM narrowed to 64 to 128 GB so tiers do not overlap.
- **2026-09-20:** Agent concurrency decoupled from LSP pool size. The prior
  draft bounded agents by the pool while also specifying 5 agents at Headroom
  and a pool clamped at 4, which is unsatisfiable.
- **2026-09-20:** Cache line made per-architecture. A single 64-byte assertion
  contradicts macOS ARM64 as a supported secondary platform.