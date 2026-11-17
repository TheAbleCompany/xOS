#ifndef IXG_SASS_ENCODE_H
#define IXG_SASS_ENCODE_H

#include "ixg_types.h"
#include "ixg_shader_vm.h"
#include "ixg_sass.h"

#define IXG_SASS_ENC_WORD_BYTES 16u
#define IXG_SASS_ENC_MAX_INSTR 1024u
#define IXG_SASS_ENC_MAX_BYTES (IXG_SASS_ENC_MAX_INSTR * IXG_SASS_ENC_WORD_BYTES)

#define IXG_V_OP_FADD  0x221u
#define IXG_V_OP_FMUL  0x220u
#define IXG_V_OP_FFMA  0x223u
#define IXG_V_OP_FMNMX 0x209u
#define IXG_V_OP_MUFU  0x308u
#define IXG_V_OP_MOV   0x202u
#define IXG_V_OP_IMAD  0xc24u
#define IXG_V_OP_LDC   0xb82u
#define IXG_V_OP_S2R   0x919u
#define IXG_V_OP_EXIT  0x94du
#define IXG_V_OP_BRA   0x947u
#define IXG_V_OP_NOP   0x918u
#define IXG_V_OP_LDG   0x381u
#define IXG_V_OP_DEPBAR 0x91au
#define IXG_M_OP_LDG   0x385u
#define IXG_M_OP_DEPBAR 0x91au
#define IXG_K_OP_LDG   0x685u

#define IXG_SASS_BARRIER_COUNT 6u
#define IXG_SASS_BARRIER_NONE  7u

#define IXG_K_OP_FADD  0x100u
#define IXG_K_OP_FMUL  0x108u
#define IXG_K_OP_FFMA  0x180u
#define IXG_K_OP_FMNMX 0x118u
#define IXG_K_OP_MUFU  0x190u
#define IXG_K_OP_MOV   0x010u
#define IXG_K_OP_EXIT  0x930u
#define IXG_K_OP_BRA   0x920u
#define IXG_K_OP_NOP   0x918u

#define IXG_M_OP_FADD  0x221u
#define IXG_M_OP_FMUL  0x220u
#define IXG_M_OP_FFMA  0x223u
#define IXG_M_OP_FMNMX 0x209u
#define IXG_M_OP_MUFU  0x208u
#define IXG_M_OP_MOV   0x202u
#define IXG_M_OP_EXIT  0x94du
#define IXG_M_OP_BRA   0x947u
#define IXG_M_OP_NOP   0x918u

#define IXG_V_RZ 0xffu
#define IXG_V_SR_TID_X 0x21u

typedef enum {
  IXG_SASS_FMT_NONE   = 0,
  IXG_SASS_FMT_KEPLER = 2,
  IXG_SASS_FMT_MAXWELL= 3,
  IXG_SASS_FMT_VOLTA  = 4
} ixg_sass_fmt_t;

typedef struct {
  ix_u64 lo;
  ix_u64 hi;
} ixg_sass_word_t;

typedef struct {
  ix_u8   bytes[IXG_SASS_ENC_MAX_BYTES];
  ix_u32  byte_len;
  ix_u32  instr_count;
  ix_u32  reg_high_water;
  ix_u32  arch;
  ix_u32  fmt;
  ix_u32  sm_version;
  ix_u8   shader_type;
  ix_u32  truncated;
} ixg_sass_binary_t;

ix_u32 ixg_sass_fmt_from_arch(ix_u32 arch);

ix_u64 ixg_sass_enc_volta_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb);
ix_u64 ixg_sass_enc_volta_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb);
ix_u64 ixg_sass_enc_volta_ldc(ix_u32 dst, ix_u32 cbank_off);
ix_u64 ixg_sass_enc_volta_s2r(ix_u32 dst, ix_u32 sr);
ix_u64 ixg_sass_enc_volta_s2r_hi(ix_u32 stall, ix_u32 sr);
ix_u64 ixg_sass_enc_volta_simple(ix_u32 op);
ix_u64 ixg_sass_enc_volta_ctrl(ix_u32 stall, ix_u32 yield, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask, ix_u32 rc);

ix_u64 ixg_sass_enc_kepler_ctrl(ix_u32 stall);
ix_u64 ixg_sass_enc_kepler_ctrl_full(ix_u32 stall, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask);
ix_u64 ixg_sass_enc_kepler_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb);
ix_u64 ixg_sass_enc_kepler_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb, ix_u32 rc);
ix_u64 ixg_sass_enc_kepler_simple(ix_u32 op);

ix_u64 ixg_sass_enc_maxwell_ctrl(ix_u32 stall, ix_u32 yield);
ix_u64 ixg_sass_enc_maxwell_ctrl_full(ix_u32 stall, ix_u32 yield, ix_u32 wr_bar, ix_u32 rd_bar, ix_u32 wait_mask);
ix_u64 ixg_sass_enc_maxwell_falu(ix_u32 op, ix_u32 dst, ix_u32 ra, ix_u32 rb);
ix_u64 ixg_sass_enc_maxwell_ffma(ix_u32 dst, ix_u32 ra, ix_u32 rb, ix_u32 rc);
ix_u64 ixg_sass_enc_maxwell_simple(ix_u32 op);

int ixg_sass_encode(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, ixg_sass_binary_t* out);
int ixg_sass_encode_selftest(ix_u32* out_checked);

#endif