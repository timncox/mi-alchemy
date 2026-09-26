# Clouds for the Alchemy Lab

Mutable Instruments Clouds (Emilie Gillet, MIT): `clouds::GranularProcessor`
unmodified except one restored line (vendor/VENDOR.md), at its native
**32 kHz / 32-frame block**. `clouds_alchemy.cpp` does what Clouds'
`cv_scaler.cc` and `ui.cc` did.

## Panel

```
        PLAY                         SETUP (hold B3)
  [POSITION] B1 [SIZE]         [MODE]    B1 [QUALITY]
  [PITCH]    B2 [DENSITY]      [SPREAD]  B2 [FEEDBACK]
  [TEXTURE]  B3 [BLEND]        [REVERB]  B3 [IN GAIN]
```

| Control | Does | Range / curve |
|---|---|---|
| POSITION | where grains read from (now to oldest) | 0..1, one-pole 0.05 at 1 kHz |
| SIZE | grain size / loop length / FFT window | 0..1, 0.01 |
| PITCH | transpose | Clouds' `lut_quantized_pitch` (semitone detents near noon), ±24 st; + J5 V/Oct |
| DENSITY | grain rate; noon = none, CW regular, CCW random | 0..1, 0.01 |
| TEXTURE | grain envelope; past 3 o'clock, diffuser | 0..1, 0.01 |
| BLEND | dry/wet | `x*1.05-0.025`, clamped to [0, 0.99999] |
| MODE | Granular / Stretch / Looping delay / Spectral | 4-zone selector |
| QUALITY | 16-bit stereo (1 s) / 16-bit mono / 8-bit µ-law stereo / 8-bit mono | 4-zone selector |
| SPREAD, FEEDBACK, REVERB | what Clouds' BLEND knob cycled to | 0..1, 0.05 |
| IN GAIN | input gain | −12..+12 dB, 0 dB at noon |

Clouds' BLEND knob was one pot shared by four parameters, chosen with a
button. Each parameter gets its own pot here, which is why the second page
exists.

**Buttons:** B1 FREEZE (hold for momentary, tap to latch; J3 ORs in).
B2 SEED fires a grain, the same as a trigger on J4. B3 held shows SETUP.
B2+B3 held 2 s opens Settings (brightness, presets); Settings page 1 is the
SD firmware picker.

**LEDs:** B1 white while frozen. B2 is the input meter: green, amber above
0.7, red at clip. It flashes white on a seed, and turns red when the
callback averages over 80 % CPU. For 2.5 s at boot, the P1 ring shows the
previous session's worst CPU load.

## Jacks

| Jack | Signal | Read |
|---|---|---|
| J1 / J2 | In L / R | codec |
| J3 | FREEZE gate | raw ADC per block, 1.5 V / 0.5 V hysteresis (`mi::Gate`) |
| J4 | TRIGGER (seed) | same; edge delayed one block, as upstream's ADC-latency delay |
| J5 | V/Oct → pitch | `CvJack::Volts()` ×12 per block; jumps > ½ st land at once, smaller glide (upstream's rule) |
| J6 / J7 / J8 | CV → POSITION / SIZE / DENSITY | SDK CvMatrix (re-routable in the web programmer) |
| J9 / J10 | Out L / R | codec |

Upstream's TEXTURE and BLEND CV inputs are left out: there are only six CV
jacks. Re-route J6–J8 in the web programmer if you need those.

## Memory and size (build 5b1b62a)

- Image 387 KB of the 480 KB SRAM region (80 %).
- Recording buffer 118,784 B and working buffer 65,408 B, both in cached D2
  SRAM (`.d2_bss`, 184 of 224 KB). DTCM was the natural home for the F4's
  CCM buffer, but the SDK leaves only ~48 KB of it free.
- `Prepare()` runs in the main loop after every `loop.Tick()`, as Clouds'
  own main loop did. It performs buffer resets on a MODE or QUALITY change,
  and the phase vocoder's FFT work in Spectral mode.

## Tests (`make -C test test-clouds`)

- All 4 modes × 4 qualities render 3 s of a 220 Hz tone: finite, not
  silent.
- Freeze holds the buffer: RMS 0.18 frozen with silent input, 0.0 unfrozen.
- The pitch curve is 0 st at noon and spans ±24 st.
- Host time is ~0.02 s of CPU per second of audio (Apple silicon; not an M7
  figure).

The test found two upstream issues:
1. **STRETCH is silent on eurorack master.** `Window::Start()` lost its
   `done_ = false` in the 2023-03 merge `d1d8839c`. The line is restored
   and listed in VENDOR.md.
2. **Interpolated lookup tables read one float past the end** at phase
   exactly 1.0 (`lut_xfade_in` at dry/wet 1.0, `lut_window`). The read is
   multiplied by a zero fraction, so it's harmless. The firmware caps
   dry/wet anyway, and the test harness disables ASan global redzones.
   UBSan also reports pointer-offset arithmetic in `stmlib/fft/shy_fft.h`.
   That's upstream idiom and non-fatal.

## Not done / open

- **Unheard, unflashed.** CPU on the M7 is unmeasured. It should be light,
  since Clouds ran on a 168 MHz F405, but Spectral mode does its FFT work
  in the main loop, where it competes with the SDK's LED and HostLink work.
- No Launchpad, no USB-audio mode, and `usb.cfg` is never written (v0.1
  scope; see the repo README).
- Clouds' "save/load buffer to flash" (hold both buttons) is not ported;
  presets hold only the knobs.
- Input level: the Lab's codec input scaling against Clouds' ±10 Vpp is
  unverified. Use IN GAIN if it's too hot or quiet.
