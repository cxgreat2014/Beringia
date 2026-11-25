# loader/ — beringia_xnu.ko (APSL-2.0)

Loads XNU and runs it on the host. Planned layout (M2):

| File | What |
|---|---|
| `main.c` | Module init/exit, parameters (`kernel=`, `cpus=`, `boot_args=`) |
| `macho.c` | Mach-O parsing, segment placement at `PA = VA - KERNEL_BASE + slide`, local relocations |
| `bootargs.c` | `boot_args` v2, EFI memory map, flattened device tree |
| `pagetables.c` | Boot page tables for `vstart` |
| `hostcalls.c` | Dispatch of `enum beringia_call` to host services |
