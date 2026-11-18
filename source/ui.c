#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define Z 0.5f

static C2D_TextBuf s_buf;
static u32 s_frame;

// フォント。romfs に同梱したもの（等倍で描く大きさで作ってある）。読めなければ本体フォント
static const char *const font_files[F_COUNT] = {"romfs:/font/body.bcfnt", "romfs:/font/small.bcfnt", "romfs:/font/title.bcfnt"};
static const float font_fallback_px[F_COUNT] = {20.0f, 17.0f, 27.0f}; // 本体フォントで代用するときのセルの高さ
static C2D_Font s_font[F_COUNT];
static float s_cell[F_COUNT];   // セルの高さ(px)
static float s_line[F_COUNT];   // 行の高さ(px)
static float s_ascent[F_COUNT]; // ベースラインまで
static int s_alt_cp[F_COUNT];   // 「見つからない文字」のグリフが本来割り当てられている文字

Pic g_nib_l, g_nib_s, g_nib_w, g_anlas;

static float cell_of(C2D_Font f) { return (float)C2D_FontGetInfo(f)->tglp->cellHeight; }

void ui_init(void)
{
	fontEnsureMapped();
	for (int i = 0; i < F_COUNT; i++) {
		s_font[i] = C2D_FontLoad(font_files[i]);
		s_alt_cp[i] = -1;
		if (s_font[i]) {
			C2D_FontSetFilter(s_font[i], GPU_NEAREST, GPU_NEAREST);
			FINF_s *fi = C2D_FontGetInfo(s_font[i]);
			s_cell[i] = fi->tglp->cellHeight;
			s_line[i] = fi->lineFeed;
			s_ascent[i] = fi->tglp->baselinePos;
			for (int cp = 0x20; cp < 0x7F; cp++)
				if (C2D_FontGlyphIndexFromCodePoint(s_font[i], (u32)cp) == fi->alterCharIndex) s_alt_cp[i] = cp;
		} else {
			s_cell[i] = font_fallback_px[i];
			s_line[i] = font_fallback_px[i];
			s_ascent[i] = font_fallback_px[i] * 0.8f;
		}
	}
	s_buf = C2D_TextBufNew(12288);
	pic_load_png(&g_nib_l, "romfs:/gfx/nib_l.png");
	pic_load_png(&g_nib_s, "romfs:/gfx/nib_s.png");
	pic_load_png(&g_nib_w, "romfs:/gfx/nib_w.png");
	pic_load_png(&g_anlas, "romfs:/gfx/anlas.png");
}

void ui_exit(void)
{
	pic_free(&g_nib_l);
	pic_free(&g_nib_s);
	pic_free(&g_nib_w);
	pic_free(&g_anlas);
	if (s_buf) C2D_TextBufDelete(s_buf);
	s_buf = NULL;
	for (int i = 0; i < F_COUNT; i++) {
		if (s_font[i]) C2D_FontFree(s_font[i]);
		s_font[i] = NULL;
	}
}

void ui_frame_begin(void)
{
	C2D_TextBufClear(s_buf);
	s_frame++;
}

u32 ui_frame(void) { return s_frame; }

// ---------------------------------------------------------------- 文字

static bool has_all(int fi, C2D_Font font, const char *s)
{
	if (!font) return true;
	int alt = C2D_FontGetInfo(font)->alterCharIndex;
	const uint8_t *p = (const uint8_t *)s;
	while (*p) {
		uint32_t cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n <= 0) return true;
		p += n;
		if (cp < 0x20) continue;
		if (C2D_FontGlyphIndexFromCodePoint(font, cp) == alt && (int)cp != s_alt_cp[fi]) return false;
	}
	return true;
}

