#ifndef IXG_DRIVER_H
#define IXG_DRIVER_H

#include "ixg_types.h"
#include "ixg_platform.h"

#define IXG_HW_MAX_TEXTURE_MIPS 12u
#define IXG_HW_TEXTURE_FLAG_FILTER_LINEAR (1u << 0)
#define IXG_HW_TEXTURE_FLAG_MIN_MIPMAP (1u << 1)
#define IXG_HW_TEXTURE_FLAG_MIP_LINEAR (1u << 2)

typedef struct {
  ix_u32 vendor_id;
  ix_u32 device_id;
  ix_u64 mmio_base;
  ix_u64 fb_base;
  ix_u32 fb_size;
} ixg_hw_info_t;

typedef struct {
  float x;
  float y;
  float z;
  float rhw;
  float u;
  float v;
  ix_u32 color;
} ixg_hw_vertex_t;

typedef struct {
  const ix_u32* pixels;
  ix_u16 width;
  ix_u16 height;
  ix_u16 stride;
  ix_u16 flags;
  ix_u8  mip_count;
  ix_u8  reserved0;
  ix_u8  reserved1;
  ix_u8  reserved2;
  const ix_u32* mip_pixels[IXG_HW_MAX_TEXTURE_MIPS];
  ix_u16 mip_width[IXG_HW_MAX_TEXTURE_MIPS];
  ix_u16 mip_height[IXG_HW_MAX_TEXTURE_MIPS];
  ix_u16 mip_stride[IXG_HW_MAX_TEXTURE_MIPS];
} ixg_hw_texture_t;

typedef struct {
  const void* vs_bytecode;
  ix_u32 vs_size;
  const void* ps_bytecode;
  ix_u32 ps_size;
  const float* vs_constants;
  ix_u32 vs_constant_count;
  const float* ps_constants;
  ix_u32 ps_constant_count;
  ix_u32 vs_profile;
  ix_u32 ps_profile;
  ix_u32 flags;
} ixg_hw_shader_bind_t;

typedef struct {
  void (*hw_draw_rect)(int x, int y, int w, int h, ix_u32 color);
  void (*hw_bitblt)(int dx, int dy, int w, int h, void* src, int sx, int sy);
  int  (*hw_draw_rect_blend)(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied);
  int  (*hw_bitblt_alpha)(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied);
  void (*hw_flush)(void);
  int  (*hw_draw_triangles)(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count);
  int  (*hw_draw_triangles_textured)(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture);
  int  (*hw_compute_dispatch)(const void* dispatch_desc);
  int  (*hw_trace_rays)(const void* trace_desc);
  int  (*hw_control_dispatch)(const void* control_desc, void* control_result);
  int  (*hw_submit_cmds)(const void* cmds, ix_u32 cmds_size);
  int  (*hw_bind_shaders)(const ixg_hw_shader_bind_t* bind);
  /*
   * CPU-visible render target ("readback" path).
   *
   * Бэкенд рендерит не в недоступный CPU фреймбуфер карты, а в
   * буфер, отображённый в GPU через DMA-aperture (PTE ixg), но
   * живущий в системной RAM.  Поэтому после flush ядро может
   * скопировать готовый кадр прямо в canvas окна -- и на экране
   * будут ПИКСЕЛИ GPU, а не программного растеризатора.
   *
   *   hw_set_render_target(w, h) -- выделить/переключить цель
   *       (идемпотентно при тех же размерах); 0 = ok.
   *   hw_readback_rect(dst, w, h) -- скопировать цель в dst
   *       (ARGB, stride dst = w); 0 = ok.
   */
  int  (*hw_set_render_target)(ix_u32 w, ix_u32 h);
  int  (*hw_readback_rect)(ix_u32* dst, ix_u32 w, ix_u32 h);
} ixg_hw_ops_t;

typedef enum {
  IXG_BACKEND_SOFTWARE = 0,
  IXG_BACKEND_INTEL    = 1,
  IXG_BACKEND_NVIDIA   = 2,
  IXG_BACKEND_AMD      = 3,
  IXG_BACKEND_EXOTIC   = 4
} ixg_backend_t;

#define IXG_BIND_OK 0
#define IXG_BIND_ERROR -1
#define IXG_BIND_SOFTWARE_FALLBACK 2

