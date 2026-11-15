# SH7305 standalone HH3 diagnostic (no gint)

Build with Hollyhock-3 SDK using `make -j4`. Copy
`dist/sh7305-probe.hh3` and `dist/romprobe.bin` to the calculator's
storage root.

## NTOS-style RAM loading and filesystem

Uses the **same native Hollyhock SDK entry points as NTOS_SH4**:
`File_Open`, `File_Fstat`, `File_Read`, `File_Close`, `File_Remove`,
`File_Write`, `Mem_Malloc`, and `Mem_Free`. It reads
`\\fls0\\romprobe.bin` in 4 KiB pieces, verifies the complete 1.5 MiB
file, and fills an explicitly allocated 1.5 MiB P1 RAM buffer.

The SDK heap is based at `0x8C280000`. A P1 pointer has a direct
physical address `VA & 0x1fffffff`; the corresponding P1 alias is
`PA | 0x80000000`. The start, middle and final bytes are compared
through these aliases. No UTLB manipulation is needed to read contiguous
**RAM** through a direct P1 address.

**Important:** this does **not** map a contiguous 1.5 MiB ROM image into P0:
the SH7305 has 64 UTLB entries and 384 separate 4 KiB pages would
require a pager. NTOS maps executable sections with its own MMU and
paging machinery; direct P1 access is the correct simple path for this
standalone RAM-backed diagnostic. Flash-file storage contiguity is not
implied by the contiguous allocation.

## Report and diagnosis

Writes `\\fls0\\results.txt` using the common `SH7305 PROBE RESULTS`
layout (CPU, USER RAM, clocks, code, ROM START/MID/END, LINUX ROM TEST).
The **HH3 SDK DIAGNOSTICS** appendix records exact file open/stat/read/close
return codes, whether the 1.5 MiB allocation succeeded, and the marker
check results.

The file check still runs if allocation fails; in that case, sample
addresses show `N/A` rather than misleading `NO`. A sample map is
shown only for memory directly mapped into P1/P2.

The executable and data file remain separate; the ROM image is not
linked into the HH3 executable. CI builds and packages both, but on-device
verification is still necessary.
