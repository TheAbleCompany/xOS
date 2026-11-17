#include "ixg_shader_vm.h"
#include <string.h>

#define IXG_SHADER_VM_JIT_CACHE_CAP 32u
#define IXG_SHADER_VM_JIT_MAX_INST 512u
#define IXG_SHADER_VM_RUNTIME_SAMPLE2D (1u << 0)
#define IXG_SHADER_VM_RUNTIME_EMIT (1u << 1)
#define IXG_SHADER_VM_TRACK_REG_COUNT (IXG_SHADER_REG_OUTPUT_BASE + IXG_SHADER_VM_MAX_IO)

typedef struct {
  ix_u8 valid;
  ix_u8 runtime_mask;
  ix_u16 reserved1;
  const ixg_shader_inst_t* bytecode;
  ix_u32 bytecode_size;
  ix_u32 stage_mask;
  ix_u32 profile;
  ix_u32 tune_flags;
  ix_u32 last_used;
  ix_u32 inst_count;
  ixg_shader_inst_t inst[IXG_SHADER_VM_JIT_MAX_INST];
} ixg_shader_vm_jit_entry_t;

static const float g_ixg_shader_zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static ixg_shader_vm_jit_entry_t g_ixg_shader_vm_jit_cache[IXG_SHADER_VM_JIT_CACHE_CAP];
static ix_u32 g_ixg_shader_vm_jit_clock = 1u;
static ix_u32 g_ixg_shader_vm_tune_flags = IXG_SHADER_VM_TUNE_JIT | IXG_SHADER_VM_TUNE_AGGRESSIVE;

static float* ixg_shader_vm_reg_dst(ixg_shader_vm_ctx_t* ctx, ix_u8 reg) {
  if (!ctx) return 0;
  if (reg < IXG_SHADER_REG_INPUT_BASE) {
    if (reg < IXG_SHADER_VM_MAX_TEMP) return ctx->temp[reg];
    return 0;
  }
  if (reg >= IXG_SHADER_REG_INPUT_BASE && reg < IXG_SHADER_REG_CONST_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_INPUT_BASE);
    if (idx < IXG_SHADER_VM_MAX_IO) return ctx->input[idx];
    return 0;
  }
  if (reg >= IXG_SHADER_REG_CONST_BASE && reg < IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_CONST_BASE);
    if (idx < IXG_SHADER_VM_MAX_CONST) return ctx->constant[idx];
    return 0;
  }
  {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    if (idx < IXG_SHADER_VM_MAX_IO) return ctx->output[idx];
  }
  return 0;
}

static const float* ixg_shader_vm_reg_src(const ixg_shader_vm_ctx_t* ctx, ix_u8 reg) {
  if (!ctx) return g_ixg_shader_zero4;
  if (reg < IXG_SHADER_REG_INPUT_BASE) {
    if (reg < IXG_SHADER_VM_MAX_TEMP) return ctx->temp[reg];
    return g_ixg_shader_zero4;
  }
  if (reg >= IXG_SHADER_REG_INPUT_BASE && reg < IXG_SHADER_REG_CONST_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_INPUT_BASE);
    if (idx < IXG_SHADER_VM_MAX_IO) return ctx->input[idx];
    return g_ixg_shader_zero4;
  }
  if (reg >= IXG_SHADER_REG_CONST_BASE && reg < IXG_SHADER_REG_OUTPUT_BASE) {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_CONST_BASE);
    if (idx < IXG_SHADER_VM_MAX_CONST) return ctx->constant[idx];
    return g_ixg_shader_zero4;
  }
  {
    ix_u8 idx = (ix_u8)(reg - IXG_SHADER_REG_OUTPUT_BASE);
    if (idx < IXG_SHADER_VM_MAX_IO) return ctx->output[idx];
  }
  return g_ixg_shader_zero4;
}

