# host/ — beringia_host.ko (GPL-2.0-only)

The only code that touches Linux internals. Planned layout (M1):

| File | What |
|---|---|
| `main.c` | Module init/exit, parameters (`carve=`, `selftest=`) |
| `mem.c` | Carve-out claim and mapping, physical-address access for host services |
| `arch/x86_64/world.S` | World switch: enter, host call, forwarded interrupt, abort |
| `arch/x86_64/state.c` | Capture of Linux CPU state, TSS busy-bit handling, XSAVE areas, interrupt re-delivery table |
| `cpu.c` | Pinned CPU threads, kicks, idle waits, hrtimers |
| `selftest.c` | A minimal foreign world for testing the switch without XNU |
| `include/beringia/host.h` | The interface `beringia_xnu.ko` uses (MIT) |
