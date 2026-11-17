#include "ixg_sass.h"
#include "ixg_sass_encode.h"
#include <string.h>

const char* ixg_sass_arch_name(ix_u32 arch) {
  switch (arch) {
    case IXG_SASS_ARCH_TESLA:     return "SM_10_TESLA_G80";
    case IXG_SASS_ARCH_FERMI:     return "SM_20_FERMI";
    case IXG_SASS_ARCH_KEPLER:    return "SM_30_KEPLER";
    case IXG_SASS_ARCH_MAXWELL:   return "SM_50_MAXWELL";
    case IXG_SASS_ARCH_PASCAL:    return "SM_60_PASCAL";
    case IXG_SASS_ARCH_VOLTA:     return "SM_70_VOLTA";
    case IXG_SASS_ARCH_TURING:    return "SM_75_TURING";
    case IXG_SASS_ARCH_AMPERE:    return "SM_86_AMPERE";
    case IXG_SASS_ARCH_ADA:       return "SM_89_ADA";
    case IXG_SASS_ARCH_HOPPER:    return "SM_90_HOPPER";
    case IXG_SASS_ARCH_BLACKWELL: return "SM_120_BLACKWELL";
    default:                      return "SM_UNKNOWN";
  }
}

ix_u32 ixg_sass_arch_from_sm(ix_u32 sm_version) {
  if (sm_version >= 0x00C0u) return IXG_SASS_ARCH_BLACKWELL;
  if (sm_version >= 0x0090u) return IXG_SASS_ARCH_HOPPER;
  if (sm_version >= 0x0089u) return IXG_SASS_ARCH_ADA;
  if (sm_version >= 0x0086u) return IXG_SASS_ARCH_AMPERE;
  if (sm_version >= 0x0075u) return IXG_SASS_ARCH_TURING;
  if (sm_version >= 0x0070u) return IXG_SASS_ARCH_VOLTA;
  if (sm_version >= 0x0060u) return IXG_SASS_ARCH_PASCAL;
  if (sm_version >= 0x0050u) return IXG_SASS_ARCH_MAXWELL;
  if (sm_version >= 0x0030u) return IXG_SASS_ARCH_KEPLER;
  if (sm_version >= 0x0020u) return IXG_SASS_ARCH_FERMI;
  return IXG_SASS_ARCH_TESLA;
}

typedef struct {
  ixg_sass_module_t* m;
  ix_u32 pc;
  ix_u32 dialect;
} ixg_sass_ctx_t;

enum {
  IXG_MN_MOV = 0, IXG_MN_FADD, IXG_MN_FMUL, IXG_MN_FFMA,
  IXG_MN_FMIN, IXG_MN_FMAX, IXG_MN_RCP, IXG_MN_RSQ,
  IXG_MN_EXIT, IXG_MN_NOP
};

static const char* ixg_sass_mnem(ix_u32 dialect, ix_u32 which) {
  (void)dialect;
  switch (which) {
    case IXG_MN_MOV:  return "MOV";
    case IXG_MN_FADD: return "FADD";
    case IXG_MN_FMUL: return "FMUL";
    case IXG_MN_FFMA: return "FFMA";
    case IXG_MN_FMIN: return "FMNMX";
    case IXG_MN_FMAX: return "FMNMX";
    case IXG_MN_RCP:  return "MUFU.RCP";
    case IXG_MN_RSQ:  return "MUFU.RSQ";
    case IXG_MN_EXIT: return "EXIT";
    default:          return "NOP";
  }
}

static void ixg_sass_putc(ixg_sass_ctx_t* c, char ch) {
  ixg_sass_module_t* m = c->m;
  if (m->length + 1u >= IXG_SASS_TEXT_CAP) { m->truncated = 1u; return; }
  m->text[m->length++] = ch;
  m->text[m->length] = 0;
}

static void ixg_sass_puts(ixg_sass_ctx_t* c, const char* s) {
  if (!s) return;
  while (*s) { ixg_sass_putc(c, *s++); if (c->m->truncated) return; }
}

static void ixg_sass_put_u32(ixg_sass_ctx_t* c, ix_u32 v) {
  char buf[12]; ix_u32 i = 0;
  if (v == 0u) { ixg_sass_putc(c, '0'); return; }
  while (v && i < sizeof(buf)) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
  while (i) ixg_sass_putc(c, buf[--i]);
}

