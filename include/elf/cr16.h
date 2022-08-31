/* CR16 ELF support for BFD.
   Please review: $(src-dir)/SPL-README for Licencing info. */

#ifndef _ELF_CR16_H
#define _ELF_CR16_H

#include "elf/reloc-macros.h"

/* Creating indices for reloc_map_index array.  */
START_RELOC_NUMBERS(elf_cr16_reloc_type)
  RELOC_NUMBER (R_CR16_NONE,           0)
  RELOC_NUMBER (R_CR16_NUM8,           1)
  RELOC_NUMBER (R_CR16_NUM16,          2)
  RELOC_NUMBER (R_CR16_NUM32,          3)
  RELOC_NUMBER (R_CR16_NUM32a,         4)
  RELOC_NUMBER (R_CR16_REGREL4,        5)
  RELOC_NUMBER (R_CR16_REGREL4a,       6)
  RELOC_NUMBER (R_CR16_REGREL14,       7)
  RELOC_NUMBER (R_CR16_REGREL14a,      8)
  RELOC_NUMBER (R_CR16_REGREL16,       9)
  RELOC_NUMBER (R_CR16_REGREL20,       10)
  RELOC_NUMBER (R_CR16_REGREL20a,      11)
  RELOC_NUMBER (R_CR16_ABS20,          12)
  RELOC_NUMBER (R_CR16_ABS24,          13)
  RELOC_NUMBER (R_CR16_IMM4,           14)
  RELOC_NUMBER (R_CR16_IMM8,           15)
  RELOC_NUMBER (R_CR16_IMM16,          16)
  RELOC_NUMBER (R_CR16_IMM20,          17)
  RELOC_NUMBER (R_CR16_IMM24,          18)
  RELOC_NUMBER (R_CR16_IMM32,          19)
  RELOC_NUMBER (R_CR16_IMM32a,         20)
  RELOC_NUMBER (R_CR16_DISP4,          21)
  RELOC_NUMBER (R_CR16_DISP8,          22)
  RELOC_NUMBER (R_CR16_DISP16,         23)
  RELOC_NUMBER (R_CR16_DISP24,         24)
  RELOC_NUMBER (R_CR16_DISP24a,        25)
  RELOC_NUMBER (R_CR16_SWITCH8,        26)
  RELOC_NUMBER (R_CR16_SWITCH16,       27)
  RELOC_NUMBER (R_CR16_SWITCH32,       28)
  RELOC_NUMBER (R_CR16_GOT_REGREL20,   29)
  RELOC_NUMBER (R_CR16_GOTC_REGREL20,  30)
  RELOC_NUMBER (R_CR16_GLOB_DAT,       31)
END_RELOC_NUMBERS(R_CR16_MAX)
        
#endif /* _ELF_CR16_H */
