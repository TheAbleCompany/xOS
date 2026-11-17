#include "ixg_isa_encode.h"
#include <string.h>

#define AMD_VOP2_FENC   0u
#define AMD_VOP2_ADD_F32 0x03u
#define AMD_VOP2_SUB_F32 0x04u
#define AMD_VOP2_MUL_F32 0x08u
#define AMD_VOP2_MIN_F32 0x0Fu
#define AMD_VOP2_MAX_F32 0x10u

#define AMD_VOP1_NOP    0x00u
#define AMD_VOP1_MOV    0x01u
#define AMD_VOP1_RCP_F32 0x2Au
#define AMD_VOP1_RSQ_F32 0x2Eu

#define AMD_SOPP_NOP    0x00u
#define AMD_SOPP_ENDPGM 0x01u

#define AMD_MUBUF_LOAD_DWORD  0x14u
#define AMD_MUBUF_ENC         0x38u
#define AMD_SGPR_SRSRC_BASE   0u
#define AMD_SGPR_SOFFSET      4u

#define AMD_VGPR_BASE   256u
#define AMD_VOP2_MOV_GUARD 0xFFu

#define INTEL_OP_MOV  0x01u
#define INTEL_OP_ADD  0x40u
#define INTEL_OP_MUL  0x41u
#define INTEL_OP_MAD  0x5Bu
#define INTEL_OP_SEND 0x31u
#define INTEL_OP_SENDC 0x32u
#define INTEL_OP_SYNC  0x01u
#define INTEL_OP_NOP  0x60u
#define INTEL_SFID_SAMPLER 0x2u

ix_u32 ixg_isa_enc_amd_vop2(ix_u32 op, ix_u32 vdst, ix_u32 src0, ix_u32 vsrc1) {
  ix_u32 w = 0u;
  w |= (AMD_VOP2_FENC & 0x1u) << 31;
  w |= (op & 0x3Fu) << 25;
  w |= (vdst & 0xFFu) << 17;
  w |= (vsrc1 & 0xFFu) << 9;
  w |= (src0 & 0x1FFu);
  return w;
}

ix_u32 ixg_isa_enc_amd_vop1(ix_u32 op, ix_u32 vdst, ix_u32 src0) {
  ix_u32 w = 0u;
  w |= 0x7Eu << 25;
  w |= (vdst & 0xFFu) << 17;
  w |= (op & 0xFFu) << 9;
  w |= (src0 & 0x1FFu);
  return w;
}

ix_u32 ixg_isa_enc_amd_sopp(ix_u32 op, ix_u32 simm16) {
  ix_u32 w = 0u;
  w |= 0xBFu << 23;
  w |= (op & 0x7Fu) << 16;
  w |= (simm16 & 0xFFFFu);
  return w;
}

ix_u32 ixg_isa_enc_amd_waitcnt_imm(ix_u32 vmcnt, ix_u32 lgkmcnt, ix_u32 expcnt) {
  ix_u32 imm = 0u;
  if (vmcnt > AMD_WAITCNT_VM_MAX) vmcnt = AMD_WAITCNT_VM_MAX;
  if (lgkmcnt > AMD_WAITCNT_LGKM_MAX) lgkmcnt = AMD_WAITCNT_LGKM_MAX;
  if (expcnt > AMD_WAITCNT_EXP_MAX) expcnt = AMD_WAITCNT_EXP_MAX;
  imm |= (vmcnt & 0xFu);
  imm |= ((vmcnt >> 4) & 0x3u) << 14;
  imm |= (expcnt & 0x7u) << 4;
  imm |= (lgkmcnt & 0x1Fu) << 8;
  return imm & 0xFFFFu;
}

ix_u64 ixg_isa_enc_amd_mubuf_load(ix_u32 op, ix_u32 vdst, ix_u32 vaddr, ix_u32 srsrc, ix_u32 soffset) {
  ix_u64 w0 = 0u;
  ix_u64 w1 = 0u;
  w0 |= (ix_u64)AMD_MUBUF_ENC << 26;
  w0 |= (ix_u64)(op & 0x7Fu) << 18;
  w1 |= (ix_u64)(vaddr & 0xFFu);
  w1 |= (ix_u64)(vdst & 0xFFu) << 8;
  w1 |= (ix_u64)((srsrc >> 2) & 0x1Fu) << 16;
  w1 |= (ix_u64)(soffset & 0xFFu) << 24;
  return (w0 << 32) | w1;
}

