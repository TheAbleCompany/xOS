#include "ixg_isa.h"
#include <string.h>

typedef struct {
  ixg_isa_module_t* m;
  ix_u32 pc;
} ixg_isa_ctx_t;

const char* ixg_isa_vendor_name(ix_u32 vendor) {
  switch (vendor) {
    case IXG_ISA_VENDOR_AMD:   return "AMD";
    case IXG_ISA_VENDOR_INTEL: return "INTEL";
    case IXG_ISA_VENDOR_SPIRV: return "SPIRV";
    default:                   return "UNKNOWN";
  }
}

const char* ixg_isa_target_name(ix_u32 vendor, ix_u32 target) {
  if (vendor == IXG_ISA_VENDOR_AMD) {
    switch (target) {
      case IXG_ISA_AMD_GCN:   return "GCN";
      case IXG_ISA_AMD_RDNA1: return "RDNA1";
      case IXG_ISA_AMD_RDNA2: return "RDNA2";
      case IXG_ISA_AMD_RDNA3: return "RDNA3";
      default:                return "AMD?";
    }
  }
  if (vendor == IXG_ISA_VENDOR_INTEL) {
    switch (target) {
      case IXG_ISA_INTEL_GEN9:  return "Gen9";
      case IXG_ISA_INTEL_GEN12: return "Gen12";
      case IXG_ISA_INTEL_XEHPG: return "XeHPG";
      case IXG_ISA_INTEL_XE2:   return "Xe2";
      default:                  return "Intel?";
    }
  }
  return "SPIRV";
}

ix_u32 ixg_isa_vendor_from_pci(ix_u16 pci_vendor) {
  if (pci_vendor == 0x1002u || pci_vendor == 0x1022u) return IXG_ISA_VENDOR_AMD;
  if (pci_vendor == 0x8086u) return IXG_ISA_VENDOR_INTEL;
  return IXG_ISA_VENDOR_SPIRV;
}

ix_u32 ixg_isa_target_from_device(ix_u32 vendor, ix_u16 device) {
  if (vendor == IXG_ISA_VENDOR_AMD) {
    if (device >= 0x7400u) return IXG_ISA_AMD_RDNA3;
    if (device >= 0x7300u) return IXG_ISA_AMD_RDNA2;
    if (device >= 0x7000u) return IXG_ISA_AMD_RDNA1;
    return IXG_ISA_AMD_GCN;
  }
  if (vendor == IXG_ISA_VENDOR_INTEL) {
    if (device >= 0xD200u) return IXG_ISA_INTEL_XE2;
    if (device >= 0x4F80u) return IXG_ISA_INTEL_XEHPG;
    if (device >= 0x1900u) return IXG_ISA_INTEL_GEN12;
    return IXG_ISA_INTEL_GEN9;
  }
  return 0u;
}

static void isa_putc(ixg_isa_ctx_t* c, char ch) {
  ixg_isa_module_t* m = c->m;
  if (m->length + 1u >= IXG_ISA_TEXT_CAP) { m->truncated = 1u; return; }
  m->text[m->length++] = ch;
  m->text[m->length] = 0;
}

static void isa_puts(ixg_isa_ctx_t* c, const char* s) {
  if (!s) return;
  while (*s) { isa_putc(c, *s++); if (c->m->truncated) return; }
}

static void isa_put_u32(ixg_isa_ctx_t* c, ix_u32 v) {
  char buf[12]; ix_u32 i = 0;
  if (v == 0u) { isa_putc(c, '0'); return; }
  while (v && i < sizeof(buf)) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
  while (i) isa_putc(c, buf[--i]);
}

static void isa_put_hex32(ixg_isa_ctx_t* c, ix_u32 v) {
  static const char hx[] = "0123456789abcdef";
  int started = 0, sh;
  isa_puts(c, "0x");
  for (sh = 28; sh >= 0; sh -= 4) {
    ix_u32 nib = (v >> sh) & 0xFu;
    if (nib || started || sh == 0) { isa_putc(c, hx[nib]); started = 1; }
  }
}

static void isa_put_imm_f32(ixg_isa_ctx_t* c, float f) {
  union { float f; ix_u32 u; } cv; cv.f = f;
  isa_put_hex32(c, cv.u);
}