static float ixg_shader_vm_clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline __attribute__((always_inline)) float ixg_shader_vm_rsqrt(float x) {
  union { float f; ix_u32 u; } conv;
  float x2;
  if (x <= 0.0f) return 0.0f;
  x2 = x * 0.5f;
  conv.f = x;
  conv.u = 0x5f3759dfu - (conv.u >> 1);
  conv.f = conv.f * (1.5f - x2 * conv.f * conv.f);
  return conv.f;
}

static void ixg_shader_vm_write(float* dst, ix_u8 mask, const float v[4], ix_u8 saturate) {
  ix_u8 use_mask = mask ? mask : (ix_u8)IXG_SHADER_WRITE_ALL;
  if (!dst || !v) return;
  for (ix_u8 i = 0; i < 4; i++) {
    if ((use_mask & (1u << i)) == 0u) continue;
    dst[i] = saturate ? ixg_shader_vm_clampf(v[i], 0.0f, 1.0f) : v[i];
  }
}

static int ixg_shader_vm_identity_mov(const ixg_shader_inst_t* inst) {
  ix_u8 use_mask;
  if (!inst || inst->op != IXG_SHADER_OP_MOV) return 0;
  use_mask = inst->write_mask ? inst->write_mask : (ix_u8)IXG_SHADER_WRITE_ALL;
  if (inst->dst != inst->src0) return 0;
  if (use_mask != (ix_u8)IXG_SHADER_WRITE_ALL) return 0;
  if ((inst->flags & IXG_SHADER_FLAG_SATURATE) != 0u) return 0;
  return 1;
}

static ix_u8 ixg_shader_vm_is_temp_reg(ix_u8 reg) {
  if (reg >= IXG_SHADER_REG_INPUT_BASE) return 0u;
  return (reg < IXG_SHADER_VM_MAX_TEMP) ? 1u : 0u;
}

static ix_u8 ixg_shader_vm_is_track_reg(ix_u8 reg) {
  return (reg < IXG_SHADER_VM_TRACK_REG_COUNT) ? 1u : 0u;
}

static ix_u8 ixg_shader_vm_inst_write_mask(const ixg_shader_inst_t* inst) {
  if (!inst) return 0u;
  return inst->write_mask ? inst->write_mask : (ix_u8)IXG_SHADER_WRITE_ALL;
}

static void ixg_shader_vm_mark_live(ix_u8 live[IXG_SHADER_VM_TRACK_REG_COUNT], ix_u8 reg) {
  if (!live || !ixg_shader_vm_is_track_reg(reg)) return;
  live[reg] |= (ix_u8)IXG_SHADER_WRITE_ALL;
}

static ix_u8 ixg_shader_vm_inst_has_side_effect(const ixg_shader_inst_t* inst, ix_u8 aggressive, ix_u8 has_emit) {
  if (!inst) return 1u;
  switch ((ixg_shader_opcode_t)inst->op) {
    case IXG_SHADER_OP_NOP: return 0u;
    case IXG_SHADER_OP_KILL:
    case IXG_SHADER_OP_END: return 1u;
    case IXG_SHADER_OP_EMIT_DRAW:
    case IXG_SHADER_OP_EMIT_DISPATCH:
    case IXG_SHADER_OP_EMIT_STATE:
      if (has_emit) return 1u;
      return aggressive ? 0u : 1u;
    default: return 0u;
  }
}

