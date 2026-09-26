# Meld for the Alchemy Lab

Meld is the Warps port Tim built for the Daisy Patch (`~/tim-os/meld`,
2026-09-11, never flashed), moved to the Alchemy Lab as its own firmware
beside Warps. It is Warps in Matthias Puech's Parasites build, plus two
things Meld added:

- **Mode ten, "Warps":** stock Mutable Warps itself. Its big knob runs past
  the comparator into the vocoder, and the knob's end freezes the vocoder's
  spectrum. Parasites' own META stops at the comparator.
- **Three outputs** that follow what the two inputs are doing: a coincidence
  clock and two envelope / pitch CVs.

Left out, because the Lab can't do them: Meld's second modulator pair (the
Lab has two audio ins and two outs, not four) and its Lissajous scope (no
screen).

The DSP runs at **96 kHz with Warps' 60-frame block** (parameters update at
1.6 kHz, as on the module). Only one engine runs at a time.

## Panel

```
          PLAY                           SETUP (hold B3)
  [ALGORITHM] B1 [TIMBRE]         [IN GAIN] B1 [TUNE]
  [LEVEL 1]   B2 [LEVEL 2]        [CV 2]    B2 [ -- ]
  [MODE]      B3 [CARRIER]        [ -- ]    B3 [ -- ]
```

| Control | Does |
|---|---|
| ALGORITHM | Warps' big knob through `lut_pot_curve`, with its detents. In META it sweeps crossfade, fold, analog ring mod, digital ring mod, XOR and comparator. In WARPS it continues into the vocoder (the knob then sets release), and the last hair is the spectral freeze. In the Parasites modes it is that mode's main control. J3 adds CV. |
| TIMBRE | Warps' small knob. J4 adds CV. |
| LEVEL 1 / 2 | Drive = level² (Warps). LEVEL 1 also tunes the internal carrier: 60 semitones of travel, plus J5 and TUNE. |
| MODE | 10 zones: Meta, Doppler, Fold, Chebyshev, Freq shift, Bitcrush, Comparator, Vocoder, Delay, **Warps** (stock). B2 steps it. |
| CARRIER | Warps' button: Ext (J1) / Sine / Tri / Saw. It is the room size in Doppler and the loop topology in Delay. B1 steps it. |
| IN GAIN | −12..+12 dB on J1/J2, 0 dB at noon. It also scales what the envelopes and the clock see. |
| TUNE | ±24 semitones on the internal carrier, in semitone steps. |
| CV 2 | What J6 puts out: **Auto** (Meld's rule: the note while CARRIER is internal, else the carrier's envelope), **Envelope**, or **Note**. |

The mapping is `src/warps/warps_params.h`, used unchanged, and
`src/meld/meld_params.h`. The native test runs both, so it proves the
firmware's own arithmetic.

