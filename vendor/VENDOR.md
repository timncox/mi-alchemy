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

Rules:
- Upstream files are byte-identical unless a change is marked
  `// mi-alchemy:` in the file and listed below.
- Never define `TEST` in a firmware build: it is stmlib's host switch, and it
  collides with ST headers (meld's lesson). Native tests define it.

Changes: none yet.