// 文字列を全部描けるフォントを選ぶ（指定 → 本文用 → 本体フォント）
static C2D_Font pick(int f, const char *s, float *px_per_unit)
{
	C2D_Font font = s_font[f];
	if (font && !has_all(f, font, s)) {
		font = f != F_BODY && s_font[F_BODY] && has_all(F_BODY, s_font[F_BODY], s) ? s_font[F_BODY] : NULL;
	}
	// citro2d はどのフォントもセル高さ30pxに正規化して扱う。指定フォントのセル高さに合わせる
	if (px_per_unit) *px_per_unit = s_cell[f] / (font ? cell_of(font) : (float)fontGetGlyphInfo(NULL)->cellHeight);
	return font;
}

static bool parse(int f, C2D_Text *t, const char *s)
{
	if (!s || !s[0]) return false;
	C2D_Font font = pick(f, s, NULL);
	if (!C2D_TextFontParse(t, font, s_buf, s)) return false;
	C2D_TextOptimize(t);
	return true;
}

static float snap(float v) { return floorf(v + 0.5f); }
static float scale_of(int f) { return s_cell[f] / 30.0f; }

void ui_text(int f, float x, float y, u32 color, const char *s)
{
	C2D_Text t;
	if (parse(f, &t, s)) C2D_DrawText(&t, C2D_WithColor, snap(x), snap(y), Z, scale_of(f), scale_of(f), color);
}

float ui_text_width(int f, const char *s)
{
	C2D_Text t;
	float w = 0, h = 0;
	if (parse(f, &t, s)) C2D_TextGetDimensions(&t, scale_of(f), scale_of(f), &w, &h);
	return w;
}

float ui_line_h(int f) { return s_line[f]; }

// cy に文字の見た目の中心が来るような、行の上端の y を返す
float ui_text_y(int f, float cy)
{
	float em = s_cell[f] / 1.448f; // Zen Maru Gothic は上下の余白込みで 1.448em
	float mid = s_font[f] ? s_ascent[f] - 0.37f * em : s_cell[f] * 0.55f;
	return floorf(cy - mid + 0.5f);
}

void ui_text_right(int f, float x, float y, u32 color, const char *s) { ui_text(f, x - ui_text_width(f, s), y, color, s); }
void ui_text_center(int f, float cx, float y, u32 color, const char *s) { ui_text(f, cx - snap(ui_text_width(f, s) / 2), y, color, s); }

static float char_w(C2D_Font font, float unit, uint32_t cp)
{
	return C2D_FontGetCharWidthInfo(font, C2D_FontGlyphIndexFromCodePoint(font, cp))->charWidth * unit;
}

// 行の頭に来てはいけない文字（、。」）！？ー… など）
static bool no_line_start(uint32_t cp)
{
	static const uint32_t list[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF09, 0x300D, 0x300F, 0x3011, 0xFF01, 0xFF1F,
					0x30FC, 0x2026, 0x30FB, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083,
					0x3085, 0x3087, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5,
					0x30E7, ',', '.', ')', '!', '?'};
	for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
		if (list[i] == cp) return true;
	return false;
}