void ixg_isa_enc_intel_inst(ix_u8 out[16], ix_u32 opcode, ix_u32 dst_grf, ix_u32 src0_grf, ix_u32 src1_grf, ix_u8 has_src1) {
  ix_u32 w0, w1, w2, w3;
  memset(out, 0, 16u);
  w0 = (opcode & 0x7Fu);
  w0 |= (1u << 8);
  w0 |= (3u << 21);
  w1 = (dst_grf & 0xFFu) << 0;
  w1 |= 0x4u << 16;
  w2 = (src0_grf & 0xFFu);
  w2 |= 0x4u << 16;
  w3 = has_src1 ? ((src1_grf & 0xFFu) | (0x4u << 16)) : 0u;
  out[0] = (ix_u8)(w0 & 0xFFu);
  out[1] = (ix_u8)((w0 >> 8) & 0xFFu);
  out[2] = (ix_u8)((w0 >> 16) & 0xFFu);
  out[3] = (ix_u8)((w0 >> 24) & 0xFFu);
  out[4] = (ix_u8)(w1 & 0xFFu);
  out[5] = (ix_u8)((w1 >> 8) & 0xFFu);
  out[6] = (ix_u8)((w1 >> 16) & 0xFFu);
  out[7] = (ix_u8)((w1 >> 24) & 0xFFu);
  out[8] = (ix_u8)(w2 & 0xFFu);
  out[9] = (ix_u8)((w2 >> 8) & 0xFFu);
  out[10] = (ix_u8)((w2 >> 16) & 0xFFu);
  out[11] = (ix_u8)((w2 >> 24) & 0xFFu);
  out[12] = (ix_u8)(w3 & 0xFFu);
  out[13] = (ix_u8)((w3 >> 8) & 0xFFu);
  out[14] = (ix_u8)((w3 >> 16) & 0xFFu);
  out[15] = (ix_u8)((w3 >> 24) & 0xFFu);
}

static void isa_enc_track(ixg_isa_binary_t* b, ix_u32 r) {
  if ((r + 1u) > b->reg_high_water) b->reg_high_water = r + 1u;
}

static void isa_enc_word32(ixg_isa_binary_t* b, ix_u32 w) {
  ix_u32 o = b->byte_len;
  if (o + 4u > IXG_ISA_ENC_MAX_BYTES) { b->truncated = 1u; return; }
  b->bytes[o + 0u] = (ix_u8)(w & 0xFFu);
  b->bytes[o + 1u] = (ix_u8)((w >> 8) & 0xFFu);
  b->bytes[o + 2u] = (ix_u8)((w >> 16) & 0xFFu);
  b->bytes[o + 3u] = (ix_u8)((w >> 24) & 0xFFu);
  b->byte_len += 4u;
  b->instr_count++;
}

static void isa_enc_bytes(ixg_isa_binary_t* b, const ix_u8* p, ix_u32 n) {
  ix_u32 i;
  if (b->byte_len + n > IXG_ISA_ENC_MAX_BYTES) { b->truncated = 1u; return; }
  for (i = 0u; i < n; i++) b->bytes[b->byte_len + i] = p[i];
  b->byte_len += n;
  b->instr_count++;
}

static ix_u32 isa_enc_phys(ix_u8 reg, ix_u8 comp) {
  if (reg < IXG_SHADER_REG_INPUT_BASE)
    return ((ix_u32)reg * IXG_ISA_SCALAR_PER_VEC) + comp;
  if (reg >= IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    return IXG_ISA_REG_OUT_BASE + ((ix_u32)idx * IXG_ISA_SCALAR_PER_VEC) + comp;
  }
  return ((ix_u32)reg * IXG_ISA_SCALAR_PER_VEC) + comp;
}

static ix_u32 isa_enc_amd_vsrc(ixg_isa_binary_t* b, ix_u8 reg, ix_u8 comp) {
  ix_u32 phys = isa_enc_phys(reg, comp);
  isa_enc_track(b, phys);
  return AMD_VGPR_BASE + (phys & 0xFFu);
}

