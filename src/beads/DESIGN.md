# Beads for the Alchemy Lab

Mutable Instruments Beads (Emilie Gillet, MIT): `beads::Processor`,
`beads::Synth` and `beads::GainControl`. They come from pichenettes/eurorack
**d8f4739** (2026-10-12), the commit that finally opened Beads' source. The
code is unmodified except for one flag (vendor/VENDOR.md, change 3).
`beads_alchemy.cpp` does what Beads' `beads.cc`, `cv_reader.cc` and `ui.cc`
did.

Upstream's README says the code is "provided as is, builds on a Desktop
platform but not verified to build and run on a module". This port builds it
for the H750 and tests it natively. Nobody has heard it on a module yet.

## Sample rate

Beads restarts its codec at whatever rate the current quality needs. By
default (`ENABLE_HARDWARE_24K`) that is 24 / 48 / 32 / 48 kHz. The Lab's SAI
has no 24 kHz rate, and its pots, CV jacks and CPU meter are set up for the
rate given at boot. So:

- **Upstream's own 48 kHz path.** With `ENABLE_HARDWARE_24K` off, Beads
  runs the codec at 48 kHz for both tape qualities. The wet signal is
  decimated by 2 inside `GranularProcessor` (`ratio() == 2`), and the wow and
  flutter's low-pass is set for 48 kHz. That is the code Beads ships for
  boards without a 24 kHz codec setting. It is the one vendor edit.
- **The Clouds quality wants 32 kHz.** For that quality, the 48 kHz stream is
  resampled to 32 kHz around the engine (`resample_3_2.h`, a 96-tap
  polyphase sinc, cutoff 13.5 kHz). This stands in for the codec's own
  anti-alias filter at 32 kHz. Each 12-frame block becomes 8 engine frames.
  The test measures 1 kHz and 10 kHz at 0.00 dB, and 20 kHz at -94 dB.
- **The block is 12 frames**, so the block rate is Beads' own 4 kHz
  `kBlockRate`.

## Panel

```
      PLAY                    MOD (hold B3)                 SETUP (hold B3, then B1)
  [TIME]    B1 [SIZE]     [TIME~]    B1 [SIZE~]       [QUALITY] B1 [INPUT]
  [PITCH]   B2 [SHAPE]    [PITCH~]   B2 [SHAPE~]      [IN GAIN] B2 [SEED IN]
  [DENSITY] B3 [DRY/WET]  [FEEDBACK] B3 [REVERB]      [OUTPUT]  B3
```

