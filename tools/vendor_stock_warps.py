#!/usr/bin/env python3
"""(mi-alchemy copy of ~/tim-os/meld/tools/vendor_stock_warps.py; only the default
output path and the stub note differ.)

Vendor stock Warps' DSP beside the Parasites copy, renamed so both link.

Usage: tools/vendor_stock_warps.py <path to upstream warps dir> [<out dir>]

Both trees declare `namespace warps` and include "warps/dsp/...", so the
stock copy cannot sit in the include path untouched. This script copies
`dsp/*` and `resources.{h,cc}` and applies exactly these textual changes:

  namespace warps        -> namespace warps_stock
  warps::                -> warps_stock::
  #include "warps/       -> #include "warps_stock/
  WARPS_..._H_ guards    -> WARPS_STOCK_..._H_
  LUT_* / WAV_* macros   -> STOCK_LUT_* / STOCK_WAV_* (resources.h index
                            constants; both trees define them with different
                            values, and a firmware file sees both headers)
  the debug_pin include  -> the no-op stub (mi-alchemy: src/shim/warps, shared with Parasites)

Nothing else. Rerun it against a fresh upstream checkout to update; the
result is reproducible, which is what stands in for byte-identical here.
"""
import os
import re
import shutil
import sys

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'vendor', 'warps_stock')
out = os.path.abspath(out)

files = [('dsp', n) for n in sorted(os.listdir(os.path.join(src, 'dsp'))) if n.endswith(('.h', '.cc'))]
files += [('', 'resources.h'), ('', 'resources.cc')]

if os.path.isdir(out):
    shutil.rmtree(out)
os.makedirs(os.path.join(out, 'dsp'))

def transform(text):
    text = text.replace('namespace warps', 'namespace warps_stock')
    text = text.replace('warps::', 'warps_stock::')
    text = text.replace('#include "warps/drivers/debug_pin.h"', '#include "warps/drivers/debug_pin.h"  // stub, shared')
    text = re.sub(r'#include "warps/(?!drivers/)', '#include "warps_stock/', text)
    text = re.sub(r'\bWARPS_(?=[A-Z_]*_H_\b)', 'WARPS_STOCK_', text)
    text = re.sub(r'\b(LUT|WAV)_([A-Z0-9_]+)\b', r'STOCK_\1_\2', text)
    return text

for sub, name in files:
    with open(os.path.join(src, sub, name), encoding='utf-8') as f:
        text = f.read()
    with open(os.path.join(out, sub, name), 'w', encoding='utf-8') as f:
        f.write(transform(text))
    print('wrote', os.path.relpath(os.path.join(out, sub, name)))
