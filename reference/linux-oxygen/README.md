# Linux CMI8788 (Oxygen HD) driver — reference copy

Unmodified copy of `sound/pci/oxygen/` from the Linux kernel, kept here as the
hardware reference for the macOS port. Not built as part of the kext.

- Source: https://github.com/torvalds/linux/tree/a74306e2e676f9775457366fc047a660fbf02f26/sound/pci/oxygen
- Commit: `a74306e2e676f9775457366fc047a660fbf02f26` (master, 2026-10-03, v7.3 cycle)
- License: GPL-2.0-only (see `COPYING-GPL-2.0` and each file's SPDX header).
  Code derived from these files inherits that license.

## Files relevant to the Xonar Essence STX (PCI subsystem `1043:835c`)

| File | Role |
|---|---|
| `oxygen_regs.h` | CMI8788 register map |
| `oxygen_io.c` | Register, I²C/SPI/AC97 access helpers |
| `oxygen_lib.c` | Chip init, interrupt handler, GPIO, S/PDIF |
| `oxygen_pcm.c` | DMA channels, formats, rates |
| `oxygen_mixer.c` | Generic mixer controls |
| `oxygen.h` | Shared structs, per-model callback interface |
| `virtuoso.c` | PCI ID table and model dispatch |
| `xonar_pcm179x.c` | STX model code (wiring notes at top; STX setup near `case 0x835c`) |
| `xonar_lib.c`, `xonar.h` | Shared Xonar helpers |
| `pcm1796.h` | PCM1792A/PCM1796 DAC registers (I²C addr `1001100`) |
| `cm9780.h` | AC97 codec in the STX input path |

The remaining files cover other Oxygen/Xonar models.

## Updating

```sh
SHA=<new commit>
for f in $(gh api "repos/torvalds/linux/contents/sound/pci/oxygen?ref=$SHA" --jq '.[].name'); do
  gh api "repos/torvalds/linux/contents/sound/pci/oxygen/$f?ref=$SHA" \
    -H "Accept: application/vnd.github.raw" > "$f"
done
```

Then update the commit hash above.
