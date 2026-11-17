#ifndef AXUI_H
#define AXUI_H

#include "../../lib/libax.h"
#include "../../lib/unistd.h"

namespace ax
{

class Painter
{
public:

    Painter(uint32_t *canvas, int width, int height)
    {
        g_canvas = canvas;
        ax_canvas_dims(width, height);
    }

    void pixel(
        int x,
        int y,
        uint32_t color)
    {
        ax_px(x, y, color);
    }

    void fill(
        uint32_t color)
    {
        ax_fill(color);
    }

    void rect(
        int x,
        int y,
        int width,
        int height,
        uint32_t color)
    {
        ax_rect(
            x,
            y,
            width,
            height,
            color
        );
    }

    void text(
        int x,
        int y,
        const char *text,
        uint32_t color)
    {
        ax_text(
            text,
            x,
            y,
            color
        );
    }
};


/*
 * Базовый GUI-элемент.
 */
class Widget
{
protected:
    int x;
    int y;
    int width;
    int height;
    bool visible;

public:

    Widget(int x, int y, int width, int height)
        : x(x),
          y(y),
          width(width),
          height(height),
          visible(true)
    {
    }

    ~Widget()
    {
    }

    virtual void paint(Painter &painter) = 0;

    void setPosition(int nx, int ny)
    {
        x = nx;
        y = ny;
    }

    void setSize(int nw, int nh)
    {
        width = nw;
        height = nh;
    }

    void show()
    {
        visible = true;
    }

    void hide()
    {
        visible = false;
    }

    bool contains(int px, int py) const
    {
        return
            px >= x &&
            py >= y &&
            px < x + width &&
            py < y + height;
    }

    int getX() const
    {
        return x;
    }

    int getY() const
    {
        return y;
    }

    int getWidth() const
    {
        return width;
    }

    int getHeight() const
    {
        return height;
    }
};


/*
 * Label.
 */
class Label : public Widget
{
private:

    const char *caption;
    uint32_t color;

public:

    Label(
        int x,
        int y,
        const char *text)
        : Widget(x, y, 0, 8),
          caption(text),
          color(AX_RGB(0, 0, 0))
    {
    }

    void setText(const char *text)
    {
        caption = text;
    }

    void setColor(uint32_t c)
    {
        color = c;
    }

    void paint(Painter &painter) override
    {
        if (!visible)
            return;

        painter.text(
            x,
            y,
            caption,
            color
        );
    }
};


/*
 * Простейшая кнопка.
 */
class Button : public Widget
{
private:

    const char *caption;

    uint32_t background;
    uint32_t border;
    uint32_t textColor;

public:

    Button(
        int x,
        int y,
        int width,
        int height,
        const char *text)
        : Widget(x, y, width, height),
          caption(text),
          background(AX_RGB(210, 210, 210)),
          border(AX_RGB(40, 40, 40)),
          textColor(AX_RGB(0, 0, 0))
    {
    }

    void setText(const char *text)
    {
        caption = text;
    }

    void paint(Painter &painter) override
    {
        if (!visible)
            return;

        /*
         * Фон.
         */
        painter.rect(
            x,
            y,
            width,
            height,
            background
        );

        /*
         * Рамка.
         */
        painter.rect(
            x,
            y,
            width,
            1,
            border
        );

        painter.rect(
            x,
            y + height - 1,
            width,
            1,
            border
        );

        painter.rect(
            x,
            y,
            1,
            height,
            border
        );

        painter.rect(
            x + width - 1,
            y,
            1,
            height,
            border
        );

        /*
         * Текст.
         */
        painter.text(
            x + 6,
            y + (height - 8) / 2,
            caption,
            textColor
        );
    }
};

} // namespace ax

#endif