| Control | Does | Range / law |
|---|---|---|
| TIME | where grains read from; the delay time in delay mode | 0..1 of the whole buffer |
| SIZE | grain length; below ⅓ reversed; fully CW (> 0.99) = delay mode, a 1.5 s crossfade | 0..1 |
| PITCH | transposition, ±24 st, plus J5 V/Oct | Beads' `LAW_DETUNE`, with detents at 0, 1, 3, 4, 7, 12, 24 st (`beads_laws.h`) |
| SHAPE | grain envelope | 0..1 |
| DENSITY | noon none; CCW regular, CW random; with J4, divisions CCW and probability CW | 0..1 |
| DRY/WET | mix | ×1.005, clamped (beads.cc) |
| TIME~ SIZE~ PITCH~ SHAPE~ | Beads' attenuverters; see below | `LAW_ATTENUVERTER`, -1..+1 with a ±3 % dead band |
| FEEDBACK | output back into the recording; in Synth input, the wavetable bank | 0..1 ×1.005 |
| REVERB | reverb amount | 0..1 ×1.005 |
| QUALITY | Hi-fi (48 kHz 16-bit) / Clouds (32 kHz 12-bit) / Clean lo-fi (24 kHz 12-bit) / Tape (24 kHz 8-bit µ-law, wow) | 4 zones = `Media` 3..0 |
| INPUT | Stereo / Mono (J1, double length) / Synth | 3 zones |
| IN GAIN | bottom 4 % AUTO (Beads' AGC), else manual ×1..×40 | `State::gain` 0 / 1..255 |
| SEED IN | Auto / Off / J4 | 3 zones |
| OUTPUT | Stereo / Mono + grain gate on J10 (Beads' gate-output mode) | 2 zones |

### The attenuverters

Beads tells an attenuverter's two roles apart with a normalization probe. With
nothing patched into the matching CV input, it is a randomness amount. With a
cable patched there, it scales the CV.

The Lab has no probes, so every `RandomizedParameter` here is the unpatched
kind (`has_cv = false`), and the ~ knobs are always randomness amounts
(`randomized_parameter.h`):
- TIME~, SIZE~ and SHAPE~ draw a new value for each grain. Clockwise of noon
  it's an even spread scaled by the knob; counter-clockwise, rarer and bigger
  jumps.
- PITCH~ is a random walk of up to ±24 st, four times faster clockwise of
  noon and four times slower counter-clockwise.
- In delay mode, TIME and SHAPE use walks as well.

CV on J6–J8
reaches TIME, SIZE and DENSITY through the SDK's CV matrix: it moves the
knob, and the web programmer can re-route it to SHAPE, a ~ knob or anything
else. V/Oct on J5 is added to PITCH's base the way `Synth::Render` folds a
patched pitch CV in.

What this gives up:
- CV scaled by an attenuverter. The matrix's own depth does that job.
- Beads' split of DENSITY CV into linear and exponential FM. CV just moves
  the knob, which is what Beads does anyway once J4 is clocking it.

### Buttons

- **B1 FREEZE:** tap to latch, hold for momentary (J3 ORs in). Beads' button
  only latches; the hold is the family's.
- **B2 SEED** follows `ui.cc`:
  - Out of the box the seed is *locked*, so DENSITY runs the grains.
  - Pressing B2 unlocks it. Grains then come only while B2 is held, or from
    J4.
  - Holding B2 for 4 s, or pressing B1 while B2 is held (Beads' SEED +
    FREEZE), locks it again.
  - The lock isn't saved, as upstream's isn't.
- **B3** holds the Mod page. With B3 held, B1 becomes the Setup shift
  (`Pager::Shift(B1).From(kPageMod)`). B2+B3 held 2 s stays free for
  Settings.

A QUALITY or INPUT change reconfigures the processor and clears the
recording. As upstream does, this waits while frozen; B3's LED turns amber
while a change is pending. A change is followed by 250 ms of silence
(`codec_transition_frames`).

**LEDs:**
- B1 is white while frozen.
- B2 shows:
  - the AGC'd input meter while the seed is locked (green, amber above 0.7,
    red at clip);
  - violet while unlocked;
  - white while held;
  - red when the callback averages over 80 % CPU.
- For 2.5 s at boot, the P1 ring shows the previous session's worst CPU load.

## Jacks

| Jack | Signal | Read |
|---|---|---|
| J1 / J2 | In L / R | codec → Beads' GainControl (auto or manual gain, soft clip by medium) |
| J3 | FREEZE gate | raw ADC per block (`mi::Gate`) |
| J4 | SEED: triggers / clock | same; per-sample `GateFlags` with the edge on the block's first frame |
| J5 | V/Oct → PITCH | `CvJack::Volts()` ×12, clamped ±60 st (Beads' range) |
| J6 / J7 / J8 | CV → TIME / SIZE / DENSITY | SDK CvMatrix (re-routable) |
| J9 / J10 | Out L / R, or mono + grain gate (+0.8 high; Beads' inverting codec wrote -0.8) | codec |

**SEED IN Auto** counts J4 as patched while it has had a rising edge in the
last 8 s, twice Beads' own 4 s divider resync. A patched J4 hands DENSITY to
the clock: with the seed locked it becomes Beads' divider (÷1…÷16) and
probability, and unlocked, J4's gates merge with B2. **Off** ignores J4.
**J4** treats it as patched even when silent, which is upstream's behavior
with a cable in.

Upstream delays the seed input by 2.5 ms so that a trigger lands with the CV
sampled alongside it. That is left out here: CV on this port comes through
the SDK's ~60 Hz matrix, so a 2.5 ms delay would not line anything up.

There's no ASSIGN CV input: it would be a seventh CV jack. Beads' assignable
CV went to FEEDBACK, DRY/WET or REVERB; re-route J6–J8 in the web programmer
to get the same.

## Memory and size

- **Image:** 474 KB of the 480 KB SRAM region (96.5 %), with the engine at
  `-Os` (`fw.mk`). At `-O2` it overflows by 7.8 KB: `processor.cc` alone is
  52 KB at `-O2` against 32 KB at `-Os`, because it holds four
  `GranularProcessor` instantiations. Resources are 105 KB of tables.
- **Delay memory:** 768 KB in SDRAM (cached, write-back; libDaisy's MPU
  region 1). Upstream had 512 KB in D1 and 256 KB in D2, but the Lab's D1
  holds the image. The three views `beads.cc` makes of it are kept.
- **Reverb buffer:** 64 KB in D2 (`.d2_bss`). Upstream kept it in D3.
- `Processor` is 25.8 KB and `Synth` 4.4 KB, both in ordinary `.bss`.

## Tests (`make -C test test-beads`, ASan + UBSan)

1. Every quality × stereo/mono renders 3 s of a 220 Hz tone through dense
   random grains: finite, not silent (wet rms 0.03 to 0.08).
2. Delay mode sounds after its crossfade, in hi-fi and in the resampled
   Clouds quality.
3. Freeze holds: rms 0.069 frozen with silent input, against 0.0 unfrozen.
4. The wavetable synth renders at all four resolutions.
5. The resampler: 1 kHz and 10 kHz at 0.00 dB, 20 kHz at -94 dB.
6. Pot laws: PITCH ±24 st with 0 at noon and a fifth detent; attenuverter
   ±1 with the dead band; gain 0 (auto) at the bottom, 255 at the top.
7. Host time without sanitizers is 0.006 s per second of audio, against
   Clouds' 0.005 on the same machine (Apple silicon, not an M7 figure).

The first run "failed" the lo-fi qualities as silent. That was the test, not
the engine: TIME is a fraction of the whole buffer, which is 8–16 s in the
lo-fi media, so TIME 0.3 read from before the 3 s recording began.

## Not done / open

- **Autosave lives on the card, not in a QSPI slot.** The firmware writes
  `/alchemy/state/beads.st` (`src/common/home_state.h`): five seconds after
  the last change, hands off, never while Settings or the picker is busy.
  It writes `beads.tmp` first, then renames it. All 16 slots were taken
  when Beads arrived. Two consequences:
  - Without a card, nothing is saved; it retries every 30 s, and every boot
    starts from defaults.
  - `host.BootSlot` is left at the SDK's default of 0, which is what
    HostLink reports to the web programmer. Slot 0 still loads first at
    boot (BootLoad), and the card file then overrides it, as a home slot
    did.

  The file write has not run on hardware yet. Only the format is tested
  natively (`make -C test test-state-file`).
- **Unheard and unflashed.** CPU on the M7 is unmeasured. Beads was written
  for a 400 MHz H7 and this is a 480 MHz one, but the engine is at `-Os` and
  grains read the delay memory from SDRAM where Beads used AXI SRAM. That is
  the main risk. If B2 shows red, there are two levers. One is the
  optimization level: `-O2` needs about 8 KB more SRAM, which means moving
  something out of the image. The other is where the delay memory lives.
  None of D2's 160 KB of free space can take one of Beads' 256 KB views.
- Beads' automatic synth mode (10 s with nothing patched) needs the
  normalization probe, so here it's INPUT = Synth.
- Not ported:
  - Beads' flash backup of a frozen buffer (`background_buffer_backup`).
    Presets hold only the knobs.
  - The calibration and factory test modes.
  - The advanced features upstream leaves `#if`'d out (monophonic grains,
    buffer trim).
- Input level: the Lab's codec scaling against Beads' input range is
  unverified. AUTO gain learns the level for 5 s after a reconfigure.
