#include "ixg_compute.h"
#include "ixg_mutants.h"
#include <string.h>
#include <math.h>
static float ixg_expf(float x) {
    float sum = 1.0f, term = 1.0f;
    int i;
    if (x < -87.0f) return 0.0f;
    if (x > 88.0f) return 1e38f;
    for (i = 1; i <= 20; i++) {
        term *= x / (float)i;
        sum += term;
    }
    return sum;
}

static ixg_compute_state_t g_ixg_compute_state;

static ix_u32 ixg_compute_thread_count(const ixg_compute_dispatch_desc_t* desc) {
  ix_u64 y = desc->y ? desc->y : 1u;
  ix_u64 z = desc->z ? desc->z : 1u;
  ix_u64 total = (ix_u64)desc->x * y * z;
  if (total == 0 || total > 0xFFFFFFFFull) return 0;
  return (ix_u32)total;
}

static int ixg_compute_dispatch_copy_u32(const ixg_compute_dispatch_desc_t* desc, ix_u32 count) {
  const ix_u32* in0 = (const ix_u32*)desc->src0;
  ix_u32* out = (ix_u32*)desc->dst;
  if (!in0 || !out) return -1;
  memcpy(out, in0, (size_t)count * sizeof(ix_u32));
  return 0;
}

static int ixg_compute_dispatch_add_u32(const ixg_compute_dispatch_desc_t* desc, ix_u32 count) {
  const ix_u32* in0 = (const ix_u32*)desc->src0;
  const ix_u32* in1 = (const ix_u32*)desc->src1;
  ix_u32* out = (ix_u32*)desc->dst;
  ix_u32 i;
  if (!in0 || !in1 || !out) return -1;
  for (i = 0u; i < count; i++) out[i] = in0[i] + in1[i];
  return 0;
}

static int ixg_compute_dispatch_saxpy_f32(const ixg_compute_dispatch_desc_t* desc, ix_u32 count) {
  const float* in0 = (const float*)desc->src0;
  const float* in1 = (const float*)desc->src1;
  float* out = (float*)desc->dst;
  ix_u32 i;
  if (!in0 || !in1 || !out) return -1;
  for (i = 0u; i < count; i++) out[i] = desc->scalar0 * in0[i] + desc->scalar1 * in1[i];
  return 0;
}

static int ixg_compute_dispatch_matmul4x4(const ixg_compute_dispatch_desc_t* desc) {
  const float* a = (const float*)desc->src0;
  const float* b = (const float*)desc->src1;
  float* out = (float*)desc->dst;
  ix_u32 row, col, k;
  if (!a || !b || !out) return -1;
  for (row = 0; row < 4; row++) {
    for (col = 0; col < 4; col++) {
      float sum = 0.0f;
      for (k = 0; k < 4; k++) sum += a[row * 4 + k] * b[k * 4 + col];
      out[row * 4 + col] = sum;
    }
  }
  return 0;
}

static float ixg_compute_dot3(float ax, float ay, float az, float bx, float by, float bz) {
  return ax * bx + ay * by + az * bz;
}

static int ixg_compute_rt_intersect(const ixg_rt_triangle_t* tri, const ixg_rt_ray_t* ray, float* out_t, float* out_u, float* out_v) {
  const float eps = 1e-6f;
  float e1x = tri->bx - tri->ax, e1y = tri->by - tri->ay, e1z = tri->bz - tri->az;
  float e2x = tri->cx - tri->ax, e2y = tri->cy - tri->ay, e2z = tri->cz - tri->az;
  float px = ray->dy * e2z - ray->dz * e2y;
  float py = ray->dz * e2x - ray->dx * e2z;
  float pz = ray->dx * e2y - ray->dy * e2x;
  float det = ixg_compute_dot3(e1x, e1y, e1z, px, py, pz);
  if (det > -eps && det < eps) return 0;
  {
    float inv_det = 1.0f / det;
    float tx = ray->ox - tri->ax, ty = ray->oy - tri->ay, tz = ray->oz - tri->az;
    float u = ixg_compute_dot3(tx, ty, tz, px, py, pz) * inv_det;
    if (u < 0.0f || u > 1.0f) return 0;
    {
      float qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
      float v = ixg_compute_dot3(ray->dx, ray->dy, ray->dz, qx, qy, qz) * inv_det;
      float t;
      if (v < 0.0f || (u + v) > 1.0f) return 0;
      t = ixg_compute_dot3(e2x, e2y, e2z, qx, qy, qz) * inv_det;
      if (t < ray->t_min || t > ray->t_max) return 0;
      *out_t = t; *out_u = u; *out_v = v;
      return 1;
    }
  }
}

