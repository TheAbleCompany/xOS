#include "ixg_sass.h"
#include "ixg_sass_encode.h"
#include "ixg_shader_compiler.h"
#include <string.h>

typedef struct {
    ix_u8* buf;
    ix_u32 cap;
    ix_u32 len;
    ix_u32 arch;
    ix_u32 fmt;
    ix_u32 reg_count;
    ix_u32 error;
    ix_u32 mem_op_count;
    ix_u32 barrier_count;
    ix_u32 hazard_unresolved;
    ix_u32 next_bar;
    ix_u8  reg_wbar[256];
} ixg_sass_binary_ctx_t;

static void ixg_bin_emit_u8(ixg_sass_binary_ctx_t* ctx, ix_u8 val) {
    if (ctx->len + 1u > ctx->cap) { ctx->error = 1u; return; }
    ctx->buf[ctx->len++] = val;
}
static void ixg_bin_emit_u16(ixg_sass_binary_ctx_t* ctx, ix_u16 val) {
    ixg_bin_emit_u8(ctx, (ix_u8)(val & 0xFFu));
    ixg_bin_emit_u8(ctx, (ix_u8)((val >> 8) & 0xFFu));
}
static void ixg_bin_emit_u32(ixg_sass_binary_ctx_t* ctx, ix_u32 val) {
    ixg_bin_emit_u8(ctx, (ix_u8)(val & 0xFFu));
    ixg_bin_emit_u8(ctx, (ix_u8)((val >> 8) & 0xFFu));
    ixg_bin_emit_u8(ctx, (ix_u8)((val >> 16) & 0xFFu));
    ixg_bin_emit_u8(ctx, (ix_u8)((val >> 24) & 0xFFu));
}
static void ixg_bin_emit_u64(ixg_sass_binary_ctx_t* ctx, ix_u64 val) {
    ix_u32 i;
    for (i = 0u; i < 8u; i++) ixg_bin_emit_u8(ctx, (ix_u8)((val >> (i * 8u)) & 0xFFu));
}

static ix_u32 ixg_bin_phys_reg(ix_u8 reg, ix_u8 comp) {
    if (reg < IXG_SHADER_REG_INPUT_BASE) return ((ix_u32)reg * 4u) + comp;
    if (reg >= IXG_SHADER_REG_OUTPUT_BASE) {
        ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
        return 128u + ((ix_u32)idx * 4u) + comp;
    }
    return 255u;
}

static void ixg_bin_track_reg(ixg_sass_binary_ctx_t* ctx, ix_u32 reg) {
    if (reg != 255u && (reg + 1u) > ctx->reg_count) ctx->reg_count = reg + 1u;
}

static ix_u32 ixg_bin_alloc_barrier(ixg_sass_binary_ctx_t* ctx) {
    ix_u32 b = ctx->next_bar % IXG_SASS_BARRIER_COUNT;
    ctx->next_bar++;
    ctx->barrier_count++;
    return b;
}

static void ixg_bin_mark_wbar(ixg_sass_binary_ctx_t* ctx, ix_u32 reg, ix_u32 bar) {
    if (reg < 256u) ctx->reg_wbar[reg] = (ix_u8)(bar + 1u);
}

static ix_u32 ixg_bin_wait_mask(ixg_sass_binary_ctx_t* ctx, const ix_u32* srcs, ix_u32 n) {
    ix_u32 mask = 0u;
    ix_u32 i;
    for (i = 0u; i < n; i++) {
        ix_u32 r = srcs[i];
        if (r < 256u && ctx->reg_wbar[r] != 0u) {
            ix_u32 bar = (ix_u32)ctx->reg_wbar[r] - 1u;
            mask |= (1u << bar);
            ctx->reg_wbar[r] = 0u;
        }
    }
    return mask & 0x3Fu;
}

