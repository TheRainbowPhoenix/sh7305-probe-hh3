# SH7305 standalone HH3 diagnostic (no gint)

Build with the Hollyhock-3 SDK using `make -j4`.

Copy `dist/sh7305-probe.hh3` and `dist/romprobe.bin` to the calculator's
storage root. The probe reads the 1.5 MiB external file in 4 KiB chunks and
writes `/results.txt` (calculator storage `\\fls0\\results.txt`),
overwriting the previous result. Diagnostic output also appears on the
Hollyhock debug console; press a key to exit.

The probe reads CPU, MMUCR, URC and URB values and performs direct P1/P2
code alias checks where appropriate. It does not modify TLB mappings.
The file verification is **not** evidence of contiguous physical ROM mapping.

Build verification runs on GitHub Actions, but device execution and
filesystem-path behavior must still be confirmed on real hardware.