static ix_u32 isa_enc_amd_op(ix_u32 ir_op) {
  switch (ir_op) {
    case IXG_SHADER_OP_ADD: return AMD_VOP2_ADD_F32;
    case IXG_SHADER_OP_SUB: return AMD_VOP2_SUB_F32;
    case IXG_SHADER_OP_MUL: return AMD_VOP2_MUL_F32;
    case IXG_SHADER_OP_MIN: return AMD_VOP2_MIN_F32;
    case IXG_SHADER_OP_MAX: return AMD_VOP2_MAX_F32;
    default: return AMD_VOP2_MOV_GUARD;
  }
}

static void isa_enc_amd_lower(ixg_isa_binary_t* b, const ixg_shader_instruction_t* in) {
  ix_u8 mask = in->write_mask ? in->write_mask : (ix_u8)IXG_SHADER_MASK_XYZW;
  ix_u8 comp;
  ix_u32 dst, s0, s1;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_MOV:
      for (comp = 0u; comp < 4u; comp++) {
        if (!(mask & (1u << comp))) continue;
        dst = AMD_VGPR_BASE + (isa_enc_phys(in->dst, comp) & 0xFFu);
        s0 = isa_enc_amd_vsrc(b, in->src[0], comp);
        isa_enc_track(b, isa_enc_phys(in->dst, comp));
        isa_enc_word32(b, ixg_isa_enc_amd_vop1(AMD_VOP1_MOV, dst & 0xFFu, s0));
      }
      break;
    case IXG_SHADER_OP_ADD:
    case IXG_SHADER_OP_SUB:
    case IXG_SHADER_OP_MUL:
    case IXG_SHADER_OP_MIN:
    case IXG_SHADER_OP_MAX:
      for (comp = 0u; comp < 4u; comp++) {
        if (!(mask & (1u << comp))) continue;
        dst = AMD_VGPR_BASE + (isa_enc_phys(in->dst, comp) & 0xFFu);
        s0 = isa_enc_amd_vsrc(b, in->src[0], comp);
        s1 = isa_enc_amd_vsrc(b, in->src[1], comp);
        isa_enc_track(b, isa_enc_phys(in->dst, comp));
        isa_enc_word32(b, ixg_isa_enc_amd_vop2(isa_enc_amd_op(in->opcode), dst & 0xFFu, s0, s1 & 0xFFu));
      }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp = 0u; comp < 4u; comp++) {
        ix_u32 acc;
        if (!(mask & (1u << comp))) continue;
        dst = AMD_VGPR_BASE + (isa_enc_phys(in->dst, comp) & 0xFFu);
        s0 = isa_enc_amd_vsrc(b, in->src[0], comp);
        s1 = isa_enc_amd_vsrc(b, in->src[1], comp);
        acc = isa_enc_amd_vsrc(b, in->src[2], comp);
        isa_enc_track(b, isa_enc_phys(in->dst, comp));
        isa_enc_word32(b, ixg_isa_enc_amd_vop2(AMD_VOP2_MUL_F32, dst & 0xFFu, s0, s1 & 0xFFu));
        isa_enc_word32(b, ixg_isa_enc_amd_vop2(AMD_VOP2_ADD_F32, dst & 0xFFu, dst, acc & 0xFFu));
      }
      break;
    case IXG_SHADER_OP_RCP:
    case IXG_SHADER_OP_RSQ:
      for (comp = 0u; comp < 4u; comp++) {
        ix_u32 op1;
        if (!(mask & (1u << comp))) continue;
        dst = AMD_VGPR_BASE + (isa_enc_phys(in->dst, comp) & 0xFFu);
        s0 = isa_enc_amd_vsrc(b, in->src[0], comp);
        op1 = (in->opcode == IXG_SHADER_OP_RCP) ? AMD_VOP1_RCP_F32 : AMD_VOP1_RSQ_F32;
        isa_enc_track(b, isa_enc_phys(in->dst, comp));
        isa_enc_word32(b, ixg_isa_enc_amd_vop1(op1, dst & 0xFFu, s0));
      }
      break;
    case IXG_SHADER_OP_TEX2D:
    case IXG_SHADER_OP_LOADI:
      {
        ix_u32 vaddr = isa_enc_amd_vsrc(b, in->src[0], 0u);
        ix_u32 issued = 0u;
        for (comp = 0u; comp < 4u; comp++) {
          ix_u64 m;
          if (!(mask & (1u << comp))) continue;
          dst = AMD_VGPR_BASE + (isa_enc_phys(in->dst, comp) & 0xFFu);
          isa_enc_track(b, isa_enc_phys(in->dst, comp));
          m = ixg_isa_enc_amd_mubuf_load(AMD_MUBUF_LOAD_DWORD, dst & 0xFFu, vaddr & 0xFFu, AMD_SGPR_SRSRC_BASE, AMD_SGPR_SOFFSET);
          isa_enc_word32(b, (ix_u32)(m & 0xFFFFFFFFu));
          isa_enc_word32(b, (ix_u32)((m >> 32) & 0xFFFFFFFFu));
          b->mem_op_count++;
          b->unguarded_mem++;
          issued++;
        }
        if (issued) {
          isa_enc_word32(b, ixg_isa_enc_amd_sopp(AMD_SOPP_WAITCNT,
                            ixg_isa_enc_amd_waitcnt_imm(0u, AMD_WAITCNT_LGKM_MAX, AMD_WAITCNT_EXP_MAX)));
          b->waitcnt_count++;
          b->unguarded_mem = 0u;
        }
      }
      break;
    case IXG_SHADER_OP_NOP:
    case IXG_SHADER_OP_END:
      break;
    default:
      isa_enc_word32(b, ixg_isa_enc_amd_vop1(AMD_VOP1_NOP, 0u, 0u));
      break;
  }
}

