# Meld: Warps Parasites (vendor/parasites/warps, shared with src/warps) plus
# stock Warps as a tenth mode (vendor/warps_stock, renamed into namespace
# warps_stock by tools/vendor_stock_warps.py) plus Meld's extras, at 96 kHz /
# 60-frame blocks. The panel mapping is src/warps/warps_params.h, reused
# read-only; src/meld/meld_params.h adds the stock mapping and the extras.
# The stock .cc files are reached through src/meld/stock/stock_*.cc because
# both trees have modulator.cc, vocoder.cc, ... and objects are keyed by
# basename.
C_INCLUDES += -Ivendor/parasites -Ivendor -Isrc/warps
CC_SOURCES += \
    vendor/parasites/warps/resources.cc \
    vendor/parasites/warps/dsp/filter_bank.cc \
    vendor/parasites/warps/dsp/modulator.cc \
    vendor/parasites/warps/dsp/oscillator.cc \
    vendor/parasites/warps/dsp/vocoder.cc \
    src/meld/stock/stock_resources.cc \
    src/meld/stock/stock_filter_bank.cc \
    src/meld/stock/stock_modulator.cc \
    src/meld/stock/stock_oscillator.cc \
    src/meld/stock/stock_vocoder.cc

# Size (build of 2026-09-26): the SRAM image is 97.4 % at -O3 (~13 KB left),
# 94.6 % at -O2 (~26 KB), 92.3 % at -Os (~38 KB). CPU at 96 kHz is this
# firmware's open risk (stock Warps' vocoder especially), so the engines take
# -O2: most of -O3's speed with twice its headroom. -Os is the next step if
# the image ever grows; -O3 if the module shows headroom and CPU is short.
FW_CC_OPT := -O2
