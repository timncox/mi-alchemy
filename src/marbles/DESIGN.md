# Marbles for the Alchemy Lab V2

Mutable Instruments Marbles (random sampler: T gates, X/Y voltages) as the
`marbles` target of mi-alchemy. The DSP is `marbles::TGenerator` and
`marbles::XYGenerator`, vendored byte-identical from pichenettes/eurorack
@ 08460a6 and run at their native 32 kHz. Everything that was
`marbles.cc`'s `Process()`, `cv_reader.cc` and `ui.cc` lives in
`marbles_engine.h` (hardware-free, run by the native test) and
`marbles_alchemy.cpp` (jacks, pots, buttons, LEDs).

Home slot **10**, preset schema tags `'MRB'` (CPU extras) + `'MRBD'` (deja vu
button states).

## Panel

Front view: `[P1] B1 [P2] / [P3] B2 [P4] / [P5] B3 [P6]`.

| | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| **PLAY** | RATE | t BIAS | t JITTER | DEJA VU | X SPREAD | X BIAS |
| **SETUP** (hold B3) | X STEPS | LENGTH | t MODE (6) | X MODE (3) | X RANGE (3) | SCALE (6) |

- t MODE: Coin toss, Clusters, Drums, Independent, Divider, Three states
  (Marbles' two banks of three; Markov, which upstream constrains away, is
  not offered).
- X MODE: Identical, Bump, Tilt. X RANGE: +2 V, +5 V, ±5 V.
- SCALE: Marbles' six factory scales (major, minor, pentatonic, pelog, raag
  Bhairav, raag Shri), copied in `preset_scales.h`.
- First boot = Marbles' factory state: coin toss, identical, ±5 V, major,
  STEPS noon, LENGTH 8.

Buttons:

- **B1 t DEJA VU**, **B2 X DEJA VU**: Marbles' gesture, decided on release —
  from off, tap = on, hold 2 s = locked; from locked, any press = on; from
  on, tap = off, hold 2 s = locked. LEDs: dark off, green on (breathing
  while DEJA VU sits in its noon lock band), amber locked. B2 turns red when
  the callback averages > 80 %.
- **B3** shift to SETUP; flashes white on each T2 tick.
- B2+B3 held 2 s = Settings (B2 stands down while B3 is held).

Settings (B1 cycles pages):

| page | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| 0 main | brightness | t RANGE (¼, 1, 4×) | preset slot | preset action | J1 CLOCK (Auto / Ignore) | X CLOCK (Auto, T1+T2+T3, T1, T2, T3, J2) |
| 1 firmware | picker FILE | picker FLASH | picker DFU | J8 CV TO | EXTERNAL X | GATE LENGTH |
| 2 Y | Y SPREAD | Y BIAS | Y STEPS | Y DIVIDER (1/64..1/1) | Y RANGE | GATE JITTER |

The Y settings and the gate length / jitter are the parameters Marbles
hid behind button-hold combinations.

## Jacks (Tim's map, 2026-09-26)

| jack | role | path | update |
|---|---|---|---|
| J1 | t CLOCK in | codec in, AC-coupled | edge-detected per sample in the engine (+1.0 V on, +0.5 V off) |
| J2 | X CLOCK in | codec in, AC-coupled | same |
| J3 / J4 / J5 | X1 / X2 / X3 | MCP4728 (I²C) | staged when changed, one flush per 1 ms poll |
| J6 | Y | MCP4728 (I²C) | same |
| **J9** | **T1** | codec out, DC | **per sample** |
| **J10** | **T2** | codec out, DC | **per sample** |
| J7 | T3 | STM32 DAC | once per 16-frame block (0.5 ms) |
| J8 | CV in (default RATE) | ADC | read once per block |

**T assignment.** The codec pair is the only sample-accurate output path, so
it carries T2 — Marbles' steady master clock (`ramps.master < 0.5`), the one
most likely to clock other modules — and T1, keeping T1/T2 adjacent. T3
takes the STM32 DAC: its level is written at the end of each block, 0.5 ms
of jitter, inaudible for gates. J9/J10 are deliberately **not** claimed with
`EnableCvOutput()`: the SDK's audio shim would then overwrite the whole
block with one staged level (`AlchemyLabV2::AudioShim`), so the callback
writes `out[]` itself (±5 V ↔ ±1.0; gates 0 / +4.995 V).

**Gate delay.** Upstream delayed T by 2 samples to match its SPI DAC. Here
X reaches its jacks through the I²C DAC up to ~2 ms after the callback
computes it (≤1 ms poll wait + ~0.45 ms bus + one block), so T is delayed
**64 samples (2 ms)**: a sample-and-hold or envelope fired by T1 sees the
new X1, as on the original. Relative to J1's clock, T is 2 ms late.

