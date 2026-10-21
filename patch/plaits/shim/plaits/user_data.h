// mi-alchemy shim for plaits/user_data.h: found before vendor/ on the
// include path (Makefile). Upstream reads user wavetables / FM patches that
// were sent over the TIMBRE jack into the STM32F37x's flash at 0x08007000
// and includes the F37x headers to write them. This port has no data
// receiver, so there is never user data: ptr() is always NULL and the
// engines use their built-in tables (the six-op engines fall back to
// fm_patches_table, exactly as a fresh Plaits does).
#ifndef PLAITS_USER_DATA_H_
#define PLAITS_USER_DATA_H_

#include "stmlib/stmlib.h"

namespace plaits {

class UserData {
 public:
  enum {
    ADDRESS = 0x08007000,
    SIZE = 0x1000
  };

  UserData() { }
  ~UserData() { }

  inline const uint8_t* ptr(int slot) const { (void)slot; return NULL; }
  inline bool Save(uint8_t* rx_buffer, int slot) {
    (void)rx_buffer; (void)slot;
    return false;
  }
};

}  // namespace plaits

#endif  // PLAITS_USER_DATA_H_
