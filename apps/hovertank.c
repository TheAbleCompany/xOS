/*
 * ============================================================
 * Hovertank -- HoverTank3DClone ported to ArtyomX64
 * ============================================================
 *
 * Original: SFML C++ raycaster (HoverTank3DClone by the author of
 * ArtyomX64).  This is a faithful C rewrite for the OS's native
 * window API (libax) with NO external dependencies:
 *
 *   * SFML window     -> ax_surface() canvas + ax_poll events
 *   * sf::Texture     -> userspace PNG loader (apps/imgload)
 *   * createMaskFromColor(Black) -> alpha baked at asset-pipeline
 *                       time (see tools/hov_assets.py)
 *   * sf::Font TTF HUD -> OS 8x8 font (ax_text)
 *   * sf::Clock        -> TSC calibrated against the RTC via
 *                         ax_time (pure userspace, no new syscalls)
 *
 * Assets are loaded AT RUNTIME from the initrd by the userspace
 * image loader:
 *
 *     sanitar.png  192x100 RGBA   (2 walk frames, 96x100)
 *     chan.png     320x192 RGBA   (15 frames, 64x64)
 *     gameover.png 512x320 RGB
 *
 * If a file is missing the game still runs with flat-shaded
 * placeholder sprites.
 *
 * Controls (ArtyomX64 keyboard has no arrow-key mapping and no
 * key-release events, so movement is per-keystroke like
 * raycaster.c):
 *
 *     W / S   move forward / backward
 *     A / D   turn left / right
 *     Q / E   strafe left / right
 *     ESC     close window
 *     Game > New Game   restart
 *
 * Goal: pick up all 3 chans before "Dream time" runs out.
 * Every chan adds +5 seconds.  Touch a patrolling sanitar and
 * it is game over.
 *
 * Performance ("no lag" edition):
 *   * raycasting is a proper grid DDA -- each ray visits every
 *     cell ONCE (<= 19 steps on this map) instead of stepping
 *     0.02 units up to 5000 times, and the perpendicular wall
 *     distance comes out directly (no per-ray sqrt, no fisheye
 *     cos, no normalize)
 *   * one sin/cos pair per FRAME for the camera basis (was: two
 *     trig calls per ray, 400 per frame)
 *   * wall shading is a precomputed 16x64 palette LUT (was: 3
 *     float multiplies per ray per frame)
 *   * sprite scaling uses 16.16 fixed-point stepping (was: float
 *     mul+div per pixel)
 *   * sanitar sheet is 2 frames of 96x100 (was: soldier 16
 *     frames of 64x96) -- 76 KiB of .bss instead of 384 KiB,
 *     and no per-frame front/back pose selection math
 * ============================================================
 */

#ifdef ARTYOM_HOST_SHIM
#include "tests-host/shim_ax.h"
#else
#include "../lib/libax.h"
#include "../lib/unistd.h"
#endif

#include "imgload/imgload.h"

/* ============================================================
 * BASIC TYPES (freestanding)
 * ============================================================ */

typedef unsigned int   u32;
typedef int            s32;
typedef unsigned char  u8;

#define PI_F 3.14159265358979323846f

static float ax_fabsf(float x) { return x < 0.0f ? -x : x; }

static float ax_sqrtf(float x)
{
    if (x <= 0.0f)
        return 0.0f;

    float r = x;

    for (int i = 0; i < 12; i++)
        r = 0.5f * (r + x / r);

    return r;
}

/*
 * atan2 via minimax polynomial of atan on [0,1].
 * Error < 1e-6 rad -- far beyond raycasting needs.
 */
static float ax_atan2f(float y, float x)
{
    if (x == 0.0f && y == 0.0f)
        return 0.0f;

    float axx = ax_fabsf(x);
    float ayy = ax_fabsf(y);
    float a   = (axx < ayy) ? axx / ayy : ayy / axx;
    float s   = a * a;
    float r   = ((-0.0464964749f * s + 0.15931422f) * s
                 - 0.327622764f) * s * a + a;

    if (axx < ayy)
        r = PI_F / 2.0f - r;

    if (x < 0.0f)
        r = PI_F - r;

    if (y < 0.0f)
        r = -r;

    return r;
}

static float normalize_angle(float a)
{
    while (a >  PI_F) a -= 2.0f * PI_F;
    while (a < -PI_F) a += 2.0f * PI_F;

    return a;
}

/* sin/cos via polynomial (same trick as raycaster.c) */
static float ax_sinf_(float x)
{
    x = normalize_angle(x);

    float x2 = x * x;

    return x * (1.0f - x2 / 6.0f
                + (x2 * x2) / 120.0f
                - (x2 * x2 * x2) / 5040.0f);
}

static float ax_cosf_(float x)
{
    return ax_sinf_(x + PI_F / 2.0f);
}

/* ============================================================
 * TIMING -- TSC calibrated against the RTC (ax_time)
 * ============================================================ */

static inline unsigned long long rdtsc(void)
{
    unsigned hi, lo;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));

    return ((unsigned long long)hi << 32) | lo;
}

static unsigned long long tsc_hz = 0;        /* 0 = fallback mode */

