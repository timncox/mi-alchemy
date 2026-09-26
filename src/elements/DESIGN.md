# Elements on the Alchemy Lab V2

Mutable Instruments Elements (Emilie Gillet, MIT): `elements::Part` from
pichenettes/eurorack 08460a6, byte-identical, at its native **32 kHz** and
**16-frame block**. The shim is `elements_alchemy.cpp`; it does what upstream's
`cv_scaler.cc`, `ui.cc` and `elements.cc` did.

Status 2026-09-26: builds, native test green. **Never flashed, never heard.**

## Pages

Panel, front view: `[P1] B1 [P2] / [P3] B2 [P4] / [P5] B3 [P6]`.

| Page | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| **PLAY** (base) | COARSE | FINE | GEOMETRY | BRIGHTNESS | DAMPING | POSITION |
| **EXCITER** (hold B3) | BOW level | BLOW level | STRIKE level | CONTOUR | FLOW (blow meta) | MALLET (strike meta) |
| **TIMBRE** (hold B2) | BOW timbre | BLOW timbre | STRIKE timbre | SPACE | MODEL | FM amount |

- Two `Pager::Shift` layers (B3, B2). B2+B3 held 2 s is still the Settings
  chord; the pager aborts a layer when both are down (pages-and-layers.md).
- **COARSE** is upstream's `LAW_QUANTIZED_NOTE`: 0-60 semitones with
  0.3-semitone hysteresis. **FINE** is `LAW_QUADRATIC_BIPOLAR`: ±2 semitones.
  **FM** is `LAW_QUARTIC_BIPOLAR`, scaled to ±12 semitones per volt on J6.
- **SPACE** runs 0-2 on the pot. Upstream's pot stopped at 1 and only CV
  reached the frozen reverb (≥ 1.75); here the last eighth of travel freezes it.
- **MODEL** is Modal / String / Strings / **Ominous**. Ominous is Elements'
  easter-egg voice (`set_easter_egg`), which upstream reached through a
  hidden button gesture. Upstream stored the model in calibration flash;
  here it is a knob and lives in the preset.
- The note is `24.1 + 12·V(J5) + coarse + 19 + fine`. Upstream's default
  calibration puts 0 V at note 24.1 (66.67 − 84.26 × a 0.505 reading at 0 V).
  At 0 V with COARSE at noon that is note 73 (554 Hz). This was derived from
  the constants, not by ear, so check it against a real Elements.
- Smoothing, once per block at 2 kHz like upstream:
  - pots use one-pole 0.01;
  - the bindings that take CV use 0.05, where upstream left the CV part
    unfiltered.

## Buttons and jacks

- **B1 PLAY**: the gate while held (momentary, so a tap is a short note, as
  on Elements). J3 is ORed in. As upstream, the gate is read one block
  after the CVs.
- **B2 / B3**: page layers only.