static void ixg_bin_clear_wbar_dst(ixg_sass_binary_ctx_t* ctx, ix_u32 reg) {
    if (reg < 256u) ctx->reg_wbar[reg] = 0u;
}

static ix_u64 ixg_sass_enc_volta_ldg(ix_u32 dst, ix_u32 addr) {
    ix_u64 lo;
    lo  = (ix_u64)IXG_V_OP_LDG;
    lo |= (ix_u64)0x7u << 12;
    lo |= (ix_u64)(dst & 0xFFu) << 16;
    lo |= (ix_u64)(addr & 0xFFu) << 24;
    return lo;
}

static ix_u64 ixg_sass_enc_maxwell_ldg(ix_u32 dst, ix_u32 addr) {
    ix_u64 lo;
    lo  = (ix_u64)IXG_M_OP_LDG;
    lo |= (ix_u64)0x3u << 12;
    lo |= (ix_u64)(dst & 0xFFu) << 14;
    lo |= (ix_u64)(addr & 0xFFu) << 22;
    lo |= (ix_u64)0x1u << 38;
    return lo;
}

static ix_u64 ixg_sass_enc_kepler_ldg(ix_u32 dst, ix_u32 addr) {
    ix_u64 inst;
    inst  = (ix_u64)IXG_K_OP_LDG;
    inst |= (ix_u64)(dst & 0xFFu) << 12;
    inst |= (ix_u64)(addr & 0xFFu) << 20;
    inst |= (ix_u64)0x1u << 36;
    return inst;
}

static void ixg_bin_emit_tesla_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ix_u8 c;
    for (c = 0u; c < 4u; c++) {
        ix_u32 dst, src0, src1;
        if (!(mask & (1u << c))) continue;
        dst = ixg_bin_phys_reg(inst->dst, c);
        src0 = ixg_bin_phys_reg(inst->src[0], c);
        src1 = ixg_bin_phys_reg(inst->src[1], c);
        ixg_bin_track_reg(ctx, dst);
        switch (inst->opcode) {
            case IXG_SHADER_OP_ADD:
                ixg_bin_emit_u32(ctx, 0x20000000u | (dst << 16) | (src0 << 8) | src1);
                break;
            case IXG_SHADER_OP_MUL:
                ixg_bin_emit_u32(ctx, 0x30000000u | (dst << 16) | (src0 << 8) | src1);
                break;
            case IXG_SHADER_OP_MAD: {
                ix_u32 src2 = ixg_bin_phys_reg(inst->src[2], c);
                ixg_bin_emit_u32(ctx, 0x40000000u | (dst << 16) | (src0 << 8) | src1);
                ixg_bin_emit_u32(ctx, src2);
            } break;
            case IXG_SHADER_OP_MOV:
                ixg_bin_emit_u32(ctx, 0x10000000u | (dst << 16) | (src0 << 8));
                break;
            case IXG_SHADER_OP_TEX2D:
            case IXG_SHADER_OP_LOADI:
                ixg_bin_emit_u32(ctx, 0x60000000u | (dst << 16) | (src0 << 8));
                ctx->mem_op_count++;
                ctx->hazard_unresolved++;
                break;
            default:
                ixg_bin_emit_u32(ctx, 0x00000000u);
                break;
        }
    }
}