static void calibrate_tsc(void)
{
    ax_time_t t0, t;

    ax_time(&t0);

    /*
     * Wait for up to 4 RTC second rollovers.  Measure TSC across
     * one full second (start on a rollover, stop on the next).
     */
    for (int guard = 0; guard < 40000; guard++) {
        ax_time(&t);

        if (t.second != t0.second)
            break;

        yield();
    }

    unsigned long long c0 = rdtsc();

    ax_time(&t0);

    for (int guard = 0; guard < 40000; guard++) {
        ax_time(&t);

        if (t.second != t0.second) {
            tsc_hz = rdtsc() - c0;
            return;
        }

        yield();
    }

    tsc_hz = 0;                              /* fallback: fixed dt */
}

/* ============================================================
 * CONFIG
 * ============================================================ */

#define RAYS          200
#define PROJECTION    300.0f
#define MAP_CELL_SIZE 1.0f

#define FRAME_FPS     20

#define PLAYER_START_X  1.5f
#define PLAYER_START_Y  1.5f

#define MOVE_STEP 0.15f
#define TURN_STEP 0.12f

#define CHAN_BONUS          5.0f
#define PICKUP_RADIUS       0.5f
#define PLAYER_RADIUS       0.2f
#define TIME_INITIAL        30.0f

static int CW = 640;
static int CH = 400;

/* ============================================================
 * MAP (identical to the original game)
 * ============================================================ */

static const u32 map_cells[10][9] = {
    { 1,  1,  1,  1,  1,  1,  1,  1,  1  },
    { 1,  0,  0,  0,  0,  0,  0,  0,  1  },
    { 1,  0,  2,  0,  3,  0,  4,  0,  1  },
    { 1,  0,  0,  0,  0,  0,  0,  0,  1  },
    { 1,  0,  5,  0,  6,  0,  7,  0,  1  },
    { 1,  0,  0,  0,  0,  0,  0,  0,  1  },
    { 1,  0,  8,  0,  9,  0, 10,  0,  1  },
    { 1,  0,  0,  0,  0,  0,  0,  0,  1  },
    { 1,  0, 11,  0, 12,  0, 13,  0,  1  },
    { 1,  1,  1,  1,  1,  1,  1,  1,  1  }
};

#define MAP_W 9
#define MAP_H 10

static u32 get_map_cell(int y, int x)
{
    if (y < 0 || y >= MAP_H || x < 0 || x >= MAP_W)
        return 1;

    return map_cells[y][x];
}

static int is_walkable(float x, float y)
{
    return get_map_cell((int)(y / MAP_CELL_SIZE),
                        (int)(x / MAP_CELL_SIZE)) == 0;
}

/* ============================================================
 * PLAYER / STATE
 * ============================================================ */

static struct {
    float x, y, angle, fov;
} player;

static int   game_over   = 0;
static int   game_won    = 0;
static float time_left   = TIME_INITIAL;
static int   chans_picked = 0;
static int   chans_total = 3;

static void reset_player(void)
{
    player.x     = PLAYER_START_X;
    player.y     = PLAYER_START_Y;
    player.angle = 0.0f;
    player.fov   = PI_F / 4.0f;
}

/* ============================================================
 * HOVERTANK WALL PALETTE (4-bit codes, from the original)
 * ============================================================ */

static const u32 wall_palette[16] = {
    AX_RGB(0,     0,   0),   /* 0  */
    AX_RGB(0,     0, 170),   /* 1  */
    AX_RGB(0,   170,   0),   /* 2  */
    AX_RGB(0,   170, 170),   /* 3  */
    AX_RGB(170,   0,   0),   /* 4  */
    AX_RGB(170,   0, 170),   /* 5  */
    AX_RGB(170,  85,   0),   /* 6  */
    AX_RGB(170, 170, 170),   /* 7  */
    AX_RGB(85,   85,  85),   /* 8  */
    AX_RGB(85,   85, 255),   /* 9  */
    AX_RGB(85,  255,  85),   /* 10 */
    AX_RGB(85,  255, 255),   /* 11 */
    AX_RGB(255,  85,  85),   /* 12 */
    AX_RGB(255,  85, 255),   /* 13 */
    AX_RGB(255, 255,  85),   /* 14 */
    AX_RGB(255, 255, 255)    /* 15 */
};

static u32 wall_color_dim(u32 code, float distance)
{
    code &= 0x0Fu;
    u32 base = wall_palette[code];

    float brightness =
        1.0f - (distance / 15.0f > 1.0f ? 1.0f
                                        : distance / 15.0f);

    brightness = 0.25f + brightness * 0.75f;

    u32 r = (u32)(((base >> 16) & 0xFF) * brightness);
    u32 g = (u32)(((base >>  8) & 0xFF) * brightness);
    u32 b = (u32)((base & 0xFF) * brightness);

    return AX_RGB(r, g, b);
}

/*
 * Precomputed shading table: wall_lut[code][q], q = distance
 * quantised to SHADE_STEPS over [0, 15].  ~4k entries built once
 * turn the per-ray float shading into a single lookup.
 */
#define SHADE_STEPS 64

static u32 wall_lut[16][SHADE_STEPS];

static void build_wall_lut(void)
{
    for (u32 code = 0; code < 16; code++)
        for (int q = 0; q < SHADE_STEPS; q++)
            wall_lut[code][q] =
                wall_color_dim(code, (float)q * (15.0f / SHADE_STEPS));
}

