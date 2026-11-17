#ifndef IXG_SHADER_COMPILER_H
#define IXG_SHADER_COMPILER_H
#include "ixg_shader_vm.h"
#include "ixg_driver_internal.h"

#define IXG_SC_MAX_SHADER_BINARY_BYTES 16384u
#define IXG_SC_MAX_SHADER_WARPS 128u
#define IXG_SC_SHADER_TYPE_VS 1u
#define IXG_SC_SHADER_TYPE_PS 2u
#define IXG_SC_SHADER_TYPE_CS 4u

#define IXG_SC_BINARY_MAGIC 0x58475342u
#define IXG_SC_BINARY_VERSION 1u

typedef enum {
  IXG_SC_BACKEND_NVIDIA     = 1,
  IXG_SC_BACKEND_AMD        = 2,
  IXG_SC_BACKEND_INTEL      = 3,
  IXG_SC_BACKEND_SPIRV      = 4,
  IXG_SC_BACKEND_RDNA       = 5,
  IXG_SC_BACKEND_TURING     = 6,
  IXG_SC_BACKEND_XE_HPG     = 7,
  IXG_SC_BACKEND_EXOTIC     = 8
} ixg_sc_backend_t;

typedef enum {
  IXG_SC_TARGET_SM30  = 0x0030u,
  IXG_SC_TARGET_SM50  = 0x0050u,
  IXG_SC_TARGET_SM60  = 0x0060u,
  IXG_SC_TARGET_SM75  = 0x0075u,
  IXG_SC_TARGET_SM86  = 0x0086u,
  IXG_SC_TARGET_SM89  = 0x0089u,
  IXG_SC_TARGET_GCN   = 0x0F00u,
  IXG_SC_TARGET_RDNA1 = 0x0F10u,
  IXG_SC_TARGET_RDNA2 = 0x0F20u,
  IXG_SC_TARGET_RDNA3 = 0x0F30u,
  IXG_SC_TARGET_GEN9  = 0x0900u,
  IXG_SC_TARGET_GEN12 = 0x0C00u,
  IXG_SC_TARGET_XEHPG = 0x0D00u,
  IXG_SC_TARGET_XE2   = 0x0D20u,
  IXG_SC_TARGET_SPIRV = 0xE000u,
  IXG_SC_TARGET_PTX   = 0xE001u,
  IXG_SC_TARGET_DXIL  = 0xE002u
} ixg_sc_target_t;

typedef struct {
  ix_u32 magic;
  ix_u16 version;
  ix_u8  type;
  ix_u8  backend;
  ix_u16 reserved;
  ix_u32 target;
  ix_u32 binary_size;
  ix_u32 checksum;
  ix_u8  binary[IXG_SC_MAX_SHADER_BINARY_BYTES];
} ixg_shader_binary_t;

int ixg_shader_compile(const ixg_shader_program_t *program,
                       ixg_sc_backend_t backend,
                       ix_u8 shader_type,
                       ix_u32 sm_version,
                       ixg_shader_binary_t *out_binary);

int ixg_shader_compile_vm_to_sm30(const ixg_shader_program_t *program,
                                    ix_u8 shader_type, ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_gcn(const ixg_shader_program_t *program,
                                   ix_u8 shader_type, ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_gen9(const ixg_shader_program_t *program,
                                    ix_u8 shader_type, ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_rdna(const ixg_shader_program_t *program,
                                    ix_u8 shader_type, ix_u32 generation,
                                    ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_turing(const ixg_shader_program_t *program,
                                      ix_u8 shader_type, ix_u32 sm_version,
                                      ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_xe_hpg(const ixg_shader_program_t *program,
                                      ix_u8 shader_type, ix_u32 gen,
                                      ixg_shader_binary_t *out_binary);
int ixg_shader_compile_vm_to_spirv(const ixg_shader_program_t *program,
                                     ix_u8 shader_type,
                                     ixg_shader_binary_t *out_binary);

int ixg_shader_select_target(ix_u16 vendor_id, ix_u16 device_id);
int ixg_shader_validate_binary(const ixg_shader_binary_t* bin, ix_u16 vendor_id);
int ixg_shader_get_target_name(ix_u32 target, char* out, ix_u32 cap);
int ixg_shader_get_backend_name(ix_u8 backend, char* out, ix_u32 cap);

#endif
