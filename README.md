# mi-alchemy

Mutable Instruments **Clouds**, **Elements**, **Marbles** and **Plaits**
(Emilie Gillet, MIT) as firmwares for the Hermetic Modular **Alchemy Lab
V2**, on the Alchemy SDK. The DSP is vendored from
[pichenettes/eurorack](https://github.com/pichenettes/eurorack) at 08460a6
and is unmodified except one restored line (`vendor/VENDOR.md`). Each
firmware's shim does what the original's `cv_scaler`/`ui` code did, on six
pots, three buttons and ten jacks.

| Firmware | Rate | Home slot | Image (of 480 KB) | Panel + jacks |
|---|---|---|---|---|
| Clouds | 32 kHz / 32 | 8 | 387 KB | [src/clouds/DESIGN.md](src/clouds/DESIGN.md) |
| Elements | 32 kHz / 16 | 9 | 366 KB, plus a 338 KB samples file on the card | [src/elements/DESIGN.md](src/elements/DESIGN.md) |
| Marbles | 32 kHz / 16 | 10 | 399 KB | [src/marbles/DESIGN.md](src/marbles/DESIGN.md) |
| Plaits | 48 kHz / 24 (2×12) | 11 | 451 KB (engine at -Os) | [src/plaits/DESIGN.md](src/plaits/DESIGN.md) |

All four are siblings of smack/mark/belt/relay-alchemy. They include the
same SD-card firmware picker (Settings page 1), keep their working state in
their own QSPI preset slot, autosave 5 s after the last change, catch pots
after a firmware switch, and show last session's CPU peak on the P1 ring at
boot.

## Build

    git submodule update --init && git -C lib/libDaisy submodule update --init --recursive
    make libdaisy        once
    make FW=clouds       build-clouds/clouds_alchemy.bin   (FW = clouds | elements | marbles | plaits)
    make every           all four
    make size            image sizes against the 480 KB SRAM region
    make test            native suites for all four engines (host compiler, ASan/UBSan)
    make stage           copy bins (+ elements.smp) to ../daisy-sdk/alchemy-lab/

**Never build with a module attached in DFU.** Flashing is a manual step:
use the SD picker, or `make FW=x program-live DFU_SERIAL=<serial>`.

## Card layout

    /alchemy/clouds_alchemy.bin  elements_alchemy.bin  marbles_alchemy.bin  plaits_alchemy.bin
    /mi/elements.smp             Elements' exciter samples (build-elements/elements.smp)

Without `/mi/elements.smp`, Elements still runs. Its strike samples and
blow noise are silent, B1 rests dim amber, and the P2 ring shows red at
boot.

## Home slots across the card

Smack 12, Mark 13, Belt 14 and Relay 15 were already taken. This repository
takes **8–11**, which leaves 0–7 for presets saved by hand.

## v0.1 scope

- HostLink only. No Launchpad, no USB-audio mode, and `/alchemy/usb.cfg` is
  never written, so a card set to Launchpad keeps that setting for the
  other firmwares.
- Nothing has been flashed or heard. Hardware checks are listed in each
  DESIGN.md.

## Layout

    src/common/    picker (shared by copy with the sibling repos), mi_family.h, linker script
    src/<fw>/      fw.mk, <fw>_alchemy.cpp, DESIGN.md
    src/shim/      stand-ins for upstream headers that pull in STM32F drivers
    vendor/        eurorack subset + VENDOR.md
    test/          native suites; test/<fw>.mk per firmware
