// 描画ヘルパーと配色（当時の3DS本体アプリ風の明るいテーマ + NovelAIの色）
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stdint.h>

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

#define RGBA8(r, g, b, a) C2D_Color32(r, g, b, a)
#define COL_BG       RGBA8(0xF3, 0xF3, 0xF1, 0xFF) // 地の色
#define COL_STRIPE   RGBA8(0xEA, 0xEA, 0xE5, 0xFF) // 横じま
#define COL_TEXT     RGBA8(0x3A, 0x3B, 0x45, 0xFF)
#define COL_SUB      RGBA8(0x78, 0x79, 0x84, 0xFF)
#define COL_FAINT    RGBA8(0xA9, 0xAA, 0xB2, 0xFF)
#define COL_WHITE    RGBA8(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_LINE     RGBA8(0xC4, 0xC5, 0xCC, 0xFF)
#define COL_LINE_D   RGBA8(0x9E, 0x9F, 0xA8, 0xFF)
#define COL_NAVY     RGBA8(0x21, 0x23, 0x35, 0xFF) // NovelAIの紺
#define COL_NAVY_HI  RGBA8(0x3B, 0x3E, 0x5E, 0xFF)
#define COL_NAVY_LO  RGBA8(0x14, 0x16, 0x28, 0xFF)
#define COL_CREAM    RGBA8(0xF5, 0xF3, 0xC2, 0xFF) // NovelAIのクリーム
#define COL_PURPLE   RGBA8(0x97, 0x73, 0xFF, 0xFF) // NovelAIの紫
#define COL_PURPLE_D RGBA8(0x6C, 0x4B, 0xE0, 0xFF)
#define COL_PURPLE_L RGBA8(0xE9, 0xE2, 0xFF, 0xFF)
#define COL_GREEN    RGBA8(0x46, 0xB0, 0x3C, 0xFF)
#define COL_RED      RGBA8(0xE0, 0x48, 0x48, 0xFF)
#define COL_ORANGE   RGBA8(0xF0, 0x9A, 0x2A, 0xFF)
#define COL_CURSOR   RGBA8(0x22, 0xB0, 0xEE, 0xFF) // 3DSの選択枠の水色
#define COL_SHADE    RGBA8(0x20, 0x22, 0x30, 0x90) // 上にかぶせる半透明

enum { F_BODY, F_SMALL, F_TITLE, F_COUNT };

// ボタンの見た目
enum { BTN_NORMAL, BTN_PRIMARY, BTN_ON, BTN_DISABLED };

typedef struct {
	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	bool valid;
	int w, h;
} Pic;

void ui_init(void);
void ui_exit(void);
void ui_frame_begin(void); // 毎フレーム最初に呼ぶ（文字バッファを空にする）
u32 ui_frame(void);

// 文字（y は行の上端。等倍で描くので文字がつぶれない）
void ui_text(int f, float x, float y, u32 color, const char *s);
void ui_text_right(int f, float x, float y, u32 color, const char *s);
void ui_text_center(int f, float cx, float y, u32 color, const char *s);
void ui_text_fit(int f, float x, float y, float width, u32 color, const char *s); // 幅を超えたら「…」
void ui_text_wrap(int f, float x, float y, u32 color, float width, const char *s, int max_lines);
float ui_text_width(int f, const char *s);
float ui_line_h(int f);
float ui_text_y(int f, float cy); // cy を中心に置くときの行の上端

// 図形
u32 ui_mix(u32 a, u32 b, float t);
void ui_rect(float x, float y, float w, float h, u32 color);
void ui_grad(float x, float y, float w, float h, u32 top, u32 bottom);
void ui_rrect(float x, float y, float w, float h, float r, u32 color);
void ui_rrect_grad(float x, float y, float w, float h, float r, u32 top, u32 bottom);
void ui_rrect_corners(float x, float y, float w, float h, float r, int corners, u32 top, u32 bottom); // corners: 1左上 2右上 4左下 8右下
void ui_rring(float x, float y, float w, float h, float r, float t, u32 color);
void ui_shadow(float x, float y, float w, float h, float r);
void ui_tri(float cx, float cy, float size, int dir, u32 color); // dir: 0左 1右 2上 3下
void ui_circle(float cx, float cy, float r, u32 color);

// 部品
void ui_stripes(float w);                   // しま模様の背景（画面全体）
void ui_field(float x, float y, float w, float h);
void ui_button(float x, float y, float w, float h, const char *label, int style);
void ui_selector(float x, float y, float w, float h, const char *value, bool enabled);
void ui_cursor(float x, float y, float w, float h, float r); // 選択中を示す点滅枠
void ui_progress(float x, float y, float w, float h, float frac); // frac<0 は進み具合が分からないとき
float ui_key_icon(float x, float y, const char *key);     // A/B/X/Y/L/R/START/SELECT/十字。幅を返す
void ui_footer(const char *spec);                         // "A:決定|B:もどる" の形
float ui_hints(float x, float cy, const char *spec, u32 text, u32 sub, bool draw); // 案内を並べて幅を返す
void ui_tabs(const char *const *labels, int n, int cur, float h);
bool ui_hit(const touchPosition *t, float x, float y, float w, float h);

// 画像テクスチャ。メインスレッドから呼ぶこと
bool pic_from_rgb(Pic *p, const uint8_t *rgb, int w, int h);
bool pic_from_rgba(Pic *p, const uint8_t *rgba, int w, int h);
bool pic_from_rgb565(Pic *p, const uint16_t *px, int w, int h); // 表示用の画像（メモリ半分）
bool pic_load_png(Pic *p, const char *path); // romfs の小さな画像用
bool pic_blank(Pic *p, int w, int h);        // 透明なRGBAテクスチャ（マスクの重ね表示用）
void pic_set(Pic *p, int x, int y, u32 rgba); // 1画素書く（RGBA8: 0xRRGGBBAA）。最後に pic_flush
void pic_flush(Pic *p);
void pic_set_nearest(Pic *p, bool nearest);
void pic_free(Pic *p);
void pic_draw(const Pic *p, float x, float y, float scale);
void pic_draw_tint(const Pic *p, float x, float y, float scale, u32 color, float blend);
void pic_slice(const Pic *p, float x, float y, float sx, float sw, float scale, u32 color); // 横の一部だけ

extern Pic g_nib_l, g_nib_s, g_nib_w, g_anlas;
