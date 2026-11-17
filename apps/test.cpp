#include "framework/axui.h"

extern "C" void _start()
{
    const int CW = 480;
    const int CH = 300;

    uint32_t *canvas =
        ax_surface("C++ GUI Test", CW, CH);

    if (!canvas)
    {
        for (;;)
            yield();
    }

    ax::Painter painter(
        canvas,
        CW,
        CH
    );

    /*
     * Фон.
     */
    painter.fill(
        AX_RGB(235, 235, 235)
    );

    /*
     * Заголовок.
     */
    ax::Label title(
        20,
        20,
        "Hello from AXUI!"
    );

    title.setColor(
        AX_RGB(20, 20, 20)
    );

    /*
     * Кнопки.
     */
    ax::Button button1(
        20,
        60,
        120,
        32,
        "Button 1"
    );

    ax::Button button2(
        160,
        60,
        120,
        32,
        "Button 2"
    );

    title.paint(painter);
    button1.paint(painter);
    button2.paint(painter);

    /*
     * Отправляем canvas на экран.
     */
    ax_commit(canvas);

    /*
     * Пока просто живём.
     */
    for (;;)
    {
        yield();
    }
}