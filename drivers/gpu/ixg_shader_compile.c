#include "ixg_shader_compiler.h"
#include "ixg_sass.h"
#include "ixg_isa.h"
#include <string.h>

static ix_u32 sc_checksum(const ix_u8* d, ix_u32 n) {
  ix_u32 ck = 0u;
  for (ix_u32 i = 0; i < n; i++) ck = (ck * 31u) + d[i];
  return ck;
}

static int sc_pack_text(ixg_shader_binary_t* bin, const char* text, ix_u32 len,
                        ix_u8 type, ix_u8 backend, ix_u32 target) {
  ix_u32 copy = len;
  if (copy > IXG_SC_MAX_SHADER_BINARY_BYTES) copy = IXG_SC_MAX_SHADER_BINARY_BYTES;
  memset(bin, 0, sizeof(*bin));
  bin->magic = IXG_SC_BINARY_MAGIC;
  bin->version = IXG_SC_BINARY_VERSION;
  bin->type = type;
  bin->backend = backend;
  bin->target = target;
  memcpy(bin->binary, text, copy);
  bin->binary_size = copy;
  bin->checksum = sc_checksum(bin->binary, copy);
  return 0;
}

static ix_u32 sc_sm_from_target(ix_u32 target) {
  switch (target) {
    case IXG_SC_TARGET_SM30: return 0x0030u;
    case IXG_SC_TARGET_SM50: return 0x0050u;
    case IXG_SC_TARGET_SM60: return 0x0060u;
    case IXG_SC_TARGET_SM75: return 0x0075u;
    case IXG_SC_TARGET_SM86: return 0x0086u;
    case IXG_SC_TARGET_SM89: return 0x0089u;
    default: break;
  }
  if (target >= 0x0030u && target <= 0x00C0u) return target;
  return 0x0075u;
}

static int sc_compile_spirv(const ixg_shader_program_t* program, ix_u8 shader_type,
                            ixg_shader_binary_t* out) {
  ixg_isa_module_t mod;
  int rc = ixg_isa_emit_amd(program, shader_type, IXG_ISA_AMD_RDNA2, &mod);
  if (rc != 0) return rc;
  return sc_pack_text(out, mod.text, mod.length, shader_type,
                      (ix_u8)IXG_SC_BACKEND_SPIRV, IXG_SC_TARGET_SPIRV);
}

int ixg_shader_compile(const ixg_shader_program_t *program,
                       ixg_sc_backend_t backend,
                       ix_u8 shader_type,
                       ix_u32 sm_version,
                       ixg_shader_binary_t *out_binary) {
  if (!program || !out_binary) return -1;
  if (program->instruction_count > IXG_SHADER_MAX_INSTRUCTIONS) return -2;
  switch (backend) {
    case IXG_SC_BACKEND_NVIDIA:
    case IXG_SC_BACKEND_TURING: {
      ix_u32 sm = sm_version ? sm_version : 0x0075u;
      int rc = ixg_sass_emit_to_binary(program, shader_type, sm, out_binary);
      if (rc != 0) return rc;
      out_binary->backend = (ix_u8)backend;
      out_binary->target = sm;
      out_binary->checksum = sc_checksum(out_binary->binary, out_binary->binary_size);
      return 0;
    }
    case IXG_SC_BACKEND_AMD:
    case IXG_SC_BACKEND_RDNA: {
      ixg_isa_module_t mod;
      ix_u32 target = sm_version;
      int rc;
      if (target < IXG_ISA_AMD_GCN || target > IXG_ISA_AMD_RDNA3) target = IXG_ISA_AMD_RDNA2;
      rc = ixg_isa_emit_amd(program, shader_type, target, &mod);
      if (rc != 0) return rc;
      return sc_pack_text(out_binary, mod.text, mod.length, shader_type, (ix_u8)backend, target);
    }
    case IXG_SC_BACKEND_INTEL:
    case IXG_SC_BACKEND_XE_HPG: {
      ixg_isa_module_t mod;
      ix_u32 target = sm_version;
      int rc;
      if (target < IXG_ISA_INTEL_GEN9 || target > IXG_ISA_INTEL_XE2) target = IXG_ISA_INTEL_GEN12;
      rc = ixg_isa_emit_intel(program, shader_type, target, &mod);
      if (rc != 0) return rc;
      return sc_pack_text(out_binary, mod.text, mod.length, shader_type, (ix_u8)backend, target);
    }
    case IXG_SC_BACKEND_SPIRV:
      return sc_compile_spirv(program, shader_type, out_binary);
    case IXG_SC_BACKEND_EXOTIC:
    default: {
      ix_u32 sm = sc_sm_from_target(sm_version);
      int rc = ixg_sass_emit_to_binary(program, shader_type, sm, out_binary);
      if (rc != 0) return rc;
      out_binary->backend = (ix_u8)IXG_SC_BACKEND_EXOTIC;
      out_binary->target = sm_version ? sm_version : sm;
      out_binary->checksum = sc_checksum(out_binary->binary, out_binary->binary_size);
      return 0;
    }
  }
}