// 幅に収まるよう改行を入れてから描く。citro2dの折り返しは空白でしか切れず、
// 日本語の文がはみ出すため自前で行う。max_lines を超えた分は「…」で省略
void ui_text_wrap(int f, float x, float y, u32 color, float width, const char *s, int max_lines)
{
	static char out[2048];
	if (!s || !s[0]) return;
	float unit;
	C2D_Font font = pick(f, s, &unit);
	float ell = char_w(font, unit, 0x2026);
	size_t o = 0, last_break = 0; // last_break: この行で最後に切ってよい位置（outの添字）
	size_t fit_o = 0;             // 最後の行で「…」を付けても収まる位置
	float line_w = 0, w_at_break = 0;
	int lines = 1;
	const uint8_t *p = (const uint8_t *)s;
	bool truncated = false;
	while (*p) {
		uint32_t cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n <= 0) break;
		if (o + (size_t)n + 8 >= sizeof(out)) {
			truncated = true;
			break;
		}
		float cw = cp == '\n' ? 0 : char_w(font, unit, cp);
		if (lines >= max_lines && line_w + ell <= width) fit_o = o;
		// 句読点や閉じかっこは行の頭に置かず、前の行に少しはみ出させる（禁則）
		bool hang = no_line_start(cp) && line_w + cw <= width + cw * 0.9f;
		if (cp == '\n' || (line_w + cw > width && !hang)) {
			if (lines >= max_lines) {
				o = fit_o;
				truncated = true;
				break;
			}
			if (cp == '\n') {
				out[o++] = '\n';
				p += n;
				line_w = 0;
			} else if (last_break > 0 && cp < 0x80 && cp != ' ') {
				// 英単語の途中では切らず、直前の空白・カンマの後ろで折る
				memmove(out + last_break + 1, out + last_break, o - last_break);
				out[last_break] = '\n';
				o++;
				line_w -= w_at_break;
			} else {
				out[o++] = '\n';
				line_w = 0;
				if (cp == ' ') { // 行頭の空白は捨てる
					p += n;
					lines++;
					last_break = 0;
					continue;
				}
			}
			lines++;
			last_break = 0;
			fit_o = o;
			if (cp == '\n') continue;
		}
		memcpy(out + o, p, (size_t)n);
		o += (size_t)n;
		p += n;
		line_w += cw;
		if (cp == ' ' || cp == ',' || cp >= 0x80) {
			last_break = o;
			w_at_break = line_w;
		}
	}
	if (truncated) {
		memcpy(out + o, "…", 3);
		o += 3;
	}
	out[o] = 0;
	ui_text(f, x, y, color, out);
}

void ui_text_fit(int f, float x, float y, float width, u32 color, const char *s) { ui_text_wrap(f, x, y, color, width, s, 1); }

// ---------------------------------------------------------------- 図形

u32 ui_mix(u32 a, u32 b, float t)
{
	if (t <= 0) return a;
	if (t >= 1) return b;
	u32 r = 0;
	for (int i = 0; i < 32; i += 8) {
		float ca = (float)((a >> i) & 0xFF), cb = (float)((b >> i) & 0xFF);
		r |= (u32)(ca + (cb - ca) * t + 0.5f) << i;
	}
	return r;
}

void ui_rect(float x, float y, float w, float h, u32 color)
{
	if (w > 0 && h > 0) C2D_DrawRectSolid(x, y, Z, w, h, color);
}

void ui_grad(float x, float y, float w, float h, u32 top, u32 bottom)
{
	if (w > 0 && h > 0) C2D_DrawRectangle(x, y, Z, w, h, top, top, bottom, bottom);
}

// 15度刻みの単位ベクトル（0〜90度）
static const float k_cos[7] = {1.0f, 0.9659258f, 0.8660254f, 0.7071068f, 0.5f, 0.2588190f, 0.0f};

// 角の扇形。(cx,cy) が中心、qx,qy は外向きの向き（-1 か 1）
static void corner(float cx, float cy, float r, int qx, int qy, u32 top, u32 bottom, float y0, float h)
{
	int step = r <= 3 ? 3 : r <= 6 ? 2 : 1;
	float px = cx + qx * r * k_cos[0], py = cy + qy * r * k_cos[6];
	u32 cc = ui_mix(top, bottom, (cy - y0) / h);
	for (int i = step; i <= 6; i += step) {
		float nx = cx + qx * r * k_cos[i], ny = cy + qy * r * k_cos[6 - i];
		C2D_DrawTriangle(cx, cy, cc, px, py, ui_mix(top, bottom, (py - y0) / h), nx, ny, ui_mix(top, bottom, (ny - y0) / h), Z);
		px = nx;
		py = ny;
	}
}