static ix_u32 ixg_shader_vm_jit_compact(const ixg_shader_program_t* program,
                                         ix_u32 tune_flags, ix_u32 runtime_mask,
                                         ixg_shader_inst_t* out_inst, ix_u32 out_cap) {
  ix_u32 src_count;
  ix_u32 write = 0u;
  ix_u8 aggressive = (tune_flags & IXG_SHADER_VM_TUNE_AGGRESSIVE) ? 1u : 0u;
  ix_u8 has_emit = (runtime_mask & IXG_SHADER_VM_RUNTIME_EMIT) ? 1u : 0u;
  ix_u8 keep[IXG_SHADER_VM_JIT_MAX_INST];
  ix_u8 live[IXG_SHADER_VM_TRACK_REG_COUNT];
  if (!program || !out_inst || out_cap == 0u) return 0u;
  if (out_cap > IXG_SHADER_VM_JIT_MAX_INST) out_cap = IXG_SHADER_VM_JIT_MAX_INST;
  src_count = program->bytecode_size / (ix_u32)sizeof(ixg_shader_inst_t);
  for (ix_u32 i = 0u; i < src_count && write < out_cap; i++) {
    ixg_shader_inst_t inst;
    memcpy(&inst, &program->instructions[i], sizeof(inst));
    if (inst.op == IXG_SHADER_OP_NOP) continue;
    if (aggressive && ixg_shader_vm_identity_mov(&inst)) continue;
    if (aggressive && !has_emit &&
        (inst.op == IXG_SHADER_OP_EMIT_DRAW || inst.op == IXG_SHADER_OP_EMIT_DISPATCH || inst.op == IXG_SHADER_OP_EMIT_STATE))
      continue;
    out_inst[write++] = inst;
    keep[write - 1u] = 1u;
    if (inst.op == IXG_SHADER_OP_END) break;
  }
  if (!aggressive || write == 0u) return write;
  memset(live, 0, sizeof(live));
  for (ix_u32 i = write; i > 0u; i--) {
    ix_u32 idx = i - 1u;
    ixg_shader_inst_t* inst = &out_inst[idx];
    ix_u8 wmask = ixg_shader_vm_inst_write_mask(inst);
    ix_u8 side_effect = ixg_shader_vm_inst_has_side_effect(inst, aggressive, has_emit);
    if (!side_effect && ixg_shader_vm_is_temp_reg(inst->dst)) {
      ix_u8 live_mask = ixg_shader_vm_is_track_reg(inst->dst) ? live[inst->dst] : 0u;
      if ((live_mask & wmask) == 0u) { keep[idx] = 0u; continue; }
    }
    if (ixg_shader_vm_is_track_reg(inst->dst))
      live[inst->dst] = (ix_u8)(live[inst->dst] & (ix_u8)(~wmask));
    switch ((ixg_shader_opcode_t)inst->op) {
      case IXG_SHADER_OP_NOP:
      case IXG_SHADER_OP_END: break;
      case IXG_SHADER_OP_MOV:
      case IXG_SHADER_OP_RSQ:
      case IXG_SHADER_OP_RCP:
      case IXG_SHADER_OP_KILL:
        ixg_shader_vm_mark_live(live, inst->src0); break;
      case IXG_SHADER_OP_TEX2D:
        ixg_shader_vm_mark_live(live, inst->src0);
        ixg_shader_vm_mark_live(live, inst->src1);
        ixg_shader_vm_mark_live(live, inst->src2); break;
      default:
        ixg_shader_vm_mark_live(live, inst->src0);
        ixg_shader_vm_mark_live(live, inst->src1);
        ixg_shader_vm_mark_live(live, inst->src2); break;
    }
  }
  {
    ix_u32 compact = 0u;
    for (ix_u32 i = 0u; i < write; i++) {
      if (!keep[i]) continue;
      if (compact != i) out_inst[compact] = out_inst[i];
      compact++;
    }
    write = compact;
  }
  return write;
}

