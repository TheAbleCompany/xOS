#include "ixg_sass_encode.h"
#include <string.h>

ix_u32 ixg_sass_fmt_from_arch(ix_u32 arch) {
  if (arch >= IXG_SASS_ARCH_VOLTA) return IXG_SASS_FMT_VOLTA;
  if (arch >= IXG_SASS_ARCH_MAXWELL) return IXG_SASS_FMT_MAXWELL;
  if (arch >= IXG_SASS_ARCH_KEPLER) return IXG_SASS_FMT_KEPLER;
  return IXG_SASS_FMT_NONE;
}

ix_u64 ixg_sass_enc_volta_ctrl(ix_u32 stall, ix_u32 yield, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask, ix_u32 rc) {
  ix_u32 ctrl;
  ix_u64 hi;
  ctrl  = (stall & 0xFu);
  ctrl |= (yield & 0x1u) << 4;
  ctrl |= (wr_bar & 0x7u) << 5;
  ctrl |= (rd_bar & 0x7u) << 8;
  ctrl |= (wait_mask & 0x3Fu) << 11;
  hi = ((ix_u64)ctrl) << 41;
  hi |= (ix_u64)(rc & 0xFFu);
  return hi;
}

ix_u64 ixg_sass_enc_volta_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb) {
  ix_u64 lo;
  lo  = (ix_u64)(op & 0xFFFu);
  lo |= (ix_u64)0x7u << 12;
  lo |= (ix_u64)(dst & 0xFFu) << 16;
  lo |= (ix_u64)(ra & 0xFFu) << 24;
  lo |= (ix_u64)(rb & 0xFFu) << 32;
  return lo;
}

ix_u64 ixg_sass_enc_volta_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb) {
  return ixg_sass_enc_volta_falu(IXG_V_OP_FFMA, dst, ra, rb);
}

ix_u64 ixg_sass_enc_volta_ldc(ix_u32 dst, ix_u32 cbank_off) {
  ix_u64 lo;
  lo  = (ix_u64)IXG_V_OP_LDC;
  lo |= (ix_u64)0x7u << 12;
  lo |= (ix_u64)(dst & 0xFFu) << 16;
  lo |= (ix_u64)IXG_V_RZ << 24;
  lo |= (ix_u64)((cbank_off >> 2) & 0xFFFFu) << 40;
  return lo;
}

ix_u64 ixg_sass_enc_volta_s2r(ix_u32 dst, ix_u32 sr) {
  ix_u64 lo;
  (void)sr;
  lo  = (ix_u64)IXG_V_OP_S2R;
  lo |= (ix_u64)0x7u << 12;
  lo |= (ix_u64)(dst & 0xFFu) << 16;
  return lo;
}

ix_u64 ixg_sass_enc_volta_s2r_hi(ix_u32 stall, ix_u32 sr) {
  ix_u64 hi = ixg_sass_enc_volta_ctrl(stall, 0u, 7u, 7u, 0x17u, 0u);
  hi |= (ix_u64)(sr & 0xFFu) << 8;
  return hi;
}

ix_u64 ixg_sass_enc_volta_simple(ix_u32 op) {
  ix_u64 lo;
  lo  = (ix_u64)(op & 0xFFFu);
  lo |= (ix_u64)0x7u << 12;
  return lo;
}

ix_u64 ixg_sass_enc_kepler_ctrl(ix_u32 stall) {
  ix_u64 ctrl = 0;
  ctrl |= (ix_u64)(stall & 0xFu) << 48;
  ctrl |= 0x7ull << 52;
  ctrl |= 0x7ull << 55;
  return ctrl;
}

ix_u64 ixg_sass_enc_kepler_ctrl_full(ix_u32 stall, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask) {
  ix_u64 ctrl = 0;
  ctrl |= (ix_u64)(stall & 0xFu) << 48;
  ctrl |= (ix_u64)(wr_bar & 0x7u) << 52;
  ctrl |= (ix_u64)(rd_bar & 0x7u) << 55;
  ctrl |= (ix_u64)(wait_mask & 0x3Fu) << 58;
  return ctrl;
}

