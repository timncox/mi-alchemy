# Plaits on the Alchemy Lab — design

Mutable Instruments Plaits (`plaits::Voice`, all 24 models) as an Alchemy Lab V2
firmware. The DSP is vendored byte-identical from pichenettes/eurorack 08460a6;
the one stand-in is `src/shim/plaits/user_data.h` (no user wavetables or FM
banks, so the six-op engines use their built-in `fm_patches_table`, as a fresh
Plaits does). Native 48 kHz. The codec block is 24 frames, rendered as two
12-frame `Render()` calls, because the Voice's envelope and LPG coefficients
assume `plaits::kBlockSize` (12) per call.

## Panel

Front view: `[P1] B1 [P2] / [P3] B2 [P4] / [P5] B3 [P6]`.

| | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| **PLAY** | MODEL (24 zones) | FREQUENCY | HARMONICS | TIMBRE | MORPH | DECAY |
| **SETUP** (hold B3) | OCTAVE (11 zones) | FINE ±1 st | FM att | TIMBRE att | MORPH att | LPG COLOUR |

Where each control comes from on the real Plaits:
- **MODEL** stands in for the two model buttons. Zones 1–8 are the 1.2 bank (VA+VCF, phase distortion, 6-op FM ×3, wave terrain, string machine, chiptune). Zones 9–16 are the classics. Zones 17–24 are noise, physical models and drums. The ring's selected dot is drawn in the bank colour.
- **OCTAVE** is Plaits' HARMONICS-hold setting, with ui.cc's formula unchanged:
  - LFO: −48.37 + 60·f
  - C0–C7: fixed octave, FREQUENCY ±7 st
  - Octaves: FREQUENCY quantised to octaves
  - Free: 60 + 48·f, the factory default
- **FINE** changes one thing from upstream. Upstream fine-tune (FREQUENCY-hold) only acts in the "Octaves" range, over ±7 st. Here it is a ±1 st trim in every range, and "Octaves" uses fine-tune's centre.
- **DECAY** and **LPG COLOUR** are the MORPH-hold and TIMBRE-hold settings.
- **FM / TIMBRE / MORPH att** are the panel's three attenuverters, with noon = off.

Buttons:
- **B1 STRIKE** is a trigger by hand, and holding it holds the gate. It ORs with J3.
- **B2 BANK** jumps MODEL to the same model in the next bank (+8, wrapping). The B2 LEDs show the active bank: orange = 1.2, green = classic, red = noise/drums. They turn red when the callback averages over 80 % CPU.
- **B3** shows the Setup page while held. B2+B3 held for 2 s opens Settings.

**Settings** (B2+B3 2 s):
- Page 0: brightness, presets, **TRIGGER** on P2 and **LEVEL CV** on P5.
- Page 1: the SD picker.

## Jacks

| Jack | Use | Scaling (Plaits' settings.cc defaults over a ±5 V span) |
|---|---|---|
| J1, J2 | unused | AC-coupled codec inputs. FM needs DC and a per-block rate, so they are left free. |
| J3 | TRIGGER gate | `mi::Gate`: >1.5 V on, <0.5 V off, polled every 24-frame block (0.5 ms). The Voice adds its own 1 ms trigger delay. |
| J4 | LEVEL | 0.2 / V, clamped 0..1. Only when Settings LEVEL CV = On. |
| J5 | V/Oct | 12 st / V (calibrated `Volts()`), one-pole 0.7 as ui.cc. |
| J6 | TIMBRE CV | 0.32 / V, scaled by TIMBRE att. |
| J7 | MORPH CV | 0.32 / V, scaled by MORPH att. |
| J8 | HARMONICS CV | 0.2 / V, added directly (Plaits has no attenuverter here). |
| J9 | OUT | main output |
| J10 | AUX | the model's aux output |

The CV matrix is off on all six jacks. Each one is read raw in the callback with
Plaits' own scaling.

### "Patched" flags without a normalisation probe

Plaits drives a probe signal through its normalled jacks to tell "patched at
0 V" from "nothing patched". The Lab has no probe, so each flag is decided another way:

- **timbre / morph_patched**: true while |V| > 50 mV has been seen in the last 2 s. The flag only changes what the attenuverter scales: the CV, or the internal decay envelope. A wrong guess is audible but harmless.
- **trigger_patched**: set by the Settings TRIGGER mode.
  - **Auto** (default): drone until the first trigger on J3 or B1 since power-up, then gated.
  - **Drone**: the LPG is always bypassed.
  - **Triggered**: always gated.
  - A timeout back to drone was rejected, because the LPG would open by itself after a quiet spell.
- **level_patched**: set by the Settings LEVEL CV switch, off by default. With it on, 0 V on J4 is silence.
- **frequency_patched**: always false, because there is no FM jack. The FM attenuverter then sets how far the internal envelope sweeps pitch once triggered, and speech prosody on the Speech model.

## Memory and size (build 2026-09-26, engine `-Os`)

| | |
|---|---|
| Image | 451,256 B (SRAM region 459,168 B of 480 KB, 93.4 %) |
| Engine at `-O2` instead | 470 KB image, SRAM 97.2 %: fits, but leaves ~13 KB for the shim |
| Shared buffer | 16 KB in D2 (`MI_D2_BSS`, memset at boot); D2 region 48 KB used |
| Voice object | `.bss`, in DTCM; DTCM total 82.7 KB of 128 KB |
| Home slot | 11; CpuExtras tag `'PLT'` |

## Left out vs the real Plaits

- **User data** (wavetables and DX7 banks sent as audio into TIMBRE): no receiver, and ptr() is always NULL.
- **Normalisation probe**: replaced by the heuristics above.
- **FM input jack**: there is no free DC input for it.
- **MODEL CV**: modulations.engine = 0.
- **Plaits' calibration procedure**: the Lab's factory CV calibration is used instead.
- **Colour-blind LED mode** and **alt navigation**: not needed on a single MODEL selector.

## Tests

`make -C test test-plaits` renders every engine for 1 s in drone and triggered
mode. It asserts no silent engine and no output pinned at the rails, and prints
an RMS table. Under ASan+UBSan the worst engine (Particle) is ~2 µs per 12-frame
call on an Apple M-series host; unsanitised at `-O2` it is 1.26 µs, against a
250 µs budget. UBSan reports one upstream left shift of a negative value
(`fm/dx_units.h:79`). It is well-defined on ARM GCC and left alone.

## Open risks (nothing here has run on hardware)

- **CPU on the M7 at `-Os`**: the host number is only a bound. Watch B2 and the P1 boot readout, especially on 6-op FM, Particle and Inharmonic string.
- **24 zones on a 16-LED ring**: MODEL's selector is dense. The coloured dot shows the choice, but zone boundaries are ~4 % of travel apart.
- **Output level**: int16 full scale maps to the codec's full scale. That hasn't been compared against a real Plaits (±5 V-ish), so it may be hot or quiet.
- **Level/timbre scaling** is derived from Plaits' default calibration, not measured.