static ix_u32 isa_track_reg(ixg_isa_ctx_t* c, ix_u32 r) {
  if (r + 1u > c->m->reg_high_water) c->m->reg_high_water = r + 1u;
  return r;
}

static ix_u32 isa_phys_reg(ix_u8 reg, ix_u8 comp) {
  if (reg < IXG_SHADER_REG_INPUT_BASE)
    return ((ix_u32)reg * IXG_ISA_SCALAR_PER_VEC) + comp;
  if (reg >= IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    return IXG_ISA_REG_OUT_BASE + ((ix_u32)idx * IXG_ISA_SCALAR_PER_VEC) + comp;
  }
  if (reg >= IXG_SHADER_REG_CONST_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_CONST_BASE);
    return 0x4000u + ((ix_u32)idx * IXG_ISA_SCALAR_PER_VEC) + comp;
  }
  {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_INPUT_BASE);
    return 0x2000u + ((ix_u32)idx * IXG_ISA_SCALAR_PER_VEC) + comp;
  }
}

static ix_u8 isa_eff_mask(ix_u8 mask) {
  return mask ? mask : (ix_u8)IXG_SHADER_MASK_XYZW;
}

static const char* isa_shader_kind(ix_u8 t) {
  if (t == 1u) return "VERTEX";
  if (t == 2u) return "PIXEL";
  if (t == 4u) return "COMPUTE";
  return "GENERIC";
}

static void isa_end_inst(ixg_isa_ctx_t* c) {
  isa_putc(c, '\n');
  c->m->instr_count++;
  c->pc++;
}

#define ISA_AMD_VTMP0 250u
#define ISA_AMD_VTMP1 251u

static void amd_addr(ixg_isa_ctx_t* c) {
  static const char hx[] = "0123456789abcdef";
  ix_u32 a = c->pc * 4u; int sh;
  isa_puts(c, "  /* ");
  for (sh = 12; sh >= 0; sh -= 4) isa_putc(c, hx[(a >> sh) & 0xFu]);
  isa_puts(c, " */ ");
}

static void amd_vreg(ixg_isa_ctx_t* c, ix_u32 phys) {
  isa_track_reg(c, phys);
  isa_putc(c, 'v');
  isa_put_u32(c, phys & 0xFFu);
}

static void amd_src(ixg_isa_ctx_t* c, ix_u8 reg, ix_u8 comp) {
  if (reg >= IXG_SHADER_REG_INPUT_BASE && reg < IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u32 off;
    if (reg >= IXG_SHADER_REG_CONST_BASE)
      off = ((ix_u32)(reg - IXG_SHADER_REG_CONST_BASE) * 16u) + ((ix_u32)comp * 4u);
    else
      off = ((ix_u32)(reg - IXG_SHADER_REG_INPUT_BASE) * 16u) + ((ix_u32)comp * 4u);
    isa_puts(c, "s["); isa_put_u32(c, off / 4u); isa_putc(c, ']');
    return;
  }
  amd_vreg(c, isa_phys_reg(reg, comp));
}

static void amd_op3(ixg_isa_ctx_t* c, const char* mnem, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u8 comp) {
  amd_addr(c);
  isa_puts(c, mnem); isa_putc(c, ' ');
  amd_vreg(c, dst); isa_puts(c, ", ");
  amd_src(c, a, comp); isa_puts(c, ", ");
  amd_src(c, b, comp);
  isa_end_inst(c);
}

static void amd_mov(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 comp) {
  amd_addr(c);
  isa_puts(c, "v_mov_b32 "); amd_vreg(c, dst); isa_puts(c, ", ");
  amd_src(c, a, comp);
  isa_end_inst(c);
}

static void amd_mov_imm(ixg_isa_ctx_t* c, ix_u32 dst, float imm) {
  amd_addr(c);
  isa_puts(c, "v_mov_b32 "); amd_vreg(c, dst); isa_puts(c, ", ");
  isa_put_imm_f32(c, imm);
  isa_end_inst(c);
}

static void amd_fma(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u8 cc, ix_u8 comp) {
  amd_addr(c);
  isa_puts(c, "v_fma_f32 "); amd_vreg(c, dst); isa_puts(c, ", ");
  amd_src(c, a, comp); isa_puts(c, ", ");
  amd_src(c, b, comp); isa_puts(c, ", ");
  amd_src(c, cc, comp);
  isa_end_inst(c);
}