void ixg_compute_refresh(void) {
  memset(&g_ixg_compute_state, 0, sizeof(g_ixg_compute_state));
  g_ixg_compute_state.ready = 1u;
  g_ixg_compute_state.device_count = 1u;
  g_ixg_compute_state.active_mask = 1u;
}

void ixg_compute_init(void) {
  ixg_compute_refresh();
}

int ixg_compute_get_state(ixg_compute_state_t* out_state) {
  if (!out_state) return -1;
  ixg_compute_refresh();
  *out_state = g_ixg_compute_state;
  return 0;
}

int ixg_compute_get_device_caps(ix_u8 index, ixg_compute_device_caps_t* out_caps) {
  if (!out_caps || index >= g_ixg_compute_state.device_count) return -1;
  memset(out_caps, 0, sizeof(*out_caps));
  out_caps->slot = (ix_u8)(index + 1);
  out_caps->backend = (ix_u8)IXG_BACKEND_SOFTWARE;
  out_caps->feature_flags = IXG_COMPUTE_FEAT_SHADER | IXG_COMPUTE_FEAT_SOFT_FALLBACK;
  return 0;
}

int ixg_compute_dispatch(const ixg_compute_dispatch_desc_t* desc) {
  ix_u32 count;
  if (!desc || !desc->dst) return -1;
  count = ixg_compute_thread_count(desc);
  if (count == 0u) return -1;
  if (desc->kernel == IXG_COMPUTE_KERNEL_MATMUL4X4_F32) count = 16u;
  switch (desc->kernel) {
    case IXG_COMPUTE_KERNEL_COPY_U32:  return ixg_compute_dispatch_copy_u32(desc, count);
    case IXG_COMPUTE_KERNEL_ADD_U32:   return ixg_compute_dispatch_add_u32(desc, count);
    case IXG_COMPUTE_KERNEL_SAXPY_F32: return ixg_compute_dispatch_saxpy_f32(desc, count);
    case IXG_COMPUTE_KERNEL_MATMUL4X4_F32: return ixg_compute_dispatch_matmul4x4(desc);
    default: return -3;
  }
}

int ixg_compute_trace_rays(const ixg_rt_trace_desc_t* desc) {
  ix_u32 r, t;
  if (!desc || !desc->triangles || !desc->rays || !desc->hits || desc->triangle_count == 0u || desc->ray_count == 0u) return -1;
  for (r = 0; r < desc->ray_count; r++) {
    const ixg_rt_ray_t* ray = &desc->rays[r];
    ixg_rt_hit_t* hit = &desc->hits[r];
    float best_t = ray->t_max, best_u = 0.0f, best_v = 0.0f;
    ix_u32 best_tri = 0xFFFFFFFFu;
    hit->hit = 0u; hit->triangle_index = 0xFFFFFFFFu; hit->t = ray->t_max; hit->u = 0.0f; hit->v = 0.0f;
    for (t = 0; t < desc->triangle_count; t++) {
      float out_t = 0.0f, out_u = 0.0f, out_v = 0.0f;
      if (ixg_compute_rt_intersect(&desc->triangles[t], ray, &out_t, &out_u, &out_v) && out_t < best_t) {
        best_t = out_t; best_u = out_u; best_v = out_v; best_tri = t;
      }
    }
    if (best_tri != 0xFFFFFFFFu) { hit->hit = 1u; hit->triangle_index = best_tri; hit->t = best_t; hit->u = best_u; hit->v = best_v; }
  }
  return 0;
}