ix_u64 ixg_sass_enc_kepler_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb) {
  ix_u64 inst;
  inst  = (ix_u64)(op & 0xFFFu);
  inst |= (ix_u64)(dst & 0xFFu) << 12;
  inst |= (ix_u64)(ra & 0xFFu) << 20;
  inst |= (ix_u64)(rb & 0xFFu) << 28;
  inst |= (ix_u64)0x1u << 36;
  return inst;
}

ix_u64 ixg_sass_enc_kepler_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb, ix_u32 rc) {
  ix_u64 inst = ixg_sass_enc_kepler_falu(IXG_K_OP_FFMA, dst, ra, rb);
  inst |= (ix_u64)(rc & 0xFFu) << 42;
  return inst;
}

ix_u64 ixg_sass_enc_kepler_simple(ix_u32 op) {
  ix_u64 inst;
  inst  = (ix_u64)(op & 0xFFFu);
  inst |= (ix_u64)0x1u << 36;
  return inst;
}

ix_u64 ixg_sass_enc_maxwell_ctrl(ix_u32 stall, ix_u32 yield) {
  ix_u64 ctrl = 0;
  ctrl |= (ix_u64)(stall & 0xFu) << 48;
  ctrl |= (ix_u64)(yield & 0x1u) << 52;
  ctrl |= 0x7ull << 53;
  ctrl |= 0x7ull << 56;
  return ctrl;
}

ix_u64 ixg_sass_enc_maxwell_ctrl_full(ix_u32 stall, ix_u32 yield, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask) {
  ix_u64 ctrl = 0;
  ctrl |= (ix_u64)(stall & 0xFu) << 48;
  ctrl |= (ix_u64)(yield & 0x1u) << 52;
  ctrl |= (ix_u64)(wr_bar & 0x7u) << 53;
  ctrl |= (ix_u64)(rd_bar & 0x7u) << 56;
  ctrl |= (ix_u64)(wait_mask & 0x3Fu) << 59;
  return ctrl;
}

ix_u64 ixg_sass_enc_maxwell_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb) {
  ix_u64 lo;
  lo  = (ix_u64)(op & 0xFFFu);
  lo |= (ix_u64)0x3u << 12;
  lo |= (ix_u64)(dst & 0xFFu) << 14;
  lo |= (ix_u64)(ra & 0xFFu) << 22;
  lo |= (ix_u64)(rb & 0xFFu) << 30;
  lo |= (ix_u64)0x1u << 38;
  return lo;
}

ix_u64 ixg_sass_enc_maxwell_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb, ix_u32 rc) {
  ix_u64 lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_FFMA, dst, ra, rb);
  lo |= (ix_u64)(rc & 0xFFu) << 39;
  return lo;
}

ix_u64 ixg_sass_enc_maxwell_simple(ix_u32 op) {
  ix_u64 lo;
  lo  = (ix_u64)(op & 0xFFFu);
  lo |= (ix_u64)0x3u << 12;
  lo |= (ix_u64)0x1u << 38;
  return lo;
}

static void ixg_enc_reset(ixg_sass_binary_t* b, ix_u32 sm) {
  memset(b, 0, sizeof(*b));
  b->arch = ixg_sass_arch_from_sm(sm);
  b->fmt = ixg_sass_fmt_from_arch(b->arch);
  b->sm_version = sm;
}

static void ixg_enc_track(ixg_sass_binary_t* b, ix_u32 r) {
  if (r != IXG_V_RZ && (r + 1u) > b->reg_high_water) b->reg_high_water = r + 1u;
}