static void ixg_bin_emit_fermi_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ix_u8 c;
    for (c = 0u; c < 4u; c++) {
        ix_u32 dst, src0, src1;
        ix_u64 encoding = 0u;
        if (!(mask & (1u << c))) continue;
        dst = ixg_bin_phys_reg(inst->dst, c);
        src0 = ixg_bin_phys_reg(inst->src[0], c);
        src1 = ixg_bin_phys_reg(inst->src[1], c);
        ixg_bin_track_reg(ctx, dst);
        switch (inst->opcode) {
            case IXG_SHADER_OP_ADD:
                encoding = 0x5000000000000000ull | ((ix_u64)dst << 40) | ((ix_u64)src0 << 32) | ((ix_u64)src1 << 24);
                break;
            case IXG_SHADER_OP_MUL:
                encoding = 0x5800000000000000ull | ((ix_u64)dst << 40) | ((ix_u64)src0 << 32) | ((ix_u64)src1 << 24);
                break;
            case IXG_SHADER_OP_MAD: {
                ix_u32 src2 = ixg_bin_phys_reg(inst->src[2], c);
                encoding = 0x6000000000000000ull | ((ix_u64)dst << 40) | ((ix_u64)src0 << 32) | ((ix_u64)src1 << 24) | ((ix_u64)src2 << 16);
            } break;
            case IXG_SHADER_OP_MOV:
                encoding = 0x4000000000000000ull | ((ix_u64)dst << 40) | ((ix_u64)src0 << 32);
                break;
            case IXG_SHADER_OP_TEX2D:
            case IXG_SHADER_OP_LOADI:
                encoding = 0x8400000000000000ull | ((ix_u64)dst << 40) | ((ix_u64)src0 << 32);
                ctx->mem_op_count++;
                ctx->hazard_unresolved++;
                break;
            default:
                encoding = 0x0000000000000000ull;
                break;
        }
        ixg_bin_emit_u64(ctx, encoding);
    }
}

