#include "axui.h"

extern "C" {
#include "../lib/libax.h"
}


void Painter::clear(unsigned int color)
{
    /*
     * Здесь вызвать твою функцию
     * очистки canvas.
     */
    ax_fill(color);
}


void Painter::pixel(
    int x,
    int y,
    unsigned int color
)
{
    /*
     * draw_pixel(...)
     */
    ax_px(x,y,color);
}

void Painter::rect(
    int x,
    int y,
    int width,
    int height,
    unsigned int color
)
{
    /*
     * draw_rect(...)
     */
    ax_rect(x, y, width, height, color);
}


void Painter::fillRect(
    int x,
    int y,
    int width,
    int height,
    unsigned int color
)
{
    /*
     * fill_rect(...)
     */

}


void Painter::text(
    int x,
    int y,
    const char* str,
    unsigned int color
)
{
    /*
     * draw_text(...)
     */
     ax_text(str, x, y, color);
}


/* ---------------- Widget ---------------- */

Widget::Widget(
    int newX,
    int newY,
    int newWidth,
    int newHeight
)
{
    x = newX;
    y = newY;

    width = newWidth;
    height = newHeight;

    visible = true;
}


Widget::~Widget()
{
}


void Widget::setPosition(
    int newX,
    int newY
)
{
    x = newX;
    y = newY;
}


void Widget::setSize(
    int newWidth,
    int newHeight
)
{
    width = newWidth;
    height = newHeight;
}


int Widget::getX() const
{
    return x;
}


int Widget::getY() const
{
    return y;
}


int Widget::getWidth() const
{
    return width;
}


int Widget::getHeight() const
{
    return height;
}


void Widget::show()
{
    visible = true;
}


void Widget::hide()
{
    visible = false;
}


bool Widget::isVisible() const
{
    return visible;
}


/* ---------------- Label ---------------- */

Label::Label(
    int x,
    int y,
    int width,
    int height,
    const char* text
)
    : Widget(x, y, width, height)
{
    caption = text;
    color = 0x000000;
}


void Label::setText(
    const char* text
)
{
    caption = text;
}


void Label::setColor(
    unsigned int newColor
)
{
    color = newColor;
}


void Label::paint(
    Painter& painter
)
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


/* ---------------- Button ---------------- */

Button::Button(
    int x,
    int y,
    int width,
    int height,
    const char* text
)
    : Widget(x, y, width, height)
{
    caption = text;

    backgroundColor = 0xC0C0C0;
    borderColor     = 0x000000;
    textColor       = 0x000000;
}


void Button::paint(
    Painter& painter
)
{
    if (!visible)
        return;

    painter.fillRect(
        x,
        y,
        width,
        height,
        backgroundColor
    );

    painter.rect(
        x,
        y,
        width,
        height,
        borderColor
    );

    painter.text(
        x + 5,
        y + 5,
        caption,
        textColor
    );
}