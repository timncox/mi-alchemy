// mi-alchemy shim for elements/drivers/debug_pin.h: found before vendor/ on the
// include path (Makefile). Upstream's pin toggles an STM32F4 GPIO for scope
// profiling; here it is a no-op, so the DSP compiles unchanged for the H750.
#ifndef ELEMENTS_DRIVERS_DEBUG_PIN_H_
#define ELEMENTS_DRIVERS_DEBUG_PIN_H_

#include "stmlib/stmlib.h"

namespace elements {

class DebugPin {
 public:
  static void Init() { }
  static void High() { }
  static void Low() { }
};

#define TIC DebugPin::High();
#define TOC DebugPin::Low();

}  // namespace elements

#endif  // ELEMENTS_DRIVERS_DEBUG_PIN_H_