#define IXG_DRIVER_FEAT_2D_FILL_RECT          (1u << 0)
#define IXG_DRIVER_FEAT_2D_BITBLT             (1u << 1)
#define IXG_DRIVER_FEAT_2D_ALPHA_BLEND        (1u << 2)
#define IXG_DRIVER_FEAT_3D_FIXED              (1u << 3)
#define IXG_DRIVER_FEAT_3D_SHADER             (1u << 4)
#define IXG_DRIVER_FEAT_3D_TEXTURE            (1u << 5)
#define IXG_DRIVER_FEAT_3D_INDEXED            (1u << 6)
#define IXG_DRIVER_FEAT_3D_INSTANCING         (1u << 7)
#define IXG_DRIVER_FEAT_3D_COMPUTE            (1u << 8)
#define IXG_DRIVER_FEAT_HEADLESS              (1u << 9)
#define IXG_DRIVER_FEAT_CONTROL_SHADER        (1u << 10)
#define IXG_DRIVER_FEAT_3D_BINDLESS          (1u << 11)
#define IXG_DRIVER_FEAT_3D_TIMELINE_SYNC      (1u << 12)
#define IXG_DRIVER_FEAT_3D_DYNAMIC_RENDERING  (1u << 13)
#define IXG_DRIVER_FEAT_3D_DRAW_INDIRECT_COUNT (1u << 14)
#define IXG_DRIVER_FEAT_3D_MESH_SHADER        (1u << 15)
#define IXG_DRIVER_FEAT_3D_RAY_TRACING        (1u << 16)
#define IXG_DRIVER_FEAT_3D_VRS               (1u << 17)
#define IXG_DRIVER_FEAT_3D_MULTI_GPU         (1u << 18)

typedef struct {
  ix_u16 vendor_id;
  ix_u16 device_id;
  ix_u8  backend;
  ix_u8  feature_level;
  ix_u8  shader_model;
  ix_u8  reserved0;
  ix_u32 feature_flags;
} ixg_driver_caps_t;

void ixg_driver_init(void);
void ixg_driver_register_ops(ixg_hw_ops_t* ops);
void ixg_driver_accel_rect(int x, int y, int w, int h, ix_u32 color);
void ixg_driver_accel_bitblt(int dx, int dy, int w, int h, void* src, int sx, int sy);
int  ixg_driver_accel_rect_blend(int x, int y, int w, int h, ix_u32 color, ix_u8 premultiplied);
int  ixg_driver_accel_bitblt_alpha(int dx, int dy, int w, int h, const void* src, ix_u32 stride_bytes, ix_u8 premultiplied);
void ixg_driver_flush(void);
ix_u32 ixg_driver_is_accel_ready(void);
ix_u32 ixg_driver_is_gpu_3d_ready(void);
ix_u32 ixg_driver_has_gpu_triangles(void);
ix_u32 ixg_driver_has_gpu_triangles_textured(void);
ix_u32 ixg_driver_has_gpu_compute(void);
ix_u32 ixg_driver_has_gpu_shader_bind(void);
int  ixg_driver_accel_bind_shaders(const ixg_hw_shader_bind_t* bind);
ix_u32 ixg_driver_gpu_prefers_hardware_only(void);
ix_u32 ixg_driver_gpu_submit_mirror_3d(void);
int  ixg_driver_accel_triangles(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count);
int  ixg_driver_accel_triangles_textured(const ixg_hw_vertex_t* vertices, ix_u32 triangle_count, const ixg_hw_texture_t* texture);
/* CPU-visible render target + readback (см. ixg_hw_ops_t). */
ix_u32 ixg_driver_has_render_readback(void);
int  ixg_driver_accel_set_render_target(ix_u32 w, ix_u32 h);
int  ixg_driver_accel_readback_rect(ix_u32* dst, ix_u32 w, ix_u32 h);
/* Пер-кадровая заливка CPU-visible цели (цвет 0 = фон для readback_over). */
void ixg_driver_accel_rt_fill(ix_u32 color);
/* Readback "поверх": пиксели == bg не копируются (в dst живёт
 * 2D-оверлей приложения: фон axgl_clear + текст ax_text). */
int  ixg_driver_accel_readback_rect_over(ix_u32* dst, ix_u32 w, ix_u32 h, ix_u32 bg);
/* Общая часть бэкендов: буфер системной RAM, отображённый в GPU. */
int    ixg_driver_render_target_setup(ix_u32 w, ix_u32 h);
void*  ixg_driver_render_target_cpu(void);
ix_u64 ixg_driver_render_target_gpu_addr(void);
ix_u32 ixg_driver_render_target_stride_px(void);
ix_u32 ixg_driver_render_target_height(void);
int  ixg_driver_accel_compute_dispatch(const void* dispatch_desc);
int  ixg_driver_accel_trace_rays(const void* trace_desc);
int  ixg_driver_accel_control_dispatch(const void* control_desc, void* control_result);
void ixg_driver_get_caps(ixg_driver_caps_t* out_caps);
const char* ixg_driver_backend_name(void);

#endif