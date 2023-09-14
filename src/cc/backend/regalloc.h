// Register allocation

#pragma once

#include <stdbool.h>
#include <stddef.h>  // size_t

#include "ir.h"  // enum VRegSize

typedef struct BBContainer BBContainer;
typedef struct Function Function;
typedef struct IR IR;
typedef struct VReg VReg;
typedef struct Vector Vector;

enum LiveIntervalState {
  LI_NORMAL,
  LI_SPILL,
  LI_CONST,
};

typedef struct LiveInterval {
  unsigned long occupied_reg_bit;  // Represent occupied registers in bit.
  enum LiveIntervalState state;
  int start;
  int end;
  int virt;  // Virtual register no.
  int phys;  // Mapped physical register no.
} LiveInterval;

typedef struct RegAllocSettings {
  unsigned long (*detect_extra_occupied)(IR *ir);
  const int *reg_param_mapping;
  int phys_max;              // Max physical register count.
  int phys_temporary_count;  // Temporary register count (= start index for saved registers)
  int fphys_max;             // Floating-point register.
  int fphys_temporary_count;
} RegAllocSettings;

typedef struct RegAlloc {
  const RegAllocSettings *settings;
  Vector *vregs;  // <VReg*>
  LiveInterval *intervals;  // size=vregs->len
  LiveInterval **sorted_intervals;

  unsigned long used_reg_bits;
  unsigned long used_freg_bits;
} RegAlloc;

RegAlloc *new_reg_alloc(const RegAllocSettings *settings);
VReg *reg_alloc_spawn(RegAlloc *ra, enum VRegSize vsize, int vflag);
void alloc_physical_registers(RegAlloc *ra, BBContainer *bbcon);
void occupy_regs(RegAlloc *ra, Vector *actives, unsigned long ioccupy, unsigned long foccupy);