static u32 wall_color_at(u32 wall, float distance)
{
    int q = (int)(distance * ((float)SHADE_STEPS / 15.0f));

    if (q < 0)
        q = 0;

    if (q >= SHADE_STEPS)
        q = SHADE_STEPS - 1;

    return wall_lut[wall & 0x0F][q];
}

/* ============================================================
 * SPRITES
 * ============================================================ */

static uint32_t sanitar_px[192 * 100];       /*  76 KiB */
static uint32_t chan_px[320 * 192];          /* 240 KiB */
static uint32_t gover_px[512 * 320];         /* 640 KiB */

static img_image_t sanitar_img;
static img_image_t chan_img;
static img_image_t gover_img;

static int sanitar_ok = 0;
static int chan_ok    = 0;
static int gover_ok   = 0;

#define SANITAR_FRAME_W 96
#define SANITAR_FRAME_H 100
#define CHAN_FRAME_W    64
#define CHAN_FRAME_H    64

typedef struct {
    int  sx, sy, sw, sh;                     /* frame rect in sheet */
} frame_rect_t;

static frame_rect_t frames[2 + 15];

#define FR_SANITAR       0                    /* 0..1 (2 walk frames) */
#define FR_CHAN          2                    /* 2..16                */

typedef struct {
    float x, y, height;

    int   tex;                               /* 0 sanitar, 1 chan  */
    int   front_first, front_n;
    int   cur;

    float frame_dur, frame_timer;

    int   is_enemy;

    float move_dir, move_speed;
    float patrol_min, patrol_max;
    float angle;

    int   alive;
} gsprite_t;

#define MAX_SPRITES 8

static gsprite_t sprites[MAX_SPRITES];
static int       sprite_count = 0;

static float z_buffer[RAYS];

static void build_frame_table(void)
{
    frames[FR_SANITAR + 0].sx = 0;
    frames[FR_SANITAR + 0].sy = 0;
    frames[FR_SANITAR + 0].sw = SANITAR_FRAME_W;
    frames[FR_SANITAR + 0].sh = SANITAR_FRAME_H;

    frames[FR_SANITAR + 1].sx = SANITAR_FRAME_W;
    frames[FR_SANITAR + 1].sy = 0;
    frames[FR_SANITAR + 1].sw = SANITAR_FRAME_W;
    frames[FR_SANITAR + 1].sh = SANITAR_FRAME_H;

    for (int j = 0; j < 5; j++) {
        for (int k = 0; k < 3; k++) {
            int n  = FR_CHAN + k * 5 + j;
            frames[n].sx = j * CHAN_FRAME_W;
            frames[n].sy = k * CHAN_FRAME_H;
            frames[n].sw = CHAN_FRAME_W;
            frames[n].sh = CHAN_FRAME_H;
        }
    }
}

static const frame_rect_t *sprite_frame(const gsprite_t *s, int *sheet_w,
                                        int *sheet_h, uint32_t **sheet)
{
    if (s->tex == 0) {
        *sheet   = sanitar_ok ? sanitar_img.pix : 0;
        *sheet_w = sanitar_ok ? sanitar_img.w : SANITAR_FRAME_W;
        *sheet_h = sanitar_ok ? sanitar_img.h : SANITAR_FRAME_H;

        /* sanitars always walk facing the player: single frame row */
        return &frames[FR_SANITAR +
                       s->cur % (s->front_n > 0 ? s->front_n : 1)];
    }

    *sheet   = chan_ok ? chan_img.pix : 0;
    *sheet_w = chan_ok ? chan_img.w : CHAN_FRAME_W;
    *sheet_h = chan_ok ? chan_img.h : CHAN_FRAME_H;

    return &frames[FR_CHAN + s->cur % (s->front_n > 0 ? s->front_n : 1)];
}

/* sin/cos helpers live at the top of the file (ax_sinf_/ax_cosf_) */

static void sprite_update(gsprite_t *s, float dt)
{
    int n = s->front_n;

    if (n <= 1)
        return;

    s->frame_timer += dt;

    while (s->frame_timer >= s->frame_dur) {
        s->frame_timer -= s->frame_dur;
        s->cur++;

        if (s->cur >= n)
            s->cur = 0;
    }
}

static void enemy_update(gsprite_t *e, float dt)
{
    if (!e->is_enemy || !e->alive)
        return;

    e->x += e->move_dir * e->move_speed * dt;

    e->angle = (e->move_dir > 0.0f) ? 0.0f : PI_F;

    if (e->x <= e->patrol_min) {
        e->x = e->patrol_min;
        e->move_dir = 1.0f;
        e->angle = 0.0f;
    }

    if (e->x >= e->patrol_max) {
        e->x = e->patrol_max;
        e->move_dir = -1.0f;
        e->angle = PI_F;
    }
}

