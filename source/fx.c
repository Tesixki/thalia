#include "fx.h"
#include <math.h>

#define PI 3.14159265f

static Pic s_nib_lw, s_nib_lc, s_glow;

void fx_init(void)
{
	pic_load_png(&s_nib_lw, "romfs:/gfx/nib_lw.png");
	pic_load_png(&s_nib_lc, "romfs:/gfx/nib_lc.png");
	pic_load_png(&s_glow, "romfs:/gfx/glow.png");
}

void fx_exit(void)
{
	pic_free(&s_nib_lw);
	pic_free(&s_nib_lc);
	pic_free(&s_glow);
}

float fx_clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

float fx_ease_out(float t)
{
	t = fx_clamp01(t);
	float u = 1 - t;
	return 1 - u * u * u;
}

float fx_ease_out_back(float t)
{
	t = fx_clamp01(t);
	const float c1 = 1.70158f, c3 = c1 + 1;
	float u = t - 1;
	return 1 + c3 * u * u * u + c1 * u * u;
}

static u32 with_alpha(u32 c, float a)
{
	a = fx_clamp01(a);
	return (c & 0x00FFFFFF) | ((u32)(((c >> 24) & 0xFF) * a + 0.5f) << 24);
}

static u32 hash32(u32 x)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

void fx_logo(float cx, float cy, bool dark, float appear, float scale, float pulse)
{
	const Pic *nib = dark ? &s_nib_lc : &g_nib_l;
	if (!nib->valid) return;
	float a = fx_clamp01(appear);
	float s = scale;
	u32 f = ui_frame();
	// 後ろのにじみ（ゆっくり脈打つ）
	if (s_glow.valid) {
		float k = (0.55f + 0.45f * sinf(f * (dark ? 0.10f : 0.045f))) * pulse * a;
		float gs = s * (1.0f + 0.06f * sinf(f * 0.045f));
		pic_draw_tint(&s_glow, cx - s_glow.w * gs / 2, cy - s_glow.h * gs / 2, gs, with_alpha(COL_PURPLE, (dark ? 0.85f : 0.55f) * k), 1.0f);
	}
	float x = cx - nib->w * s / 2, y = cy - nib->h * s / 2;
	pic_draw_tint(nib, x, y, s, with_alpha(COL_WHITE, a), 0.0f);
	// 光の筋（数秒おきに左から右へ）
	if (s_nib_lw.valid && a >= 1.0f) {
		u32 period = dark ? 70 : 220, len = 36;
		u32 ph = f % period;
		if (ph < len) {
			float p = (float)ph / len;
			float sx = -10 + p * (nib->w + 20);
			pic_slice(&s_nib_lw, x, y, sx - 4, 3, s, with_alpha(COL_WHITE, 0.30f));
			pic_slice(&s_nib_lw, x, y, sx - 1, 4, s, with_alpha(COL_WHITE, 0.70f));
			pic_slice(&s_nib_lw, x, y, sx + 3, 3, s, with_alpha(COL_WHITE, 0.30f));
		}
	}
}

void fx_spinner(float cx, float cy, float r, u32 color)
{
	const int n = 12;
	float head = (float)(ui_frame() % 48) / 4.0f; // 48フレームで1周
	for (int i = 0; i < n; i++) {
		float ang = i * 2 * PI / n - PI / 2;
		float d = fmodf(head - i + n, (float)n); // 0 が先頭
		float k = 1.0f - d / n;
		k = k * k;
		ui_circle(cx + cosf(ang) * r, cy + sinf(ang) * r, 1.4f + 2.0f * k, with_alpha(color, 0.12f + 0.88f * k));
	}
}

// 4本の光の筋が出る星
static void star(float x, float y, float s, u32 c)
{
	float w = s * 0.26f;
	C2D_DrawTriangle(x, y - s, c, x - w, y, c, x + w, y, c, 0.5f);
	C2D_DrawTriangle(x, y + s, c, x - w, y, c, x + w, y, c, 0.5f);
	C2D_DrawTriangle(x - s, y, c, x, y - w, c, x, y + w, c, 0.5f);
	C2D_DrawTriangle(x + s, y, c, x, y - w, c, x, y + w, c, 0.5f);
}