static void ixg_bin_emit_kepler_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ix_u8 c;
    for (c = 0u; c < 4u; c++) {
        ix_u32 dst, src0, src1;
        ix_u64 lo = 0u, hi = 0u;
        ix_u32 srcs[3];
        ix_u32 wmask;
        if (!(mask & (1u << c))) continue;
        dst = ixg_bin_phys_reg(inst->dst, c);
        src0 = ixg_bin_phys_reg(inst->src[0], c);
        src1 = ixg_bin_phys_reg(inst->src[1], c);
        ixg_bin_track_reg(ctx, dst);
        srcs[0] = src0; srcs[1] = src1; srcs[2] = ixg_bin_phys_reg(inst->src[2], c);
        wmask = ixg_bin_wait_mask(ctx, srcs, 3u);
        switch (inst->opcode) {
            case IXG_SHADER_OP_ADD:
                lo = ixg_sass_enc_kepler_falu(IXG_K_OP_FADD, dst, src0, src1);
                hi = ixg_sass_enc_kepler_ctrl_full(4u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MUL:
                lo = ixg_sass_enc_kepler_falu(IXG_K_OP_FMUL, dst, src0, src1);
                hi = ixg_sass_enc_kepler_ctrl_full(4u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MAD: {
                ix_u32 src2 = ixg_bin_phys_reg(inst->src[2], c);
                lo = ixg_sass_enc_kepler_ffma(dst, src0, src1, src2);
                hi = ixg_sass_enc_kepler_ctrl_full(4u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
            } break;
            case IXG_SHADER_OP_MOV:
                lo = ixg_sass_enc_kepler_falu(IXG_K_OP_MOV, dst, src0, 255u);
                hi = ixg_sass_enc_kepler_ctrl_full(2u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_TEX2D:
            case IXG_SHADER_OP_LOADI: {
                ix_u32 bar = ixg_bin_alloc_barrier(ctx);
                lo = ixg_sass_enc_kepler_ldg(dst, src0);
                hi = ixg_sass_enc_kepler_ctrl_full(1u, bar, 7u, wmask);
                ctx->mem_op_count++;
                ixg_bin_mark_wbar(ctx, dst, bar);
            } break;
            default:
                lo = ixg_sass_enc_kepler_simple(IXG_K_OP_NOP);
                hi = ixg_sass_enc_kepler_ctrl_full(1u, 7u, 7u, wmask);
                break;
        }
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    }
}

static void ixg_bin_emit_maxwell_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ix_u8 c;
    for (c = 0u; c < 4u; c++) {
        ix_u32 dst, src0, src1;
        ix_u64 lo = 0u, hi = 0u;
        ix_u32 srcs[3];
        ix_u32 wmask;
        if (!(mask & (1u << c))) continue;
        dst = ixg_bin_phys_reg(inst->dst, c);
        src0 = ixg_bin_phys_reg(inst->src[0], c);
        src1 = ixg_bin_phys_reg(inst->src[1], c);
        ixg_bin_track_reg(ctx, dst);
        srcs[0] = src0; srcs[1] = src1; srcs[2] = ixg_bin_phys_reg(inst->src[2], c);
        wmask = ixg_bin_wait_mask(ctx, srcs, 3u);
        switch (inst->opcode) {
            case IXG_SHADER_OP_ADD:
                lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_FADD, dst, src0, src1);
                hi = ixg_sass_enc_maxwell_ctrl_full(4u, 0u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MUL:
                lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_FMUL, dst, src0, src1);
                hi = ixg_sass_enc_maxwell_ctrl_full(4u, 0u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MAD: {
                ix_u32 src2 = ixg_bin_phys_reg(inst->src[2], c);
                lo = ixg_sass_enc_maxwell_ffma(dst, src0, src1, src2);
                hi = ixg_sass_enc_maxwell_ctrl_full(4u, 0u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
            } break;
            case IXG_SHADER_OP_MOV:
                lo = ixg_sass_enc_maxwell_falu(IXG_M_OP_MOV, dst, src0, 255u);
                hi = ixg_sass_enc_maxwell_ctrl_full(2u, 0u, 7u, 7u, wmask);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_TEX2D:
            case IXG_SHADER_OP_LOADI: {
                ix_u32 bar = ixg_bin_alloc_barrier(ctx);
                lo = ixg_sass_enc_maxwell_ldg(dst, src0);
                hi = ixg_sass_enc_maxwell_ctrl_full(1u, 0u, bar, 7u, wmask);
                ctx->mem_op_count++;
                ixg_bin_mark_wbar(ctx, dst, bar);
            } break;
            default:
                lo = ixg_sass_enc_maxwell_simple(IXG_M_OP_NOP);
                hi = ixg_sass_enc_maxwell_ctrl_full(1u, 0u, 7u, 7u, wmask);
                break;
        }
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    }
}

static void ixg_bin_emit_pascal_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_maxwell_inst(ctx, inst, mask);
}

static void ixg_bin_emit_volta_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ix_u8 c;
    for (c = 0u; c < 4u; c++) {
        ix_u32 dst, src0, src1;
        ix_u64 lo = 0u, hi = 0u;
        ix_u32 srcs[3];
        ix_u32 wmask;
        if (!(mask & (1u << c))) continue;
        dst = ixg_bin_phys_reg(inst->dst, c);
        src0 = ixg_bin_phys_reg(inst->src[0], c);
        src1 = ixg_bin_phys_reg(inst->src[1], c);
        ixg_bin_track_reg(ctx, dst);
        srcs[0] = src0; srcs[1] = src1; srcs[2] = ixg_bin_phys_reg(inst->src[2], c);
        wmask = ixg_bin_wait_mask(ctx, srcs, 3u);
        switch (inst->opcode) {
            case IXG_SHADER_OP_ADD:
                lo = ixg_sass_enc_volta_falu(IXG_V_OP_FADD, dst, src0, src1);
                hi = ixg_sass_enc_volta_ctrl(4u, 0u, 7u, 7u, wmask, 0u);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MUL:
                lo = ixg_sass_enc_volta_falu(IXG_V_OP_FMUL, dst, src0, src1);
                hi = ixg_sass_enc_volta_ctrl(4u, 0u, 7u, 7u, wmask, 0u);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_MAD: {
                ix_u32 src2 = ixg_bin_phys_reg(inst->src[2], c);
                lo = ixg_sass_enc_volta_ffma(dst, src0, src1);
                hi = ixg_sass_enc_volta_ctrl(4u, 0u, 7u, 7u, wmask, src2);
                ixg_bin_clear_wbar_dst(ctx, dst);
            } break;
            case IXG_SHADER_OP_MOV:
                lo = ixg_sass_enc_volta_falu(IXG_V_OP_MOV, dst, 255u, src0);
                hi = ixg_sass_enc_volta_ctrl(2u, 0u, 7u, 7u, wmask, 0u);
                ixg_bin_clear_wbar_dst(ctx, dst);
                break;
            case IXG_SHADER_OP_TEX2D:
            case IXG_SHADER_OP_LOADI: {
                ix_u32 bar = ixg_bin_alloc_barrier(ctx);
                lo = ixg_sass_enc_volta_ldg(dst, src0);
                hi = ixg_sass_enc_volta_ctrl(1u, 0u, bar, 7u, wmask, 0u);
                ctx->mem_op_count++;
                ixg_bin_mark_wbar(ctx, dst, bar);
            } break;
            default:
                lo = ixg_sass_enc_volta_simple(IXG_V_OP_NOP);
                hi = ixg_sass_enc_volta_ctrl(1u, 0u, 7u, 7u, wmask, 0u);
                break;
        }
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    }
}

static void ixg_bin_emit_turing_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_volta_inst(ctx, inst, mask);
}
static void ixg_bin_emit_ampere_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_volta_inst(ctx, inst, mask);
}
static void ixg_bin_emit_ada_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_volta_inst(ctx, inst, mask);
}
static void ixg_bin_emit_hopper_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_volta_inst(ctx, inst, mask);
}
static void ixg_bin_emit_blackwell_inst(ixg_sass_binary_ctx_t* ctx, const ixg_shader_instruction_t* inst, ix_u8 mask) {
    ixg_bin_emit_volta_inst(ctx, inst, mask);
}