static void move_player(float dx, float dy)
{
    float nx = player.x + dx;
    float cx = nx + (dx >= 0.0f ? PLAYER_RADIUS : -PLAYER_RADIUS);

    if (is_walkable(cx, player.y) &&
        is_walkable(cx, player.y + PLAYER_RADIUS) &&
        is_walkable(cx, player.y - PLAYER_RADIUS))
        player.x = nx;

    float ny = player.y + dy;
    float cy = ny + (dy >= 0.0f ? PLAYER_RADIUS : -PLAYER_RADIUS);

    if (is_walkable(player.x, cy) &&
        is_walkable(player.x + PLAYER_RADIUS, cy) &&
        is_walkable(player.x - PLAYER_RADIUS, cy))
        player.y = ny;
}

/* ============================================================
 * RAYCASTING
 *
 * Camera-plane DDA (grid walk).  The old version stepped every
 * ray 0.02 units up to 5000 times (1M iterations per frame worst
 * case, each with a cell lookup + float math) and needed a
 * 12-iteration sqrt + fisheye cos per ray.  A DDA visits each
 * cell at most once -- <= 19 steps on this 9x10 map -- and the
 * perpendicular wall distance falls out directly, so wall
 * projection needs no sqrt, no cos and no fisheye correction
 * at all.
 * ============================================================ */

#define TAN_HALF_FOV 0.41421356f              /* tan(fov/2), fov = 45 deg */

static float cam_x[RAYS];                     /* camera-plane pos of ray i */

static void build_camera_table(void)
{
    for (int i = 0; i < RAYS; i++)
        cam_x[i] = 2.0f * (float)i / (float)(RAYS - 1) - 1.0f;
}

static int screen_x_to_ray(int screen_x)
{
    int ray = (int)((float)screen_x / (float)CW * (float)RAYS);

    if (ray < 0)       ray = 0;
    if (ray >= RAYS)   ray = RAYS - 1;

    return ray;
}

static void draw_wall_slice(int x, float wall_height, int slice_w, u32 color)
{
    int y = CH / 2 - (int)(wall_height / 2.0f);
    int h = (int)wall_height;

    if (y < 0) {
        h += y;
        y = 0;
    }

    if (y + h > CH)
        h = CH - y;

    if (h <= 0)
        return;

    if (x + slice_w > CW)
        slice_w = CW - x;

    if (slice_w <= 0)
        return;

    /* direct column fill into the canvas (much faster than ax_rect) */
    for (int j = 0; j < h; j++) {
        uint32_t *row = g_canvas + (y + j) * g_cw + x;

        for (int i = 0; i < slice_w; i++)
            row[i] = color;
    }
}

static void raycast_render(void)
{
    const float slice_w = (float)CW / (float)RAYS;

    /*
     * Camera basis: ONE sin/cos pair per frame (the old code did
     * two polynomial trig calls per ray, ~400 per frame).  Ray i
     * is then dir + plane * cam_x[i] with two multiplies.
     */
    float dir_x   = ax_cosf_(player.angle);
    float dir_y   = ax_sinf_(player.angle);
    float plane_x = -dir_y * TAN_HALF_FOV;
    float plane_y =  dir_x * TAN_HALF_FOV;

    int pmap_x = (int)player.x;
    int pmap_y = (int)player.y;

    for (int i = 0; i < RAYS; i++) {
        float rdx = dir_x + plane_x * cam_x[i];
        float rdy = dir_y + plane_y * cam_x[i];

        float delta_x = (rdx == 0.0f) ? 1e30f : ax_fabsf(1.0f / rdx);
        float delta_y = (rdy == 0.0f) ? 1e30f : ax_fabsf(1.0f / rdy);

        int   step_x, step_y;
        float side_x, side_y;

        if (rdx < 0.0f) {
            step_x = -1;
            side_x = (player.x - (float)pmap_x) * delta_x;
        } else {
            step_x = 1;
            side_x = ((float)pmap_x + 1.0f - player.x) * delta_x;
        }

        if (rdy < 0.0f) {
            step_y = -1;
            side_y = (player.y - (float)pmap_y) * delta_y;
        } else {
            step_y = 1;
            side_y = ((float)pmap_y + 1.0f - player.y) * delta_y;
        }

        int map_x = pmap_x;
        int map_y = pmap_y;

        u32 wall = 0;
        int side = 0;

        /*
         * The map is 9x10 and fully walled in, so the walk always
         * terminates within 19 crossings; 64 is a belt-and-braces
         * guard that can never fire.
         */
        for (int guard = 0; guard < 64; guard++) {
            if (side_x < side_y) {
                side_x += delta_x;
                map_x  += step_x;
                side    = 0;
            } else {
                side_y += delta_y;
                map_y  += step_y;
                side    = 1;
            }

            wall = get_map_cell(map_y, map_x);

            if (wall)
                break;
        }

        float dist = (side == 0) ? side_x - delta_x : side_y - delta_y;

        if (dist < 0.01f)
            dist = 0.01f;

        z_buffer[i] = dist;

        float wall_h = PROJECTION / dist;

        if (wall_h > (float)CH)
            wall_h = (float)CH;

        int x0 = (int)(i * slice_w);
        int x1 = (int)((i + 1) * slice_w);

        draw_wall_slice(x0, wall_h, x1 - x0 + 1,
                        wall_color_at(wall, dist));
    }
}

/* ============================================================
 * ALPHA-AWARE SCALED BLIT
 * ============================================================ */

