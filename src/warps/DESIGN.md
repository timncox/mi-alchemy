# Warps (Parasites) for the Alchemy Lab

Mutable Instruments Warps (Emilie Gillet, MIT) in Matthias Puech's
**Parasites** build, which is Tim's choice (2026-09-26). The engine is
`warps::Modulator` from mqtthiqs/parasites @ 32fa66f, vendored via
`~/tim-os/meld` with one marked fix (vendor/VENDOR.md). It runs at its
native **96 kHz with a 60-frame block**. `warps_params.h` is Parasites'
`cv_scaler.cc` mapping, shared with the native test so the test proves the
firmware's own arithmetic.

## Panel

```
          PLAY                          SETUP (hold B3)
  [ALGORITHM] B1 [TIMBRE]         [IN GAIN] B1 [TUNE]
  [LEVEL 1]   B2 [LEVEL 2]        [  -  ]   B2 [  -  ]
  [MODE]      B3 [CARRIER]        [  -  ]   B3 [  -  ]
```

| Control | Does | Mapping |
|---|---|---|
| ALGORITHM | Warps' big knob | `lut_pot_curve` (a flat spot per META algorithm), the ×1.08−0.01 anti-bleed below 0.125, capped at 0.9999 so the 513-entry table is never read past its end; + J3 |
| TIMBRE | Warps' small knob, the algorithm's parameter | 0..1; + J4 |
| LEVEL 1 | carrier drive (level²); with the internal carrier on, also its pitch | note = 60·level + 36, + 12/V on J5, + TUNE; + J6 |
| LEVEL 2 | modulator drive (level²) | + J7 |
| MODE | META (stock Warps) then Parasites' eight | 9-zone selector; B2 steps it |
| CARRIER | Warps' button state, 0–3 | 4-zone selector; B1 steps it |
| IN GAIN | J1/J2 input gain | −12..+12 dB, 0 dB at noon |
| TUNE | internal carrier transpose | ±24 st in semitone steps |

The knobs are smoothed per block with cv_scaler.cc's pot coefficients
(0.33 × its BIND low-pass: 0.165 on the levels, 0.0264 on ALGORITHM and
TIMBRE) at the same 1.6 kHz.

**Modes and what the four knobs mean.** From meld's table, which comes from
the Parasites documentation. Columns: LEVEL 1 / LEVEL 2 / ALGORITHM / TIMBRE.

| Mode | L1 | L2 | ALGORITHM | TIMBRE | CARRIER means |
|---|---|---|---|---|---|
| Meta (stock) | level 1 | level 2 | algorithm | timbre | internal carrier: ext / sine / tri / saw |
| Doppler | LFO freq | LFO amount | X | Y | room size, tiny → huge |
| Fold | level 1 | level 2 | fold | bias | internal carrier |
| Chebyshev | level 1 | level 2 | order | gain | internal carrier |
| Freq shifter | feedback | mix | shift | up/down | internal carrier |
| Bitcrusher | level 1 | level 2 | crush | x-mod | internal carrier |
| Comparator | level 1 | level 2 | function | Chebyshev | internal carrier |
| Vocoder | level 1 | level 2 | warp | release | internal carrier |
| Delay | feedback | mix | speed | head | loop: open / dual / tape / ping-pong |

The MODE selector puts META first because it's stock Warps. After that the
modes follow Parasites' enum order.

