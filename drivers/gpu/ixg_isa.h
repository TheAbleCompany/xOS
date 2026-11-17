#ifndef IXG_ISA_H
#define IXG_ISA_H

#include "ixg_types.h"
#include "ixg_shader_vm.h"

#define IXG_ISA_TEXT_CAP 16384u
#define IXG_ISA_SCALAR_PER_VEC 4u
#define IXG_ISA_REG_OUT_BASE 128u
#define IXG_ISA_REG_SCRATCH0 200u
#define IXG_ISA_REG_SCRATCH1 201u

typedef enum {
  IXG_ISA_VENDOR_AMD   = 1,
  IXG_ISA_VENDOR_INTEL = 2,
  IXG_ISA_VENDOR_SPIRV = 3
} ixg_isa_vendor_t;

typedef enum {
  IXG_ISA_AMD_GCN    = 0x0F00,
  IXG_ISA_AMD_RDNA1  = 0x0F10,
  IXG_ISA_AMD_RDNA2  = 0x0F20,
  IXG_ISA_AMD_RDNA3  = 0x0F30,
  IXG_ISA_INTEL_GEN9 = 0x0900,
  IXG_ISA_INTEL_GEN12= 0x0C00,
  IXG_ISA_INTEL_XEHPG= 0x0D00,
  IXG_ISA_INTEL_XE2  = 0x0D20
} ixg_isa_target_t;

typedef struct {
  char     text[IXG_ISA_TEXT_CAP];
  ix_u32   length;
  ix_u32   instr_count;
  ix_u32   reg_high_water;
  ix_u32   barrier_count;
  ix_u32   truncated;
  ix_u32   vendor;
  ix_u32   target;
  ix_u32   shader_type;
} ixg_isa_module_t;

const char* ixg_isa_vendor_name(ix_u32 vendor);
const char* ixg_isa_target_name(ix_u32 vendor, ix_u32 target);
ix_u32      ixg_isa_vendor_from_pci(ix_u16 pci_vendor);
ix_u32      ixg_isa_target_from_device(ix_u32 vendor, ix_u16 device);

int ixg_isa_emit_amd(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module);
int ixg_isa_emit_intel(const ixg_shader_program_t* program, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module);
int ixg_isa_emit(const ixg_shader_program_t* program, ix_u32 vendor, ix_u8 shader_type, ix_u32 target, ixg_isa_module_t* out_module);

#endif