# Vendored code

`eurorack/` is a subset of [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
at **08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4** (2023-08-16), with its pinned
stmlib at **e3bd7c9cc00e4364166f9905c0509b6ffd0535ec**. The STM32F projects are
MIT-licensed (upstream README: "Code (STM32F projects): MIT license"); every
file keeps its copyright header.

Taken:
- `clouds/`, `elements/`, `marbles/`, `plaits/` -- whole module directories
  minus `bootloader/`, `hardware_design/` and the Python build hooks. Only the
  DSP (`dsp/`, `random/`, `ramp/`, `resources.*`) is compiled; the drivers,
  UI and main files stay as reference for how the original panel drove it.
- `stmlib/` -- `stmlib.h`, `dsp/`, `fft/`, `utils/`, `algorithms/`, `ui/`,
  `test/` and the licence. Not `third_party/` (43 MB of ST CMSIS/HAL for the
  F-series), `system/`, `linker_scripts/` or `programming/`.

`beads/` comes from a later commit, pichenettes/eurorack **d8f473915232**
(2026-10-12, "Beads code/sch"), which is the first release of Beads' source.
The next commit, f4cfa76, only edits README.md, and its note reads "Code
provided as is, builds on a Desktop platform but not verified to build and
run on a module". It is MIT, with headers as the rest. The whole directory
is taken except `hardware_design/`, `bootloader/` and the `__init__.py`
build hooks. Only the DSP (`dsp/`, `resources.*`) and `gain_control.*` are
compiled.

The commit touches only `beads/` and README.md, and it leaves the stmlib
submodule pointer alone, so it builds against the stmlib above (e3bd7c9).
`beads/drivers/codec.h` is shadowed by `src/shim/beads/` (only `Codec::Frame`
is needed, and the real header pulls in the H7 HAL through `pins.h`).

Rules:
- Upstream files are byte-identical unless a change is marked
  `// mi-alchemy:` in the file and listed below.
- Never define `TEST` in a firmware build: it is stmlib's host switch, and it
  collides with ST headers (meld's lesson). Native tests define it.

`parasites/warps/` is Warps in Matthias Puech's Parasites build, from
[mqtthiqs/parasites](https://github.com/mqtthiqs/parasites) `master` at
**32fa66f5ac** (2021-02-18), directory `warps/`: `dsp/*.h`, `dsp/*.cc`,
`resources.h`, `resources.cc`. Copied from `~/tim-os/meld/src/vendor/warps`,
which took it from that commit. MIT (see `parasites/LICENSE.md`). Not taken:
`drivers/` (`warps/drivers/debug_pin.h` is shadowed by `src/shim/warps/`),
`ui.cc`, `cv_scaler.cc`, `settings.cc`, `warps.cc`. It compiles against the
eurorack stmlib above (e3bd7c9) unchanged; Parasites' own pinned stmlib
(mqtthiqs/stmlib 8ab2aae) is not vendored, and test/test_warps.cpp passes
against this one.

`warps_stock/` is stock Mutable Warps, used by Meld's tenth mode, from
pichenettes/eurorack `warps/` (last touched at **3c23d0306d**, 2019-04-02,
identical at 08460a6): `dsp/*.h`, `dsp/*.cc`, `resources.h`, `resources.cc`.
MIT. **Not byte-identical, by construction:** both it and Parasites declare
`namespace warps` and include `"warps/dsp/..."`, so this copy is produced by
`tools/vendor_stock_warps.py` (copied from `~/tim-os/meld`, only its default
output path changed). The tool renames the namespace to `warps_stock`, the
include prefix to `warps_stock/`, header guards to `WARPS_STOCK_...`, the
`LUT_*`/`WAV_*` index macros to `STOCK_LUT_*`/`STOCK_WAV_*` (the two trees
give them different values, and Meld's firmware sees both headers), and
marks the debug-pin include as the shared stub (`src/shim/warps/`). Nothing
else. Verified 2026-09-26: rerunning the tool on this repository's eurorack
checkout reproduces `vendor/warps_stock/` exactly. The firmware compiles
the stock `.cc` files through `src/meld/stock/stock_*.cc` because the
Makefile keys objects by basename and both trees have `modulator.cc` etc.

Changes:

1. `clouds/dsp/window.h`, `Window::Start()`: restored `done_ = false;`.
   Upstream had it twice until 2023-03; `fbb53ba2` ("Duplicate variable
   assignment") removed one and the merge `d1d8839c` (2023-03-13) lost the
   other, so on eurorack master a WSOLA window, once finished, is never
   restarted and Clouds' STRETCH mode goes silent after its first window.
   Found by test/test_clouds.cpp (stretch RMS 0.001 vs 0.1-0.3 for the other
   modes); upstream's own host test never runs stretch. Shipped Clouds
   firmware predates the regression.

2. `parasites/warps/dsp/modulator.cc`, `ProcessDelay()`: after the upstream
   `while (index < 0)` wrap, a matching `while (index >= DELAY_SIZE)` wrap.
   Upstream only wraps negative read indices; a reverse-direction read can
   land past `DELAY_SIZE` and fetch memory past the object (a silent
   garbage read on Warps, a heap overflow under ASan). Found and fixed in
   meld; carried over and re-marked `// mi-alchemy (fix from meld):`.

3. `beads/dsp/processor.h`: `#define ENABLE_HARDWARE_24K` commented out.
   That switch is upstream's own, and the header documents both paths. With
   it set, Beads restarts its codec at 24 kHz for the tape qualities. With
   it off, the codec stays at 48 kHz and `GranularProcessor` decimates the
   wet signal by 2. The Lab's SAI has no 24 kHz setting, and the SDK's
   controls are set up for the boot rate, so the 48 kHz path is the one that
   fits. The Clouds quality's 32 kHz is handled outside the vendor tree
   (`src/beads/resample_3_2.h`).
