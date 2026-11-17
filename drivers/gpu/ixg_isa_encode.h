#ifndef IXG_ISA_ENCODE_H
#define IXG_ISA_ENCODE_H

#include "ixg_types.h"
#include "ixg_shader_vm.h"
#include "ixg_isa.h"

#define IXG_ISA_ENC_MAX_BYTES 16384u
#define IXG_ISA_ENC_AMD_WORD 4u
#define IXG_ISA_ENC_INTEL_WORD 16u

#define IXG_ISA_ENC_AMD_VTMP0 200u
#define IXG_ISA_ENC_AMD_VTMP1 201u
#define IXG_ISA_ENC_INTEL_TMP0 112u
#define IXG_ISA_ENC_INTEL_TMP1 116u

#define AMD_SOPP_WAITCNT 0x0Cu
#define AMD_WAITCNT_VM_MAX 0x3Fu
#define AMD_WAITCNT_LGKM_MAX 0x1Fu
#define AMD_WAITCNT_EXP_MAX 0x7u

typedef struct {
  ix_u8  bytes[IXG_ISA_ENC_MAX_BYTES];
  ix_u32 byte_len;
  ix_u32 instr_count;
  ix_u32 reg_high_water;
  ix_u32 vendor;
  ix_u32 target;
  ix_u8  shader_type;
  ix_u32 truncated;
  ix_u32 mem_op_count;
  ix_u32 waitcnt_count;
  ix_u32 unguarded_mem;
  ix_u32 validated;
} ixg_isa_binary_t;

ix_u32 ixg_isa_enc_amd_vop2(ix_u32 op, ix_u32 vdst, ix_u32 src0, ix_u32 vsrc1);
ix_u32 ixg_isa_enc_amd_vop1(ix_u32 op, ix_u32 vdst, ix_u32 src0);
ix_u32 ixg_isa_enc_amd_sopp(ix_u32 op, ix_u32 simm16);
ix_u32 ixg_isa_enc_amd_waitcnt_imm(ix_u32 vmcnt, ix_u32 lgkmcnt, ix_u32 expcnt);
ix_u64 ixg_isa_enc_amd_mubuf_load(ix_u32 op, ix_u32 vdst, ix_u32 vaddr, ix_u32 srsrc, ix_u32 soffset);
void ixg_isa_enc_intel_inst(ix_u8 out[16], ix_u32 opcode, ix_u32 dst_grf, ix_u32 src0_grf, ix_u32 src1_grf, ix_u8 has_src1);

int ixg_isa_encode(const ixg_shader_program_t* program, ix_u32 vendor, ix_u8 shader_type, ix_u32 target, ixg_isa_binary_t* out);
int ixg_isa_encode_selftest(ix_u32* out_checked);
int ixg_isa_encode_waitcnt_selftest(ix_u32* out_checked);

#endif