static void ixg_enc_word(ixg_sass_binary_t* b, ix_u64 lo, ix_u64 hi) {
  ix_u32 o = b->byte_len;
  ix_u32 i;
  if (o + IXG_SASS_ENC_WORD_BYTES > IXG_SASS_ENC_MAX_BYTES) { b->truncated = 1u; return; }
  for (i = 0; i < 8u; i++) b->bytes[o + i] = (ix_u8)((lo >> (i * 8u)) & 0xFFu);
  for (i = 0; i < 8u; i++) b->bytes[o + 8u + i] = (ix_u8)((hi >> (i * 8u)) & 0xFFu);
  b->byte_len += IXG_SASS_ENC_WORD_BYTES;
  b->instr_count++;
}

static ix_u32 ixg_enc_phys(ix_u8 reg, ix_u8 comp) {
  if (reg < IXG_SHADER_REG_INPUT_BASE)
    return ((ix_u32)reg * IXG_SASS_SCALAR_PER_VEC) + comp;
  if (reg >= IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    return 128u + ((ix_u32)idx * IXG_SASS_SCALAR_PER_VEC) + comp;
  }
  return ((ix_u32)reg * IXG_SASS_SCALAR_PER_VEC) + comp;
}

static int ixg_enc_is_reg(ix_u8 reg) {
  return (reg < IXG_SHADER_REG_INPUT_BASE) || (reg >= IXG_SHADER_REG_OUTPUT_BASE);
}

static ix_u32 ixg_enc_src_reg(ixg_sass_binary_t* b, ix_u8 reg, ix_u8 comp) {
  ix_u32 p;
  if (!ixg_enc_is_reg(reg)) return IXG_V_RZ;
  p = ixg_enc_phys(reg, comp);
  if (p > 254u) p = IXG_SASS_MAX_TEMP_REGS;
  ixg_enc_track(b, p);
  return p;
}

static void ixg_enc_falu(ixg_sass_binary_t* b, ix_u32 op, ix_u32 dst, ix_u8 ra, ix_u8 rb, ix_u8 comp, ix_u32 stall) {
  ix_u32 pa = ixg_enc_src_reg(b, ra, comp);
  ix_u32 pb = ixg_enc_src_reg(b, rb, comp);
  ix_u64 lo, hi;
  if (b->fmt == IXG_SASS_FMT_KEPLER) {
    lo = ixg_sass_enc_kepler_falu(op, dst, pa, pb);
    hi = ixg_sass_enc_kepler_ctrl(stall);
  } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
    lo = ixg_sass_enc_maxwell_falu(op, dst, pa, pb);
    hi = ixg_sass_enc_maxwell_ctrl(stall, 0u);
  } else {
    lo = ixg_sass_enc_volta_falu(op, dst, pa, pb);
    hi = ixg_sass_enc_volta_ctrl(stall, 0u, 7u, 7u, 0u, 0u);
  }
  ixg_enc_track(b, dst);
  ixg_enc_word(b, lo, hi);
}

static void ixg_enc_ffma(ixg_sass_binary_t* b, ix_u32 dst, ix_u8 ra, ix_u8 rb, ix_u8 rc, ix_u8 comp, ix_u32 stall) {
  ix_u32 pa = ixg_enc_src_reg(b, ra, comp);
  ix_u32 pb = ixg_enc_src_reg(b, rb, comp);
  ix_u32 pc = ixg_enc_src_reg(b, rc, comp);
  ix_u64 lo, hi;
  if (b->fmt == IXG_SASS_FMT_KEPLER) {
    lo = ixg_sass_enc_kepler_falu(IXG_K_OP_FFMA, dst, pa, pb);
    hi = ixg_sass_enc_kepler_ctrl(stall);
  } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
    lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_FFMA, dst, pa, pb);
    hi = ixg_sass_enc_maxwell_ctrl(stall, 0u);
  } else {
    lo = ixg_sass_enc_volta_falu(IXG_V_OP_FFMA, dst, pa, pb);
    hi = ixg_sass_enc_volta_ctrl(stall, 0u, 7u, 7u, 0u, pc);
  }
  ixg_enc_track(b, dst);
  ixg_enc_word(b, lo, hi);
}

