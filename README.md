# CMI8788Driver

macOS (IOAudioFamily) kext for the Asus Xonar Essence STX / STX II, which are
built on the C-Media CMI8788 ("Oxygen HD") chip. It is a port of the Linux
`snd-virtuoso` driver; the Linux sources it follows are kept unmodified in
[`reference/linux-oxygen/`](reference/linux-oxygen/), and each function in
[`CMI8788Chip.cpp`](CMI8788Driver/CMI8788Chip.cpp) names the Linux function it
mirrors.

Target: Intel Macs / hackintoshes, developed on macOS Catalina 10.15.

## Status

Working on hardware: plays and records on a Xonar Essence STX.

- Stereo playback through the PCM1792A at 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz,
  24-bit samples
- Stereo line-in capture through the CS5381 at the same rates
- Input selection: Line In, Microphone, Front Panel Microphone (mic through the
  CM9780 preamp with +20 dB boost and a gain slider; **mic path untested**)
- Volume (-60 to 0 dB in 0.5 dB steps, done in the DAC), mute
- Output selection: Headphones (rear jack), Line Out, Front Panel Headphones
- Headphone gain offset from the `HeadphoneImpedance` personality key (ohms),
  like the Linux "Headphones Impedance" control; without it, -18 dB (< 32 ohm)

- Sleep / wake (playback resumes after wake)

Not yet: S/PDIF, the H6 daughterboard's extra channels.

Tested on the dev box (Catalina 10.15.7, i7-3770, original STX `1043:835c`
behind a PEX8112): playback at 44.1-192 kHz, volume / mute / balance, switching
between Headphones and Line Out during playback, stereo line-in capture (clean
1 kHz tone, correct channels), sleep / wake with audio playing, and repeated
load / unload with the card present. Untested: Front Panel output, the STX II,
other macOS versions, and loading through OpenCore injection.

### Debugging

Set `Debug` to true in the kext personality (`IOKitPersonalities` in
`CMI8788Driver-Info.plist`) to have the engine publish capture/playback DMA
positions, the input buffer peak and the routing registers to the I/O registry
once a second:

```sh
ioreg -l -r -c CMI8788AudioEngine | grep Debug
```

Recording from a command-line tool over SSH returns silence on Catalina unless
the tool has microphone permission; test capture with a normal app (QuickTime,
Audacity) instead.

## Installing

Requirements: an Intel Mac or hackintosh with a Xonar Essence STX or STX II
(PCIe; the plain Essence ST is PCI and isn't supported), with the card's
auxiliary power connector plugged in. Developed and tested on macOS 10.15
Catalina. Download `CMI8788Driver-<version>.zip` from the releases page.

The kext is not signed, so macOS's kext signing check has to be out of the way
one of these two ways.

### Hackintosh / OpenCore Legacy Patcher Macs: inject with OpenCore

1. Copy `CMI8788Driver.kext` to `EFI/OC/Kexts/`.
2. Add an entry to `config.plist` under `Kernel` → `Add`:

   | Key | Type | Value |
   |---|---|---|
   | Arch | String | `x86_64` |
   | BundlePath | String | `CMI8788Driver.kext` |
   | Comment | String | `Xonar Essence STX` |
   | Enabled | Boolean | `true` |
   | ExecutablePath | String | `Contents/MacOS/CMI8788Driver` |
   | MaxKernel | String | (empty) |
   | MinKernel | String | (empty) |
   | PlistPath | String | `Contents/Info.plist` |

3. Run `ocvalidate`, reboot. SIP can stay enabled.

> This route is the intended one but **hasn't been verified yet**; the tested
> path so far is the next one.

### Any Intel Mac: SIP off, load from /Library/Extensions

1. Disable SIP's kext signing check from Recovery: `csrutil disable` (or
   `csrutil enable --without kext`). On a hackintosh, OpenCore's Toggle SIP
   entry works too.
2. Install and load:

   ```sh
   sudo cp -R CMI8788Driver.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/CMI8788Driver.kext
   sudo chmod -R 755 /Library/Extensions/CMI8788Driver.kext
   sudo kextutil /Library/Extensions/CMI8788Driver.kext
   ```

   On Big Sur and later, macOS asks you to approve the extension in Security &
   Privacy and reboot.

"Xonar Essence STX" then shows up in Sound preferences. Uninstall with
`sudo kextunload -b com.lukesau.driver.CMI8788Driver` and deleting the kext.

## Configuration

Set these keys in `CMI8788Driver.kext/Contents/Info.plist`, under
`IOKitPersonalities` → `CMI8788Driver`, then reload the kext (or reboot):

| Key | Type | Effect |
|---|---|---|
| `HeadphoneImpedance` | Number | Your headphones' impedance in ohms (the Linux "Headphones Impedance" / Windows "HP Amp Gain" setting). Picks the headphone gain offset like the Linux driver: < 32 Ω −18 dB, 32–63 Ω −12 dB, 64–299 Ω −6 dB, 300 Ω and up 0 dB. Without it: −18 dB, the safe default. |
| `Debug` | Boolean | Publish diagnostics to the I/O registry (see Debugging). |

Output (Headphones / Line Out / Front Panel Headphones) and input (Line In /
Microphone / Front Panel Microphone) are chosen in Sound preferences.

`stxctl` changes the headphone impedance at runtime, without reloading; it
doesn't persist across reboots (use the Info.plist key for that). It's a
separate command-line tool in the release zip (or `build/stxctl`), not part of
the kext; install it once with:

```sh
sudo mkdir -p /usr/local/bin && sudo cp stxctl /usr/local/bin/
```

Then:

```sh
stxctl status
sudo stxctl impedance 300
```

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
make dist       # release zip in dist/ (version from the Makefile)
```

## License

GPL-2.0-only (see [`COPYING`](COPYING)), because the driver is derived from the
Linux snd-oxygen code by Clemens Ladisch.
