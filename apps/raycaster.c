#include "../lib/libax.h"
#include "../lib/unistd.h"

typedef unsigned long long u64int;
typedef long long          s64int;
typedef unsigned int       u32int;
typedef int                s32int;
typedef unsigned short     u16int;
typedef short              s16int;
typedef unsigned char      u8int;
typedef char               s8int;

#define CW 480
#define CH 300

#define MAP_W 9
#define MAP_H 10

#define RAYS 200

#define PI 3.14159265358979323846f

struct Player {
    float x;
    float y;
    float angle;
    float fov;
};

static struct Player player = {
    1.5f,
    1.5f,
    0.0f,
    PI / 4.0f
};

static const int map[MAP_H][MAP_W] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 1, 0, 1, 0, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 1, 1, 0, 1, 0, 0, 1},
    {1, 0, 1, 0, 0, 1, 0, 0, 1},
    {1, 0, 0, 1, 0, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 1, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1}
};

#define PI 3.14159265358979323846f

static float ax_sin(float x)
{
    while (x > PI)
        x -= 2.0f * PI;

    while (x < -PI)
        x += 2.0f * PI;

    /*
     * Быстрая полиномиальная аппроксимация.
     *
     * Точности более чем достаточно для raycaster.
     */
    float x2 = x * x;

    return x *
        (1.0f
        - x2 / 6.0f
        + (x2 * x2) / 120.0f
        - (x2 * x2 * x2) / 5040.0f);
}

static float ax_cos(float x)
{
    return ax_sin(x + PI / 2.0f);
}

static float ax_sqrt(float x)
{
    if (x <= 0.0f)
        return 0.0f;

    float r = x;

    for (int i = 0; i < 8; i++)
        r = 0.5f * (r + x / r);

    return r;
}

static int getMapCell(int y, int x)
{
    if (y < 0 || y >= MAP_H ||
        x < 0 || x >= MAP_W)
        return 1;

    return map[y][x];
}

static float castRay(float rayAngle)
{
    float x = player.x;
    float y = player.y;

    float dx = ax_cos(rayAngle);
    float dy =ax_sin(rayAngle);

    int i = 0;

    while (getMapCell((int)y, (int)x) == 0) {
        x += dx * 0.1f;
        y += dy * 0.1f;

        i++;

        if (i > 400)
            break;
    }

    float distanceX = x - player.x;
    float distanceY = y - player.y;

    float distance =
        __builtin_sqrtf(
            distanceX * distanceX +
            distanceY * distanceY
        );

    if (distance < 0.01f)
        distance = 0.01f;

    return 300.0f / distance;
}

static void drawWallSlice(
    int x,
    float wallHeight,
    int sliceWidth
)
{
    int y =
        CH / 2 -
        (int)(wallHeight / 2.0f);

    int h = (int)wallHeight;

    if (y < 0) {
        h += y;
        y = 0;
    }

    if (y + h > CH)
        h = CH - y;

    if (h <= 0)
        return;

    ax_rect(
        x,
        y,
        sliceWidth,
        h,
        AX_RGB(180, 0, 180)
    );
}

static void raycast(void)
{
    float sliceWidth =
        (float)CW / (float)RAYS;

    float angleStep =
        player.fov / (float)RAYS;

    for (int i = 0; i < RAYS; i++) {

        float rayAngle =
            player.angle
            - player.fov / 2.0f
            + i * angleStep;

        float wallHeight =
            castRay(rayAngle);

        int x =
            (int)(i * sliceWidth);

        int nextX =
            (int)((i + 1) * sliceWidth);

        int width =
            nextX - x;

        if (width < 1)
            width = 1;

        drawWallSlice(
            x,
            wallHeight,
            width
        );
    }
}

static void render(void)
{
    /*
     * Чёрный фон.
     */
    ax_fill(
        AX_RGB(0, 0, 0)
    );

    /*
     * Небо.
     */
    ax_rect(
        0,
        0,
        CW,
        CH / 2,
        AX_RGB(20, 20, 40)
    );

    /*
     * Пол.
     */
    ax_rect(
        0,
        CH / 2,
        CW,
        CH / 2,
        AX_RGB(35, 25, 20)
    );

    /*
     * Сам raycaster.
     */
    raycast();
}

void _start(void)
{
    g_canvas =
        ax_surface(
            "Raycaster",
            CW,
            CH
        );

    ax_canvas_dims(CW, CH);

    if (!g_canvas)
        for (;;)
            yield();

    render();
    ax_commit(g_canvas);

    ax_event ev;

    for (;;) {

        while (ax_poll(g_canvas, &ev) > 0) {

            if (ev.type == AX_EV_CLOSE)
                return;

            if (ev.type == AX_EV_KEY) {

                /*
                 * A / D.
                 */
                if (ev.key == 'a' ||
                    ev.key == 'A')
                {
                    player.angle -= 0.1f;
                }

                if (ev.key == 'd' ||
                    ev.key == 'D')
                {
                    player.angle += 0.1f;
                }

                /*
                 * W / S.
                 */
                if (ev.key == 'w' ||
                    ev.key == 'W')
                {
                    float dx =
                        ax_cos(player.angle) * 0.1f;

                    float dy =
                       ax_sin(player.angle) * 0.1f;

                    float nx =
                        player.x + dx;

                    float ny =
                        player.y + dy;

                    if (getMapCell(
                            (int)ny,
                            (int)nx) == 0)
                    {
                        player.x = nx;
                        player.y = ny;
                    }
                }

                if (ev.key == 's' ||
                    ev.key == 'S')
                {
                    float dx =
                        ax_cos(player.angle) * 0.1f;

                    float dy =
                       ax_sin(player.angle) * 0.1f;

                    float nx =
                        player.x - dx;

                    float ny =
                        player.y - dy;

                    if (getMapCell(
                            (int)ny,
                            (int)nx) == 0)
                    {
                        player.x = nx;
                        player.y = ny;
                    }
                }

                render();
                ax_commit(g_canvas);
            }
        }

        yield();
    }
}