static void ixg_enc_prologue(ixg_sass_binary_t* b) {
  if (b->fmt == IXG_SASS_FMT_KEPLER) {
    ixg_enc_word(b, ixg_sass_enc_kepler_simple(IXG_K_OP_MOV), ixg_sass_enc_kepler_ctrl(6u));
  } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
    ixg_enc_word(b, ixg_sass_enc_maxwell_simple(IXG_M_OP_MOV), ixg_sass_enc_maxwell_ctrl(6u, 0u));
  } else {
    ixg_enc_word(b, ixg_sass_enc_volta_ldc(1u, 0x28u), ixg_sass_enc_volta_ctrl(6u, 0u, 7u, 7u, 0u, 0u));
  }
}

static void ixg_enc_epilogue(ixg_sass_binary_t* b) {
  if (b->fmt == IXG_SASS_FMT_KEPLER) {
    ixg_enc_word(b, ixg_sass_enc_kepler_simple(IXG_K_OP_EXIT), ixg_sass_enc_kepler_ctrl(1u));
    ixg_enc_word(b, ixg_sass_enc_kepler_simple(IXG_K_OP_BRA), ixg_sass_enc_kepler_ctrl(15u));
  } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
    ixg_enc_word(b, ixg_sass_enc_maxwell_simple(IXG_M_OP_EXIT), ixg_sass_enc_maxwell_ctrl(1u, 0u));
    ixg_enc_word(b, ixg_sass_enc_maxwell_simple(IXG_M_OP_BRA), ixg_sass_enc_maxwell_ctrl(15u, 0u));
  } else {
    ixg_enc_word(b, ixg_sass_enc_volta_simple(IXG_V_OP_EXIT), ixg_sass_enc_volta_ctrl(1u, 0u, 7u, 7u, 0u, 0u));
    ixg_enc_word(b, ixg_sass_enc_volta_simple(IXG_V_OP_BRA), ixg_sass_enc_volta_ctrl(15u, 0u, 7u, 7u, 0u, 0u));
  }
}

static ix_u32 ixg_enc_op(ixg_sass_binary_t* b, ix_u32 v_op, ix_u32 m_op, ix_u32 k_op) {
  if (b->fmt == IXG_SASS_FMT_KEPLER) return k_op;
  if (b->fmt == IXG_SASS_FMT_MAXWELL) return m_op;
  return v_op;
}