void fx_sparkles(float cx, float cy, float rx, float ry, int n, u32 color, u32 seed)
{
	u32 f = ui_frame();
	for (int i = 0; i < n; i++) {
		u32 period = 60 + hash32(i * 7 + seed) % 60;
		u32 off = hash32(i * 13 + seed) % period;
		u32 cycle = (f + off) / period;
		float t = (float)((f + off) % period) / period; // 一生の進み 0..1
		u32 h = hash32(i * 31 + cycle * 977 + seed);
		float ang = (h % 6283) / 1000.0f;
		float rad = 0.45f + ((h >> 12) % 550) / 1000.0f;
		float x = cx + cosf(ang) * rx * rad, y = cy + sinf(ang) * ry * rad - t * 10;
		float a = sinf(t * PI);
		float s = 2.5f + ((h >> 22) % 4) * 1.0f;
		star(x, y, s * (0.4f + 0.6f * a), with_alpha(color, a));
	}
}

void fx_ring(float cx, float cy, float r, float thick, u32 c)
{
	const int n = 40;
	float ri = r - thick;
	if (ri < 0) ri = 0;
	float pc = 1, ps = 0;
	for (int i = 1; i <= n; i++) {
		float ang = i * 2 * PI / n, nc = cosf(ang), ns = sinf(ang);
		C2D_DrawTriangle(cx + pc * r, cy + ps * r, c, cx + nc * r, cy + ns * r, c, cx + pc * ri, cy + ps * ri, c, 0.5f);
		C2D_DrawTriangle(cx + pc * ri, cy + ps * ri, c, cx + nc * r, cy + ns * r, c, cx + nc * ri, cy + ns * ri, c, 0.5f);
		pc = nc;
		ps = ns;
	}
}

void fx_bar(float x, float y, float w, float h, float frac)
{
	ui_rrect(x, y, w, h, h / 2, RGBA8(0x00, 0x00, 0x00, 0x30));
	ui_rrect(x + 1, y + 1, w - 2, h - 2, h / 2 - 1, RGBA8(0xE6, 0xE6, 0xEE, 0xFF));
	float bx, bw;
	if (frac < 0) {
		// 左の外から右の外へ流れて、また左から（往復しない）
		float seg = (w - 2) * 0.35f, ph = (float)(ui_frame() % 90) / 90.0f;
		float a = x + 1 - seg + (w - 2 + seg) * ph, b = a + seg;
		bx = fmaxf(a, x + 1);
		bw = fminf(b, x + w - 1) - bx;
	} else {
		bx = x + 1;
		bw = (w - 2) * fx_clamp01(frac);
	}
	if (bw < h - 2) return;
	ui_rrect_grad(bx, y + 1, bw, h - 2, h / 2 - 1, RGBA8(0xBE, 0xA8, 0xFF, 0xFF), COL_PURPLE_D);
	// 斜めのしま（右へ流れる）
	float ih = h - 2, step = 10, off = (float)(ui_frame() % 20) / 20.0f * step;
	u32 c = RGBA8(0xFF, 0xFF, 0xFF, 0x38);
	for (float sx = bx - ih - step + off; sx < bx + bw; sx += step) {
		float x0 = sx, x1 = sx + 4, x2 = sx + ih, x3 = sx + ih + 4; // 下辺 x0..x1、上辺 x2..x3
		float lo = bx + 2, hi = bx + bw - 2;
		x0 = fmaxf(lo, fminf(hi, x0));
		x1 = fmaxf(lo, fminf(hi, x1));
		x2 = fmaxf(lo, fminf(hi, x2));
		x3 = fmaxf(lo, fminf(hi, x3));
		float yt = y + 1, yb = y + 1 + ih;
		C2D_DrawTriangle(x0, yb, c, x1, yb, c, x2, yt, c, 0.5f);
		C2D_DrawTriangle(x1, yb, c, x3, yt, c, x2, yt, c, 0.5f);
	}
	// 上のつや
	ui_rrect_corners(bx + 1, y + 1.5f, bw - 2, ih * 0.4f, ih * 0.4f / 2, 3, RGBA8(0xFF, 0xFF, 0xFF, 0x50), RGBA8(0xFF, 0xFF, 0xFF, 0x10));
}