static void ixg_bin_emit_prologue(ixg_sass_binary_ctx_t* ctx) {
    if (ctx->arch >= IXG_SASS_ARCH_VOLTA) {
        ix_u64 lo = ixg_sass_enc_volta_ldc(1u, 0x28u);
        ix_u64 hi = ixg_sass_enc_volta_ctrl(6u, 0u, 7u, 7u, 0u, 0u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    } else if (ctx->arch >= IXG_SASS_ARCH_KEPLER) {
        ix_u64 lo = ixg_sass_enc_kepler_falu(IXG_K_OP_MOV, 1u, 255u, 255u);
        ix_u64 hi = ixg_sass_enc_kepler_ctrl(6u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    } else if (ctx->arch >= IXG_SASS_ARCH_FERMI) {
        ixg_bin_emit_u64(ctx, 0x4000000100000000ull);
    } else {
        ixg_bin_emit_u32(ctx, 0x10000100u);
    }
}

static void ixg_bin_emit_epilogue(ixg_sass_binary_ctx_t* ctx) {
    if (ctx->arch >= IXG_SASS_ARCH_VOLTA) {
        ix_u64 lo = ixg_sass_enc_volta_simple(IXG_V_OP_EXIT);
        ix_u64 hi = ixg_sass_enc_volta_ctrl(1u, 0u, 7u, 7u, 0u, 0u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
        lo = ixg_sass_enc_volta_simple(IXG_V_OP_BRA);
        hi = ixg_sass_enc_volta_ctrl(15u, 0u, 7u, 7u, 0u, 0u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    } else if (ctx->arch >= IXG_SASS_ARCH_KEPLER) {
        ix_u64 lo = ixg_sass_enc_kepler_simple(IXG_K_OP_EXIT);
        ix_u64 hi = ixg_sass_enc_kepler_ctrl(1u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
        lo = ixg_sass_enc_kepler_simple(IXG_K_OP_BRA);
        hi = ixg_sass_enc_kepler_ctrl(15u);
        ixg_bin_emit_u64(ctx, lo);
        ixg_bin_emit_u64(ctx, hi);
    } else if (ctx->arch >= IXG_SASS_ARCH_FERMI) {
        ixg_bin_emit_u64(ctx, 0x9000000000000000ull);
        ixg_bin_emit_u64(ctx, 0x8000000000000000ull);
    } else {
        ixg_bin_emit_u32(ctx, 0x90000000u);
        ixg_bin_emit_u32(ctx, 0x80000000u);
    }
}

static int ixg_sass_emit_binary_core(const ixg_shader_program_t* program,
                                     ix_u8 shader_type, ix_u32 sm_version,
                                     void* out_buf, ix_u32 buf_cap,
                                     ix_u32* out_size) {
    ixg_sass_binary_ctx_t ctx;
    ix_u32 i;
    if (!program || !out_buf || !out_size || buf_cap == 0u) return -1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = (ix_u8*)out_buf;
    ctx.cap = buf_cap;
    ctx.len = 0u;
    ctx.arch = ixg_sass_arch_from_sm(sm_version);
    ctx.fmt = ixg_sass_fmt_from_arch(ctx.arch);
    ctx.reg_count = 0u;
    ctx.error = 0u;
    (void)shader_type;
    ixg_bin_emit_prologue(&ctx);
    if (ctx.error) return -2;
    for (i = 0u; i < program->instruction_count; i++) {
        const ixg_shader_instruction_t* inst = &program->instructions[i];
        ix_u8 mask;
        if (inst->opcode == IXG_SHADER_OP_END) break;
        if (inst->opcode == IXG_SHADER_OP_EMIT_DRAW ||
            inst->opcode == IXG_SHADER_OP_EMIT_DISPATCH ||
            inst->opcode == IXG_SHADER_OP_EMIT_STATE) continue;
        mask = inst->write_mask ? inst->write_mask : (ix_u8)IXG_SHADER_MASK_XYZW;
        switch (ctx.arch) {
            case IXG_SASS_ARCH_TESLA: ixg_bin_emit_tesla_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_FERMI: ixg_bin_emit_fermi_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_KEPLER: ixg_bin_emit_kepler_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_MAXWELL: ixg_bin_emit_maxwell_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_PASCAL: ixg_bin_emit_pascal_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_VOLTA: ixg_bin_emit_volta_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_TURING: ixg_bin_emit_turing_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_AMPERE: ixg_bin_emit_ampere_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_ADA: ixg_bin_emit_ada_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_HOPPER: ixg_bin_emit_hopper_inst(&ctx, inst, mask); break;
            case IXG_SASS_ARCH_BLACKWELL: ixg_bin_emit_blackwell_inst(&ctx, inst, mask); break;
            default: return -3;
        }
        if (ctx.error) return -4;
    }
    ixg_bin_emit_epilogue(&ctx);
    if (ctx.error) return -5;
    *out_size = ctx.len;
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

int ixg_sass_emit_program(const ixg_shader_program_t* program, ix_u8 shader_type,
                          ix_u32 sm_version, void* out_buf, ix_u32 buf_cap,
                          ix_u32* out_total_size, ix_u32* out_code_offset) {
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
    rc = ixg_sass_emit_binary_core(program, shader_type, sm_version,
                                   (ix_u8*)out_buf + sph_size,
                                   buf_cap - sph_size, &code_size);
    if (rc != 0) return rc;
    *out_total_size = sph_size + code_size;
    if (out_code_offset) *out_code_offset = sph_size;
    return 0;
}

int ixg_sass_emit_binary(const ixg_shader_program_t* program, ix_u8 shader_type,
                         ix_u32 sm_version, void* out_buf, ix_u32 buf_cap, ix_u32* out_size) {
    return ixg_sass_emit_binary_core(program, shader_type, sm_version, out_buf, buf_cap, out_size);
}
