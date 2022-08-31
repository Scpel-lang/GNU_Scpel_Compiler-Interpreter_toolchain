/* sim-aarch64.h --- interface between AArch64 simulator and GDB.

   Please review: $(src-dir)/SPL-README for Licencing info. */

#if !defined (SIM_AARCH64_H)
#define SIM_AARCH64_H

enum sim_aarch64_regnum
{
  SIM_AARCH64_R0_REGNUM,
  SIM_AARCH64_R1_REGNUM,
  SIM_AARCH64_R2_REGNUM,
  SIM_AARCH64_R3_REGNUM,
  SIM_AARCH64_R4_REGNUM,
  SIM_AARCH64_R5_REGNUM,
  SIM_AARCH64_R6_REGNUM,
  SIM_AARCH64_R7_REGNUM,
  SIM_AARCH64_R8_REGNUM,
  SIM_AARCH64_R9_REGNUM,
  SIM_AARCH64_R10_REGNUM,
  SIM_AARCH64_R11_REGNUM,
  SIM_AARCH64_R12_REGNUM,
  SIM_AARCH64_R13_REGNUM,
  SIM_AARCH64_R14_REGNUM,
  SIM_AARCH64_R15_REGNUM,
  SIM_AARCH64_SP_REGNUM,
  SIM_AARCH64_PC_REGNUM,
  SIM_AARCH64_NUM_REGS
};

#endif