**Buttons:**
- B1 steps CARRIER (Warps' button).
- B2 steps MODE. It stands down while B3 is held, so the B2+B3 Settings
  chord doesn't also step the mode.
- B3 held shows SETUP.
- B2+B3 held 2 s opens Settings (brightness, presets). Settings page 1 is
  the SD firmware picker.

A button tap moves the selector's stored value, so the ring follows and the
change saves, and the pot has to catch the new value before it takes over
again.

**LEDs:**
- **B1:** the CARRIER state in Warps' button colours: dim for state 1
  (external / tiny / open), then green, yellow and red.
- **B2:** the mode's colour, or red when the callback averages over 80 %
  CPU.
- **B3:** a dim Setup tint.
- **P1:** for 2.5 s at boot, the ring shows the previous session's worst
  CPU load.

## Jacks

| Jack | Signal | Read |
|---|---|---|
| J1 | carrier in | codec L; with the internal carrier on, Warps uses it as through-zero FM into that oscillator (upstream behaviour) |
| J2 | modulator in | codec R |
| J3 | CV → ALGORITHM | SDK CvMatrix, summed into the knob |
| J4 | CV → TIMBRE | CvMatrix |
| J5 | V/Oct → internal carrier | `CvJack::Volts()` ×12 per block. Jumps over 0.4 st land at once, smaller changes glide (0.1), upstream's rule |
| J6 / J7 | CV → LEVEL 1 / LEVEL 2 | CvMatrix, summed |
| J8 | unassigned | route it in the web programmer |
| J9 / J10 | out / aux | the Modulator's main and aux outputs |

**Level CV differs from the real module.** On a real Warps, a cable in a
LEVEL jack makes it a VCA: drive = pot² × CV × 1.6, found by the
normalization probe. The Lab can't probe for cables, so the firmware always
takes Warps' "nothing patched" branch (drive = level²), and CV adds to the
knob through the SDK matrix. On a real Warps the LEVEL 1 CV jack also
doubles as V/Oct. Here V/Oct has its own jack, J5, which frees LEVEL 1's CV
to stay a level CV.

## Memory and size (build of this commit)

| Region | Used |
|---|---|
| SRAM | 401,312 B of 480 KB (81.7 %), of which the image (`make size`) is 393,328 B |
| D2 | 155,520 B (59 %): the Modulator, 123,296 B at 0x30010000, plus the LED window |
| DTCM | 73,480 B (56 %) |
| SDRAM | 65,472 B (SDK only) |

Placement-new into a zeroed D2 block. `Modulator::Init()` leaves some
`previous_parameters_` fields unset and relies on zeroed statics, and a
garbage value there indexes a table out of bounds (meld's ASan finding).
`TEST` is never defined in the firmware build.

## Tests (`make -C test test-warps`)

- **Digital ring mod** (META, algorithm 3/8): 1000 Hz × 200 Hz gives
  sidebands at 800 + 1200 Hz with power 569. The inputs sit at 1.3e-24,
  suppressed by over 20 orders of magnitude.
- **Analog ring mod and crossfade** produce output in range: RMS 0.555 and
  0.127 at drive 0.5.
- **Every mode × all four CARRIER states** runs 1 s at 96 kHz. All output is
  finite, nothing sticks at full scale, and every mode except the vocoder
  sounds. The vocoder is silent here only because a 3 Hz modulator has no
  band energy, which is correct. DELAY runs in all four loop topologies,
  which exercises the restored read-index wrap.
- **Parasites' vocoder** with an internal saw carrier and a 440 Hz
  modulator: RMS 0.165.
- **The mapping:**
  - Note at LEVEL 1 noon is 66, and +1 V on J5 makes it 78.
  - ALGORITHM at exactly 1.0 gives 0.998 without reading past
    `lut_pot_curve`.
  - The internal sine carrier measures 370.0 Hz at 0 V and 740.0 Hz at
    +1 V (targets 370.0 and 740.0).

## Not done / open

- **Unflashed and unheard.** CPU at 96 kHz on the M7 is the main risk. Warps
  ran this at 96 kHz on a 168 MHz F405, and the Modulator runs from D2, but
  it's unmeasured here. Watch B2 and the P1 boot readout, especially in
  Vocoder mode (the 20-band filter bank).
- **Input level:** the Lab's codec against Warps' 16 Vpp full scale is
  unmeasured. Meld derived ×2.2 for the Patch; this port starts at 0 dB with
  IN GAIN to trim. Fold, drive and the comparator respond to level.
- **Left out vs the real module:** the normalization probe (see Jacks), the
  easter-egg frequency shifter of stock Warps (Parasites has its own
  frequency-shifter mode), and calibration (the Lab's factory CV
  calibration is used).
- **Meld's Patch extras** are not here: the coincidence clock, envelope CV
  outs, the second modulator (Follow/Chain), and the scope. They depend on
  Patch hardware, or the Lab's spare outputs would need a design of their
  own.
- **Meld's "stock Warps as a tenth mode"** (meld eb71964) is not here. Tim
  chose Parasites. It would be a second vendored tree under another
  namespace, as meld does.
- HostLink only, as in v0.1 of the other four: no Launchpad, no USB audio.
