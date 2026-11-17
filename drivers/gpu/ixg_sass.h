#ifndef IXG_SASS_H
#define IXG_SASS_H

#include "ixg_types.h"
#include "ixg_shader_vm.h"

#define IXG_SASS_TEXT_CAP 16384u
#define IXG_SASS_MAX_TEMP_REGS 64u
#define IXG_SASS_SCALAR_PER_VEC 4u

typedef enum {
  IXG_SASS_ARCH_TESLA   = 0x10,
  IXG_SASS_ARCH_FERMI   = 0x20,
  IXG_SASS_ARCH_KEPLER  = 0x30,
  IXG_SASS_ARCH_MAXWELL = 0x50,
  IXG_SASS_ARCH_PASCAL  = 0x60,
  IXG_SASS_ARCH_VOLTA   = 0x70,
  IXG_SASS_ARCH_TURING  = 0x75,
  IXG_SASS_ARCH_AMPERE  = 0x86,
  IXG_SASS_ARCH_ADA     = 0x89,
  IXG_SASS_ARCH_HOPPER  = 0x90,
  IXG_SASS_ARCH_BLACKWELL = 0xC0
} ixg_sass_arch_t;

typedef struct {
  char     text[IXG_SASS_TEXT_CAP];
  ix_u32   length;
  ix_u32   instr_count;
  ix_u32   reg_high_water;
  ix_u32   barrier_count;
  ix_u32   truncated;
  ix_u32   arch;
  ix_u32   shader_type;
  ix_u32   sm_version;
} ixg_sass_module_t;

const char* ixg_sass_arch_name(ix_u32 arch);
ix_u32      ixg_sass_arch_from_sm(ix_u32 sm_version);

int ixg_sass_emit(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, ixg_sass_module_t* out_module);
int ixg_sass_emit_to_binary(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_binary);

int ixg_sass_emit_binary(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_buf, ix_u32 buf_cap, ix_u32* out_size);
ix_u32 ixg_sass_sph_size_for_arch(ix_u32 arch);
int ixg_sass_emit_sph(ix_u32 arch, ix_u8 is_pixel, void* out_buf, ix_u32 buf_cap, ix_u32* out_size);
int ixg_sass_emit_program(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 sm_version, void* out_buf, ix_u32 buf_cap, ix_u32* out_total_size, ix_u32* out_code_offset);

#endif