**I²C cost.** A flush is one 12-byte multi-write to the MCP4728 (13 bytes
with the address, ~0.3 ms at 400 kHz) plus two PCA9557 writes for the LDAC
pulse (~0.15 ms): ~0.45 ms of blocking I²C in the control thread, only in
milliseconds where a value moved by ≥1 LSB (2.4 mV). Quantized X flushes
once per step; smooth/slewed X (STEPS left of noon) flushes every
millisecond, taking about half the 1 ms poll loop (which also does the B3
expander read). Never inside the audio callback.

**Patch sensing.** Marbles knew a cable was in J1/J2 from the jack's
normalling switch; the Lab cannot sense that. J1 counts as patched while
rising edges arrive (last one within 3 s); J1 CLOCK = Ignore forces the
internal clock. X CLOCK Auto = J2 while edges arrive, else T1+T2+T3.
Marbles' self-patching detector (a T output patched into X CLOCK) is not
ported — pick T1/T2/T3 in X CLOCK instead.

**J8.** Read in the callback, not through the SDK's CV matrix, so RATE gets
exact 1 V/oct (12 semitones per volt, as upstream's T_RATE channel). Other
destinations move their knob's whole travel over 10 V. EXTERNAL X = on
makes J8 the register input (Marbles' external processing: X samples and
quantizes the incoming voltage through `NoteFilter`); J8 then has no other
destination.

## Parameter scaling

From `cv_reader.cc`'s channel table: RATE = pot × 120 − 60 semitones
(clamped ±120) → 2 Hz × 2^(rate/12) internal, or the input divider table
with a clock; t BIAS × 1.05 − 0.025; X SPREAD / BIAS × 1.02 − 0.01 and
STEPS × 1.04 − 0.02 (the hysteresis widening in `CvReaderChannel::Init`);
DEJA VU through marbles.cc's noon deadband; LENGTH through the
`HysteresisQuantizer2` over `loop_length[]` (1..16). A 2 kHz one-pole
smooths the SDK's frame-rate knob values.

Random numbers: the STM32 hardware RNG feeds `RandomStream` from the 1 ms
poll, as upstream's RNG driver did; it falls back to its own generator.

## Sizes (2026-09-26)

`build-marbles/marbles_alchemy.bin` 398,844 B. SRAM 406,752 B of 480 KB
(82.75 %), DTCM 83,880 B, D2 32 KB (the LED window only), SDRAM 65,472 B.

## Left out vs. Marbles

- Scale recording (hold + external X) and the per-channel scale edits — the
  six factory scales only.
- Explicit reset mode, the self-patching detector, factory calibration and
  output test modes, colour-blind LEDs.
- Markov t model (upstream constrains it away too).
- CV on every knob: one CV input (J8), Tim's call for keeping Y.

## Native test

`make -C test test-marbles` runs `marbles_engine.h` over the vendored
generators at 32 kHz / 16 frames (ASan + UBSan): internal RATE noon = 2.000
Hz, +12 st = 4.000 Hz, J8 +1 V = 4.000 Hz; J1 at 3 Hz → 3.000 Hz (1/1),
12.000 Hz (×4), 64.000 Hz with J1 ignored; X within +2 / +5 / ±5 V and
filling them; deja vu locked at LENGTH 2, 5, 16 repeats X and T exactly;
quantized X sits on all six scales; EXTERNAL X tracks J8 (+2 V in → 2.000 V).

## Open risks

- **Nothing heard or seen: never run on hardware.**
- **X pitch accuracy.** `cv_jack.h`: J3..J8 read back ~±50 mV in output mode
  (calibration measured DG411-open); if the DAC fit is off by that much,
  X is ±60 cents out — fine for random modulation, not for melodies into a
  tuned oscillator. Measure a quantized X against a tuner first.
- **I²C jitter.** X steps land 0.5-2 ms after the callback computed them;
  the 2 ms T delay covers it on paper. The control loop shares I²C with the
  B3 expander read; a failed write returns false and the value retries on
  the next change only.
- **AC-coupled clock inputs.** Clocks and triggers are fine; a long held
  gate on J2 (used as an X clock with gates, or EXTERNAL X's timing) sags
  and releases early.
- Codec-out DC accuracy and polarity for J9/J10 gates are unverified; the
  SDK states ±5 V ↔ ±1.0.
- CPU: expected tiny (upstream ran on an F405 at 168 MHz with the whole UI);
  B2 turns red above 80 %.