void ui_rrect_corners(float x, float y, float w, float h, float r, int corners, u32 top, u32 bottom)
{
	if (w <= 0 || h <= 0) return;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r < 1) {
		ui_grad(x, y, w, h, top, bottom);
		return;
	}
	float rtl = corners & 1 ? r : 0, rtr = corners & 2 ? r : 0, rbl = corners & 4 ? r : 0, rbr = corners & 8 ? r : 0;
	// 真ん中の帯（上下いっぱい）
	ui_grad(x + r, y, w - 2 * r, h, top, bottom);
	// 左右の帯（角の分だけ短く）
	float lt = y + rtl, lb = y + h - rbl, rt = y + rtr, rb = y + h - rbr;
	if (lb > lt) C2D_DrawRectangle(x, lt, Z, r, lb - lt, ui_mix(top, bottom, (lt - y) / h), ui_mix(top, bottom, (lt - y) / h),
				      ui_mix(top, bottom, (lb - y) / h), ui_mix(top, bottom, (lb - y) / h));
	if (rb > rt) C2D_DrawRectangle(x + w - r, rt, Z, r, rb - rt, ui_mix(top, bottom, (rt - y) / h), ui_mix(top, bottom, (rt - y) / h),
				      ui_mix(top, bottom, (rb - y) / h), ui_mix(top, bottom, (rb - y) / h));
	if (corners & 1) corner(x + r, y + r, r, -1, -1, top, bottom, y, h);
	if (corners & 2) corner(x + w - r, y + r, r, 1, -1, top, bottom, y, h);
	if (corners & 4) corner(x + r, y + h - r, r, -1, 1, top, bottom, y, h);
	if (corners & 8) corner(x + w - r, y + h - r, r, 1, 1, top, bottom, y, h);
}

void ui_rrect_grad(float x, float y, float w, float h, float r, u32 top, u32 bottom) { ui_rrect_corners(x, y, w, h, r, 15, top, bottom); }
void ui_rrect(float x, float y, float w, float h, float r, u32 color) { ui_rrect_corners(x, y, w, h, r, 15, color, color); }

