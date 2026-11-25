<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Beringia 中文说明

**目标**：把 macOS 内核 XNU（x86_64，`xnu-12377.1.9`，对应 macOS 26.0 / 25A353）做成 Linux 内核模块，在 WSL2 的 x86_64
CPU 上原生运行 macOS 26 用户态。中间没有虚拟机，也没有指令模拟。验证用的是 x86_64 macOS 26.0（25A353）系统盘。

项目**公开开发**（build in public）。进度见 [ROADMAP.md](../../ROADMAP.md)，设计见 [architecture.md](../architecture.md)，
工作分解见 [plan.md](../plan.md)。

## 方案要点

| 问题 | 做法 | 为什么（ADR） |
|---|---|---|
| XNU 的内存 | 用启动参数 `memmap=` 预留出 Linux 不使用的**真实物理内存**交给 XNU；内核按原生 KASLR 方式整体滑动（低 4 GB 区） | [0001](../adr/0001-real-physical-memory.md)：页表必须能直接交给 MMU |
| CPU 状态 | XNU 运行时使用**自己的** CR3、GDT、IDT、TSS、GS、syscall MSR、XSAVE 状态；回到 Linux 时整套切回 | [0002](../adr/0002-full-world-switch.md)：段选择子冲突，XNU 原生机器层不用改 |
| 宿主中断 | 硬件中断进入 XNU 的 IDT 后立即切回 Linux，用 `int $vector` 交给 Linux 原处理程序（由它 EOI）；XNU 的时钟和 IPI 由宿主投递 | [0003](../adr/0003-interrupt-forwarding.md) |
| 地址空间 | PML4[511] 在 PDPT 粒度上共享（XNU 用 0–509，Linux 用 510/511）；physmap、双映射放在 Linux 不用的槽；跨 ABI 只传物理地址 | [0004](../adr/0004-address-space-sharing.md) |
| 平台 | 自写 `BeringiaPlatform`/`BeringiaCPU` 等 kext，不用 AppleACPIPlatform；Apple 的通用 kext 取自 25A353 的 BootKC | [0005](../adr/0005-own-platform-kexts.md) |
| 版本 | 只用 `xnu-12377.1.9` 配 25A353 资产 | [0006](../adr/0006-target-macos-26-25A353.md) |

## 里程碑

| | 内容 | 状态 |
|---|---|---|
| M0 | Linux 上可复现地编译 XNU | ✅ 原版 tag + 3 个构建补丁，编出 23.6 MB 的 `kernel.development` |
| M1 | 宿主基础：预留内存、完整世界切换、中断转发（不依赖 XNU 的自测） | ✅ 6 CPU × 30 s：1087 万次世界往返、2.4 万次中断转发，零内核告警 |
| M2 | XNU 早期启动到 `kernel_bootstrap_thread`，控制台/panic 输出到宿主 | ✅ 已启动到 IOKit 平台匹配 |
| M3 | 时钟、idle、抢占让渡、多 CPU | 🔨 单核：宿主定时器+idle 已通，多核未做 |
| M4 | 平台 kext 与内核集合（KC）构建，IOKit 到"等待根设备" | ⏳ 下一步（需要 BeringiaPlatform） |
| M5 | 第一个 macOS 26 用户态：用 25A353 恢复 ramdisk 启动 `launchd` | ⬜ |
| M6 | 完整 macOS 26 系统卷，多用户启动，网络、ssh | ⬜ |
| M7 | 显示与输入，WindowServer | ⬜ |
| M8 | 产品化：CLI、CI、测试 | ⬜ |

## 与 xnulinux 的关系

[xnulinux](https://github.com/AFK-surf/xnulinux) 是参照项目（arm64）。Beringia 是独立的 x86_64 项目：沿用它
"GPL 宿主模块 + APSL XNU 侧模块"的整体架构，但 x86_64 的机制（世界切换、中断转发、地址空间）是重新设计的。
之前在 xnulinux 分支里做过的 x86 尝试已经审计，结论见 [audit](../audit/2026-11-prior-x86-attempt.md)。
