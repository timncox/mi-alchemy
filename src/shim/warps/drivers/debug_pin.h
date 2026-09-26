// mi-alchemy shim for warps/drivers/debug_pin.h: found before vendor/ on the
// include path (Makefile). Upstream toggles an STM32F4 GPIO around the DSP
// for scope profiling; here TIC/TOC do nothing, so the Parasites DSP
// compiles unchanged for the H750. (Same stand-in as meld's src/stubs.)
#ifndef WARPS_DRIVERS_DEBUG_PIN_H_
#define WARPS_DRIVERS_DEBUG_PIN_H_

#include "stmlib/stmlib.h"

namespace warps {

class DebugPin {
 public:
  static void Init() { }
  static void High() { }
  static void Low() { }
};

#define TIC
#define TOC

}  // namespace warps

#endif  // WARPS_DRIVERS_DEBUG_PIN_H_