static void blit_scaled(const uint32_t *sheet, int sheet_w, int /*sheet_h*/,
                        const frame_rect_t *fr,
                        float dxf, float dyf, float dwf, float dhf)
{
    int dx = (int)dxf;
    int dy = (int)dyf;
    int dw = (int)dwf;
    int dh = (int)dhf;

    if (dw <= 0 || dh <= 0)
        return;

    int x0 = dx < 0 ? 0 : dx;
    int y0 = dy < 0 ? 0 : dy;
    int x1 = dx + dw > CW ? CW : dx + dw;
    int y1 = dy + dh > CH ? CH : dy + dh;

    if (x0 >= x1 || y0 >= y1)
        return;

    /*
     * 16.16 fixed-point stepping: the old inner loop did a float
     * multiply + divide + cast PER PIXEL; this one does an add
     * and a shift.  (x0 - dx) / (y0 - dy) are never negative:
     * x0 = max(dx, 0) etc.
     */
    u32 step_x = ((u32)fr->sw << 16) / (u32)dw;
    u32 step_y = ((u32)fr->sh << 16) / (u32)dh;

    const int sx0 = fr->sx;
    const int sy0 = fr->sy;
    const int sxm = fr->sx + fr->sw - 1;
    const int sym = fr->sy + fr->sh - 1;

    u32 ty = (u32)(y0 - dy) * step_y;

    for (int y = y0; y < y1; y++) {
        int syy = sy0 + (int)(ty >> 16);

        ty += step_y;

        if (syy > sym)
            syy = sym;

        const uint32_t *src = sheet + syy * sheet_w;
        uint32_t *dstrow = g_canvas + y * g_cw;

        u32 tx = (u32)(x0 - dx) * step_x;

        for (int x = x0; x < x1; x++) {
            int sxx = sx0 + (int)(tx >> 16);

            tx += step_x;

            if (sxx > sxm)
                sxx = sxm;

            u32 c = src[sxx];
            u32 a = (c >> 24) & 0xFF;

            if (a < 16)
                continue;                    /* fully transparent */

            if (a >= 240) {
                dstrow[x] = AX_RGB((c >> 16) & 0xFF,
                                   (c >> 8) & 0xFF,
                                   c & 0xFF);
            } else {
                u32 dr = dstrow[x];
                u32 br_ = ((dr >> 16) & 0xFF);
                u32 bg_ = ((dr >> 8) & 0xFF);
                u32 bb_ = (dr & 0xFF);

                u32 r = ((c >> 16) & 0xFF) * a + br_ * (255 - a) + 127;
                u32 g = ((c >> 8) & 0xFF) * a + bg_ * (255 - a) + 127;
                u32 b = (c & 0xFF) * a + bb_ * (255 - a) + 127;

                dstrow[x] = AX_RGB(r >> 8, g >> 8, b >> 8);
            }
        }
    }
}

/* ============================================================
 * SPRITE DRAW (billboard projection, z-buffered)
 * ============================================================ */

static void draw_sprite_world(const gsprite_t *obj)
{
    float dx = obj->x - player.x;
    float dy = obj->y - player.y;
    float distance = ax_sqrtf(dx * dx + dy * dy);

    if (distance < 0.05f)
        return;

    float obj_angle = ax_atan2f(dy, dx);
    float rel = normalize_angle(obj_angle - player.angle);
    float half_fov = player.fov / 2.0f;

    if (ax_fabsf(rel) > half_fov + 0.5f)
        return;

    uint32_t *sheet = 0;
    int sheet_w = 0, sheet_h = 0;
    const frame_rect_t *fr;

    if (!obj->alive)
        return;

    if (obj->tex == 0 && !sanitar_ok) {
        /* placeholder box */
        float ph = PROJECTION * obj->height / distance;

        if (ph < 1.0f || ph > CH * 1.5f) ph = CH * 1.5f;

        float pw = ph * 0.6f;
        float floor_y = CH / 2.0f + (PROJECTION * 0.5f) / distance;
        float top = floor_y - ph;
        float sx = CW / 2.0f + (rel / half_fov) * (CW / 2.0f) - pw / 2.0f;

        int ray = screen_x_to_ray((int)sx);

        if (ray >= 0 && ray < RAYS && z_buffer[ray] < distance * 0.9f)
            return;

        ax_rect((int)sx, (int)top, (int)pw, (int)ph,
                obj->is_enemy ? AX_RGB(170, 0, 0) : AX_RGB(0, 170, 170));
        return;
    }

    if (obj->tex == 1 && !chan_ok) {
        float ph = PROJECTION * obj->height / distance;

        if (ph < 1.0f || ph > CH * 1.5f) ph = CH * 1.5f;

        float pw = ph * 0.6f;
        float floor_y = CH / 2.0f + (PROJECTION * 0.5f) / distance;
        float top = floor_y - ph;
        float sx = CW / 2.0f + (rel / half_fov) * (CW / 2.0f) - pw / 2.0f;

        int ray = screen_x_to_ray((int)sx);

        if (ray >= 0 && ray < RAYS && z_buffer[ray] < distance * 0.9f)
            return;

        ax_rect((int)sx, (int)top, (int)pw, (int)ph,
                AX_RGB(0, 170, 170));
        return;
    }

    fr = sprite_frame(obj, &sheet_w, &sheet_h, &sheet);

    if (!sheet || fr->sw <= 0 || fr->sh <= 0)
        return;

    float screen_cx = CW / 2.0f;
    float screen_x  = screen_cx + (rel / half_fov) * screen_cx;

    float proj_h = PROJECTION * obj->height / distance;

    if (proj_h < 1.0f)
        return;

    if (proj_h > CH * 1.5f)
        proj_h = CH * 1.5f;

    float aspect = (float)fr->sw / (float)fr->sh;
    float proj_w = proj_h * aspect;

    float floor_y = CH / 2.0f + (PROJECTION * 0.5f) / distance;
    float top = floor_y - proj_h;

    int center_ray = screen_x_to_ray((int)screen_x);

    if (center_ray >= 0 && center_ray < RAYS)
        if (z_buffer[center_ray] < distance * 0.9f)
            return;

    blit_scaled(sheet, sheet_w, sheet_h, fr,
                screen_x - proj_w / 2.0f, top, proj_w, proj_h);
}