static ix_u32 isa_enc_intel_op(ix_u32 ir_op) {
  switch (ir_op) {
    case IXG_SHADER_OP_ADD: return INTEL_OP_ADD;
    case IXG_SHADER_OP_SUB: return INTEL_OP_ADD;
    case IXG_SHADER_OP_MUL: return INTEL_OP_MUL;
    case IXG_SHADER_OP_MAD: return INTEL_OP_MAD;
    default: return INTEL_OP_MOV;
  }
}

static void isa_enc_intel_lower(ixg_isa_binary_t* b, const ixg_shader_instruction_t* in) {
  ix_u8 mask = in->write_mask ? in->write_mask : (ix_u8)IXG_SHADER_MASK_XYZW;
  ix_u8 comp;
  ix_u8 word[16];
  ix_u32 dst, s0, s1;
  switch ((ix_u32)in->opcode) {
    case IXG_SHADER_OP_MOV:
      for (comp = 0u; comp < 4u; comp++) {
        if (!(mask & (1u << comp))) continue;
        dst = isa_enc_phys(in->dst, comp);
        s0 = isa_enc_phys(in->src[0], comp);
        isa_enc_track(b, dst);
        ixg_isa_enc_intel_inst(word, INTEL_OP_MOV, dst / IXG_ISA_SCALAR_PER_VEC, s0 / IXG_ISA_SCALAR_PER_VEC, 0u, 0u);
        isa_enc_bytes(b, word, 16u);
      }
      break;
    case IXG_SHADER_OP_ADD:
    case IXG_SHADER_OP_SUB:
    case IXG_SHADER_OP_MUL:
      for (comp = 0u; comp < 4u; comp++) {
        if (!(mask & (1u << comp))) continue;
        dst = isa_enc_phys(in->dst, comp);
        s0 = isa_enc_phys(in->src[0], comp);
        s1 = isa_enc_phys(in->src[1], comp);
        isa_enc_track(b, dst);
        ixg_isa_enc_intel_inst(word, isa_enc_intel_op(in->opcode), dst / IXG_ISA_SCALAR_PER_VEC, s0 / IXG_ISA_SCALAR_PER_VEC, s1 / IXG_ISA_SCALAR_PER_VEC, 1u);
        isa_enc_bytes(b, word, 16u);
      }
      break;
    case IXG_SHADER_OP_MAD:
      for (comp = 0u; comp < 4u; comp++) {
        if (!(mask & (1u << comp))) continue;
        dst = isa_enc_phys(in->dst, comp);
        s0 = isa_enc_phys(in->src[0], comp);
        s1 = isa_enc_phys(in->src[1], comp);
        isa_enc_track(b, dst);
        ixg_isa_enc_intel_inst(word, INTEL_OP_MAD, dst / IXG_ISA_SCALAR_PER_VEC, s0 / IXG_ISA_SCALAR_PER_VEC, s1 / IXG_ISA_SCALAR_PER_VEC, 1u);
        isa_enc_bytes(b, word, 16u);
      }
      break;
    case IXG_SHADER_OP_TEX2D:
    case IXG_SHADER_OP_LOADI:
      {
        ix_u32 d = isa_enc_phys(in->dst, 0u) / IXG_ISA_SCALAR_PER_VEC;
        ix_u32 a = isa_enc_phys(in->src[0], 0u) / IXG_ISA_SCALAR_PER_VEC;
        isa_enc_track(b, isa_enc_phys(in->dst, 0u));
        ixg_isa_enc_intel_inst(word, INTEL_OP_SEND, d, a, 0u, 0u);
        word[3] |= (ix_u8)((INTEL_SFID_SAMPLER & 0xFu) << 0);
        word[8] |= 0x01u;
        word[9] |= 0x01u;
        isa_enc_bytes(b, word, 16u);
        b->mem_op_count++;
        b->unguarded_mem++;
        ixg_isa_enc_intel_inst(word, INTEL_OP_SYNC, 0u, 0u, 0u, 0u);
        word[1] |= 0x0Fu;
        isa_enc_bytes(b, word, 16u);
        b->waitcnt_count++;
        b->unguarded_mem = 0u;
      }
      break;
    case IXG_SHADER_OP_NOP:
    case IXG_SHADER_OP_END:
      break;
    default:
      ixg_isa_enc_intel_inst(word, INTEL_OP_NOP, 0u, 0u, 0u, 0u);
      isa_enc_bytes(b, word, 16u);
      break;
  }
}