| Jack | Role |
|---|---|
| J1 | Blow in (audio; replaces the blow exciter). Upstream's noise gate below −80 dB |
| J2 | Strike in (audio; replaces the strike exciter). Same gate |
| J3 | Gate (`mi::Gate`, raw ADC, 1.5 V / 0.5 V) |
| J4 | Strength: `0.5 + 0.1·V`, so ±5 V spans 0..1 and 0 V (unpatched) gives 0.5, upstream's value at 0 V |
| J5 | V/Oct (calibrated `Volts()`); jumps > 0.4 semitone land at once, smaller moves glide 0.1 |
| J6 | FM, through the FM attenuverter |
| J7 / J8 | CV → GEOMETRY / BRIGHTNESS via `CvMatrix` (re-routable in the web programmer) |
| J9 / J10 | Main / Aux (upstream's two outs; with SPACE they are the stereo pair) |

## LEDs

- **B1**:
  - white while the gate is on;
  - otherwise the exciter level in orange over a dim base;
  - the base is dim blue when the samples are loaded and **dim amber when
    `/mi/elements.smp` is missing or bad**.
- **B2**: the resonator level in green; red when the callback averages over 80 %.
- **B3**: dim tint.
- **Boot, 2.5 s**:
  - P1 shows last session's CPU peak (as in Clouds);
  - **P2 shows the samples**: full green = loaded, one red pip = no file,
    full red = file refused.

## Samples on the SD card

`resources.cc` is 373 KB. Two of its tables are the exciter samples:
- `smp_sample_data`: 128,013 int16, the strike samples;
- `smp_noise_sample`: 40,963 int16, the blow noise.

Together they are 338 KB, and the 480 KB SRAM image can't hold them next to
the SDK. So:

1. **fw.mk** generates `build-elements/elements_resources_img.cc` from the
   vendored `resources.cc` with awk. It cuts exactly those two array
   definitions and checks that it found both. The copy under `vendor/`
   stays byte-identical.
   - If the cut misses, the link fails with duplicate definitions.
   - If it cuts too much, the link fails with undefined references.
2. **elements_storage.cpp** defines two SDRAM arrays under the engine's
   mangled names (`_ZN8elements15smp_sample_dataE`,
   `_ZN8elements16smp_noise_sampleE`). `exciter.cc` and `sample_table` link
   to them unchanged. That translation unit does not include `resources.h`,
   so no compiler ever sees the writable and const declarations together.
3. **At boot**, before audio starts, `load_samples()` reads
   `0:/mi/elements.smp`:
   - it reads in 4 KB chunks into an AXI staging buffer, and `es::Loader`
     streams each chunk into SDRAM;
   - anything short of an intact file zeroes both tables. The strike
     samples and blow noise then go silent, and everything else (bow,
     mallet synthesis, particles, external inputs, the resonator, reverb)
     works.
   - The folder is `/mi`, not `/alchemy`, because the picker lists `/alchemy`.

**File format** (`elements_samples.h`), little-endian: a 24-byte header,
then the two arrays. The header fields are:
- magic `MIES`
- version 1
- the two counts
- CRC-32 (zlib) of the payload
- a reserved word

The file is 337,976 bytes. `tools/elements_samples.cpp` writes it. The tool
`#include`s the vendored `resources.cc`, so the bytes are upstream's arrays,
and it `static_assert`s the counts. `make FW=elements` builds and runs it,
writing `build-elements/elements.smp`. At 08460a6 the file's sha256 is
`c1838da0e3ed431f02446bdc349998f1b8089b5139416af14eb39230424e90c5`.

It has to be copied to the card by hand as **`/mi/elements.smp`**. The repo
`stage` rule doesn't do that yet.

## Memory (build of 2026-09-26)

| Region | Use |
|---|---|
| SRAM (image) | 366 KB `.bin`, 378,848 B = 77.1 % of 480 KB. The image copy of `resources.cc` is 34.7 KB |
| D2 (cached) | `Part` 113 KB + reverb buffer 64 KB (`MI_D2_BSS`); 80.8 % of the region incl. the LED window. `Part` did not fit in DTCM beside the SDK (58 KB over) |
| DTCM | 75.8 KB (57.8 %) |
| SDRAM | 338 KB samples + the SDK's FatFS arena (403 KB total) |

## Tests

`make -C test test-elements` (host, `-DTEST`, ASan + UBSan):
- **Samples file.** The tool's output goes back through `es::Loader` in
  5 chunkings (4096, 1, 7, 23, 1 MiB) and is byte-identical to the vendored
  arrays. It also refuses:
  - a flipped payload bit (CRC);
  - bad magic;
  - a bad count;
  - truncation;
  - a short header;
  - trailing bytes;
  - and treats an empty file as Missing.
- **Engine, 2 s each at 32 kHz / 16 frames:** modal strike / bow / blow,
  string strike, strings strike and Ominous blow. Output is finite, not
  silent, and within the soft limit.
- **Pitch.** String model, autocorrelation f0:
  - note 57 → 220.17 Hz (+1.4 cents);
  - note 69 → 441.14 Hz (+4.5 cents).
  - The test uses geometry 0.25, where the string's dispersion is zero. At
    0.4 upstream's stiffness reads about +130 cents sharp. That is by design.
- **Known upstream UB**, reported by UBSan and left as upstream:
  `ominous_voice.h:155` casts a negative float to `uint32_t`. The F4 and the
  M7 both use the same saturating `VCVT`, so the behaviour is identical to
  the original module.

## Left out vs a real Elements

- **Calibration.** Upstream's V/Oct offset and scale are replaced by the
  Lab's calibrated `Volts()`. The 24.1 base note is from upstream's defaults.
- **Per-knob CV attenuverters.** Elements had 7. Here J7/J8 go through the
  SDK matrix instead, and there are no CVs for damping, position, space or
  the timbres unless re-routed.
- **Bypass mode and the factory test port.**
- **Upstream's "boot in easter-egg mode" flag.** MODEL covers it.
- **Launchpad and USB audio:** the mi-alchemy v0.1 scope, HostLink only.
- **Polyphony.** Upstream is `kNumVoices = 1` as well.

## Open risks

- **CPU.** Elements ran on a 168 MHz F405, so the M7 at 480 MHz should be
  comfortable, but it is untested. `Part` runs from D2 rather than DTCM.
  The boot readout on P1 and B2's red alarm will show the result.
- **Card timing.** The load happens at boot. If the card is slow or absent,
  `EnsureMounted` fails once and the samples stay silent until the next
  boot. There is no retry.
- **Pitch and strength at 0 V** are derived from constants and not heard
  (see Pages).