int ixg_shader_compile_vm_to_sm30(const ixg_shader_program_t *program, ix_u8 shader_type, ixg_shader_binary_t *out_binary) {
  return ixg_shader_compile(program, IXG_SC_BACKEND_NVIDIA, shader_type, 0x0030u, out_binary);
}
int ixg_shader_compile_vm_to_gcn(const ixg_shader_program_t *program, ix_u8 shader_type, ixg_shader_binary_t *out_binary) {
  return ixg_shader_compile(program, IXG_SC_BACKEND_AMD, shader_type, IXG_ISA_AMD_GCN, out_binary);
}
int ixg_shader_compile_vm_to_gen9(const ixg_shader_program_t *program, ix_u8 shader_type, ixg_shader_binary_t *out_binary) {
  return ixg_shader_compile(program, IXG_SC_BACKEND_INTEL, shader_type, IXG_ISA_INTEL_GEN9, out_binary);
}
int ixg_shader_compile_vm_to_rdna(const ixg_shader_program_t *program, ix_u8 shader_type, ix_u32 generation, ixg_shader_binary_t *out_binary) {
  ix_u32 target = IXG_ISA_AMD_RDNA1;
  if (generation >= 3u) target = IXG_ISA_AMD_RDNA3;
  else if (generation == 2u) target = IXG_ISA_AMD_RDNA2;
  return ixg_shader_compile(program, IXG_SC_BACKEND_RDNA, shader_type, target, out_binary);
}
int ixg_shader_compile_vm_to_turing(const ixg_shader_program_t *program, ix_u8 shader_type, ix_u32 sm_version, ixg_shader_binary_t *out_binary) {
  return ixg_shader_compile(program, IXG_SC_BACKEND_TURING, shader_type, sm_version ? sm_version : 0x0075u, out_binary);
}
int ixg_shader_compile_vm_to_xe_hpg(const ixg_shader_program_t *program, ix_u8 shader_type, ix_u32 gen, ixg_shader_binary_t *out_binary) {
  ix_u32 target = IXG_ISA_INTEL_XEHPG;
  if (gen >= 2u) target = IXG_ISA_INTEL_XE2;
  return ixg_shader_compile(program, IXG_SC_BACKEND_XE_HPG, shader_type, target, out_binary);
}
int ixg_shader_compile_vm_to_spirv(const ixg_shader_program_t *program, ix_u8 shader_type, ixg_shader_binary_t *out_binary) {
  return ixg_shader_compile(program, IXG_SC_BACKEND_SPIRV, shader_type, 0u, out_binary);
}