/* ============================================================
 * GAME SETUP
 * ============================================================ */

static gsprite_t *spawn_sanitar(float x, float y, float speed,
                                float pmin, float pmax, float dir)
{
    if (sprite_count >= MAX_SPRITES)
        return 0;

    gsprite_t *s = &sprites[sprite_count++];

    s->x = x;
    s->y = y;
    s->height = 1.0f;
    s->tex = 0;
    s->front_first = FR_SANITAR;
    s->front_n = 2;
    s->cur = 0;
    s->frame_dur = 0.35f;
    s->frame_timer = 0.0f;
    s->is_enemy = 1;
    s->move_dir = dir;
    s->move_speed = speed;
    s->patrol_min = pmin;
    s->patrol_max = pmax;
    s->angle = (dir >= 0.0f) ? 0.0f : PI_F;
    s->alive = 1;

    return s;
}

static gsprite_t *spawn_chan(float x, float y, float timer_offset)
{
    if (sprite_count >= MAX_SPRITES)
        return 0;

    gsprite_t *s = &sprites[sprite_count++];

    s->x = x;
    s->y = y;
    s->height = 1.0f;
    s->tex = 1;
    s->front_first = FR_CHAN;
    s->front_n = 15;
    s->cur = 0;
    s->frame_dur = 0.2f;
    s->frame_timer = timer_offset;
    s->is_enemy = 0;
    s->move_dir = 0.0f;
    s->move_speed = 0.0f;
    s->patrol_min = 0.0f;
    s->patrol_max = 0.0f;
    s->angle = 0.0f;
    s->alive = 1;

    return s;
}

static void new_game(void)
{
    reset_player();

    sprite_count = 0;
    game_over = 0;
    game_won = 0;
    time_left = TIME_INITIAL;
    chans_picked = 0;

    /* sanitar 1 (same slot as in the original main.cpp) */
    spawn_sanitar(4.5f, 1.5f, 0.5f, 3.0f, 6.0f, 1.0f);

    /* sanitar 2, walk-cycle phase shifted so they don't step in sync */
    gsprite_t *e2 = spawn_sanitar(6.5f, 3.5f, 0.35f, 5.0f, 7.5f, -1.0f);

    if (e2) {
        e2->cur = 1;
        e2->frame_timer = 0.15f;
    }

    /* three chans (same positions as the original) */
    spawn_chan(2.5f, 7.5f, 0.00f);
    spawn_chan(7.5f, 1.5f, 0.05f);
    spawn_chan(3.5f, 5.5f, 0.12f);
}

/* ============================================================
 * ASSET LOADING (userspace image loader)
 * ============================================================ */

static int load_assets(void)
{
    sanitar_ok = (img_load_file("sanitar.png",
                                sanitar_px, 192 * 100,
                                &sanitar_img) == IMG_OK);

    chan_ok = (img_load_file("chan.png",
                             chan_px, 320 * 192,
                             &chan_img) == IMG_OK);

    gover_ok = (img_load_file("gameover.png",
                              gover_px, 512 * 320,
                              &gover_img) == IMG_OK);

    return sanitar_ok && chan_ok && gover_ok;
}

/* ============================================================
 * SCREENS
 * ============================================================ */

static void text_shadow(const char *s, int x, int y, u32 c)
{
    /* soft shadow + 1px outline so text survives bright backgrounds */
    ax_text(s, x + 1, y + 1, AX_RGB(0, 0, 0));
    ax_text(s, x, y, c);
}

static void text_outline(const char *s, int x, int y, u32 c)
{
    ax_text(s, x - 1, y, AX_RGB(0, 0, 0));
    ax_text(s, x + 1, y, AX_RGB(0, 0, 0));
    ax_text(s, x, y - 1, AX_RGB(0, 0, 0));
    ax_text(s, x, y + 1, AX_RGB(0, 0, 0));
    ax_text(s, x, y, c);
}

static void draw_centered(const char *s, int y, u32 c)
{
    int w = 8 * (int)img_strlen(s);

    text_shadow(s, CW / 2 - w / 2, y, c);
}

static void draw_centered_o(const char *s, int y, u32 c)
{
    int w = 8 * (int)img_strlen(s);

    text_outline(s, CW / 2 - w / 2, y, c);
}