int ixg_isa_encode(const ixg_shader_program_t* program, ix_u32 vendor, ix_u8 shader_type, ix_u32 target, ixg_isa_binary_t* out) {
  ix_u32 i;
  if (!program || !out) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  memset(out, 0, sizeof(*out));
  out->vendor = vendor;
  out->target = target;
  out->shader_type = shader_type;
  if (vendor != IXG_ISA_VENDOR_AMD && vendor != IXG_ISA_VENDOR_INTEL) return -3;
  for (i = 0u; i < program->instruction_count; i++) {
    const ixg_shader_instruction_t* in = &program->instructions[i];
    if (in->opcode == IXG_SHADER_OP_END) break;
    if (in->opcode == IXG_SHADER_OP_EMIT_DRAW || in->opcode == IXG_SHADER_OP_EMIT_DISPATCH || in->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
    if (vendor == IXG_ISA_VENDOR_AMD) isa_enc_amd_lower(out, in);
    else isa_enc_intel_lower(out, in);
    if (out->truncated) return -4;
  }
  if (vendor == IXG_ISA_VENDOR_AMD) {
    isa_enc_word32(out, ixg_isa_enc_amd_sopp(AMD_SOPP_ENDPGM, 0u));
  } else {
    ix_u8 eot[16];
    ixg_isa_enc_intel_inst(eot, INTEL_OP_SEND, 0u, 0u, 0u, 0u);
    eot[15] |= 0x80u;
    isa_enc_bytes(out, eot, 16u);
  }
  if (out->truncated) return -4;
  out->validated = (out->unguarded_mem == 0u) ? 1u : 0u;
  return 0;
}

int ixg_isa_encode_selftest(ix_u32* out_checked) {
  ix_u32 checked = 0u;
  if (ixg_isa_enc_amd_vop2(AMD_VOP2_ADD_F32, 2u, 256u + 0u, 1u) == 0u) return -1;
  checked++;
  if (ixg_isa_enc_amd_vop1(AMD_VOP1_MOV, 3u, 256u + 4u) == 0u) return -2;
  checked++;
  if (ixg_isa_enc_amd_sopp(AMD_SOPP_ENDPGM, 0u) == 0u) return -3;
  checked++;
  {
    ix_u8 w[16];
    ix_u32 nz = 0u, i;
    ixg_isa_enc_intel_inst(w, INTEL_OP_MOV, 1u, 2u, 0u, 0u);
    for (i = 0u; i < 16u; i++) nz |= w[i];
    if (nz == 0u) return -4;
    checked++;
  }
  {
    ix_u32 wc = 0u;
    int rc = ixg_isa_encode_waitcnt_selftest(&wc);
    if (rc != 0) return rc - 100;
    checked += wc;
  }
  if (out_checked) *out_checked = checked;
  return 0;
}

int ixg_isa_encode_waitcnt_selftest(ix_u32* out_checked) {
  static ixg_shader_program_t prog;
  static ixg_isa_binary_t bin;
  ix_u32 checked = 0u;
  ix_u32 i;
  ix_u32 wait_seen = 0u;
  ix_u32 mem_seen = 0u;

  if (ixg_isa_enc_amd_waitcnt_imm(0u, 0u, 0u) != 0u) return -1;
  checked++;
  if ((ixg_isa_enc_amd_waitcnt_imm(0u, AMD_WAITCNT_LGKM_MAX, AMD_WAITCNT_EXP_MAX) & 0xFu) != 0u) return -2;
  checked++;
  if (ixg_isa_enc_amd_mubuf_load(AMD_MUBUF_LOAD_DWORD, 2u, 0u, 0u, 4u) == 0u) return -3;
  checked++;

  memset(&prog, 0, sizeof(prog));
  prog.stage_mask = (ix_u32)IXG_SHADER_STAGE_3D_PIXEL;
  prog.instructions[0].opcode = (ix_u8)IXG_SHADER_OP_TEX2D;
  prog.instructions[0].dst = (ix_u8)IXG_SHADER_REG_TEMP_BASE;
  prog.instructions[0].src[0] = (ix_u8)IXG_SHADER_REG_INPUT_BASE;
  prog.instructions[0].write_mask = (ix_u8)IXG_SHADER_MASK_XYZW;
  prog.instructions[1].opcode = (ix_u8)IXG_SHADER_OP_ADD;
  prog.instructions[1].dst = (ix_u8)IXG_SHADER_REG_OUTPUT_BASE;
  prog.instructions[1].src[0] = (ix_u8)IXG_SHADER_REG_TEMP_BASE;
  prog.instructions[1].src[1] = (ix_u8)IXG_SHADER_REG_TEMP_BASE;
  prog.instructions[1].write_mask = (ix_u8)IXG_SHADER_MASK_XYZW;
  prog.instructions[2].opcode = (ix_u8)IXG_SHADER_OP_END;
  prog.instruction_count = 3u;

  if (ixg_isa_encode(&prog, IXG_ISA_VENDOR_AMD, 1u, IXG_ISA_AMD_GCN, &bin) != 0) return -4;
  checked++;
  if (bin.mem_op_count == 0u) return -5;
  checked++;
  if (bin.waitcnt_count == 0u) return -6;
  checked++;
  if (bin.validated == 0u) return -7;
  checked++;

  for (i = 0u; i + 4u <= bin.byte_len; i += 4u) {
    ix_u32 w = (ix_u32)bin.bytes[i] | ((ix_u32)bin.bytes[i + 1u] << 8) |
             ((ix_u32)bin.bytes[i + 2u] << 16) | ((ix_u32)bin.bytes[i + 3u] << 24);
    if (((w >> 26) & 0x3Fu) == AMD_MUBUF_ENC && mem_seen == 0u) mem_seen = i + 4u;
    if (((w >> 23) & 0x1FFu) == 0x17Eu && ((w >> 16) & 0x7Fu) == AMD_SOPP_WAITCNT) {
      if (mem_seen != 0u && wait_seen == 0u) wait_seen = i + 4u;
    }
  }
  if (mem_seen == 0u || wait_seen == 0u || wait_seen < mem_seen) return -8;
  checked++;

  if (out_checked) *out_checked = checked;
  return 0;
}