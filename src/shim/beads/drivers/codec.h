// mi-alchemy shim for beads/drivers/codec.h: found before vendor/ on the
// include path (Makefile; test/beads.mk puts it first too). Upstream's header
// declares the PCM3060 SAI driver and pulls in beads/drivers/pins.h, which
// needs the STM32H7 HAL. The vendored DSP only needs Codec::Frame (GainControl
// takes the codec's 32-bit frames), so this is that type and nothing else.
#ifndef BEADS_DRIVERS_CODEC_H_
#define BEADS_DRIVERS_CODEC_H_

#include "stmlib/stmlib.h"

namespace beads {

class Codec {
 public:
  struct Frame {
    int32_t l;
    int32_t r;
  } __attribute__((packed, aligned(4)));
};

}  // namespace beads

#endif  // BEADS_DRIVERS_CODEC_H_
