#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_DIR "sdmc:/3ds/Navegador3DS"

// ---------- text (text.c): cached system-font text ----------
void  text_init(void);
void  text_exit(void);
void  text_frame(void);
float text_width(const char *s, float scale);
// align: 0 left, 1 center, 2 right; maxw > 0 cuts with "..."
void  text_draw(const char *s, float x, float y, float scale, u32 color, float maxw, int align);
int   text_wrap2(const char *s, float scale, float maxw, char *l1, char *l2, size_t n);
int   text_next_cp(const char *s, u32 *cp);
float text_cp_width(u32 cp);     // unscaled advance of one glyph
void  utf8_sanitize(char *s);
void  fold_text(const char *in, char *out, size_t n);

// ---------- colors ----------
#define C_BG       C2D_Color32(245, 246, 248, 255)
#define C_CARD     C2D_Color32(255, 255, 255, 255)
#define C_BAR      C2D_Color32(38, 52, 64, 255)
#define C_BAR2     C2D_Color32(52, 70, 86, 255)
#define C_ACCENT   C2D_Color32(214, 69, 40, 255)
#define C_TEXT     C2D_Color32(28, 32, 38, 255)
#define C_SUB      C2D_Color32(105, 112, 122, 255)
#define C_LINE     C2D_Color32(218, 222, 228, 255)
#define C_LINK     C2D_Color32(20, 90, 200, 255)
#define C_SEL      C2D_Color32(225, 236, 252, 255)
#define C_WHITE    C2D_Color32(255, 255, 255, 255)
#define C_GREEN    C2D_Color32(30, 140, 70, 255)
#define C_RED      C2D_Color32(200, 40, 40, 255)
#define C_ORANGE   C2D_Color32(220, 130, 20, 255)