static void ixg_sass_put_reg(ixg_sass_ctx_t* c, ix_u32 r) {
  if (r == 255u) { ixg_sass_puts(c, "RZ"); return; }
  if (r + 1u > c->m->reg_high_water) c->m->reg_high_water = r + 1u;
  ixg_sass_putc(c, 'R');
  ixg_sass_put_u32(c, r);
}

#define IXG_SASS_REG_OUT_BASE 128u
#define IXG_SASS_REG_SCRATCH0 250u
#define IXG_SASS_REG_SCRATCH1 251u

static ix_u32 ixg_sass_phys_reg(ix_u8 reg, ix_u8 comp) {
  if (reg < IXG_SHADER_REG_INPUT_BASE)
    return ((ix_u32)reg * IXG_SASS_SCALAR_PER_VEC) + comp;
  {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    return IXG_SASS_REG_OUT_BASE + ((ix_u32)idx * IXG_SASS_SCALAR_PER_VEC) + comp;
  }
}

static void ixg_sass_put_addr(ixg_sass_ctx_t* c) {
  char buf[8];
  int i;
  ix_u32 a = c->pc * 16u;
  for (i = 3; i >= 0; --i) {
    ix_u32 nib = (a >> (i * 4)) & 0xFu;
    buf[3 - i] = (char)((nib < 10u) ? ('0' + nib) : ('a' + (nib - 10u)));
  }
  buf[4] = 0;
  ixg_sass_puts(c, "        /*");
  ixg_sass_puts(c, buf);
  ixg_sass_puts(c, "*/ ");
}

static void ixg_sass_newline(ixg_sass_ctx_t* c) { ixg_sass_putc(c, '\n'); }

static void ixg_sass_end_line(ixg_sass_ctx_t* c, ix_u32 stall) {
  ixg_sass_putc(c, ';');
  ixg_sass_puts(c, "  /* sched: stall=");
  ixg_sass_put_u32(c, stall & 0xFu);
  ixg_sass_puts(c, " */");
  ixg_sass_newline(c);
  c->m->instr_count++;
  c->pc++;
}

static void ixg_sass_put_mnem(ixg_sass_ctx_t* c, ix_u32 which) {
  ixg_sass_puts(c, ixg_sass_mnem(c->dialect, which));
  ixg_sass_putc(c, ' ');
}

static void ixg_sass_emit_bin(ixg_sass_ctx_t* c, ix_u32 which, ix_u32 dst, ix_u8 ra, ix_u8 rb, ix_u8 comp, ix_u32 stall) {
  ixg_sass_put_addr(c);
  ixg_sass_put_mnem(c, which);
  ixg_sass_put_reg(c, dst);
  ixg_sass_puts(c, ", R");
  ixg_sass_put_u32(c, ixg_sass_phys_reg(ra, comp));
  ixg_sass_puts(c, ", R");
  ixg_sass_put_u32(c, ixg_sass_phys_reg(rb, comp));
  ixg_sass_end_line(c, stall);
}

static void ixg_sass_emit_mov(ixg_sass_ctx_t* c, ix_u32 dst, ix_u8 ra, ix_u8 comp, ix_u32 stall) {
  ixg_sass_put_addr(c);
  ixg_sass_put_mnem(c, IXG_MN_MOV);
  ixg_sass_put_reg(c, dst);
  ixg_sass_puts(c, ", R");
  ixg_sass_put_u32(c, ixg_sass_phys_reg(ra, comp));
  ixg_sass_end_line(c, stall);
}

static void ixg_sass_emit_mov_imm(ixg_sass_ctx_t* c, ix_u32 dst, float imm) {
  union { float f; ix_u32 u; } conv;
  conv.f = imm;
  ixg_sass_put_addr(c);
  ixg_sass_put_mnem(c, IXG_MN_MOV);
  ixg_sass_put_reg(c, dst);
  ixg_sass_puts(c, ", 0x");
  {
    static const char hx[] = "0123456789abcdef";
    int sh;
    for (sh = 28; sh >= 0; sh -= 4) {
      ix_u32 nib = (conv.u >> sh) & 0xFu;
      ixg_sass_putc(c, hx[nib]);
    }
  }
  ixg_sass_end_line(c, 2u);
}