// 角丸の枠線（半透明でも重ならないように、帯と角の弧を別々に描く）
void ui_rring(float x, float y, float w, float h, float r, float t, u32 c)
{
	if (r < t) r = t;
	ui_rect(x + r, y, w - 2 * r, t, c);
	ui_rect(x + r, y + h - t, w - 2 * r, t, c);
	ui_rect(x, y + r, t, h - 2 * r, c);
	ui_rect(x + w - t, y + r, t, h - 2 * r, c);
	const float cx[4] = {x + r, x + w - r, x + r, x + w - r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
	const int qx[4] = {-1, 1, -1, 1}, qy[4] = {-1, -1, 1, 1};
	float ri = r - t;
	for (int k = 0; k < 4; k++)
		for (int i = 0; i < 6; i += 2) {
			float a0x = k_cos[i], a0y = k_cos[6 - i], a1x = k_cos[i + 2], a1y = k_cos[4 - i];
			float ox0 = cx[k] + qx[k] * r * a0x, oy0 = cy[k] + qy[k] * r * a0y;
			float ox1 = cx[k] + qx[k] * r * a1x, oy1 = cy[k] + qy[k] * r * a1y;
			float ix0 = cx[k] + qx[k] * ri * a0x, iy0 = cy[k] + qy[k] * ri * a0y;
			float ix1 = cx[k] + qx[k] * ri * a1x, iy1 = cy[k] + qy[k] * ri * a1y;
			C2D_DrawTriangle(ox0, oy0, c, ox1, oy1, c, ix0, iy0, c, Z);
			C2D_DrawTriangle(ix0, iy0, c, ox1, oy1, c, ix1, iy1, c, Z);
		}
}

void ui_shadow(float x, float y, float w, float h, float r)
{
	ui_rrect(x - 1, y + 1, w + 2, h + 2, r + 1, RGBA8(0, 0, 0, 0x0E));
	ui_rrect(x, y + 2, w, h, r, RGBA8(0, 0, 0, 0x16));
}

void ui_tri(float cx, float cy, float s, int dir, u32 c)
{
	float h = s * 0.5f;
	switch (dir) {
	case 0: C2D_DrawTriangle(cx - h, cy, c, cx + h * 0.8f, cy - s * 0.6f, c, cx + h * 0.8f, cy + s * 0.6f, c, Z); break;
	case 1: C2D_DrawTriangle(cx + h, cy, c, cx - h * 0.8f, cy - s * 0.6f, c, cx - h * 0.8f, cy + s * 0.6f, c, Z); break;
	case 2: C2D_DrawTriangle(cx, cy - h, c, cx - s * 0.6f, cy + h * 0.8f, c, cx + s * 0.6f, cy + h * 0.8f, c, Z); break;
	default: C2D_DrawTriangle(cx, cy + h, c, cx - s * 0.6f, cy - h * 0.8f, c, cx + s * 0.6f, cy - h * 0.8f, c, Z); break;
	}
}

void ui_circle(float cx, float cy, float r, u32 c) { C2D_DrawCircleSolid(cx, cy, Z, r, c); }

// ---------------------------------------------------------------- 部品

void ui_stripes(float w)
{
	ui_rect(0, 0, w, SCR_H, COL_BG);
	for (int y = 1; y < SCR_H; y += 4) ui_rect(0, (float)y, w, 2, COL_STRIPE);
}

void ui_field(float x, float y, float w, float h)
{
	ui_rrect(x, y, w, h, 5, COL_LINE_D);
	ui_rrect_grad(x + 1, y + 1, w - 2, h - 2, 4, RGBA8(0xEE, 0xEE, 0xF0, 0xFF), COL_WHITE);
	ui_rrect(x + 1, y + 3, w - 2, h - 4, 4, COL_WHITE); // 上に薄い内側の影が残る
}

void ui_button(float x, float y, float w, float h, const char *label, int style)
{
	u32 top, bot, edge, text;
	switch (style) {
	case BTN_PRIMARY:
		top = RGBA8(0xB0, 0x96, 0xFF, 0xFF), bot = RGBA8(0x7A, 0x56, 0xF0, 0xFF), edge = COL_PURPLE_D, text = COL_WHITE;
		break;
	case BTN_ON:
		top = RGBA8(0x4A, 0x4D, 0x70, 0xFF), bot = COL_NAVY, edge = COL_NAVY_LO, text = COL_CREAM;
		break;
	case BTN_DISABLED:
		top = RGBA8(0xF2, 0xF2, 0xF2, 0xFF), bot = RGBA8(0xE4, 0xE4, 0xE6, 0xFF), edge = COL_LINE, text = COL_FAINT;
		break;
	default:
		top = COL_WHITE, bot = RGBA8(0xE2, 0xE2, 0xE6, 0xFF), edge = COL_LINE_D, text = COL_TEXT;
		break;
	}
	float r = h < 20 ? 5 : 7;
	if (style != BTN_DISABLED) ui_shadow(x, y, w, h, r);
	ui_rrect(x, y, w, h, r, edge);
	ui_rrect_grad(x + 1, y + 1, w - 2, h - 2, r - 1, top, bot);
	// つや（上半分を少し明るく）
	ui_rrect_corners(x + 2, y + 1.5f, w - 4, (h - 2) * 0.45f, r - 2, 3, RGBA8(0xFF, 0xFF, 0xFF, style == BTN_NORMAL ? 0x50 : 0x30),
			 RGBA8(0xFF, 0xFF, 0xFF, 0x08));
	if (!label || !label[0]) return;
	int f = ui_text_width(F_BODY, label) <= w - 8 ? F_BODY : F_SMALL;
	ui_text_center(f, x + w / 2, ui_text_y(f, y + h / 2), text, label);
}

void ui_selector(float x, float y, float w, float h, const char *value, bool enabled)
{
	ui_button(x, y, w, h, NULL, enabled ? BTN_NORMAL : BTN_DISABLED);
	u32 ac = enabled ? COL_PURPLE : COL_LINE;
	ui_tri(x + 10, y + h / 2, 8, 0, ac);
	ui_tri(x + w - 10, y + h / 2, 8, 1, ac);
	int f = ui_text_width(F_BODY, value) <= w - 34 ? F_BODY : F_SMALL;
	ui_text_fit(f, x + 18 + fmaxf(0, (w - 36 - ui_text_width(f, value)) / 2), ui_text_y(f, y + h / 2), w - 36,
		    enabled ? COL_TEXT : COL_FAINT, value);
}

void ui_cursor(float x, float y, float w, float h, float r)
{
	// 3DS本体のメニューのように、水色の枠がゆっくり明滅する
	float ph = (float)(s_frame % 90) / 90.0f;
	float a = 0.55f + 0.45f * sinf(ph * 6.2831853f);
	u32 c = (COL_CURSOR & 0x00FFFFFF) | ((u32)(a * 255.0f) << 24);
	ui_rring(x - 3, y - 3, w + 6, h + 6, r + 3, 3, c);
	ui_rring(x - 4, y - 4, w + 8, h + 8, r + 4, 1, (COL_CURSOR & 0x00FFFFFF) | ((u32)(a * 70.0f) << 24));
}

void ui_progress(float x, float y, float w, float h, float frac)
{
	ui_rrect(x, y, w, h, h / 2, RGBA8(0xC8, 0xC9, 0xD2, 0xFF));
	ui_rrect(x + 1, y + 1, w - 2, h - 2, h / 2 - 1, RGBA8(0xE6, 0xE6, 0xEC, 0xFF));
	float bx, bw;
	if (frac < 0) {
		// 左の外から右の外へ流れて繰り返す
		float seg = w * 0.3f, ph = (float)(s_frame % 80) / 80.0f;
		float a = x + 1 - seg + (w - 2 + seg) * ph, b = a + seg;
		bx = fmaxf(a, x + 1);
		bw = fminf(b, x + w - 1) - bx;
	} else {
		if (frac > 1) frac = 1;
		bx = x + 1;
		bw = (w - 2) * frac;
	}
	if (bw >= h - 2) ui_rrect_grad(bx, y + 1, bw, h - 2, h / 2 - 1, RGBA8(0xB4, 0x9C, 0xFF, 0xFF), COL_PURPLE_D);
}

float ui_key_icon(float x, float y, const char *k)
{
	u32 dark = RGBA8(0x5C, 0x5D, 0x68, 0xFF);
	if (!strcmp(k, "A") || !strcmp(k, "B") || !strcmp(k, "X") || !strcmp(k, "Y")) {
		ui_circle(x + 8, y + 8, 8, dark);
		ui_text_center(F_SMALL, x + 8, ui_text_y(F_SMALL, y + 8), COL_WHITE, k);
		return 16;
	}
	if (!strcmp(k, "+")) { // 十字ボタン
		ui_rrect(x + 6, y + 1, 4, 14, 1, dark);
		ui_rrect(x + 1, y + 6, 14, 4, 1, dark);
		return 16;
	}
	float w = ui_text_width(F_SMALL, k) + 10;
	ui_rrect(x, y + 1, w, 14, 5, dark);
	ui_text_center(F_SMALL, x + w / 2, ui_text_y(F_SMALL, y + 8), COL_WHITE, k);
	return w;
}

// "A:決定|B:もどる" の形の案内を (x, 中心cy) から並べる。draw が false なら幅だけ返す
float ui_hints(float x, float cy, const char *spec, u32 text, u32 sub, bool draw)
{
	float x0 = x;
	char item[64];
	while (spec && *spec) {
		const char *bar = strchr(spec, '|');
		size_t n = bar ? (size_t)(bar - spec) : strlen(spec);
		if (n >= sizeof(item)) n = sizeof(item) - 1;
		memcpy(item, spec, n);
		item[n] = 0;
		char *colon = strchr(item, ':');
		const char *label = colon ? colon + 1 : item;
		if (colon) {
			*colon = 0;
			x += (draw ? ui_key_icon(x, cy - 8, item) : ui_key_icon(x, -100, item)) + 3;
		}
		if (draw) ui_text(F_SMALL, x, ui_text_y(F_SMALL, cy), colon ? text : sub, label);
		x += ui_text_width(F_SMALL, label);
		if (!bar) break;
		x += 10;
		spec = bar + 1;
	}
	return x - x0;
}

void ui_footer(const char *spec)
{
	const float y = SCR_H - 22;
	ui_grad(0, y, BOT_W, 22, RGBA8(0xFA, 0xFA, 0xFB, 0xFF), RGBA8(0xD9, 0xDA, 0xDF, 0xFF));
	ui_rect(0, y, BOT_W, 1, RGBA8(0xB4, 0xB5, 0xBE, 0xFF));
	ui_rect(0, y + 1, BOT_W, 1, COL_WHITE);
	ui_hints(6, y + 11, spec, COL_TEXT, COL_SUB, true);
}

void ui_tabs(const char *const *labels, int n, int cur, float h)
{
	ui_grad(0, 0, BOT_W, h, COL_NAVY_HI, COL_NAVY_LO);
	ui_rect(0, h - 1, BOT_W, 1, RGBA8(0x0C, 0x0D, 0x1A, 0xFF));
	float tw = (float)BOT_W / n;
	for (int i = 0; i < n; i++) {
		float x = snap(i * tw);
		if (i == cur) {
			ui_rrect_corners(x + 2, 3, tw - 4, h - 2, 6, 3, COL_WHITE, COL_BG);
			ui_rect(x + 2, h - 1, tw - 4, 2, COL_BG);
			int f = ui_text_width(F_BODY, labels[i]) <= tw - 8 ? F_BODY : F_SMALL;
			ui_text_center(f, x + tw / 2, ui_text_y(f, 3 + (h - 3) / 2.0f), COL_NAVY, labels[i]);
		} else {
			if (i > 0 && i != cur + 1) ui_rect(x, 6, 1, h - 12, RGBA8(0x55, 0x58, 0x7A, 0xFF));
			int f = ui_text_width(F_BODY, labels[i]) <= tw - 8 ? F_BODY : F_SMALL;
			ui_text_center(f, x + tw / 2, ui_text_y(f, h / 2.0f), RGBA8(0xC2, 0xC4, 0xDA, 0xFF), labels[i]);
		}
	}
}

bool ui_hit(const touchPosition *t, float x, float y, float w, float h)
{
	return t->px >= x && t->px < x + w && t->py >= y && t->py < y + h;
}

// ---------------------------------------------------------------- 画像テクスチャ

static int pow2(int v)
{
	int p = 8;
	while (p < v) p <<= 1;
	return p;
}

// 3DSのテクスチャは8x8タイル内がZ順（Morton順）に並ぶ
static inline u32 morton(int x, int y, int tw)
{
	return ((((u32)y >> 3) * ((u32)tw >> 3) + ((u32)x >> 3)) << 6) +
	       ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

static bool pic_init_fmt(Pic *p, int w, int h, GPU_TEXCOLOR fmt, int bpp)
{
	pic_free(p);
	if (w < 1 || h < 1 || w > 1024 || h > 1024) return false;
	int tw = pow2(w), th = pow2(h);
	if (!C3D_TexInit(&p->tex, (u16)tw, (u16)th, fmt)) return false;
	C3D_TexSetFilter(&p->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&p->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	memset(p->tex.data, 0, (size_t)tw * th * bpp);
	p->sub.width = (u16)w;
	p->sub.height = (u16)h;
	p->sub.left = 0.0f;
	p->sub.top = 1.0f;
	p->sub.right = (float)w / (float)tw;
	p->sub.bottom = 1.0f - (float)h / (float)th;
	p->w = w;
	p->h = h;
	p->valid = true;
	return true;
}

static bool pic_init(Pic *p, int w, int h) { return pic_init_fmt(p, w, h, GPU_RGBA8, 4); }

// RGB565 のテクスチャ（表示用の画像。RGBA8 の半分のメモリで済む）
bool pic_from_rgb565(Pic *p, const uint16_t *px, int w, int h)
{
	if (!pic_init_fmt(p, w, h, GPU_RGB565, 2)) return false;
	u16 *dst = (u16 *)p->tex.data;
	for (int y = 0; y < h; y++) {
		const uint16_t *s = px + (size_t)y * w;
		for (int x = 0; x < w; x++) dst[morton(x, y, p->tex.width)] = s[x];
	}
	C3D_TexFlush(&p->tex);
	return true;
}

bool pic_from_rgb(Pic *p, const uint8_t *rgb, int w, int h)
{
	if (!pic_init(p, w, h)) return false;
	u32 *dst = (u32 *)p->tex.data;
	for (int y = 0; y < h; y++) {
		const uint8_t *s = rgb + (size_t)y * w * 3;
		for (int x = 0; x < w; x++, s += 3) dst[morton(x, y, p->tex.width)] = ((u32)s[0] << 24) | ((u32)s[1] << 16) | ((u32)s[2] << 8) | 0xFF;
	}
	C3D_TexFlush(&p->tex);
	return true;
}

bool pic_from_rgba(Pic *p, const uint8_t *rgba, int w, int h)
{
	if (!pic_init(p, w, h)) return false;
	u32 *dst = (u32 *)p->tex.data;
	for (int y = 0; y < h; y++) {
		const uint8_t *s = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++, s += 4) dst[morton(x, y, p->tex.width)] = ((u32)s[0] << 24) | ((u32)s[1] << 16) | ((u32)s[2] << 8) | s[3];
	}
	C3D_TexFlush(&p->tex);
	return true;
}

bool pic_load_png(Pic *p, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = n > 0 && n < 4 * 1024 * 1024 ? malloc((size_t)n) : NULL;
	bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	int w, h, comp;
	uint8_t *px = ok ? stbi_load_from_memory(buf, (int)n, &w, &h, &comp, 4) : NULL;
	free(buf);
	if (!px) return false;
	ok = pic_from_rgba(p, px, w, h);
	stbi_image_free(px);
	return ok;
}

bool pic_blank(Pic *p, int w, int h)
{
	if (!pic_init(p, w, h)) return false;
	C3D_TexFlush(&p->tex);
	return true;
}

void pic_set(Pic *p, int x, int y, u32 rgba)
{
	if (!p->valid || x < 0 || y < 0 || x >= p->w || y >= p->h) return;
	((u32 *)p->tex.data)[morton(x, y, p->tex.width)] = rgba;
}

void pic_flush(Pic *p)
{
	if (p->valid) C3D_TexFlush(&p->tex);
}

void pic_set_nearest(Pic *p, bool nearest)
{
	if (p->valid) C3D_TexSetFilter(&p->tex, nearest ? GPU_NEAREST : GPU_LINEAR, nearest ? GPU_NEAREST : GPU_LINEAR);
}

void pic_free(Pic *p)
{
	if (p->valid) C3D_TexDelete(&p->tex);
	memset(p, 0, sizeof(*p));
}

void pic_draw(const Pic *p, float x, float y, float scale)
{
	if (!p->valid) return;
	C2D_Image img = {(C3D_Tex *)&p->tex, &p->sub};
	C2D_DrawImageAt(img, x, y, Z, NULL, scale, scale);
}

void pic_draw_tint(const Pic *p, float x, float y, float scale, u32 color, float blend)
{
	if (!p->valid) return;
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, blend);
	C2D_Image img = {(C3D_Tex *)&p->tex, &p->sub};
	C2D_DrawImageAt(img, x, y, Z, &tint, scale, scale);
}

// 画像の横方向の一部（sx から幅 sw ピクセル）だけを描く。光の筋の演出用
void pic_slice(const Pic *p, float x, float y, float sx, float sw, float scale, u32 color)
{
	if (!p->valid) return;
	int a = (int)floorf(sx), b = (int)floorf(sx + sw);
	if (a < 0) a = 0;
	if (b > p->w) b = p->w;
	if (b <= a) return;
	Tex3DS_SubTexture sub = p->sub;
	float tw = p->tex.width;
	sub.left = a / tw;
	sub.right = b / tw;
	sub.width = (u16)(b - a);
	C2D_Image img = {(C3D_Tex *)&p->tex, &sub};
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 0.0f);
	C2D_DrawImageAt(img, x + a * scale, y, 0.5f, &tint, scale, scale);
}
