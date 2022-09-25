#include "../config.h"
#include "ir.h"

#include <assert.h>
#include <stdlib.h>  // malloc

#include "aarch64.h"
#include "regalloc.h"
#include "table.h"
#include "util.h"

#define WORK_REG_NO  (PHYSICAL_REG_MAX)

static void push_caller_save_regs(unsigned short living, int base);
static void pop_caller_save_regs(unsigned short living, int base);

static enum ConditionKind invert_cond(enum ConditionKind cond) {
  assert(COND_EQ <= cond && cond <= COND_UGT);
  if (cond <= COND_NE)
    return COND_NE + COND_EQ - cond;
  if (cond <= COND_ULT)
    return COND_LT + ((cond - COND_LT) ^ 2);
  return COND_ULT + ((cond - COND_ULT) ^ 2);
}

// Register allocator

const char *kRegSizeTable[][7] = {
  {W20, W21, W22, W23, W24, W25, W26},
  {W20, W21, W22, W23, W24, W25, W26},
  {W20, W21, W22, W23, W24, W25, W26},
  {X20, X21, X22, X23, X24, X25, X26},
};

#define kReg32s  (kRegSizeTable[2])
#define kReg64s  (kRegSizeTable[3])

const char *kRetRegTable[] = {W0, W0, W0, X0};

const char *kZeroRegTable[] = {WZR, WZR, WZR, ZR};

const char *kTmpRegTable[] = {W9, W9, W9, X9};
const char *kTmpRegTable2[] = {W10, W10, W10, X10};

#ifndef __NO_FLONUM
#define SZ_FLOAT   (4)
#define SZ_DOUBLE  (8)
const char *kFReg32s[7] = {S8, S9, S10, S11, S12, S13, S14};
const char *kFReg64s[7] = {D8, D9, D10, D11, D12, D13, D14};
#endif

#define CALLEE_SAVE_REG_COUNT  ((int)(sizeof(kCalleeSaveRegs) / sizeof(*kCalleeSaveRegs)))
const int kCalleeSaveRegs[] = {
  0, 1, 2, 3, 4, 5, 6,
};

#define CALLER_SAVE_REG_COUNT  ((int)(sizeof(kCallerSaveRegs) / sizeof(*kCallerSaveRegs)))
const int kCallerSaveRegs[] = {
  1,  // R10
  2,  // R11
};

#ifndef __NO_FLONUM
#define CALLER_SAVE_FREG_COUNT  ((int)(sizeof(kCallerSaveFRegs) / sizeof(*kCallerSaveFRegs)))
const int kCallerSaveFRegs[] = {0, 1, 2, 3, 4, 5};
#endif

static const int kPow2Table[] = {-1, 0, 1, -1, 2, -1, -1, -1, 3};
#define kPow2TableSize ((int)(sizeof(kPow2Table) / sizeof(*kPow2Table)))

//

static void mov_immediate(const char *dst, intptr_t value, bool b64) {
  if (is_im16(value)) {
    MOV(dst, IM(value));
  } else if (!b64 || is_im32(value)) {
    int32_t v = value;
    MOV(dst, IM(v & 0xffff));
    MOVK(dst, IM((v >> 16) & 0xffff), _LSL(16));
  } else {
    MOV(dst, IM(value & 0xffff));
    MOVK(dst, IM((value >> 16) & 0xffff), _LSL(16));
    MOVK(dst, IM((value >> 32) & 0xffff), _LSL(32));
    MOVK(dst, IM((value >> 48) & 0xffff), _LSL(48));
  }
}

static const char *im12_or_tmpreg(uintptr_t value, int pow) {
  if (value <= 0x0fff)  // 0~4095
    return IM(value);
  const char *tmp = kTmpRegTable[pow];
  mov_immediate(tmp, value, pow >= 3);
  return tmp;
}

static void ir_memcpy(int dst_reg, int src_reg, ssize_t size) {
  switch (size) {
  case 1:
    LDRB(W9, IMMEDIATE_OFFSET(kReg64s[src_reg], 0));
    STRB(W9, IMMEDIATE_OFFSET(kReg64s[dst_reg], 0));
    break;
  case 2:
    LDRH(W9, IMMEDIATE_OFFSET(kReg64s[src_reg], 0));
    STRH(W9, IMMEDIATE_OFFSET(kReg64s[dst_reg], 0));
    break;
  case 4:
  case 8:
    {
      const char *reg = size == 4 ? W9 : X9;
      LDR(reg, IMMEDIATE_OFFSET(kReg64s[src_reg], 0));
      STR(reg, IMMEDIATE_OFFSET(kReg64s[dst_reg], 0));
    }
    break;
  default:
    // Break %x9~%x12
    {
      const Name *label = alloc_label();
      MOV(X9, kReg64s[src_reg]);
      MOV(X10, kReg64s[dst_reg]);
      mov_immediate(W11, size, false);
      EMIT_LABEL(fmt_name(label));
      LDRB(W12, POST_INDEX(X9, 1));
      STRB(W12, POST_INDEX(X10, 1));
      SUBS(W11, W11, IM(1));
      Bcc(CNE, fmt_name(label));
    }
    break;
  }
}