static void amd_mufu(ixg_isa_ctx_t* c, const char* mnem, ix_u32 dst, ix_u8 a, ix_u8 comp) {
  amd_addr(c);
  isa_puts(c, mnem); isa_putc(c, ' ');
  amd_vreg(c, dst); isa_puts(c, ", ");
  amd_src(c, a, comp);
  isa_end_inst(c);
}

static void amd_dot(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u32 lanes) {
  ix_u32 acc = ISA_AMD_VTMP1;
  ix_u8 comp;
  amd_addr(c);
  isa_puts(c, "v_mul_f32 "); amd_vreg(c, acc); isa_puts(c, ", ");
  amd_src(c, a, 0); isa_puts(c, ", "); amd_src(c, b, 0);
  isa_end_inst(c);
  for (comp = 1; comp < (ix_u8)lanes; comp++) {
    amd_addr(c);
    isa_puts(c, "v_fma_f32 "); amd_vreg(c, acc); isa_puts(c, ", ");
    amd_src(c, a, comp); isa_puts(c, ", ");
    amd_src(c, b, comp); isa_puts(c, ", ");
    amd_vreg(c, acc);
    isa_end_inst(c);
  }
  amd_addr(c);
  isa_puts(c, "v_mov_b32 "); amd_vreg(c, dst); isa_puts(c, ", ");
  amd_vreg(c, acc);
  isa_end_inst(c);
}

