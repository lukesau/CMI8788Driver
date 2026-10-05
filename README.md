# CMI8788Driver

macOS (IOAudioFamily) kext for the Asus Xonar Essence STX / STX II, which are
built on the C-Media CMI8788 ("Oxygen HD") chip. It is a port of the Linux
`snd-virtuoso` driver; the Linux sources it follows are kept unmodified in
[`reference/linux-oxygen/`](reference/linux-oxygen/), and each function in
[`CMI8788Chip.cpp`](CMI8788Driver/CMI8788Chip.cpp) names the Linux function it
mirrors.

Target: Intel Macs / hackintoshes, developed on macOS Catalina 10.15.

## Status

Written, builds cleanly, **not yet tested on hardware**.

- Stereo playback through the PCM1792A at 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz,
  24-bit samples
- Stereo line-in capture through the CS5381 at the same rates
- Volume (-60 to 0 dB in 0.5 dB steps, done in the DAC), mute
- Output selection: Headphones (rear jack), Line Out, Front Panel Headphones
- Headphone gain offset from the `HeadphoneImpedance` personality key (ohms),
  like the Linux "Headphones Impedance" control; without it, -18 dB (< 32 ohm)

Not yet: mic input, S/PDIF, sleep/wake, the H6 daughterboard's extra channels.

## Apple Silicon

Not supported, and not realistically possible, even with the card in a
Thunderbolt PCIe enclosure:

- The CMI8788 exposes its registers only through an I/O-port BAR (Linux uses
  `inb`/`outb`; there is no memory-mapped alternative). Thunderbolt-attached
  devices generally don't get PCI I/O space, ARM has no port I/O, and Apple
  Silicon's PCIe isn't known to provide it, so the registers would be
  unreachable.
- Kexts on Apple Silicon need Reduced Security and an arm64 build; the
  supported route is a DriverKit driver (PCIDriverKit + AudioDriverKit), whose
  register access is built around memory BARs, so it hits the same wall.

The chip knowledge in `CMI8788Chip.cpp` would carry over to any future
attempt; the IOAudioFamily code would not. This driver targets Intel Macs and
hackintoshes, which are supported up to macOS 26 Tahoe.

## Layout

| File | Role |
|---|---|
| `CMI8788Driver/OxygenRegs.h` | Register map, generated from the Linux headers |
| `CMI8788Driver/CMI8788Chip.*` | Hardware layer: register/I2C/AC'97 access, chip and STX init, rates, DMA, interrupts |
| `CMI8788Driver/CMI8788AudioEngine.*` | IOAudioEngine: DMA buffers, timestamps, format changes, sample conversion |
| `CMI8788Driver/CMI8788AudioDevice.*` | IOAudioDevice: matching, bring-up, volume / mute / output controls |

## Building

Needs Command Line Tools with the 10.15 SDK (no Xcode project).

```sh
make            # build/CMI8788Driver.kext
make remote     # rsync to the dev box (ssh host "hackintosh") and build there
make load       # on the box: copy to /tmp, chown root:wheel, kextutil (SIP must be off)
make unload
```

## License

GPL-2.0-only (see [`COPYING`](COPYING)), because the driver is derived from the
Linux snd-oxygen code by Clemens Ladisch.
