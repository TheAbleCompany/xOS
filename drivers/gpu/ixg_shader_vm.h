#ifndef IXG_SHADER_VM_H
#define IXG_SHADER_VM_H

#include "ixg_types.h"

#define IXG_SHADER_REG_TEMP_BASE   0u
#define IXG_SHADER_REG_INPUT_BASE  32u
#define IXG_SHADER_REG_CONST_BASE  64u
#define IXG_SHADER_REG_OUTPUT_BASE 128u

#define IXG_SHADER_MASK_X  1u
#define IXG_SHADER_MASK_Y  2u
#define IXG_SHADER_MASK_Z  4u
#define IXG_SHADER_MASK_W  8u
#define IXG_SHADER_MASK_XYZW 15u
#define IXG_SHADER_WRITE_ALL 15u

#define IXG_SHADER_FLAG_SATURATE (1u << 0)

#define IXG_SHADER_MAX_INSTRUCTIONS 1024u
#define IXG_SHADER_MAX_CONSTANTS 256u

#define IXG_SHADER_STAGE_VERTEX   (1u << 0)
#define IXG_SHADER_STAGE_PIXEL    (1u << 1)
#define IXG_SHADER_STAGE_COMPUTE  (1u << 2)
#define IXG_SHADER_STAGE_CONTROL (1u << 3)
#define IXG_SHADER_STAGE_3D_VERTEX   (1u << 0)
#define IXG_SHADER_STAGE_3D_PIXEL    (1u << 1)
#define IXG_SHADER_STAGE_3D_COMPUTE  (1u << 2)
#define IXG_SHADER_STAGE_3D_CONTROL  (1u << 3)

typedef enum {
  IXG_SHADER_OP_NOP = 0,
  IXG_SHADER_OP_MOV,
  IXG_SHADER_OP_ADD,
  IXG_SHADER_OP_SUB,
  IXG_SHADER_OP_MUL,
  IXG_SHADER_OP_DIV,
  IXG_SHADER_OP_MIN,
  IXG_SHADER_OP_MAX,
  IXG_SHADER_OP_MAD,
  IXG_SHADER_OP_DP3,
  IXG_SHADER_OP_DP4,
  IXG_SHADER_OP_RCP,
  IXG_SHADER_OP_RSQ,
  IXG_SHADER_OP_TEX2D,
  IXG_SHADER_OP_KILL,
  IXG_SHADER_OP_END,
  IXG_SHADER_OP_LERP,
  IXG_SHADER_OP_NORMALIZE,
  IXG_SHADER_OP_LOADI,
  IXG_SHADER_OP_EMIT_DRAW,
  IXG_SHADER_OP_EMIT_DISPATCH,
  IXG_SHADER_OP_EMIT_STATE
} ixg_shader_opcode_t;

typedef struct {
  ix_u8 op;
  ix_u8 dst;
  ix_u8 src0;
  ix_u8 src1;
  ix_u8 src2;
  ix_u8 write_mask;
  ix_u8 flags;
  ix_u8 reserved0;
  float imm;
} ixg_shader_inst_t;

typedef struct {
  ix_u8  opcode;
  ix_u8  dst;
  ix_u8  src[3];
  ix_u8  write_mask;
  ix_u8  flags;
  ix_u8  reserved0;
  float  imm;
} ixg_shader_instruction_t;

typedef struct {
  ix_u32 stage_mask;
  ix_u32 flags;
  ix_u32 instruction_count;
  ix_u32 profile;
  ixg_shader_instruction_t instructions[IXG_SHADER_MAX_INSTRUCTIONS];
  ix_u32 bytecode_size;
  const float* constants;
  ix_u32 constant_count;
  const void* bytecode;
} ixg_shader_program_t;

#define IXG_SHADER_VM_MAX_TEMP 32u
#define IXG_SHADER_VM_MAX_IO 16u
#define IXG_SHADER_VM_MAX_CONST 64u

typedef struct ixg_shader_vm_ctx_t ixg_shader_vm_ctx_t;

typedef int (*ixg_shader_vm_sample2d_fn)(void* user, float u, float v, float out_rgba[4]);
typedef int (*ixg_shader_vm_emit_fn)(void* user, ix_u32 opcode, const float payload[4]);

struct ixg_shader_vm_ctx_t {
  float input[IXG_SHADER_VM_MAX_IO][4];
  float output[IXG_SHADER_VM_MAX_IO][4];
  float temp[IXG_SHADER_VM_MAX_TEMP][4];
  float constant[IXG_SHADER_VM_MAX_CONST][4];
  ixg_shader_vm_sample2d_fn sample2d;
  ixg_shader_vm_emit_fn emit;
  void* user;
  ix_u32 flags;
};

#define IXG_SHADER_VM_FLAG_KILLED (1u << 0)
#define IXG_SHADER_VM_TUNE_JIT (1u << 0)
#define IXG_SHADER_VM_TUNE_AGGRESSIVE (1u << 1)

void ixg_shader_vm_ctx_init(ixg_shader_vm_ctx_t* ctx);
int  ixg_shader_vm_run(const ixg_shader_program_t* program, ixg_shader_vm_ctx_t* ctx);
void ixg_shader_vm_tune_set(ix_u32 flags);
ix_u32 ixg_shader_vm_tune_get(void);

#endif