static void amd_lower(ixg_isa_ctx_t* c, const ixg_shader_instruction_t* in) {
  ix_u8 mask = isa_eff_mask(in->write_mask);
  ix_u8 comp;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_NOP:
      amd_addr(c); isa_puts(c, "s_nop 0"); isa_end_inst(c); break;
    case IXG_SHADER_OP_MOV:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_mov(c, isa_phys_reg(in->dst,comp), in->src[0], comp); }
      break;
    case IXG_SHADER_OP_LOADI:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_mov_imm(c, isa_phys_reg(in->dst,comp), in->imm); }
      break;
    case IXG_SHADER_OP_ADD:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_op3(c,"v_add_f32",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_SUB:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_op3(c,"v_sub_f32",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MUL:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_op3(c,"v_mul_f32",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MIN:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_op3(c,"v_min_f32",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MAX:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_op3(c,"v_max_f32",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_fma(c,isa_phys_reg(in->dst,comp),in->src[0],in->src[1],in->src[2],comp); }
      break;
    case IXG_SHADER_OP_DP3:
      amd_dot(c, ISA_AMD_VTMP0, in->src[0], in->src[1], 3u);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_addr(c); isa_puts(c,"v_mov_b32 "); amd_vreg(c,isa_phys_reg(in->dst,comp)); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_DP4:
      amd_dot(c, ISA_AMD_VTMP0, in->src[0], in->src[1], 4u);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_addr(c); isa_puts(c,"v_mov_b32 "); amd_vreg(c,isa_phys_reg(in->dst,comp)); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_RCP:
      amd_mufu(c, "v_rcp_f32", ISA_AMD_VTMP0, in->src[0], 0);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_addr(c); isa_puts(c,"v_mov_b32 "); amd_vreg(c,isa_phys_reg(in->dst,comp)); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_RSQ:
      amd_mufu(c, "v_rsq_f32", ISA_AMD_VTMP0, in->src[0], 0);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; amd_addr(c); isa_puts(c,"v_mov_b32 "); amd_vreg(c,isa_phys_reg(in->dst,comp)); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_NORMALIZE:
      amd_dot(c, ISA_AMD_VTMP0, in->src[0], in->src[0], 3u);
      amd_addr(c); isa_puts(c,"v_rsq_f32 "); amd_vreg(c,ISA_AMD_VTMP1); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP0); isa_end_inst(c);
      for (comp=0;comp<3;comp++) { if(!(mask&(1u<<comp))) continue; amd_addr(c); isa_puts(c,"v_mul_f32 "); amd_vreg(c,isa_phys_reg(in->dst,comp)); isa_puts(c,", "); amd_src(c,in->src[0],comp); isa_puts(c,", "); amd_vreg(c,ISA_AMD_VTMP1); isa_end_inst(c); }
      if (mask & IXG_SHADER_MASK_W) amd_mov(c, isa_phys_reg(in->dst,3), in->src[0], 3);
      break;
    case IXG_SHADER_OP_TEX2D:
      amd_addr(c);
      isa_puts(c, "image_sample "); amd_vreg(c, isa_phys_reg(in->dst, 0));
      isa_puts(c, ", "); amd_vreg(c, isa_phys_reg(in->src[0], 0));
      isa_puts(c, ", s[0:7], s[8:11] dmask:0xf");
      isa_end_inst(c); c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_KILL:
      amd_addr(c); isa_puts(c, "v_cmp_lt_f32 vcc, "); amd_src(c, in->src[0], 0);
      isa_puts(c, ", "); isa_put_imm_f32(c, in->imm); isa_end_inst(c);
      amd_addr(c); isa_puts(c, "s_cbranch_vccnz _kill; s_mov_b64 exec, 0"); isa_end_inst(c);
      c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_END: break;
    default:
      amd_addr(c); isa_puts(c, "s_nop 0"); isa_end_inst(c); break;
  }
}

#define ISA_INTEL_GRF_TMP0 120u
#define ISA_INTEL_GRF_TMP1 122u

static void intel_addr(ixg_isa_ctx_t* c) {
  static const char hx[] = "0123456789abcdef";
  ix_u32 a = c->pc * 16u; int sh;
  isa_puts(c, "  /* ");
  for (sh = 16; sh >= 0; sh -= 4) isa_putc(c, hx[(a >> sh) & 0xFu]);
  isa_puts(c, " */ ");
}

static void intel_grf(ixg_isa_ctx_t* c, ix_u32 phys, ix_u8 comp) {
  ix_u32 grf = (phys / IXG_ISA_SCALAR_PER_VEC);
  isa_track_reg(c, phys);
  isa_putc(c, 'g'); isa_put_u32(c, grf & 0xFFu);
  isa_putc(c, '.'); isa_put_u32(c, comp & 0x3u);
  isa_puts(c, "<0;1,0>:f");
}

static void intel_dst(ixg_isa_ctx_t* c, ix_u32 phys, ix_u8 comp) {
  ix_u32 grf = (phys / IXG_ISA_SCALAR_PER_VEC);
  isa_track_reg(c, phys);
  isa_putc(c, 'g'); isa_put_u32(c, grf & 0xFFu);
  isa_putc(c, '.'); isa_put_u32(c, comp & 0x3u);
  isa_puts(c, "<1>:f");
}

static void intel_src(ixg_isa_ctx_t* c, ix_u8 reg, ix_u8 comp) {
  intel_grf(c, isa_phys_reg(reg, comp), comp);
}

static void intel_bin(ixg_isa_ctx_t* c, const char* mnem, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u8 comp) {
  intel_addr(c);
  isa_puts(c, "(+f0) "); isa_puts(c, mnem); isa_puts(c, " (8|M0) ");
  intel_dst(c, dst, comp);
  isa_puts(c, " "); intel_src(c, a, comp);
  isa_puts(c, " "); intel_src(c, b, comp);
  isa_end_inst(c);
}

static void intel_mov(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 comp) {
  intel_addr(c);
  isa_puts(c, "mov (8|M0) "); intel_dst(c, dst, comp);
  isa_puts(c, " "); intel_src(c, a, comp);
  isa_end_inst(c);
}

static void intel_mov_imm(ixg_isa_ctx_t* c, ix_u32 dst, float imm, ix_u8 comp) {
  intel_addr(c);
  isa_puts(c, "mov (8|M0) "); intel_dst(c, dst, comp);
  isa_puts(c, " "); isa_put_imm_f32(c, imm); isa_puts(c, ":f");
  isa_end_inst(c);
}

static void intel_mad(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u8 cc, ix_u8 comp) {
  intel_addr(c);
  isa_puts(c, "mad (8|M0) "); intel_dst(c, dst, comp);
  isa_puts(c, " "); intel_src(c, cc, comp);
  isa_puts(c, " "); intel_src(c, a, comp);
  isa_puts(c, " "); intel_src(c, b, comp);
  isa_end_inst(c);
}

static void intel_math(ixg_isa_ctx_t* c, const char* fn, ix_u32 dst, ix_u8 a, ix_u8 comp) {
  intel_addr(c);
  isa_puts(c, "math."); isa_puts(c, fn); isa_puts(c, " (8|M0) ");
  intel_dst(c, dst, comp);
  isa_puts(c, " "); intel_src(c, a, comp);
  isa_end_inst(c);
}

static void intel_dot(ixg_isa_ctx_t* c, ix_u32 dst, ix_u8 a, ix_u8 b, ix_u32 lanes) {
  ix_u32 acc = ISA_INTEL_GRF_TMP1;
  ix_u8 comp;
  intel_addr(c);
  isa_puts(c, "mul (8|M0) "); intel_dst(c, acc, 0);
  isa_puts(c, " "); intel_src(c, a, 0);
  isa_puts(c, " "); intel_src(c, b, 0);
  isa_end_inst(c);
  for (comp = 1; comp < (ix_u8)lanes; comp++) {
    intel_addr(c);
    isa_puts(c, "mad (8|M0) "); intel_dst(c, acc, 0);
    isa_puts(c, " "); intel_src(c, a, comp);
    isa_puts(c, " "); intel_src(c, b, comp);
    isa_end_inst(c);
  }
  intel_addr(c);
  isa_puts(c, "mov (8|M0) "); intel_dst(c, dst, 0);
  isa_puts(c, " "); intel_src(c, (ix_u8)(acc / IXG_ISA_SCALAR_PER_VEC * IXG_ISA_SCALAR_PER_VEC), 0);
  isa_end_inst(c);
}

static void intel_lower(ixg_isa_ctx_t* c, const ixg_shader_instruction_t* in) {
  ix_u8 mask = isa_eff_mask(in->write_mask);
  ix_u8 comp;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_NOP:
      intel_addr(c); isa_puts(c, "nop"); isa_end_inst(c); break;
    case IXG_SHADER_OP_MOV:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_mov(c,isa_phys_reg(in->dst,comp),in->src[0],comp); }
      break;
    case IXG_SHADER_OP_LOADI:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_mov_imm(c,isa_phys_reg(in->dst,comp),in->imm,comp); }
      break;
    case IXG_SHADER_OP_ADD:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_bin(c,"add",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_SUB:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"add (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,in->src[0],comp); isa_puts(c," -"); intel_src(c,in->src[1],comp); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_MUL:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_bin(c,"mul",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MIN:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_bin(c,"sel.l",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MAX:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_bin(c,"sel.ge",isa_phys_reg(in->dst,comp),in->src[0],in->src[1],comp); }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_mad(c,isa_phys_reg(in->dst,comp),in->src[0],in->src[1],in->src[2],comp); }
      break;
    case IXG_SHADER_OP_DP3:
      intel_dot(c, ISA_INTEL_GRF_TMP0, in->src[0], in->src[1], 3u);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"mov (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP0),0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_DP4:
      intel_dot(c, ISA_INTEL_GRF_TMP0, in->src[0], in->src[1], 4u);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"mov (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP0),0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_RCP:
      intel_math(c, "inv", ISA_INTEL_GRF_TMP0, in->src[0], 0);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"mov (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP0),0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_RSQ:
      intel_math(c, "rsqt", ISA_INTEL_GRF_TMP0, in->src[0], 0);
      for (comp=0;comp<4;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"mov (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP0),0); isa_end_inst(c); }
      break;
    case IXG_SHADER_OP_NORMALIZE:
      intel_dot(c, ISA_INTEL_GRF_TMP0, in->src[0], in->src[0], 3u);
      intel_addr(c); isa_puts(c,"math.rsqt (8|M0) "); intel_dst(c,ISA_INTEL_GRF_TMP1,0); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP0),0); isa_end_inst(c);
      for (comp=0;comp<3;comp++) { if(!(mask&(1u<<comp))) continue; intel_addr(c); isa_puts(c,"mul (8|M0) "); intel_dst(c,isa_phys_reg(in->dst,comp),comp); isa_puts(c," "); intel_src(c,in->src[0],comp); isa_puts(c," "); intel_src(c,(ix_u8)(ISA_INTEL_GRF_TMP1),0); isa_end_inst(c); }
      if (mask & IXG_SHADER_MASK_W) intel_mov(c, isa_phys_reg(in->dst,3), in->src[0], 3);
      break;
    case IXG_SHADER_OP_TEX2D:
      intel_addr(c);
      isa_puts(c, "send (8|M0) "); intel_dst(c, isa_phys_reg(in->dst, 0), 0);
      isa_puts(c, " "); intel_src(c, in->src[0], 0);
      isa_puts(c, " 0x4 sampler mlen 2 rlen 4");
      isa_end_inst(c); c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_KILL:
      intel_addr(c); isa_puts(c, "cmp.l (8|M0) f0 null "); intel_src(c, in->src[0], 0);
      isa_puts(c, " "); isa_put_imm_f32(c, in->imm); isa_puts(c, ":f"); isa_end_inst(c);
      intel_addr(c); isa_puts(c, "(+f0) halt (8|M0)"); isa_end_inst(c);
      c->m->barrier_count++;
      break;
    case IXG_SHADER_OP_END: break;
    default:
      intel_addr(c); isa_puts(c, "nop"); isa_end_inst(c); break;
  }
}