static const ixg_shader_inst_t* ixg_shader_vm_jit_get(const ixg_shader_program_t* program,
                                                        ix_u32 tune_flags, ix_u32 runtime_mask,
                                                        ix_u32* out_count) {
  ix_u32 i;
  ix_u32 victim = 0u;
  ix_u32 victim_age = 0xFFFFFFFFu;
  if (!out_count) return 0;
  *out_count = 0u;
  if (!program || program->bytecode_size == 0u || (tune_flags & IXG_SHADER_VM_TUNE_JIT) == 0u)
    return 0;
  for (i = 0u; i < IXG_SHADER_VM_JIT_CACHE_CAP; i++) {
    ixg_shader_vm_jit_entry_t* e = &g_ixg_shader_vm_jit_cache[i];
    if (e->valid && e->bytecode_size == program->bytecode_size &&
        e->stage_mask == program->stage_mask && e->profile == program->profile &&
        e->runtime_mask == (ix_u8)runtime_mask && e->tune_flags == tune_flags) {
      e->last_used = g_ixg_shader_vm_jit_clock++;
      *out_count = e->inst_count;
      return e->inst;
    }
  }
  for (i = 0u; i < IXG_SHADER_VM_JIT_CACHE_CAP; i++) {
    ixg_shader_vm_jit_entry_t* e = &g_ixg_shader_vm_jit_cache[i];
    if (!e->valid) { victim = i; victim_age = 0u; break; }
    if (e->last_used < victim_age) { victim_age = e->last_used; victim = i; }
  }
  {
    ixg_shader_vm_jit_entry_t* e = &g_ixg_shader_vm_jit_cache[victim];
    memset(e, 0, sizeof(*e));
    e->inst_count = ixg_shader_vm_jit_compact(program, tune_flags, runtime_mask, e->inst, IXG_SHADER_VM_JIT_MAX_INST);
    if (e->inst_count == 0u) return 0;
    e->valid = 1u;
    e->bytecode_size = program->bytecode_size;
    e->stage_mask = program->stage_mask;
    e->profile = program->profile;
    e->runtime_mask = (ix_u8)runtime_mask;
    e->tune_flags = tune_flags;
    e->last_used = g_ixg_shader_vm_jit_clock++;
    *out_count = e->inst_count;
    return e->inst;
  }
}

void ixg_shader_vm_ctx_init(ixg_shader_vm_ctx_t* ctx) {
  if (!ctx) return;
  memset(ctx, 0, sizeof(*ctx));
}

void ixg_shader_vm_tune_set(ix_u32 flags) {
  ix_u32 valid = flags & (IXG_SHADER_VM_TUNE_JIT | IXG_SHADER_VM_TUNE_AGGRESSIVE);
  g_ixg_shader_vm_tune_flags = valid;
  memset(g_ixg_shader_vm_jit_cache, 0, sizeof(g_ixg_shader_vm_jit_cache));
  g_ixg_shader_vm_jit_clock = 1u;
}

ix_u32 ixg_shader_vm_tune_get(void) { return g_ixg_shader_vm_tune_flags; }

