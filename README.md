# SH7305 standalone HH3 diagnostic (no gint)

Build with the Hollyhock-3 SDK using `make -j4`.

Copy `dist/sh7305-probe.hh3` and `dist/romprobe.bin` to the calculator's
storage root. The add-in verifies the exact 1.5 MiB payload (markers at
offsets 0, 0x080000, and 0x17ffff; 0x5a, 0xa5, and 0x3c).

The reference-style report is written to `/results.txt` (calculator storage
`\\fls0\\results.txt`) and printed to the debug console. It includes:

- SH7305 CPU identification and MMUCR state.
- Raw FRQCR / PLLCR / FLLFRQ registers; no guessed frequencies.
- Code virtual address and physical P1 alias **when directly addressed**.
- A separately allocated, 1.5 MiB **RAM-resident copy** of the file, with
  start/middle/end sample VA, PA, P1, byte value and alias comparison.
- Physical sample contiguity *in RAM*, and independent full file-integrity
  results.

The resident allocation is **not linked into the HH3 executable**. If
allocation fails, the tool still verifies the complete external file with
a 4 KiB read buffer and marks resident-memory measurements as unavailable.

When HH3 runs with MMU translation disabled, no P0 UTLB page mapping
exists; the probe cannot truthfully report values like
`VA 003002BC -> PA 01A5A2BC, PAGE 0x1000` in that context.
P1/P2 addresses are physically direct and therefore have no translated
UTLB page size. The fixed reference API-window address `0x08100000`
is left unresolved rather than dereferenced without an active translation.
The resident sample test measures **RAM**, not a memory-mapped storage ROM
file, so it cannot establish on-disk/flash physical contiguity.

Build verification runs on GitHub Actions. Test MMU and heap results on
real hardware and share `results.txt` for comparison.