static void ixg_sass_emit_dot(ixg_sass_ctx_t* c, ix_u32 dst, ix_u8 ra, ix_u8 rb, ix_u32 lanes) {
  ix_u32 acc = IXG_SASS_REG_SCRATCH1;
  ix_u8 comp;
  ixg_sass_put_addr(c);
  ixg_sass_put_mnem(c, IXG_MN_FMUL);
  ixg_sass_put_reg(c, acc);
  ixg_sass_puts(c, ", R");
  ixg_sass_put_u32(c, ixg_sass_phys_reg(ra, 0));
  ixg_sass_puts(c, ", R");
  ixg_sass_put_u32(c, ixg_sass_phys_reg(rb, 0));
  ixg_sass_end_line(c, 4u);
  for (comp = 1; comp < (ix_u8)lanes; comp++) {
    ixg_sass_put_addr(c);
    ixg_sass_put_mnem(c, IXG_MN_FFMA);
    ixg_sass_put_reg(c, acc);
    ixg_sass_puts(c, ", R");
    ixg_sass_put_u32(c, ixg_sass_phys_reg(ra, comp));
    ixg_sass_puts(c, ", R");
    ixg_sass_put_u32(c, ixg_sass_phys_reg(rb, comp));
    ixg_sass_puts(c, ", ");
    ixg_sass_put_reg(c, acc);
    ixg_sass_end_line(c, 4u);
  }
  ixg_sass_emit_mov(c, dst, (ix_u8)(acc / IXG_SASS_SCALAR_PER_VEC * IXG_SASS_SCALAR_PER_VEC), 0, 4u);
}

static ix_u8 ixg_sass_eff_mask(ix_u8 mask) {
  return mask ? mask : (ix_u8)IXG_SHADER_MASK_XYZW;
}

static void ixg_sass_lower_inst(ixg_sass_ctx_t* c, const ixg_shader_instruction_t* in) {
  ix_u8 mask = ixg_sass_eff_mask(in->write_mask);
  ix_u8 comp;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_NOP:
      ixg_sass_put_addr(c); ixg_sass_puts(c, "NOP"); ixg_sass_end_line(c, 1u); break;
    case IXG_SHADER_OP_MOV:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov(c, ixg_sass_phys_reg(in->dst, comp), in->src[0], comp, 2u);
      }
      break;
    case IXG_SHADER_OP_LOADI:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov_imm(c, ixg_sass_phys_reg(in->dst, comp), in->imm);
      }
      break;
    case IXG_SHADER_OP_ADD:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_bin(c, IXG_MN_FADD, ixg_sass_phys_reg(in->dst, comp), in->src[0], in->src[1], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_SUB:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_put_addr(c);
        ixg_sass_put_mnem(c, IXG_MN_FADD);
        ixg_sass_put_reg(c, ixg_sass_phys_reg(in->dst, comp));
        ixg_sass_puts(c, ", R");
        ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], comp));
        ixg_sass_puts(c, ", -R");
        ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[1], comp));
        ixg_sass_end_line(c, 4u);
      }
      break;
    case IXG_SHADER_OP_MUL:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_bin(c, IXG_MN_FMUL, ixg_sass_phys_reg(in->dst, comp), in->src[0], in->src[1], comp, 4u);
      }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_put_addr(c);
        ixg_sass_put_mnem(c, IXG_MN_FFMA);
        ixg_sass_put_reg(c, ixg_sass_phys_reg(in->dst, comp));
        ixg_sass_puts(c, ", R");
        ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], comp));
        ixg_sass_puts(c, ", R");
        ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[1], comp));
        ixg_sass_puts(c, ", R");
        ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[2], comp));
        ixg_sass_end_line(c, 4u);
      }
      break;
    case IXG_SHADER_OP_DP3:
      ixg_sass_emit_dot(c, IXG_SASS_REG_SCRATCH0, in->src[0], in->src[1], 3u);
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov(c, ixg_sass_phys_reg(in->dst, comp), (ix_u8)(IXG_SASS_REG_SCRATCH0 / IXG_SASS_SCALAR_PER_VEC * IXG_SASS_SCALAR_PER_VEC), 0, 4u);
      }
      break;
    case IXG_SHADER_OP_DP4:
      ixg_sass_emit_dot(c, IXG_SASS_REG_SCRATCH0, in->src[0], in->src[1], 4u);
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov(c, ixg_sass_phys_reg(in->dst, comp), (ix_u8)(IXG_SASS_REG_SCRATCH0 / IXG_SASS_SCALAR_PER_VEC * IXG_SASS_SCALAR_PER_VEC), 0, 4u);
      }
      break;
    case IXG_SHADER_OP_RCP:
      ixg_sass_put_addr(c);
      ixg_sass_put_mnem(c, IXG_MN_RCP);
      ixg_sass_put_reg(c, IXG_SASS_REG_SCRATCH0);
      ixg_sass_puts(c, ", R");
      ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], 0));
      ixg_sass_end_line(c, 4u);
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov(c, ixg_sass_phys_reg(in->dst, comp), (ix_u8)(IXG_SASS_REG_SCRATCH0 / IXG_SASS_SCALAR_PER_VEC * IXG_SASS_SCALAR_PER_VEC), 0, 4u);
      }
      break;
    case IXG_SHADER_OP_RSQ:
      ixg_sass_put_addr(c);
      ixg_sass_put_mnem(c, IXG_MN_RSQ);
      ixg_sass_put_reg(c, IXG_SASS_REG_SCRATCH0);
      ixg_sass_puts(c, ", R");
      ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], 0));
      ixg_sass_end_line(c, 4u);
      for (comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        ixg_sass_emit_mov(c, ixg_sass_phys_reg(in->dst, comp), (ix_u8)(IXG_SASS_REG_SCRATCH0 / IXG_SASS_SCALAR_PER_VEC * IXG_SASS_SCALAR_PER_VEC), 0, 4u);
      }
      break;
    case IXG_SHADER_OP_TEX2D:
      ixg_sass_put_addr(c);
      ixg_sass_puts(c, "TEX.P ");
      ixg_sass_put_reg(c, ixg_sass_phys_reg(in->dst, 0));
      ixg_sass_puts(c, ", R");
      ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], 0));
      ixg_sass_puts(c, ", 0x0, 0x0, 2D, 0xf");
      ixg_sass_end_line(c, 0u);
      c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_KILL:
      ixg_sass_put_addr(c);
      ixg_sass_puts(c, "FSETP.LT.AND P0, PT, R");
      ixg_sass_put_u32(c, ixg_sass_phys_reg(in->src[0], 0));
      ixg_sass_puts(c, ", 0x0, PT");
      ixg_sass_end_line(c, 13u);
      ixg_sass_put_addr(c);
      ixg_sass_puts(c, "@P0 EXIT");
      ixg_sass_end_line(c, 1u);
      c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_END: break;
    default:
      ixg_sass_put_addr(c); ixg_sass_puts(c, "NOP"); ixg_sass_end_line(c, 1u); break;
  }
}