int ixg_compute_program_create(const ixg_compute_program_desc_t* desc, ix_u32* out_program) {
  if (!desc || !out_program) return -1;
  *out_program = 1u;
  return 0;
}

int ixg_compute_program_release(ix_u32 program) { (void)program; return 0; }

int ixg_compute_program_get_ixsl(ix_u32 program, char* out_text, ix_u32 out_cap, ix_u32* out_len) {
  (void)program;
  if (out_len) *out_len = 0u;
  if (out_text && out_cap) out_text[0] = 0;
  return 0;
}

int ixg_compute_kernel_create(ix_u32 program, const char* entry, ix_u32* out_kernel) {
  (void)program; (void)entry;
  if (!out_kernel) return -1;
  *out_kernel = 1u;
  return 0;
}

int ixg_compute_kernel_release(ix_u32 kernel) { (void)kernel; return 0; }

int ixg_compute_kernel_set_arg(ix_u32 kernel, ix_u32 index, ix_u32 size, const void* data) {
  (void)kernel; (void)index; (void)size; (void)data;
  return 0;
}

int ixg_compute_kernel_dispatch(ix_u32 kernel, ix_u64 device_mask, ix_u32 x, ix_u32 y, ix_u32 z) {
  ixg_compute_dispatch_desc_t desc;
  (void)kernel;
  memset(&desc, 0, sizeof(desc));
  desc.device_mask = device_mask;
  desc.kernel = IXG_COMPUTE_KERNEL_COPY_U32;
  desc.x = x; desc.y = y; desc.z = z;
  return ixg_compute_dispatch(&desc);
}

int ixg_compute_launch_kernel(ix_u32 device, const ixg_compute_kernel_t* kernel) {
  ixg_compute_dispatch_desc_t desc;
  if (!kernel) return -1;
  memset(&desc, 0, sizeof(desc));
  desc.device_mask = (1ull << (device & 0x3F));
  desc.x = kernel->thread_count[0]; desc.y = kernel->thread_count[1]; desc.z = kernel->thread_count[2];
  desc.kernel = kernel->flags;
  return ixg_compute_dispatch(&desc);
}

int ixg_compute_tensor_gemm(const ixg_compute_tensor_gemm_desc_t* desc) {
  ix_u32 m, n, k_dim, i, j, kk;
  const float *a, *b;
  float *d;
  if (!desc || !desc->a || !desc->b || !desc->d) return -1;
  if (desc->m == 0u || desc->n == 0u || desc->k == 0u) return -1;
  m = desc->m; n = desc->n; k_dim = desc->k;
  a = (const float*)desc->a; b = (const float*)desc->b; d = (float*)desc->d;
  for (i = 0u; i < m; i++) {
    for (j = 0u; j < n; j++) {
      float sum = 0.0f;
      for (kk = 0u; kk < k_dim; kk++) sum += a[i * k_dim + kk] * b[kk * n + j];
      sum *= desc->alpha;
      if (desc->c && desc->beta != 0.0f) sum += desc->beta * ((const float*)desc->c)[i * n + j];
      d[i * n + j] = sum;
    }
  }
  return 0;
}

static void ixg_compute_bvh_compute_bounds(const ixg_rt_triangle_t* tris, const ix_u32* indices, ix_u32 count, float* out_min, float* out_max) {
  ix_u32 i;
  out_min[0] = out_min[1] = out_min[2] = 1e30f;
  out_max[0] = out_max[1] = out_max[2] = -1e30f;
  for (i = 0u; i < count; i++) {
    const ixg_rt_triangle_t* t = &tris[indices[i]];
    float verts[9];
    ix_u32 v;
    verts[0]=t->ax; verts[1]=t->ay; verts[2]=t->az;
    verts[3]=t->bx; verts[4]=t->by; verts[5]=t->bz;
    verts[6]=t->cx; verts[7]=t->cy; verts[8]=t->cz;
    for (v = 0u; v < 9u; v += 3u) {
      if (verts[v] < out_min[0]) out_min[0] = verts[v];
      if (verts[v+1] < out_min[1]) out_min[1] = verts[v+1];
      if (verts[v+2] < out_min[2]) out_min[2] = verts[v+2];
      if (verts[v] > out_max[0]) out_max[0] = verts[v];
      if (verts[v+1] > out_max[1]) out_max[1] = verts[v+1];
      if (verts[v+2] > out_max[2]) out_max[2] = verts[v+2];
    }
  }
}