**LEDs:**
- B1: the CARRIER state in Warps' button colours.
- B2: the MODE's colour, or red when the callback averages over 80 % CPU.
- B3: white for 30 ms on every coincidence (J7's gate); otherwise the dim
  Setup tint.
- P1 ring: the previous session's CPU peak, for 2.5 s at boot.

## Jacks (as built; each output is a default that can be changed)

| Jack | Signal | Path | Update rate |
|---|---|---|---|
| J1 | Carrier in | codec | per sample |
| J2 | Modulator in | codec | per sample |
| J3 | CV → ALGORITHM | SDK CV matrix | control rate |
| J4 | CV → TIMBRE | SDK CV matrix | control rate |
| J5 | V/Oct for the internal carrier | `Volts()` × 12, jumps over 0.4 st land at once, smaller moves glide | per block |
| **J6** | **CV 2 out:** the carrier's envelope (0..+5 V) or the internal note (1 V/oct from C2 = 0 V, clamped 0..5 V) | MCP4728 (I²C) | staged when it moves a DAC step; flushed from the 1 ms poll, never from the callback |
| **J7** | **Gate out:** the coincidence clock, 0 / +5 V, 5 ms | STM32 DAC | per block (0.625 ms) |
| **J8** | **CV 1 out:** the modulator's envelope, 0..+5 V | STM32 DAC | per block |
| J9 / J10 | Out / aux | codec | per sample |

- **The note scaling is Meld's own.** It spread notes 36..96 over the
  Patch's 0–5 V CV out, which is exactly 1 V/oct from note 36.
- **The coincidence clock is Meld's callback code, unchanged.** It fires
  when the rising zero crossings of the carrier and the modulator land
  within 2 samples of each other, counting pairs that straddle a block
  edge.
  - In-phase inputs fire once per shared cycle.
  - Anti-phase inputs never fire.
  - An octave pair fires once per low cycle.
  - Unrelated frequencies fire rarely.
- **The envelopes** are Meld's followers: 1 ms attack, 50 ms release, on
  the gained inputs.
- **Dropped from Warps:** the LEVEL CV jacks. The outputs needed three
  jacks, and the Lab can't tell whether a cable is plugged in, so a LEVEL
  CV could only be additive anyway (src/warps does it that way).

## Memory and size (2026-09-26)

- **Image:** 457 KB of the 480 KB SRAM region (93.0 %), with the engines
  at `-O2`. The alternatives, as SRAM use: 97.4 % at `-O3`, 94.6 % at
  `-O2`, 92.3 % at `-Os`. `-O2` keeps most of `-O3`'s speed with twice
  the headroom.
- **D2:** both modulators, in cached SRAM above the LED window.
  `warps_stock::Modulator` is 73,192 B at `0x30010000`, and
  `warps::Modulator` is 123,296 B at `0x30021be0`. D2 is 87 % used.
- **Stock sources:** they compile through `src/meld/stock/stock_*.cc`,
  because both trees have `modulator.cc`, `vocoder.cc` and so on, and the
  Makefile keys objects by basename.
- **Home slot 6**, schema tag `'MLD'`.

## Tests (`make -C test test-meld`)

1. **Stock digital ring mod:** 1000 × 200 Hz gives sidebands of 4.9e3
   against inputs of 1.2e-23.
2. **Stock big-knob sweep, 9 positions** (internal saw, hot modulator):
   no NaN. The vocoder region sounds (position 7: RMS 0.29), and a
   freeze reached from silence stays silent.
3. **The freeze holds a charged spectrum:** RMS 0.31 with the modulator
   removed, while release 0.2 decays to 0.
4. **Parasites' vocoder mode** is alive: RMS 0.18.
5. **The ALGORITHM pot at its stop reaches the freeze:** 1.0 maps to
   0.998, above 0.995. At 0.97 it maps to 0.88, so the freeze is only at
   the knob's end. Meld's README had left this unmeasured on the Patch.
   On the Lab it holds if the pot reads full scale at its stop, which is
   not yet seen on hardware.
6. **`MapStock` copies Meld's fields exactly,** with the easter-egg
   inputs neutral.
7. **The coincidence clock over 2 s:** 380 fires in phase, 0 in
   anti-phase, 380 for an octave pair, 7 for 190 against 263 Hz.
8. **Envelopes:** a 0.5 input gives 0.48 and a 0.1 input gives 0.095,
   both release to 0, and CV 2's three sources and clamping behave as
   specified.

The Parasites modes are covered by `test/test_warps.cpp` and aren't
repeated here.

## Open, on hardware

- **CPU at 96 kHz,** with the engines at `-O2`. Stock Warps' vocoder is
  the one to watch; read the P1 boot readout and B2.
- **IN GAIN defaults to 0 dB.** The Lab's codec scaling against Warps'
  16 Vpp input is unmeasured. Meld used ×2.2 on the Patch, a derivation
  for a different front end.
- **J6 accuracy:** the MCP4728 path is rated about ±50 mV in output mode,
  so the NOTE CV is good to about ±60 cents. Use it for modulation, not
  melody, until it's measured.
- **The J7 gate is 5 ms,** so it steps at the block rate: up to 0.6 ms of
  jitter against the true crossing.
- **Not ported from Meld:** the second pair (Off / Follow / Chain /
  Modulate), the scope, and the Mods-row sample-rate readout. The P1 CPU
  readout stands in for that last one.