static void isa_emit_header(ixg_isa_ctx_t* c) {
  isa_puts(c, "//----------------------------------------------------------\n");
  isa_puts(c, "//   iXlinx ISA listing  vendor=");
  isa_puts(c, ixg_isa_vendor_name(c->m->vendor));
  isa_puts(c, "  target=");
  isa_puts(c, ixg_isa_target_name(c->m->vendor, c->m->target));
  isa_puts(c, "  stage=");
  isa_puts(c, isa_shader_kind((ix_u8)c->m->shader_type));
  isa_putc(c, '\n');
  isa_puts(c, "//----------------------------------------------------------\n");
  isa_puts(c, ".text.shader:\n");
}

int ixg_isa_emit_amd(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module) {
  ixg_isa_ctx_t c; ix_u32 i;
  if (!program || !out_module) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  memset(out_module, 0, sizeof(*out_module));
  out_module->vendor = IXG_ISA_VENDOR_AMD;
  out_module->target = target;
  out_module->shader_type = shader_type;
  c.m = out_module; c.pc = 0u;
  isa_emit_header(&c);
  for (i = 0; i < program->instruction_count; i++) {
    const ixg_shader_instruction_t* in = &program->instructions[i];
    if (in->opcode == IXG_SHADER_OP_END) break;
    if (in->opcode == IXG_SHADER_OP_EMIT_DRAW || in->opcode == IXG_SHADER_OP_EMIT_DISPATCH || in->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
    amd_lower(&c, in);
    if (out_module->truncated) return -3;
  }
  amd_addr(&c); isa_puts(&c, "s_endpgm"); isa_end_inst(&c);
  if (out_module->truncated) return -3;
  return 0;
}

int ixg_isa_emit_intel(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module) {
  ixg_isa_ctx_t c; ix_u32 i;
  if (!program || !out_module) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  memset(out_module, 0, sizeof(*out_module));
  out_module->vendor = IXG_ISA_VENDOR_INTEL;
  out_module->target = target;
  out_module->shader_type = shader_type;
  c.m = out_module; c.pc = 0u;
  isa_emit_header(&c);
  for (i = 0; i < program->instruction_count; i++) {
    const ixg_shader_instruction_t* in = &program->instructions[i];
    if (in->opcode == IXG_SHADER_OP_END) break;
    if (in->opcode == IXG_SHADER_OP_EMIT_DRAW || in->opcode == IXG_SHADER_OP_EMIT_DISPATCH || in->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
    intel_lower(&c, in);
    if (out_module->truncated) return -3;
  }
  intel_addr(&c); isa_puts(&c, "send (8|M0) null null 0x2 EOT"); isa_end_inst(&c);
  if (out_module->truncated) return -3;
  return 0;
}

int ixg_isa_emit(const ixg_shader_program_t* program, ix_u32 vendor, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module) {
  if (vendor == IXG_ISA_VENDOR_AMD)   return ixg_isa_emit_amd(program, shader_type, target, out_module);
  if (vendor == IXG_ISA_VENDOR_INTEL) return ixg_isa_emit_intel(program, shader_type, target, out_module);
  return -1;
}