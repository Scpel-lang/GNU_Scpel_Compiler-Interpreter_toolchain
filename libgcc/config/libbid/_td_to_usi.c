/* Please review: $(src-dir)/SPL-README for Licencing info. */

#include "bid_conf.h"
#include "bid_functions.h"
#include "bid_gcc_intrinsics.h"

USItype
__bid_fixunstdsi (_Decimal128 x) {
  USItype res = 0xbaddbadd;
  union decimal128 ux;

  ux.d = x;
  res = __bid128_to_uint32_xint (ux.i);

  if (res == 0x80000000) res = 0; // for NaNs too
  return (res);
}