static void ixg_enc_lower(ixg_sass_binary_t* b, const ixg_shader_instruction_t* in) {
  ix_u8 mask = in->write_mask ? in->write_mask : (ix_u8)IXG_SHADER_MASK_XYZW;
  ix_u8 comp;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_ADD:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_enc_falu(b, ixg_enc_op(b, IXG_V_OP_FADD, IXG_M_OP_FADD, IXG_K_OP_FADD), ixg_enc_phys(in->dst, comp), in->src[0], in->src[1], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_MUL:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_enc_falu(b, ixg_enc_op(b, IXG_V_OP_FMUL, IXG_M_OP_FMUL, IXG_K_OP_FMUL), ixg_enc_phys(in->dst, comp), in->src[0], in->src[1], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_MIN:
    case IXG_SHADER_OP_MAX:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_enc_falu(b, ixg_enc_op(b, IXG_V_OP_FMNMX, IXG_M_OP_FMNMX, IXG_K_OP_FMNMX), ixg_enc_phys(in->dst, comp), in->src[0], in->src[1], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_enc_ffma(b, ixg_enc_phys(in->dst, comp), in->src[0], in->src[1], in->src[2], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_MOV:
      for (comp = 0; comp < 4; comp++) {
        ix_u32 pa;
        ix_u64 lo, hi;
        if (!(mask & (1u << comp))) continue;
        pa = ixg_enc_src_reg(b, in->src[0], comp);
        if (b->fmt == IXG_SASS_FMT_KEPLER) {
          lo = ixg_sass_enc_kepler_falu(IXG_K_OP_MOV, ixg_enc_phys(in->dst, comp), IXG_V_RZ, pa);
          hi = ixg_sass_enc_kepler_ctrl(2u);
        } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
          lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_MOV, ixg_enc_phys(in->dst, comp), IXG_V_RZ, pa);
          hi = ixg_sass_enc_maxwell_ctrl(2u, 0u);
        } else {
          lo = ixg_sass_enc_volta_falu(IXG_V_OP_MOV, ixg_enc_phys(in->dst, comp), IXG_V_RZ, pa);
          hi = ixg_sass_enc_volta_ctrl(2u, 0u, 7u, 7u, 0u, 0u);
        }
        ixg_enc_word(b, lo, hi);
      }
      break;
    case IXG_SHADER_OP_NOP:
    case IXG_SHADER_OP_END:
      break;
    default:
      if (b->fmt == IXG_SASS_FMT_KEPLER) {
        ixg_enc_word(b, ixg_sass_enc_kepler_simple(IXG_K_OP_NOP), ixg_sass_enc_kepler_ctrl(1u));
      } else if (b->fmt == IXG_SASS_FMT_MAXWELL) {
        ixg_enc_word(b, ixg_sass_enc_maxwell_simple(IXG_M_OP_NOP), ixg_sass_enc_maxwell_ctrl(1u, 0u));
      } else {
        ixg_enc_word(b, ixg_sass_enc_volta_simple(IXG_V_OP_NOP), ixg_sass_enc_volta_ctrl(1u, 0u, 7u, 7u, 0u, 0u));
      }
      break;
  }
}

int ixg_sass_encode(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, ixg_sass_binary_t* out) {
  ix_u32 i;
  if (!program || !out) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  ixg_enc_reset(out, sm_version);
  out->shader_type = shader_type;
  if (out->fmt == IXG_SASS_FMT_NONE) return 1;
  ixg_enc_prologue(out);
  for (i = 0u; i < program->instruction_count; i++) {
    const ixg_shader_instruction_t* in = &program->instructions[i];
    if (in->opcode == IXG_SHADER_OP_END) break;
    if (in->opcode == IXG_SHADER_OP_EMIT_DRAW || in->opcode == IXG_SHADER_OP_EMIT_DISPATCH || in->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
    ixg_enc_lower(out, in);
    if (out->truncated) return -3;
  }
  ixg_enc_epilogue(out);
  if (out->truncated) return -3;
  return 0;
}

int ixg_sass_encode_selftest(ix_u32* out_checked) {
  ix_u32 checked = 0u;
  if (ixg_sass_enc_volta_falu(IXG_V_OP_FADD, 9u, 2u, 5u) != 0x0000000502097221ull) return -1;
  checked++;
  if (ixg_sass_enc_volta_falu(IXG_V_OP_FFMA, 11u, 2u, 5u) != 0x00000005020b7223ull) return -2;
  checked++;
  if ((ixg_sass_enc_volta_falu(IXG_V_OP_FFMA, 11u, 2u, 5u) | ixg_sass_enc_volta_ctrl(5u, 0u, 7u, 7u, 0u, 7u)) != 0x00000005020b7223ull) return -3;
  checked++;
  if (ixg_sass_enc_volta_ctrl(5u, 0u, 7u, 7u, 0x10u, 0u) != 0x010fca0000000000ull) return -4;
  checked++;
  if (ixg_sass_enc_volta_ctrl(5u, 0u, 7u, 7u, 0x10u, 7u) != 0x010fca0000000007ull) return -5;
  checked++;
  if (ixg_sass_enc_volta_ldc(1u, 0x37cu) != 0x0000df00ff017b82ull) return -6;
  checked++;
  if (ixg_sass_enc_volta_s2r(0u, IXG_V_SR_TID_X) != 0x0000000000007919ull) return -7;
  checked++;
  if (ixg_sass_enc_volta_simple(IXG_V_OP_EXIT) != 0x000000000000794dull) return -8;
  checked++;
  if (ixg_sass_enc_volta_simple(IXG_V_OP_NOP) != 0x0000000000007918ull) return -9;
  checked++;
  if (out_checked) *out_checked = checked;
  return 0;
}