static void ir_out(IR *ir) {
  switch (ir->kind) {
  case IR_BOFS:
    if (ir->opr1->flag & VRF_CONST)
      mov_immediate(kReg64s[ir->dst->phys], ir->opr1->fixnum, true);
    else
      ADD(kReg64s[ir->dst->phys], FP, IM(ir->opr1->offset));
    break;

  case IR_IOFS:
    {
      char *label = fmt_name(ir->iofs.label);
      if (ir->iofs.global)
        label = MANGLE(label);
      label = quote_label(label);
      const char *dst = kReg64s[ir->dst->phys];
      ADRP(dst, LABEL_AT_PAGE(label));
      ADD(dst, dst, LABEL_AT_PAGEOFF(label));
    }
    break;

  case IR_SOFS:
    assert(ir->opr1->flag & VRF_CONST);
    ADD(kReg64s[ir->dst->phys], SP, IM(ir->opr1->fixnum));
    break;

  case IR_LOAD:
  case IR_LOAD_SPILLED:
    {
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      assert(!(ir->opr1->flag & VRF_CONST));
      const char *src;
      if (ir->kind == IR_LOAD) {
        src = IMMEDIATE_OFFSET(kReg64s[ir->opr1->phys], 0);
      } else {
        if (ir->opr1->offset >= -256 && ir->opr1->offset <= 256) {
          src = IMMEDIATE_OFFSET(FP, ir->opr1->offset);
        } else {
          const char *tmp = kTmpRegTable[3];
          mov_immediate(tmp, ir->opr1->offset, true);
          src = REG_OFFSET(FP, tmp, _LSL(0));
        }
      }

      const char *dst;
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        switch (ir->size) {
        case SZ_FLOAT:   dst = kFReg32s[ir->dst->phys]; break;
        case SZ_DOUBLE:  dst = kFReg64s[ir->dst->phys]; break;
        default: assert(false); break;
        }
      } else
#endif
      {
        const char **regs = kRegSizeTable[pow];
        dst = regs[ir->dst->phys];
      }

      switch (pow) {
      case 0:
        if (ir->dst->vtype->flag & VRTF_UNSIGNED) LDRB(dst, src);
        else                                      LDRSB(dst, src);
        break;
      case 1:
        if (ir->dst->vtype->flag & VRTF_UNSIGNED) LDRH(dst, src);
        else                                      LDRSH(dst, src);
        break;
      case 2: case 3:
        LDR(dst, src);
        break;
      default: assert(false); break;
      }
    }
    break;

  case IR_STORE:
  case IR_STORE_SPILLED:
    {
      assert(!(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      const char *target;
      if (ir->kind == IR_STORE) {
        target = IMMEDIATE_OFFSET(kReg64s[ir->opr2->phys], 0);
      } else {
        if (ir->opr2->offset >= -256 && ir->opr2->offset <= 256) {
          target = IMMEDIATE_OFFSET(FP, ir->opr2->offset);
        } else {
          const char *tmp = kTmpRegTable[3];
          mov_immediate(tmp, ir->opr2->offset, true);
          target = REG_OFFSET(FP, tmp, _LSL(0));
        }
      }
      const char *src;
#ifndef __NO_FLONUM
      if (ir->opr1->vtype->flag & VRTF_FLONUM) {
        switch (ir->size) {
        default: assert(false); // Fallthrough
        case SZ_FLOAT:   src = kFReg32s[ir->opr1->phys]; break;
        case SZ_DOUBLE:  src = kFReg64s[ir->opr1->phys]; break;
        }
      } else
#endif
      if (ir->opr1->flag & VRF_CONST) {
        src = kTmpRegTable[pow];
        mov_immediate(src, ir->opr1->fixnum, pow >= 3);
      } else {
        src = kRegSizeTable[pow][ir->opr1->phys];
      }
      switch (pow) {
      case 0:          STRB(src, target); break;
      case 1:          STRH(src, target); break;
      case 2: case 3:  STR(src, target); break;
      default: assert(false); break;
      }
    }
    break;

  case IR_ADD:
    {
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        const char **regs;
        switch (ir->size) {
        default: assert(false);  // Fallthrough
        case SZ_FLOAT:   regs = kFReg32s; break;
        case SZ_DOUBLE:  regs = kFReg64s; break;
        }
        FADD(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
        break;
      }
#endif
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        if (ir->opr1->fixnum >= 0)
          ADD(regs[ir->dst->phys], regs[ir->opr2->phys], im12_or_tmpreg(ir->opr1->fixnum, pow));
        else
          SUB(regs[ir->dst->phys], regs[ir->opr2->phys], im12_or_tmpreg(-ir->opr1->fixnum, pow));
      } else if (ir->opr2->flag & VRF_CONST) {
        if (ir->opr2->fixnum >= 0)
          ADD(regs[ir->dst->phys], regs[ir->opr1->phys], im12_or_tmpreg(ir->opr2->fixnum, pow));
        else
          SUB(regs[ir->dst->phys], regs[ir->opr1->phys], im12_or_tmpreg(-ir->opr2->fixnum, pow));
      } else {
        ADD(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_SUB:
    {
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        const char **regs;
        switch (ir->size) {
        default: assert(false);  // Fallthrough
        case SZ_FLOAT:   regs = kFReg32s; break;
        case SZ_DOUBLE:  regs = kFReg64s; break;
        }
        FSUB(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
        break;
      }
#endif
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr1->fixnum, pow >= 3);
        SUB(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
      } else if (ir->opr2->flag & VRF_CONST) {
        if (ir->opr2->fixnum >= 0)
          SUB(regs[ir->dst->phys], regs[ir->opr1->phys], im12_or_tmpreg(ir->opr2->fixnum, pow));
        else
          ADD(regs[ir->dst->phys], regs[ir->opr1->phys], im12_or_tmpreg(-ir->opr2->fixnum, pow));
      } else {
        SUB(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_MUL:
    {
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        const char **regs;
        switch (ir->size) {
        default: assert(false);  // Fallthrough
        case SZ_FLOAT:   regs = kFReg32s; break;
        case SZ_DOUBLE:  regs = kFReg64s; break;
        }
        FMUL(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
        break;
      }
#endif
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr1->fixnum, pow >= 3);
        MUL(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
      } else if (ir->opr2->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr2->fixnum, pow >= 3);
        MUL(regs[ir->dst->phys], regs[ir->opr1->phys], tmp);
      } else {
        MUL(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_DIV:
    {
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        const char **regs;
        switch (ir->size) {
        default: assert(false);  // Fallthrough
        case SZ_FLOAT:   regs = kFReg32s; break;
        case SZ_DOUBLE:  regs = kFReg64s; break;
        }
        FDIV(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
        break;
      }
#endif
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr1->fixnum, pow >= 3);
        if (!(ir->dst->vtype->flag & VRTF_UNSIGNED))
          SDIV(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
        else
          UDIV(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
      } else if (ir->opr2->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr2->fixnum, pow >= 3);
        if (!(ir->dst->vtype->flag & VRTF_UNSIGNED))
          SDIV(regs[ir->dst->phys], regs[ir->opr1->phys], tmp);
        else
          UDIV(regs[ir->dst->phys], regs[ir->opr1->phys], tmp);
      } else {
        if (!(ir->dst->vtype->flag & VRTF_UNSIGNED))
          SDIV(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
        else
          UDIV(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_MOD:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      const char *num, *div;
      if (ir->opr1->flag & VRF_CONST) {
        num = kTmpRegTable2[pow];
        mov_immediate(num, ir->opr1->fixnum, pow >= 3);
        div = regs[ir->opr2->phys];
      } else if (ir->opr2->flag & VRF_CONST) {
        div = kTmpRegTable2[pow];
        mov_immediate(div, ir->opr2->fixnum, pow >= 3);
        num = regs[ir->opr1->phys];
      } else {
        num = regs[ir->opr1->phys];
        div = regs[ir->opr2->phys];
      }
      const char *tmp = kTmpRegTable[pow];
      if (!(ir->dst->vtype->flag & VRTF_UNSIGNED))
        SDIV(tmp, num, div);
      else
        UDIV(tmp, num, div);
      const char *dst = regs[ir->dst->phys];
      MSUB(dst, tmp, div, num);
    }
    break;

  case IR_BITAND:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      const char *opr1, *opr2;
      if (ir->opr1->flag & VRF_CONST)
        mov_immediate(opr1 = kTmpRegTable[pow], ir->opr1->fixnum, pow >= 3);
      else
        opr1 = regs[ir->opr1->phys];
      if (ir->opr2->flag & VRF_CONST)
        mov_immediate(opr2 = kTmpRegTable[pow], ir->opr2->fixnum, pow >= 3);
      else
        opr2 = regs[ir->opr2->phys];
      AND(regs[ir->dst->phys], opr1, opr2);
    }
    break;

  case IR_BITOR:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      const char *opr1, *opr2;
      if (ir->opr1->flag & VRF_CONST)
        mov_immediate(opr1 = kTmpRegTable[pow], ir->opr1->fixnum, pow >= 3);
      else
        opr1 = regs[ir->opr1->phys];
      if (ir->opr2->flag & VRF_CONST)
        mov_immediate(opr2 = kTmpRegTable[pow], ir->opr2->fixnum, pow >= 3);
      else
        opr2 = regs[ir->opr2->phys];
      ORR(regs[ir->dst->phys], opr1, opr2);
    }
    break;

  case IR_BITXOR:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      const char *opr1, *opr2;
      if (ir->opr1->flag & VRF_CONST)
        mov_immediate(opr1 = kTmpRegTable[pow], ir->opr1->fixnum, pow >= 3);
      else
        opr1 = regs[ir->opr1->phys];
      if (ir->opr2->flag & VRF_CONST)
        mov_immediate(opr2 = kTmpRegTable[pow], ir->opr2->fixnum, pow >= 3);
      else
        opr2 = regs[ir->opr2->phys];
      EOR(regs[ir->dst->phys], opr1, opr2);
    }
    break;

  case IR_LSHIFT:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr1->fixnum, pow >= 3);
        LSL(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
      } else if (ir->opr2->flag & VRF_CONST) {
        LSL(regs[ir->dst->phys], regs[ir->opr1->phys], IM(ir->opr2->fixnum));
      } else {
        LSL(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;
  case IR_RSHIFT:
    {
      assert(!(ir->opr1->flag & VRF_CONST) || !(ir->opr2->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        const char *tmp = kTmpRegTable[pow];
        mov_immediate(tmp, ir->opr1->fixnum, pow >= 3);
        ASR(regs[ir->dst->phys], tmp, regs[ir->opr2->phys]);
      } else if (ir->opr2->flag & VRF_CONST) {
        ASR(regs[ir->dst->phys], regs[ir->opr1->phys], IM(ir->opr2->fixnum));
      } else {
        ASR(regs[ir->dst->phys], regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_RESULT:
#ifndef __NO_FLONUM
    if (ir->opr1->vtype->flag & VRTF_FLONUM) {
      const char *src, *dst;
      switch (ir->size) {
      default: assert(false);  // Fallthroguh
      case SZ_FLOAT:  dst = S0; src = kFReg32s[ir->opr1->phys]; break;
      case SZ_DOUBLE: dst = D0; src = kFReg64s[ir->opr1->phys]; break;
      }
      FMOV(dst, src);
      break;
    }
#endif
    {
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      if (ir->opr1->flag & VRF_CONST) {
        mov_immediate(kRetRegTable[pow], ir->opr1->fixnum, pow >= 3);
      } else {
        MOV(kRetRegTable[pow], kRegSizeTable[pow][ir->opr1->phys]);
      }
    }
    break;

  case IR_SUBSP:
    if (ir->opr1->flag & VRF_CONST) {
      assert(ir->opr1->fixnum % 16 == 0);
      if (ir->opr1->fixnum > 0)
        SUB(SP, SP, IM(ir->opr1->fixnum));
      else if (ir->opr1->fixnum < 0)
        ADD(SP, SP, IM(-ir->opr1->fixnum));
    } else {
      SUB(SP, SP, kReg64s[ir->opr1->phys]);
    }
    if (ir->dst != NULL)
      MOV(kReg64s[ir->dst->phys], SP);
    break;

  case IR_MOV:
    {
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        if (ir->opr1->phys != ir->dst->phys) {
          const char *src, *dst;
          switch (ir->size) {
          default: assert(false); // Fallthrough
          case SZ_FLOAT:   dst = kFReg32s[ir->dst->phys]; src = kFReg32s[ir->opr1->phys]; break;
          case SZ_DOUBLE:  dst = kFReg64s[ir->dst->phys]; src = kFReg64s[ir->opr1->phys]; break;
          }
          FMOV(dst, src);
          break;
        }
      }
#endif
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      assert(!(ir->dst->flag & VRF_CONST));
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr1->flag & VRF_CONST) {
        mov_immediate(regs[ir->dst->phys], ir->opr1->fixnum, pow >= 3);
      } else {
        if (ir->opr1->phys != ir->dst->phys)
          MOV(regs[ir->dst->phys], regs[ir->opr1->phys]);
      }
    }
    break;

  case IR_CMP:
    {
#ifndef __NO_FLONUM
      if (ir->opr1->vtype->flag & VRTF_FLONUM) {
        assert(ir->opr2->vtype->flag & VRTF_FLONUM);
        const char *opr1, *opr2;
        switch (ir->size) {
        default: assert(false); // Fallthrough
        case SZ_FLOAT:   opr1 = kFReg32s[ir->opr1->phys]; opr2 = kFReg32s[ir->opr2->phys]; break;
        case SZ_DOUBLE:  opr1 = kFReg64s[ir->opr1->phys]; opr2 = kFReg64s[ir->opr2->phys]; break;
        }
        FCMP(opr1, opr2);
        break;
      }
#endif
      assert(!(ir->opr1->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      if (ir->opr2->flag & VRF_CONST) {
        if (ir->opr2->fixnum >= 0)
          CMP(regs[ir->opr1->phys], im12_or_tmpreg(ir->opr2->fixnum, pow));
        else
          CMN(regs[ir->opr1->phys], im12_or_tmpreg(-ir->opr2->fixnum, pow));
      } else {
        CMP(regs[ir->opr1->phys], regs[ir->opr2->phys]);
      }
    }
    break;

  case IR_NEG:
    {
      assert(!(ir->dst->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      NEG(regs[ir->dst->phys], regs[ir->opr1->phys]);
    }
    break;

  case IR_BITNOT:
    {
      assert(!(ir->dst->flag & VRF_CONST));
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int pow = kPow2Table[ir->size];
      assert(0 <= pow && pow < 4);
      const char **regs = kRegSizeTable[pow];
      mov_immediate(regs[ir->dst->phys], -1, pow >= 3);
      EOR(regs[ir->dst->phys], regs[ir->dst->phys], regs[ir->opr1->phys]);
    }
    break;

  case IR_COND:
    {
      assert(!(ir->dst->flag & VRF_CONST));
      const char *dst = kReg32s[ir->dst->phys];  // Assume bool is 4 byte.
      switch (ir->cond.kind) {
      case COND_EQ:  CSET(dst, CEQ); break;
      case COND_NE:  CSET(dst, CNE); break;
      case COND_LT:  CSET(dst, CLT); break;
      case COND_GT:  CSET(dst, CGT); break;
      case COND_LE:  CSET(dst, CLE); break;
      case COND_GE:  CSET(dst, CGE); break;
      case COND_ULT: CSET(dst, CLO); break;
      case COND_UGT: CSET(dst, CHI); break;
      case COND_ULE: CSET(dst, CLS); break;
      case COND_UGE: CSET(dst, CHS); break;
      default: assert(false); break;
      }
    }
    break;

  case IR_JMP:
    switch (ir->jmp.cond) {
    case COND_ANY: BRANCH(fmt_name(ir->jmp.bb->label)); break;
    case COND_EQ:  Bcc(CEQ, fmt_name(ir->jmp.bb->label)); break;
    case COND_NE:  Bcc(CNE, fmt_name(ir->jmp.bb->label)); break;
    case COND_LT:  Bcc(CLT, fmt_name(ir->jmp.bb->label)); break;
    case COND_GT:  Bcc(CGT, fmt_name(ir->jmp.bb->label)); break;
    case COND_LE:  Bcc(CLE, fmt_name(ir->jmp.bb->label)); break;
    case COND_GE:  Bcc(CGE, fmt_name(ir->jmp.bb->label)); break;
    case COND_ULT: Bcc(CLO, fmt_name(ir->jmp.bb->label)); break;
    case COND_UGT: Bcc(CHI, fmt_name(ir->jmp.bb->label)); break;
    case COND_ULE: Bcc(CLS, fmt_name(ir->jmp.bb->label)); break;
    case COND_UGE: Bcc(CHS, fmt_name(ir->jmp.bb->label)); break;
    default: assert(false); break;
    }
    break;

  case IR_TJMP:
    {
      int phys = ir->opr1->phys;
      const int powd = 3;
      assert(0 <= ir->opr1->vtype->size && ir->opr1->vtype->size < kPow2TableSize);
      int pows = kPow2Table[ir->opr1->vtype->size];
      assert(0 <= pows && pows < 4);

      const char *dst = kTmpRegTable[3];
      const Name *table_label = alloc_label();
      char *label = fmt_name(table_label);
      ADRP(dst, LABEL_AT_PAGE(label));
      ADD(dst, dst, LABEL_AT_PAGEOFF(label));
      LDR(dst, REG_OFFSET(dst, kRegSizeTable[pows][phys], pows < powd ? _UXTW(3) : _LSL(3)));  // dst = label + (opr1 << 3)
      BR(dst);

      _RODATA();
      EMIT_ALIGN(8);
      EMIT_LABEL(fmt_name(table_label));
      for (size_t i = 0, len = ir->tjmp.len; i < len; ++i) {
        BB *bb = ir->tjmp.bbs[i];
        _QUAD(fmt("%.*s", bb->label->bytes, bb->label->chars));
      }
      _TEXT();
    }
    break;

  case IR_PRECALL:
    {
      // Make room for caller save.
      int add = 0;
      unsigned short living_pregs = ir->precall.living_pregs;
      for (int i = 0; i < CALLER_SAVE_REG_COUNT; ++i) {
        int ireg = kCallerSaveRegs[i];
        if (living_pregs & (1 << ireg))
          add += WORD_SIZE;
      }
#ifndef __NO_FLONUM
      for (int i = 0; i < CALLER_SAVE_FREG_COUNT; ++i) {
        int freg = kCallerSaveFRegs[i];
        if (living_pregs & (1 << (freg + PHYSICAL_REG_MAX)))
          add += WORD_SIZE;
      }
#endif

      int align_stack = (16 - (add + ir->precall.stack_args_size)) & 15;
      ir->precall.stack_aligned = align_stack;
      add += align_stack;

      if (add > 0) {
        SUB(SP, SP, IM(add));
      }
    }
    break;

  case IR_PUSHARG:
    {
#ifndef __NO_FLONUM
    if (ir->opr1->vtype->flag & VRTF_FLONUM) {
      switch (ir->opr1->vtype->size) {
      case SZ_FLOAT:  STR(kFReg32s[ir->opr1->phys], PRE_INDEX(SP, -16)); break;
      case SZ_DOUBLE: STR(kFReg64s[ir->opr1->phys], PRE_INDEX(SP, -16)); break;
      default: assert(false); break;
      }
      break;
    }
#endif
      const char *src;
      if (ir->opr1->flag & VRF_CONST) {
        src = kTmpRegTable[3];
        mov_immediate(src, ir->opr1->fixnum, true);
      } else {
        src = kRegSizeTable[3][ir->opr1->phys];
      }
      STR(src, PRE_INDEX(SP, -16));
    }
    break;

  case IR_CALL:
    {
      const int FIELD_SIZE = 16;
      IR *precall = ir->call.precall;
      int reg_args = ir->call.reg_arg_count;
      push_caller_save_regs(
          precall->precall.living_pregs,
          reg_args * FIELD_SIZE + precall->precall.stack_args_size + precall->precall.stack_aligned);

      static const char *kArgReg64s[] = {X0, X1, X2, X3, X4, X5, X6, X7};
#ifndef __NO_FLONUM
      static const char *kArgFReg32s[] = {S0, S1, S2, S3, S4, S5, S6, S7};
      static const char *kArgFReg64s[] = {D0, D1, D2, D3, D4, D5, D6, D7};
      int freg = 0;
#endif

      int ireg = 0;
      int total_arg_count = ir->call.total_arg_count;
      for (int i = 0; i < total_arg_count; ++i) {
#if defined(VAARG_ON_STACK)
        if (ir->call.vaarg_start >= 0 && i >= ir->call.vaarg_start)
          break;
#endif
        if (ir->call.arg_vtypes[i]->flag & VRTF_NON_REG)
          continue;
#ifndef __NO_FLONUM
        if (ir->call.arg_vtypes[i]->flag & VRTF_FLONUM) {
          if (freg < MAX_FREG_ARGS) {
            switch (ir->call.arg_vtypes[i]->size) {
            case SZ_FLOAT:  LDR(kArgFReg32s[freg], POST_INDEX(SP, 16)); break;
            case SZ_DOUBLE: LDR(kArgFReg64s[freg], POST_INDEX(SP, 16)); break;
            default: assert(false); break;
            }
            ++freg;
          }
          continue;
        }
#endif
        if (ireg < MAX_REG_ARGS) {
          LDR(kArgReg64s[ireg++], POST_INDEX(SP, 16));
        }
      }

      if (ir->call.label != NULL) {
        char *label = fmt_name(ir->call.label);
        if (ir->call.global)
          label = MANGLE(label);
        BL(quote_label(label));
      } else {
        assert(!(ir->opr1->flag & VRF_CONST));
        BLR(kReg64s[ir->opr1->phys]);
      }

      // Resore caller save registers.
      pop_caller_save_regs(precall->precall.living_pregs, precall->precall.stack_aligned);

{
int add = 0;
unsigned short living_pregs = precall->precall.living_pregs;
for (int i = 0; i < CALLER_SAVE_REG_COUNT; ++i) {
  int ireg = kCallerSaveRegs[i];
  if (living_pregs & (1 << ireg))
    add += WORD_SIZE;
}
#ifndef __NO_FLONUM
for (int i = 0; i < CALLER_SAVE_FREG_COUNT; ++i) {
  int freg = kCallerSaveFRegs[i];
  if (living_pregs & (1 << (freg + PHYSICAL_REG_MAX)))
    add += WORD_SIZE;
}
#endif

      int align_stack = precall->precall.stack_aligned + precall->precall.stack_args_size;
      if (add + align_stack != 0) {
        ADD(SP, SP, IM(add + align_stack));
      }
}

      assert(0 <= ir->size && ir->size < kPow2TableSize);
#ifndef __NO_FLONUM
      if (ir->dst->vtype->flag & VRTF_FLONUM) {
        const char *src, *dst;
        switch (ir->size) {
        default: assert(false);  // Fallthrough
        case SZ_FLOAT:   src = S0; dst = kFReg32s[ir->dst->phys]; break;
        case SZ_DOUBLE:  src = D0; dst = kFReg64s[ir->dst->phys]; break;
        }
        FMOV(dst, src);
      } else
#endif
      if (ir->size > 0) {
        int pow = kPow2Table[ir->size];
        assert(0 <= pow && pow < 4);
        const char **regs = kRegSizeTable[pow];
        MOV(regs[ir->dst->phys], kRetRegTable[pow]);
      }
    }
    break;

  case IR_CAST:
#ifndef __NO_FLONUM
    if (ir->dst->vtype->flag & VRTF_FLONUM) {
      if (ir->opr1->vtype->flag & VRTF_FLONUM) {
        // flonum->flonum
        // Assume flonum are just two types.
        const char *src, *dst;
        switch (ir->size) {
        default: assert(false); // Fallthrough
        case SZ_FLOAT:   dst = kFReg32s[ir->dst->phys]; src = kFReg64s[ir->opr1->phys]; break;
        case SZ_DOUBLE:  dst = kFReg64s[ir->dst->phys]; src = kFReg32s[ir->opr1->phys]; break;
        }
        FCVT(dst, src);
      } else {
        // fix->flonum
        assert(0 <= ir->opr1->vtype->size && ir->opr1->vtype->size < kPow2TableSize);
        int pows = kPow2Table[ir->opr1->vtype->size];

        const char *dst;
        switch (ir->size) {
        case SZ_FLOAT:   dst = kFReg32s[ir->dst->phys]; break;
        case SZ_DOUBLE:  dst = kFReg64s[ir->dst->phys]; break;
        default: assert(false); break;
        }
        const char *src = kRegSizeTable[pows][ir->opr1->phys];
        if (ir->opr1->vtype->flag & VRTF_UNSIGNED)  UCVTF(dst, src);
        else                                        SCVTF(dst, src);
      }
      break;
    } else if (ir->opr1->vtype->flag & VRTF_FLONUM) {
      // flonum->fix
      int powd = kPow2Table[ir->dst->vtype->size];
      switch (ir->opr1->vtype->size) {
      case SZ_FLOAT:   FCVTZS(kRegSizeTable[powd][ir->dst->phys], kFReg32s[ir->opr1->phys]); break;
      case SZ_DOUBLE:  FCVTZS(kRegSizeTable[powd][ir->dst->phys], kFReg64s[ir->opr1->phys]); break;
      default: assert(false); break;
      }
      break;
    }
#endif
    assert((ir->opr1->flag & VRF_CONST) == 0);
    if (ir->size <= ir->opr1->vtype->size) {
      if (ir->dst->phys != ir->opr1->phys) {
        assert(0 <= ir->size && ir->size < kPow2TableSize);
        int pow = kPow2Table[ir->size];
        assert(0 <= pow && pow < 3);
        const char **regs = kRegSizeTable[pow];
        MOV(regs[ir->dst->phys], regs[ir->opr1->phys]);
      }
    } else {
      assert(0 <= ir->opr1->vtype->size && ir->opr1->vtype->size < kPow2TableSize);
      int pows = kPow2Table[ir->opr1->vtype->size];
      assert(0 <= ir->size && ir->size < kPow2TableSize);
      int powd = kPow2Table[ir->size];
      assert(0 <= pows && pows < 4);
      assert(0 <= powd && powd < 4);
      if (ir->opr1->vtype->flag & VRTF_UNSIGNED) {
        switch (pows) {
        case 0:  UXTB(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        case 1:  UXTH(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        case 2:  UXTW(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        default: assert(false); break;
        }
      } else {
        switch (pows) {
        case 0:  SXTB(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        case 1:  SXTH(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        case 2:  SXTW(kRegSizeTable[powd][ir->dst->phys], kRegSizeTable[pows][ir->opr1->phys]); break;
        default: assert(false); break;
        }
      }
    }
    break;

  case IR_MEMCPY:
    assert(!(ir->opr1->flag & VRF_CONST));
    assert(!(ir->opr2->flag & VRF_CONST));
    ir_memcpy(ir->opr2->phys, ir->opr1->phys, ir->size);
    break;

  case IR_CLEAR:
    {
      assert(!(ir->opr1->flag & VRF_CONST));
      const Name *label = alloc_label();

      MOV(X9, kReg64s[ir->opr1->phys]);
      mov_immediate(W10, ir->size, false);
      EMIT_LABEL(fmt_name(label));
      STRB(WZR, POST_INDEX(X9, 1));
      SUBS(W10, W10, IM(1));
      Bcc(CNE, fmt_name(label));
    }
    break;

  default: assert(false); break;
  }
}

//

static IR *is_last_jmp(BB *bb) {
  int len;
  IR *ir;
  if ((len = bb->irs->len) > 0 && (ir = bb->irs->data[len - 1])->kind == IR_JMP)
    return ir;
  return NULL;
}

static IR *is_last_any_jmp(BB *bb) {
  IR *ir = is_last_jmp(bb);
  return ir != NULL && ir->jmp.cond == COND_ANY ? ir : NULL;
}

static IR *is_last_jtable(BB *bb) {
  int len;
  IR *ir;
  if ((len = bb->irs->len) > 0 && (ir = bb->irs->data[len - 1])->kind == IR_TJMP)
    return ir;
  return NULL;
}

static void replace_jmp_destination(BBContainer *bbcon, BB *src, BB *dst) {
  Vector *bbs = bbcon->bbs;
  for (int i = 0; i < bbs->len; ++i) {
    BB *bb = bbs->data[i];
    if (bb == src)
      continue;

    IR *ir = is_last_jmp(bb);
    if (ir != NULL && ir->jmp.bb == src)
      ir->jmp.bb = dst;

    IR *tjmp = is_last_jtable(bb);
    if (tjmp != NULL) {
      BB **bbs = tjmp->tjmp.bbs;
      for (size_t j = 0, len = tjmp->tjmp.len; j < len; ++j) {
        if (bbs[j] == src)
          bbs[j] = dst;
      }
    }
  }
}

void remove_unnecessary_bb(BBContainer *bbcon) {
  Vector *bbs = bbcon->bbs;
  Table keeptbl;
  for (;;) {
    table_init(&keeptbl);
    assert(bbs->len > 0);
    BB *bb0 = bbs->data[0];
    table_put(&keeptbl, bb0->label, bb0);

    for (int i = 0; i < bbs->len - 1; ++i) {
      BB *bb = bbs->data[i];
      bool remove = false;
      IR *ir_jmp = is_last_jmp(bb);
      if (bb->irs->len == 0) {  // Empty BB.
        replace_jmp_destination(bbcon, bb, bb->next);
        remove = true;
      } else if (bb->irs->len == 1 && ir_jmp != NULL && ir_jmp->jmp.cond == COND_ANY &&
                 !equal_name(bb->label, ir_jmp->jmp.bb->label)) {  // jmp only.
        replace_jmp_destination(bbcon, bb, ir_jmp->jmp.bb);
        if (i > 0) {
          BB *pbb = bbs->data[i - 1];
          IR *ir0 = is_last_jmp(pbb);
          if (ir0 != NULL && ir0->jmp.cond != COND_ANY) {  // Fallthrough pass exists.
            if (ir0->jmp.bb == bb->next) {                 // Skip jmp: Fix bb connection.
              // Invert prev jmp condition and change jmp destination.
              ir0->jmp.cond = invert_cond(ir0->jmp.cond);
              ir0->jmp.bb = ir_jmp->jmp.bb;
              remove = true;
            }
          }
        }
      }

      if (ir_jmp != NULL)
        table_put(&keeptbl, ir_jmp->jmp.bb->label, bb);
      if (ir_jmp == NULL || ir_jmp->jmp.cond != COND_ANY)
        table_put(&keeptbl, bb->next->label, bb);

      IR *tjmp = is_last_jtable(bb);
      if (tjmp != NULL) {
        BB **bbs = tjmp->tjmp.bbs;
        for (size_t j = 0, len = tjmp->tjmp.len; j < len; ++j)
          table_put(&keeptbl, bbs[j]->label, bb);
      }

      if (remove)
        table_delete(&keeptbl, bb->label);
    }

    bool again = false;
    for (int i = 0; i < bbs->len - 1; ++i) {  // Make last one keeps alive.
      BB *bb = bbs->data[i];
      if (!table_try_get(&keeptbl, bb->label, NULL)) {
        if (i > 0) {
          BB *pbb = bbs->data[i - 1];
          pbb->next = bb->next;
        }

        vec_remove_at(bbs, i);
        --i;
        again = true;
      }
    }
    if (!again)
      break;
  }

  // Remove jmp to next instruction.
  for (int i = 0; i < bbs->len - 1; ++i) {  // Make last one keeps alive.
    BB *bb = bbs->data[i];
    IR *ir = is_last_any_jmp(bb);
    if (ir != NULL && ir->jmp.bb == bb->next)
      vec_pop(bb->irs);
  }
}

static int enum_save_regs(unsigned short used, const char *saves[CALLEE_SAVE_REG_COUNT]) {
  int count = 0;
  for (int i = 0; i < CALLEE_SAVE_REG_COUNT; ++i) {
    int ireg = kCalleeSaveRegs[i];
    if (used & (1 << ireg))
      saves[count++] = kReg64s[ireg];
  }
  return count;
}

int push_callee_save_regs(unsigned short used) {
  const char *saves[(CALLEE_SAVE_REG_COUNT + 1) & ~1];
  int count = enum_save_regs(used, saves);
  for (int i = 0; i < count; i += 2) {
    if (i + 1 < count)
      STP(saves[i], saves[i + 1], PRE_INDEX(SP, -16));
    else
      STR(saves[i], PRE_INDEX(SP, -16));
  }
  return count;
}

void pop_callee_save_regs(unsigned short used) {
  const char *saves[(CALLEE_SAVE_REG_COUNT + 1) & ~1];
  int count = enum_save_regs(used, saves);
  if ((count & 1) != 0)
    LDR(saves[--count], POST_INDEX(SP, 16));
  for (int i = count; i > 0; ) {
    i -= 2;
    LDP(saves[i], saves[i + 1], POST_INDEX(SP, 16));
  }
}

static void push_caller_save_regs(unsigned short living, int base) {
  const int FIELD_SIZE = 16;

#ifndef __NO_FLONUM
  {
    for (int i = CALLER_SAVE_FREG_COUNT; i > 0;) {
      int ireg = kCallerSaveFRegs[--i];
      if (living & (1U << (ireg + PHYSICAL_REG_MAX))) {
        // TODO: Detect register size.
        STR(kFReg64s[ireg], IMMEDIATE_OFFSET(SP, base));
        base += FIELD_SIZE;
      }
    }
  }
#endif

  for (int i = CALLER_SAVE_REG_COUNT; i > 0;) {
    int ireg = kCallerSaveRegs[--i];
    if (living & (1 << ireg)) {
      STR(kReg64s[ireg], IMMEDIATE_OFFSET(SP, base));
      base += FIELD_SIZE;
    }
  }
}

static void pop_caller_save_regs(unsigned short living, int base) {
  const int FIELD_SIZE = 16;

#ifndef __NO_FLONUM
  {
    for (int i = CALLER_SAVE_FREG_COUNT; i > 0;) {
      int ireg = kCallerSaveFRegs[--i];
      if (living & (1U << (ireg + PHYSICAL_REG_MAX))) {
        // TODO: Detect register size.
        LDR(kFReg64s[ireg], IMMEDIATE_OFFSET(SP, base));
        base += FIELD_SIZE;
      }
    }
  }
#endif

  for (int i = CALLER_SAVE_REG_COUNT; --i >= 0;) {
    int ireg = kCallerSaveRegs[i];
    if (living & (1 << ireg)) {
      LDR(kReg64s[ireg], IMMEDIATE_OFFSET(SP, base));
      base += FIELD_SIZE;
    }
  }
}

void emit_bb_irs(BBContainer *bbcon) {
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
#ifndef NDEBUG
    // Check BB connection.
    if (i < bbcon->bbs->len - 1) {
      BB *nbb = bbcon->bbs->data[i + 1];
      UNUSED(nbb);
      assert(bb->next == nbb);
    } else {
      assert(bb->next == NULL);
    }
#endif

    EMIT_LABEL(fmt_name(bb->label));
    for (int j = 0; j < bb->irs->len; ++j) {
      IR *ir = bb->irs->data[j];
      ir_out(ir);
    }
  }
}

void convert_3to2(BBContainer *bbcon) {
  UNUSED(bbcon);
}