static float ixg_compute_tri_centroid(const ixg_rt_triangle_t* t, ix_u32 axis) {
  switch (axis) {
    case 0: return (t->ax + t->bx + t->cx) / 3.0f;
    case 1: return (t->ay + t->by + t->cy) / 3.0f;
    default: return (t->az + t->bz + t->cz) / 3.0f;
  }
}

static ix_u32 ixg_compute_bvh_build_recursive(ixg_rt_bvh_node_t* nodes, ix_u32* node_count, ix_u32 max_nodes,
                                               const ixg_rt_triangle_t* tris, ix_u32* indices, ix_u32 count, ix_u32 depth) {
  ix_u32 node_idx;
  float bmin[3], bmax[3];
  ixg_rt_bvh_node_t* node;
  if (*node_count >= max_nodes || depth >= IXG_RT_BVH_MAX_DEPTH) {
    if (*node_count >= max_nodes) return 0xFFFFFFFFu;
    node_idx = (*node_count)++;
    node = &nodes[node_idx];
    ixg_compute_bvh_compute_bounds(tris, indices, count, bmin, bmax);
    node->min_x=bmin[0]; node->min_y=bmin[1]; node->min_z=bmin[2];
    node->max_x=bmax[0]; node->max_y=bmax[1]; node->max_z=bmax[2];
    node->left=0xFFFFFFFFu; node->right=0xFFFFFFFFu;
    node->tri_offset=indices[0]; node->tri_count=count;
    return node_idx;
  }
  if (count <= 4u) {
    node_idx = (*node_count)++;
    node = &nodes[node_idx];
    ixg_compute_bvh_compute_bounds(tris, indices, count, bmin, bmax);
    node->min_x=bmin[0]; node->min_y=bmin[1]; node->min_z=bmin[2];
    node->max_x=bmax[0]; node->max_y=bmax[1]; node->max_z=bmax[2];
    node->left=0xFFFFFFFFu; node->right=0xFFFFFFFFu;
    node->tri_offset=indices[0]; node->tri_count=count;
    return node_idx;
  }
  {
    ix_u32 axis;
    float extent[3];
    ix_u32 mid, i, j;
    float pivot;
    ixg_compute_bvh_compute_bounds(tris, indices, count, bmin, bmax);
    extent[0]=bmax[0]-bmin[0]; extent[1]=bmax[1]-bmin[1]; extent[2]=bmax[2]-bmin[2];
    axis = 0u;
    if (extent[1] > extent[axis]) axis = 1u;
    if (extent[2] > extent[axis]) axis = 2u;
    pivot = (bmin[axis] + bmax[axis]) * 0.5f;
    i = 0u; j = count - 1u;
    while (i < j) {
      while (i < count && ixg_compute_tri_centroid(&tris[indices[i]], axis) < pivot) i++;
      while (j > 0u && ixg_compute_tri_centroid(&tris[indices[j]], axis) >= pivot) j--;
      if (i < j) { ix_u32 tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp; i++; if (j > 0u) j--; }
    }
    mid = i;
    if (mid == 0u) mid = 1u;
    if (mid >= count) mid = count - 1u;
    node_idx = (*node_count)++;
    node = &nodes[node_idx];
    node->min_x=bmin[0]; node->min_y=bmin[1]; node->min_z=bmin[2];
    node->max_x=bmax[0]; node->max_y=bmax[1]; node->max_z=bmax[2];
    node->tri_offset=0u; node->tri_count=0u;
    node->left = ixg_compute_bvh_build_recursive(nodes, node_count, max_nodes, tris, indices, mid, depth + 1u);
    node->right = ixg_compute_bvh_build_recursive(nodes, node_count, max_nodes, tris, &indices[mid], count - mid, depth + 1u);
    return node_idx;
  }
}