static void render_loading(void)
{
    ax_fill(AX_RGB(12, 12, 16));
    draw_centered("Hovertank", CH / 2 - 24, AX_RGB(255, 255, 255));
    draw_centered("loading sprites (userspace imgload)...", CH / 2 + 8,
                  AX_RGB(140, 140, 150));
    ax_commit(g_canvas);
}

static void render_hud(void)
{
    char buf[64];
    int  secs = (int)time_left;

    if (secs < 0)
        secs = 0;

    /* simple int -> string */
    int n = 0;

    if (secs == 0) {
        buf[n++] = '0';
    } else {
        char tmp[12];
        int  t = 0, v = secs;

        while (v) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }

        while (t)
            buf[n++] = tmp[--t];
    }

    buf[n] = 0;

    char line[64] = "Dream time: ";
    int  p = 12;

    for (int i = 0; buf[i]; i++)
        line[p++] = buf[i];

    line[p] = 0;

    text_shadow(line, 8, 8, AX_RGB(255, 255, 255));

    text_shadow("W/S move  A/D turn  Q/E strafe  ESC exit",
                8, CH - 12, AX_RGB(150, 150, 160));
}

static void render_world(void)
{
    ax_fill(AX_RGB(0, 0, 0));

    raycast_render();

    /* draw sprites far -> near (insertion sort, <= MAX_SPRITES) */
    int order[MAX_SPRITES];

    int n = 0;

    for (int i = 0; i < sprite_count; i++)
        if (sprites[i].alive)
            order[n++] = i;

    for (int i = 1; i < n; i++) {
        int key = order[i];
        float kd =
            (sprites[key].x - player.x) * (sprites[key].x - player.x) +
            (sprites[key].y - player.y) * (sprites[key].y - player.y);

        int j = i - 1;

        while (j >= 0) {
            float od =
                (sprites[order[j]].x - player.x) *
                    (sprites[order[j]].x - player.x) +
                (sprites[order[j]].y - player.y) *
                    (sprites[order[j]].y - player.y);

            if (od > kd)
                break;                       /* far elements stay front */

            order[j + 1] = order[j];
            j--;
        }

        order[j + 1] = key;
    }

    for (int i = 0; i < n; i++)
        draw_sprite_world(&sprites[order[i]]);

    render_hud();
}

static void render_game_over(void)
{
    if (gover_ok) {
        /* stretch 512x320 over the whole window */
        for (int y = 0; y < CH; y++) {
            int syy = y * gover_img.h / CH;

            const uint32_t *src = gover_img.pix + syy * gover_img.w;
            uint32_t *dstrow = g_canvas + y * g_cw;

            for (int x = 0; x < CW; x++) {
                int sxx = x * gover_img.w / CW;

                u32 c = src[sxx];

                dstrow[x] = AX_RGB((c >> 16) & 0xFF,
                                   (c >> 8) & 0xFF,
                                   c & 0xFF);
            }
        }
    } else {
        ax_fill(AX_RGB(60, 0, 0));
    }

    draw_centered_o("GAME OVER", CH / 2 - 20, AX_RGB(200, 16, 16));
    draw_centered_o("Press ESC to exit", CH / 2 + 8, AX_RGB(255, 255, 255));
}

static void render_win(void)
{
    ax_fill(AX_RGB(6, 40, 12));

    draw_centered("YOU ESCAPED!", CH / 2 - 40, AX_RGB(120, 255, 140));
    draw_centered("You avoided the fate to be caught by sanitars",
                  CH / 2, AX_RGB(230, 255, 230));
    draw_centered("and successfully ran away from the durka.",
                  CH / 2 + 14, AX_RGB(230, 255, 230));
    draw_centered("Return to the real world!",
                  CH / 2 + 44, AX_RGB(255, 255, 255));
    draw_centered("Press ESC to exit", CH / 2 + 76,
                  AX_RGB(180, 220, 180));
}

/* ============================================================
 * MENU (macOS model: the app owns its menus)
 * ============================================================ */

static const char *game_menu_items[] = { "New Game", "Close Window" };

static const ax_menu_def_t ht_menus[] = {
    { "Game", game_menu_items, 2 },
};

static const ax_menubar_def_t ht_menubar = {
    "Hovertank",
    ht_menus,
    1
};

/* ============================================================
 * MAIN
 * ============================================================ */

#ifdef ARTYOM_HOST_SHIM
#define APP_ENTRY int game_main(void)
#define APP_EXIT() return 0
#else
#define APP_ENTRY void _start(void)
#define APP_EXIT() return
#endif

