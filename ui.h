#ifndef FROGGY_UI_H
#define FROGGY_UI_H

#include <stdint.h>

#define SCREEN_W  320
#define SCREEN_H  240

/* RGB565 conversion */
#define RGB565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

/* MinUI-style palette */
#define COL_BG          RGB565(0, 0, 0)
#define COL_TEXT        RGB565(255, 255, 255)
#define COL_SELECT_BG   RGB565(255, 255, 255)
#define COL_SELECT_FG   RGB565(0, 0, 0)
#define COL_DIM         RGB565(132, 132, 132)
#define COL_PILL        RGB565(33, 33, 33)
#define COL_BAR_BG      RGB565(33, 33, 33)
#define COL_BAR_FG      RGB565(255, 255, 255)

/* Global framebuffer - defined in libretro.c */
extern uint16_t pixels[];

/* Drawing primitives */
void ui_clear(uint16_t col);
void ui_fill(int x, int y, int w, int h, uint16_t col);
void ui_pill(int x, int y, int w, int h, int r, uint16_t col);
void ui_text_pill(int x, int y, const char *str, uint16_t bg, uint16_t fg, int pad);
void ui_legend_btn(int x, int y, const char *str);

#endif