int ixg_shader_vm_run(const ixg_shader_program_t* program, ixg_shader_vm_ctx_t* ctx) {
  ix_u32 inst_count;
  ix_u32 runtime_mask = 0u;
  const ixg_shader_inst_t* jit_inst = 0;
  ix_u8 has_sample, has_emit, aggressive;
  if (!program || !ctx || program->bytecode_size == 0u) return -1;
  if ((program->bytecode_size % (ix_u32)sizeof(ixg_shader_inst_t)) != 0u) return -2;
  has_sample = ctx->sample2d ? 1u : 0u;
  has_emit = ctx->emit ? 1u : 0u;
  if (has_sample) runtime_mask |= IXG_SHADER_VM_RUNTIME_SAMPLE2D;
  if (has_emit) runtime_mask |= IXG_SHADER_VM_RUNTIME_EMIT;
  aggressive = (g_ixg_shader_vm_tune_flags & IXG_SHADER_VM_TUNE_AGGRESSIVE) ? 1u : 0u;
  jit_inst = ixg_shader_vm_jit_get(program, g_ixg_shader_vm_tune_flags, runtime_mask, &inst_count);
  if (!jit_inst) inst_count = program->bytecode_size / (ix_u32)sizeof(ixg_shader_inst_t);
  if (inst_count > 4096u) return -4;
  if (has_emit) return -5;
  for (ix_u32 ip = 0; ip < inst_count; ip++) {
    ixg_shader_inst_t inst;
    const float *s0, *s1, *s2;
    float out[4] = {0,0,0,0};
    float* dst;
    ix_u8 sat;
    if (jit_inst) inst = jit_inst[ip];
    else memcpy(&inst, &program->instructions[ip], sizeof(inst));
    s0 = ixg_shader_vm_reg_src(ctx, inst.src0);
    s1 = ixg_shader_vm_reg_src(ctx, inst.src1);
    s2 = ixg_shader_vm_reg_src(ctx, inst.src2);
    dst = ixg_shader_vm_reg_dst(ctx, inst.dst);
    sat = (inst.flags & IXG_SHADER_FLAG_SATURATE) ? 1u : 0u;
    if (aggressive && !has_emit &&
        (inst.op == IXG_SHADER_OP_EMIT_DRAW || inst.op == IXG_SHADER_OP_EMIT_DISPATCH || inst.op == IXG_SHADER_OP_EMIT_STATE))
      continue;
    switch ((ixg_shader_opcode_t)inst.op) {
      case IXG_SHADER_OP_NOP: break;
      case IXG_SHADER_OP_MOV:
        out[0]=s0[0]; out[1]=s0[1]; out[2]=s0[2]; out[3]=s0[3];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_ADD:
        for (ix_u8 i=0;i<4;i++) out[i]=s0[i]+s1[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_SUB:
        for (ix_u8 i=0;i<4;i++) out[i]=s0[i]-s1[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_MUL:
        for (ix_u8 i=0;i<4;i++) out[i]=s0[i]*s1[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_MAD:
        for (ix_u8 i=0;i<4;i++) out[i]=(s0[i]*s1[i])+s2[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_MIN:
        for (ix_u8 i=0;i<4;i++) out[i]=(s0[i]<s1[i])?s0[i]:s1[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_MAX:
        for (ix_u8 i=0;i<4;i++) out[i]=(s0[i]>s1[i])?s0[i]:s1[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_DP3: {
        float d=(s0[0]*s1[0])+(s0[1]*s1[1])+(s0[2]*s1[2]);
        out[0]=d; out[1]=d; out[2]=d; out[3]=d;
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_DP4: {
        float d=(s0[0]*s1[0])+(s0[1]*s1[1])+(s0[2]*s1[2])+(s0[3]*s1[3]);
        out[0]=d; out[1]=d; out[2]=d; out[3]=d;
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_RSQ: {
        float v=ixg_shader_vm_rsqrt(s0[0]);
        out[0]=v; out[1]=v; out[2]=v; out[3]=v;
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_RCP: {
        float v=(s0[0]!=0.0f)?(1.0f/s0[0]):0.0f;
        out[0]=v; out[1]=v; out[2]=v; out[3]=v;
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_TEX2D: {
        if (has_sample && ctx->sample2d(ctx->user, s0[0], s0[1], out) == 0)
          ixg_shader_vm_write(dst, inst.write_mask, out, sat);
        else { out[0]=s0[0]; out[1]=s0[1]; out[2]=s0[2]; out[3]=1.0f; ixg_shader_vm_write(dst, inst.write_mask, out, sat); }
        break;
      }
      case IXG_SHADER_OP_KILL: {
        float threshold = inst.imm;
        if (s0[0] < threshold) { ctx->flags |= IXG_SHADER_VM_FLAG_KILLED; return 1; }
        break;
      }
      case IXG_SHADER_OP_EMIT_DRAW:
      case IXG_SHADER_OP_EMIT_DISPATCH:
      case IXG_SHADER_OP_EMIT_STATE:
        return -6;
      case IXG_SHADER_OP_LERP:
        for (ix_u8 i=0;i<4;i++) out[i]=s0[i]+(s1[i]-s0[i])*s2[i];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      case IXG_SHADER_OP_NORMALIZE: {
        float len_sq=s0[0]*s0[0]+s0[1]*s0[1]+s0[2]*s0[2];
        float inv=(len_sq>0.000001f)?ixg_shader_vm_rsqrt(len_sq):1.0f;
        out[0]=s0[0]*inv; out[1]=s0[1]*inv; out[2]=s0[2]*inv; out[3]=s0[3];
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_LOADI: {
        float imm; memcpy(&imm, &inst.imm, sizeof(float));
        out[0]=imm; out[1]=imm; out[2]=imm; out[3]=imm;
        ixg_shader_vm_write(dst, inst.write_mask, out, sat); break;
      }
      case IXG_SHADER_OP_END: return 0;
      default: return -3;
    }
  }
  return 0;
}