int ixg_compute_bvh_build(const ixg_rt_triangle_t* tris, ix_u32 tri_count, ixg_rt_bvh_t* out_bvh) {
  ix_u32 max_nodes, i;
  ix_u32* indices;
  if (!tris || tri_count == 0u || !out_bvh) return -1;
  max_nodes = tri_count * 2u;
  if (max_nodes > 65536u) max_nodes = 65536u;
  memset(out_bvh, 0, sizeof(*out_bvh));
  out_bvh->triangles = tris;
  out_bvh->tri_count = tri_count;
  out_bvh->nodes = (ixg_rt_bvh_node_t*)malloc(max_nodes * sizeof(ixg_rt_bvh_node_t));
  if (!out_bvh->nodes) return -1;
  indices = (ix_u32*)malloc(tri_count * sizeof(ix_u32));
  if (!indices) { free(out_bvh->nodes); return -1; }
  for (i = 0u; i < tri_count; i++) indices[i] = i;
  {
    ix_u32 node_count = 0u;
    memset(out_bvh->nodes, 0, max_nodes * sizeof(ixg_rt_bvh_node_t));
    (void)ixg_compute_bvh_build_recursive(out_bvh->nodes, &node_count, max_nodes, tris, indices, tri_count, 0u);
    out_bvh->node_count = node_count;
  }
  free(indices);
  return 0;
}

void ixg_compute_bvh_destroy(ixg_rt_bvh_t* bvh) {
  if (!bvh) return;
  if (bvh->nodes) free(bvh->nodes);
  memset(bvh, 0, sizeof(*bvh));
}

static int ixg_compute_bvh_intersect_aabb(const ixg_rt_bvh_node_t* node, const ixg_rt_ray_t* ray) {
  float tmin, tmax, tymin, tymax, tzmin, tzmax;
  float inv_dx = (ray->dx != 0.0f) ? 1.0f / ray->dx : 1e30f;
  float inv_dy = (ray->dy != 0.0f) ? 1.0f / ray->dy : 1e30f;
  float inv_dz = (ray->dz != 0.0f) ? 1.0f / ray->dz : 1e30f;
  if (inv_dx >= 0.0f) { tmin = (node->min_x - ray->ox) * inv_dx; tmax = (node->max_x - ray->ox) * inv_dx; }
  else { tmin = (node->max_x - ray->ox) * inv_dx; tmax = (node->min_x - ray->ox) * inv_dx; }
  if (inv_dy >= 0.0f) { tymin = (node->min_y - ray->oy) * inv_dy; tymax = (node->max_y - ray->oy) * inv_dy; }
  else { tymin = (node->max_y - ray->oy) * inv_dy; tymax = (node->min_y - ray->oy) * inv_dy; }
  if (tmin > tymax || tymin > tmax) return 0;
  if (tymin > tmin) tmin = tymin;
  if (tymax < tmax) tmax = tymax;
  if (inv_dz >= 0.0f) { tzmin = (node->min_z - ray->oz) * inv_dz; tzmax = (node->max_z - ray->oz) * inv_dz; }
  else { tzmin = (node->max_z - ray->oz) * inv_dz; tzmax = (node->min_z - ray->oz) * inv_dz; }
  if (tmin > tzmax || tzmin > tmax) return 0;
  if (tzmin > tmin) tmin = tzmin;
  if (tzmax < tmax) tmax = tzmax;
  return (tmax >= ray->t_min && tmin <= ray->t_max) ? 1 : 0;
}