int ixg_shader_select_target(ix_u16 vendor_id, ix_u16 device_id) {
  if (vendor_id == 0x10DEu) {
    if (device_id >= 0x2900u) return IXG_SC_TARGET_SM89;
    if (device_id >= 0x2600u) return IXG_SC_TARGET_SM89;
    if (device_id >= 0x1E00u) return IXG_SC_TARGET_SM86;
    if (device_id >= 0x1B00u) return IXG_SC_TARGET_SM60;
    if (device_id >= 0x1200u) return IXG_SC_TARGET_SM50;
    return IXG_SC_TARGET_SM30;
  }
  if (vendor_id == 0x1002u || vendor_id == 0x1022u) {
    if (device_id >= 0x7600u) return IXG_SC_TARGET_RDNA3;
    if (device_id >= 0x7400u) return IXG_SC_TARGET_RDNA3;
    if (device_id >= 0x7300u) return IXG_SC_TARGET_RDNA2;
    if (device_id >= 0x7000u) return IXG_SC_TARGET_RDNA1;
    return IXG_SC_TARGET_GCN;
  }
  if (vendor_id == 0x8086u) {
    if (device_id >= 0xD200u) return IXG_SC_TARGET_XE2;
    if (device_id >= 0x4F80u) return IXG_SC_TARGET_XEHPG;
    if (device_id >= 0x1900u) return IXG_SC_TARGET_GEN12;
    return IXG_SC_TARGET_GEN9;
  }
  return IXG_SC_TARGET_SPIRV;
}

int ixg_shader_validate_binary(const ixg_shader_binary_t* bin, ix_u16 vendor_id) {
  if (!bin) return -1;
  if (bin->magic != IXG_SC_BINARY_MAGIC) return -10;
  if (bin->version != IXG_SC_BINARY_VERSION) return -11;
  if (bin->binary_size > IXG_SC_MAX_SHADER_BINARY_BYTES) return -12;
  {
    ix_u32 calc = 0u;
    for (ix_u32 i = 0; i < bin->binary_size; i++) calc = (calc * 31u) + bin->binary[i];
    if (calc != bin->checksum && bin->checksum != 0) return -13;
  }
  if (vendor_id == 0x10DEu) {
    if (bin->target < 0x0030u || bin->target > 0x0089u) return -20;
  } else if (vendor_id == 0x1002u || vendor_id == 0x1022u) {
    if (bin->target < 0x0F00u || bin->target > 0x0F30u) return -21;
  } else if (vendor_id == 0x8086u) {
    if (bin->target < 0x0900u || bin->target > 0x0D20u) return -22;
  }
  return 0;
}

int ixg_shader_get_target_name(ix_u32 target, char* out, ix_u32 cap) {
  if (!out || cap==0) return -1;
  const char* n = "?";
  switch(target){
  case 0x0030u:n="SM3.0";break;case 0x0050u:n="SM5.0";break;case 0x0060u:n="SM6.0";break;
  case 0x0075u:n="SM7.5";break;case 0x0086u:n="SM8.6";break;case 0x0089u:n="SM8.9";break;
  case 0x0F00u:n="GCN";break;case 0x0F10u:n="RDNA1";break;case 0x0F20u:n="RDNA2";break;
  case 0x0F30u:n="RDNA3";break;case 0x0900u:n="Gen9";break;case 0x0C00u:n="Gen12";break;
  case 0x0D00u:n="XeHPG";break;case 0x0D20u:n="Xe2";break;case 0xE000u:n="SPIR-V";break;
  case 0xE001u:n="PTX";break;case 0xE002u:n="DXIL";break;}
  ix_u32 i=0;while(n[i]&&i<cap-1){out[i]=n[i];i++;}out[i]=0;return 0;
}

int ixg_shader_get_backend_name(ix_u8 be, char* out, ix_u32 cap) {
  if (!out||cap==0) return -1;
  const char* n="?";
  switch(be){
  case 1:n="NVIDIA";break;case 2:n="AMD";break;case 3:n="Intel";break;
  case 4:n="SPIRV";break;case 5:n="RDNA";break;case 6:n="Turing";break;
  case 7:n="Xe-HPG";break;case 8:n="Exotic";break;}
  ix_u32 i=0;while(n[i]&&i<cap-1){out[i]=n[i];i++;}out[i]=0;return 0;
}