static void ixg_sass_emit_header(ixg_sass_ctx_t* c) {
  ixg_sass_puts(c, "//----------------------------------------------------------\n");
  ixg_sass_puts(c, "//   iXlinx SASS listing  arch=");
  ixg_sass_puts(c, ixg_sass_arch_name(c->m->arch));
  ixg_sass_puts(c, "\n//----------------------------------------------------------\n");
  ixg_sass_puts(c, ".text.shader:\n");
}

static void ixg_sass_emit_prologue(ixg_sass_ctx_t* c) {
  ixg_sass_put_addr(c);
  ixg_sass_puts(c, "MOV R1, c[0x0][0x28]");
  ixg_sass_end_line(c, 6u);
}

static void ixg_sass_emit_epilogue(ixg_sass_ctx_t* c) {
  ixg_sass_put_addr(c);
  ixg_sass_puts(c, "EXIT");
  ixg_sass_end_line(c, 1u);
  ixg_sass_put_addr(c);
  ixg_sass_puts(c, "BRA `(.L_END)");
  ixg_sass_end_line(c, 15u);
  ixg_sass_puts(c, ".L_END:\n");
}

int ixg_sass_emit(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, ixg_sass_module_t* out_module) {
  ixg_sass_ctx_t c;
  ix_u32 i;
  if (!program || !out_module) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  memset(out_module, 0, sizeof(*out_module));
  out_module->arch = ixg_sass_arch_from_sm(sm_version);
  out_module->shader_type = shader_type;
  out_module->sm_version = sm_version;
  c.m = out_module;
  c.pc = 0u;
  c.dialect = 0;
  ixg_sass_emit_header(&c);
  ixg_sass_emit_prologue(&c);
  for (i = 0u; i < program->instruction_count; i++) {
    const ixg_shader_instruction_t* in = &program->instructions[i];
    if (in->opcode == IXG_SHADER_OP_END) break;
    if (in->opcode == IXG_SHADER_OP_EMIT_DRAW || in->opcode == IXG_SHADER_OP_EMIT_DISPATCH || in->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
    ixg_sass_lower_inst(&c, in);
    if (out_module->truncated) return -3;
  }
  ixg_sass_emit_epilogue(&c);
  if (out_module->truncated) return -3;
  return 0;
}

int ixg_sass_emit_to_binary(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_binary) {
  ixg_sass_module_t mod;
  ix_u32 copy, ck = 0u, i;
  int rc;
  if (!program || !out_binary) return -1;
  rc = ixg_sass_emit(program, shader_type, sm_version, &mod);
  if (rc != 0) return rc;
  {
    char* bin = (char*)out_binary;
    memset(bin, 0, 4096);
    copy = mod.length;
    if (copy > 4000u) copy = 4000u;
    memcpy(bin, mod.text, copy);
    for (i = 0u; i < copy; i++) ck = (ck * 31u) + bin[i];
  }
  return 0;
}

#define IXG_SPH_TYPE_VTG        1u
#define IXG_SPH_TYPE_PS         2u
#define IXG_SPH_HW_STAGE_VERTEX 1u
#define IXG_SPH_HW_STAGE_PIXEL  5u
#define IXG_SPH_VERSION_KEPLER  2u
#define IXG_SPH_VERSION_MAXWELL 3u

ix_u32 ixg_sass_sph_size_for_arch(ix_u32 arch) {
  if (arch >= IXG_SASS_ARCH_VOLTA) return 0x60u;
  if (arch >= IXG_SASS_ARCH_KEPLER) return 0x50u;
  return 0u;
}

int ixg_sass_emit_sph(ix_u32 arch, ix_u8 is_pixel, void* out_buf, ix_u32 buf_cap, ix_u32* out_size) {
  ix_u32 size = ixg_sass_sph_size_for_arch(arch);
  ix_u8* p = (ix_u8*)out_buf;
  ix_u32 sph_type, hw_stage, version, word0;
  if (!out_buf || !out_size) return -1;
  if (size == 0u) { *out_size = 0u; return 0; }
  if (buf_cap < size) return -1;
  memset(p, 0, size);
  sph_type = is_pixel ? IXG_SPH_TYPE_PS : IXG_SPH_TYPE_VTG;
  hw_stage = is_pixel ? IXG_SPH_HW_STAGE_PIXEL : IXG_SPH_HW_STAGE_VERTEX;
  version  = (arch <= IXG_SASS_ARCH_KEPLER) ? IXG_SPH_VERSION_KEPLER : IXG_SPH_VERSION_MAXWELL;
  word0  = (sph_type & 0x1Fu);
  word0 |= (version  & 0x1Fu) << 5;
  word0 |= (hw_stage & 0x0Fu) << 10;
  if (is_pixel) word0 |= (1u << 14);
  p[0] = (ix_u8)(word0 & 0xFFu);
  p[1] = (ix_u8)((word0 >> 8) & 0xFFu);
  p[2] = (ix_u8)((word0 >> 16) & 0xFFu);
  p[3] = (ix_u8)((word0 >> 24) & 0xFFu);
  *out_size = size;
  return 0;
}

int ixg_sass_emit_program(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_buf, ix_u32 buf_cap, ix_u32* out_total_size, ix_u32* out_code_offset) {
  ix_u32 arch;
  ix_u32 sph_size = 0u;
  ix_u32 code_size = 0u;
  ix_u8 is_pixel;
  int rc;
  if (!program || !out_buf || !out_total_size || buf_cap == 0u) return -1;
  arch = ixg_sass_arch_from_sm(sm_version);
  is_pixel = (ix_u8)((program->stage_mask & (ix_u32)IXG_SHADER_STAGE_3D_PIXEL) ? 1u : 0u);
  rc = ixg_sass_emit_sph(arch, is_pixel, out_buf, buf_cap, &sph_size);
  if (rc != 0) return rc;
  if (sph_size >= buf_cap) return -2;
  rc = ixg_sass_emit_binary(program, shader_type, sm_version, (ix_u8*)out_buf + sph_size, buf_cap - sph_size, &code_size);
  if (rc != 0) return rc;
  *out_total_size = sph_size + code_size;
  if (out_code_offset) *out_code_offset = sph_size;
  return 0;
}

int ixg_sass_emit_binary(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_buf, ix_u32 buf_cap, ix_u32* out_size) {
  ixg_sass_binary_t bin;
  int rc;
  if (!program || !out_buf || !out_size || buf_cap == 0u) return -1;
  rc = ixg_sass_encode(program, shader_type, sm_version, &bin);
  if (rc != 0) return rc;
  if (bin.byte_len > buf_cap) return -2;
  memcpy(out_buf, bin.bytes, bin.byte_len);
  *out_size = bin.byte_len;
  return 0;
}