static void ixg_compute_bvh_trace_node(const ixg_rt_bvh_t* bvh, ix_u32 node_idx, const ixg_rt_ray_t* ray, ixg_rt_hit_t* hit) {
  const ixg_rt_bvh_node_t* node;
  ix_u32 i;
  if (node_idx >= bvh->node_count || node_idx == 0xFFFFFFFFu) return;
  node = &bvh->nodes[node_idx];
  if (!ixg_compute_bvh_intersect_aabb(node, ray)) return;
  if (node->left == 0xFFFFFFFFu && node->right == 0xFFFFFFFFu) {
    for (i = 0u; i < node->tri_count && (node->tri_offset + i) < bvh->tri_count; i++) {
      float out_t = 0.0f, out_u = 0.0f, out_v = 0.0f;
      ix_u32 tri_idx = node->tri_offset + i;
      if (ixg_compute_rt_intersect(&bvh->triangles[tri_idx], ray, &out_t, &out_u, &out_v)) {
        if (out_t < hit->t) { hit->hit = 1u; hit->triangle_index = tri_idx; hit->t = out_t; hit->u = out_u; hit->v = out_v; }
      }
    }
    return;
  }
  ixg_compute_bvh_trace_node(bvh, node->left, ray, hit);
  ixg_compute_bvh_trace_node(bvh, node->right, ray, hit);
}

int ixg_compute_bvh_trace(const ixg_rt_bvh_trace_desc_t* desc) {
  ix_u32 r;
  if (!desc || !desc->bvh || !desc->rays || !desc->hits || desc->ray_count == 0u) return -1;
  if (!desc->bvh->nodes || desc->bvh->node_count == 0u) return -1;
  for (r = 0u; r < desc->ray_count; r++) {
    ixg_rt_hit_t* hit = &desc->hits[r];
    hit->hit = 0u; hit->triangle_index = 0xFFFFFFFFu; hit->t = desc->rays[r].t_max; hit->u = 0.0f; hit->v = 0.0f;
    ixg_compute_bvh_trace_node(desc->bvh, 0u, &desc->rays[r], hit);
  }
  return 0;
}

int ixg_compute_buffer_create(ix_u32 size_bytes, ix_u32 flags, ixg_compute_buffer_t* out_buf) {
  (void)flags;
  if (!out_buf || size_bytes == 0u) return -1;
  memset(out_buf, 0, sizeof(*out_buf));
  out_buf->cpu_ptr = malloc(size_bytes);
  if (!out_buf->cpu_ptr) return -1;
  out_buf->size_bytes = size_bytes;
  out_buf->handle = 1u;
  return 0;
}

void ixg_compute_buffer_destroy(ixg_compute_buffer_t* buf) {
  if (!buf) return;
  if (buf->cpu_ptr) free(buf->cpu_ptr);
  memset(buf, 0, sizeof(*buf));
}

int ixg_compute_dispatch_fill_u32(ix_u64 device_mask, void* dst, ix_u32 value, ix_u32 count) {
  ix_u32* out; ix_u32 i;
  (void)device_mask;
  if (!dst || count == 0u) return -1;
  out = (ix_u32*)dst;
  for (i = 0u; i < count; i++) out[i] = value;
  return 0;
}

int ixg_compute_dispatch_reduce_sum_f32(ix_u64 device_mask, const float* src, ix_u32 count, float* out_result) {
  float sum = 0.0f; ix_u32 i;
  (void)device_mask;
  if (!src || count == 0u || !out_result) return -1;
  for (i = 0u; i < count; i++) sum += src[i];
  *out_result = sum;
  return 0;
}

int ixg_compute_dispatch_softmax_f32(ix_u64 device_mask, const float* src, float* dst, ix_u32 count) {
  float max_val, sum; ix_u32 i;
  (void)device_mask;
  if (!src || !dst || count == 0u) return -1;
  max_val = src[0];
  for (i = 1u; i < count; i++) if (src[i] > max_val) max_val = src[i];
  sum = 0.0f;
  for (i = 0u; i < count; i++) {
    float v = src[i] - max_val;
    float e = ixg_expf(v);
    dst[i] = e; sum += e;
  }
  if (sum > 0.0f) { float inv = 1.0f / sum; for (i = 0u; i < count; i++) dst[i] *= inv; }
  return 0;
}