APP_ENTRY
{
    /* ---- window (adaptive 640x400) ---- */
    ax_screen_t scr;

    scr.width = 800;
    scr.height = 600;

    ax_screen(&scr);

    int sw = (int)scr.width;
    int sh = (int)scr.height;

    CW = 640;
    CH = 400;

    if (CW > sw - 24)
        CW = sw - 24;

    if (CH > sh - 130)
        CH = sh - 130;

    if (CW < 320)
        CW = 320;

    if (CH < 200)
        CH = 200;

    ax_menu_set(&ht_menubar);

    g_canvas = ax_surface("Hovertank", CW, CH);
    ax_canvas_dims(CW, CH);

    if (!g_canvas)
        for (;;)
            yield();

    render_loading();

    build_frame_table();
    build_camera_table();
    build_wall_lut();

    /* ---- timing ---- */
    calibrate_tsc();

    /* ---- assets ---- */
    load_assets();

    new_game();

    /* ---- main loop ---- */
    ax_event ev;
    unsigned long long last_tsc = rdtsc();
    unsigned long long next_frame = last_tsc;
    unsigned long long frame_ticks =
        tsc_hz ? tsc_hz / FRAME_FPS : 0;
    unsigned long long fallback_spin = 0;

    for (;;) {
        int had_key = 0;

        while (ax_poll(g_canvas, &ev) > 0) {
            if (ev.type == AX_EV_CLOSE ||
                ev.type == AX_EV_QUIT)
                APP_EXIT();

            if (ev.type == AX_EV_MENU) {
                if (ev.key == 0) {           /* Game */
                    if (ev.w == 0) {         /* New Game */
                        new_game();
                        last_tsc = rdtsc();
                        next_frame = last_tsc;
                    } else if (ev.w == 1) {  /* Close Window */
                        ax_win_close(g_canvas);
                        APP_EXIT();
                    }
                }

                continue;
            }

            if (ev.type == AX_EV_KEY) {
                had_key = 1;

                if (ev.key == 27) {          /* ESC */
                    ax_win_close(g_canvas);
                    APP_EXIT();
                }

                if (game_over || game_won)
                    continue;

                switch (ev.key) {
                    case 'w': case 'W':
                        move_player(
                            ax_cosf_(player.angle) * MOVE_STEP,
                            ax_sinf_(player.angle) * MOVE_STEP);
                        break;

                    case 's': case 'S':
                        move_player(
                            -ax_cosf_(player.angle) * MOVE_STEP,
                            -ax_sinf_(player.angle) * MOVE_STEP);
                        break;

                    case 'a': case 'A':
                        player.angle -= TURN_STEP;
                        break;

                    case 'd': case 'D':
                        player.angle += TURN_STEP;
                        break;

                    case 'q': case 'Q':
                        move_player(
                            ax_cosf_(player.angle - PI_F / 2.0f) *
                                MOVE_STEP,
                            ax_sinf_(player.angle - PI_F / 2.0f) *
                                MOVE_STEP);
                        break;

                    case 'e': case 'E':
                        move_player(
                            ax_cosf_(player.angle + PI_F / 2.0f) *
                                MOVE_STEP,
                            ax_sinf_(player.angle + PI_F / 2.0f) *
                                MOVE_STEP);
                        break;

                    default:
                        break;
                }
            }
        }

        if (game_over) {
            render_game_over();
            ax_commit(g_canvas);

            /* static screen: wait for the next event */
            int r;

            while ((r = ax_poll(g_canvas, &ev)) == 0)
                yield();

            if (r < 0)
                APP_EXIT();                      /* window is gone */

            continue;
        }

        if (game_won) {
            render_win();
            ax_commit(g_canvas);

            int r;

            while ((r = ax_poll(g_canvas, &ev)) == 0)
                yield();

            if (r < 0)
                APP_EXIT();

            continue;
        }

        /* ---- dt ---- */
        unsigned long long now = rdtsc();
        float dt;

        if (tsc_hz) {
            dt = (float)(now - last_tsc) / (float)tsc_hz;

            /* clamp huge stalls (serial console, etc.) */
            if (dt > 0.1f)
                dt = 0.1f;
        } else {
            dt = 1.0f / (float)FRAME_FPS;
        }

        last_tsc = now;

        /* ---- update ---- */
        time_left -= dt;

        if (time_left <= 0.0f) {
            time_left = 0.0f;
            game_over = 1;
            continue;
        }

        for (int i = 0; i < sprite_count; i++) {
            sprite_update(&sprites[i], dt);
            enemy_update(&sprites[i], dt);
        }

        /* ---- chan pickup / enemy collision ---- */
        for (int i = 0; i < sprite_count && !game_over; i++) {
            if (!sprites[i].alive)
                continue;

            float dx = sprites[i].x - player.x;
            float dy = sprites[i].y - player.y;
            float d  = ax_sqrtf(dx * dx + dy * dy);

            if (d >= PICKUP_RADIUS)
                continue;

            if (sprites[i].is_enemy) {
                game_over = 1;
            } else {
                sprites[i].alive = 0;
                time_left += CHAN_BONUS;
                chans_picked++;
            }
        }

        if (chans_picked >= chans_total) {
            game_won = 1;
            continue;
        }

        /* ---- render + commit ---- */
        render_world();
        ax_commit(g_canvas);

        (void)had_key;

        /* ---- frame pacing ---- */
        if (tsc_hz && frame_ticks) {
            next_frame += frame_ticks;

            unsigned long long t = rdtsc();

            while (t < next_frame) {
                yield();
                t = rdtsc();
            }

            if (next_frame < t)              /* fell behind: resync */
                next_frame = t;
        } else {
            /* no TSC: crude fixed delay via calibrated busy-wait */
            for (volatile int i = 0; i < 200000; i++)
                ;

            fallback_spin++;
        }
    }
}
