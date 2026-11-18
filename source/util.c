#include "util.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// ---------------------------------------------------------------- StrBuf

void sb_init(StrBuf *s) { memset(s, 0, sizeof(*s)); }

void sb_free(StrBuf *s)
{
	free(s->p);
	memset(s, 0, sizeof(*s));
}

static bool sb_reserve(StrBuf *s, size_t extra)
{
	if (s->oom) return false;
	size_t need = s->len + extra + 1;
	if (need <= s->cap) return true;
	size_t cap = s->cap ? s->cap : 256;
	while (cap < need) cap *= 2;
	char *np = realloc(s->p, cap);
	if (!np) {
		s->oom = true;
		return false;
	}
	s->p = np;
	s->cap = cap;
	return true;
}

void sb_putn(StrBuf *s, const char *str, size_t n)
{
	if (!sb_reserve(s, n)) return;
	memcpy(s->p + s->len, str, n);
	s->len += n;
	s->p[s->len] = 0;
}

void sb_put(StrBuf *s, const char *str) { sb_putn(s, str, strlen(str)); }

void sb_printf(StrBuf *s, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n >= sizeof(tmp)) n = sizeof(tmp) - 1;
	sb_putn(s, tmp, (size_t)n);
}

void sb_json_str(StrBuf *s, const char *str)
{
	sb_putn(s, "\"", 1);
	for (const unsigned char *c = (const unsigned char *)str; *c; c++) {
		switch (*c) {
		case '"': sb_putn(s, "\\\"", 2); break;
		case '\\': sb_putn(s, "\\\\", 2); break;
		case '\n': sb_putn(s, "\\n", 2); break;
		case '\r': sb_putn(s, "\\r", 2); break;
		case '\t': sb_putn(s, "\\t", 2); break;
		default:
			if (*c < 0x20) sb_printf(s, "\\u%04x", *c);
			else sb_putn(s, (const char *)c, 1);
		}
	}
	sb_putn(s, "\"", 1);
}

void sb_urlenc(StrBuf *s, const char *str)
{
	for (const unsigned char *c = (const unsigned char *)str; *c; c++) {
		if (isalnum(*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~')
			sb_putn(s, (const char *)c, 1);
		else
			sb_printf(s, "%%%02X", *c);
	}
}

// ---------------------------------------------------------------- JSON読み取り

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

// "key" の直後に : が続く位置を探し、値の先頭を返す
static const char *json_find_value(const char *p, const char *key)
{
	char pat[64];
	int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (n < 0 || (size_t)n >= sizeof(pat)) return NULL;
	while (p && (p = strstr(p, pat))) {
		const char *q = skip_ws(p + n);
		if (*q == ':') return skip_ws(q + 1);
		p += n;
	}
	return NULL;
}

static void put_utf8(char **o, char *end, unsigned cp)
{
	char tmp[4];
	int n;
	if (cp < 0x80) {
		tmp[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		tmp[0] = (char)(0xC0 | (cp >> 6));
		tmp[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		tmp[0] = (char)(0xE0 | (cp >> 12));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		tmp[0] = (char)(0xF0 | (cp >> 18));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	if (*o + n <= end) {
		memcpy(*o, tmp, (size_t)n);
		*o += n;
	}
}

static int hex4(const char *p, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0;
	}
	*out = v;
	return 1;
}

const char *json_find_str(const char *p, const char *key, char *out, size_t outsz)
{
	const char *v = json_find_value(p, key);
	if (!v || *v != '"' || outsz == 0) return NULL;
	v++;
	char *o = out, *end = out + outsz - 1;
	while (*v && *v != '"') {
		if (*v == '\\') {
			v++;
			unsigned cp;
			switch (*v) {
			case 'n': put_utf8(&o, end, '\n'); break;
			case 't': put_utf8(&o, end, '\t'); break;
			case 'r': put_utf8(&o, end, '\r'); break;
			case 'b': case 'f': break;
			case 'u':
				if (!hex4(v + 1, &cp)) return NULL;
				v += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
					unsigned lo;
					if (hex4(v + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						v += 6;
					}
				}
				put_utf8(&o, end, cp);
				break;
			case 0: return NULL;
			default: put_utf8(&o, end, (unsigned char)*v);
			}
			v++;
		} else {
			if (o < end) *o++ = *v;
			v++;
		}
	}
	*o = 0;
	if (*v != '"') return NULL;
	return v + 1;
}

const char *json_find_num(const char *p, const char *key, double *out)
{
	const char *v = json_find_value(p, key);
	if (!v) return NULL;
	char *e;
	double d = strtod(v, &e);
	if (e == v) return NULL;
	*out = d;
	return e;
}

const char *json_find_bool(const char *p, const char *key, bool *out)
{
	const char *v = json_find_value(p, key);
	if (!v) return NULL;
	if (!strncmp(v, "true", 4)) {
		*out = true;
		return v + 4;
	}
	if (!strncmp(v, "false", 5)) {
		*out = false;
		return v + 5;
	}
	return NULL;
}

// ---------------------------------------------------------------- 文字列

void str_copy(char *dst, size_t dstsz, const char *src)
{
	if (!dstsz) return;
	size_t n = strlen(src);
	if (n >= dstsz) {
		n = dstsz - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--; // UTF-8の途中なら戻る
	}
	memmove(dst, src, n);
	dst[n] = 0;
}

void str_append_tag(char *dst, size_t dstsz, const char *tag)
{
	size_t n = strlen(dst);
	while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == ',')) n--;
	dst[n] = 0;
	size_t need = n + (n ? 2 : 0) + strlen(tag) + 1;
	if (need > dstsz) return; // 入りきらない場合は何もしない
	if (n) strcat(dst, ", ");
	strcat(dst, tag);
}

bool str_has_nonascii(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s >= 0x80) return true;
	return false;
}

void str_ellipsis(char *dst, size_t dstsz, const char *src, size_t maxbytes)
{
	if (dstsz < 8) {
		if (dstsz) dst[0] = 0;
		return;
	}
	if (maxbytes > dstsz - 4) maxbytes = dstsz - 4;
	if (strlen(src) <= maxbytes) {
		str_copy(dst, dstsz, src);
		return;
	}
	str_copy(dst, maxbytes + 1, src);
	strcat(dst, "…"); // 3バイト
}

// ---------------------------------------------------------------- ZIP

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static bool has_img_ext(const uint8_t *name, size_t n)
{
	static const char *exts[] = {".png", ".webp", ".jpg", ".jpeg"};
	for (size_t i = 0; i < 4; i++) {
		size_t l = strlen(exts[i]);
		if (n < l) continue;
		bool ok = true;
		for (size_t k = 0; k < l; k++)
			if (tolower(name[n - l + k]) != exts[i][k]) ok = false;
		if (ok) return true;
	}
	return false;
}

bool zip_extract_nth(const uint8_t *zip, size_t n, int index, uint8_t **out, size_t *outlen)
{
	static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	*out = NULL;
	*outlen = 0;
	bool webp = n >= 12 && !memcmp(zip, "RIFF", 4) && !memcmp(zip + 8, "WEBP", 4);
	if ((n >= 8 && !memcmp(zip, png_sig, 8)) || webp) { // ZIPでなく画像が直接返ってきた場合
		if (index != 0) return false;
		*out = malloc(n);
		if (!*out) return false;
		memcpy(*out, zip, n);
		*outlen = n;
		return true;
	}
	if (n < 22) return false;

	// End of Central Directory を後ろから探す
	size_t eocd = (size_t)-1;
	size_t lim = n > 22 + 65535 ? n - 22 - 65535 : 0;
	for (size_t i = n - 22 + 1; i-- > lim;) {
		if (rd32(zip + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	}
	if (eocd == (size_t)-1) return false;
	uint32_t count = rd16(zip + eocd + 10);
	size_t cd = rd32(zip + eocd + 16);

	for (uint32_t e = 0; e < count; e++) {
		if (cd > n || n - cd < 46 || rd32(zip + cd) != 0x02014b50) return false;
		uint32_t method = rd16(zip + cd + 10);
		size_t csize = rd32(zip + cd + 20), usize = rd32(zip + cd + 24);
		size_t nlen = rd16(zip + cd + 28), xlen = rd16(zip + cd + 30), clen = rd16(zip + cd + 32);
		size_t lho = rd32(zip + cd + 42);
		if (n - cd - 46 < nlen) return false;
		bool wanted = has_img_ext(zip + cd + 46, nlen);
		cd += 46 + nlen + xlen + clen;
		if (!wanted) continue;
		if (index-- > 0) continue;

		if (lho > n || n - lho < 30 || rd32(zip + lho) != 0x04034b50) return false;
		size_t data = lho + 30 + rd16(zip + lho + 26) + rd16(zip + lho + 28);
		if (data > n || n - data < csize) return false;
		if (usize == 0 || usize > 16u * 1024 * 1024) return false;

		uint8_t *buf = malloc(usize);
		if (!buf) return false;
		if (method == 0) {
			if (csize != usize) {
				free(buf);
				return false;
			}
			memcpy(buf, zip + data, usize);
		} else if (method == 8) {
			z_stream zs;
			memset(&zs, 0, sizeof(zs));
			if (inflateInit2(&zs, -15) != Z_OK) {
				free(buf);
				return false;
			}
			zs.next_in = (Bytef *)(zip + data);
			zs.avail_in = (uInt)csize;
			zs.next_out = buf;
			zs.avail_out = (uInt)usize;
			int r = inflate(&zs, Z_FINISH);
			size_t got = zs.total_out;
			inflateEnd(&zs);
			if (r != Z_STREAM_END || got != usize) {
				free(buf);
				return false;
			}
		} else {
			free(buf);
			return false;
		}
		*out = buf;
		*outlen = usize;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- 画像縮小

bool zip_extract_image(const uint8_t *zip, size_t n, uint8_t **out, size_t *outlen)
{
	return zip_extract_nth(zip, n, 0, out, outlen);
}

uint8_t *img_resize_fit(const uint8_t *rgb, int w, int h, int maxw, int maxh, int *ow, int *oh)
{
	if (w <= 0 || h <= 0 || maxw <= 0 || maxh <= 0) return NULL;
	int nw = w, nh = h;
	if (w > maxw || h > maxh) {
		// 縦横比を保って収める
		if ((int64_t)w * maxh >= (int64_t)h * maxw) {
			nw = maxw;
			nh = (int)(((int64_t)h * maxw + w / 2) / w);
		} else {
			nh = maxh;
			nw = (int)(((int64_t)w * maxh + h / 2) / h);
		}
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
	}
	uint8_t *out = malloc((size_t)nw * nh * 3);
	if (!out) return NULL;
	*ow = nw;
	*oh = nh;
	if (nw == w && nh == h) {
		memcpy(out, rgb, (size_t)w * h * 3);
		return out;
	}
	for (int y = 0; y < nh; y++) {
		int y0 = (int)((int64_t)y * h / nh), y1 = (int)((int64_t)(y + 1) * h / nh);
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < nw; x++) {
			int x0 = (int)((int64_t)x * w / nw), x1 = (int)((int64_t)(x + 1) * w / nw);
			if (x1 <= x0) x1 = x0 + 1;
			uint32_t r = 0, g = 0, b = 0, cnt = 0;
			for (int sy = y0; sy < y1; sy++) {
				const uint8_t *row = rgb + ((size_t)sy * w + x0) * 3;
				for (int sx = x0; sx < x1; sx++, row += 3) {
					r += row[0];
					g += row[1];
					b += row[2];
					cnt++;
				}
			}
			uint8_t *d = out + ((size_t)y * nw + x) * 3;
			d[0] = (uint8_t)(r / cnt);
			d[1] = (uint8_t)(g / cnt);
			d[2] = (uint8_t)(b / cnt);
		}
	}
	return out;
}

// RGB画像を nw×nh に変換（縮小は面積平均、拡大は最近傍）
uint8_t *img_resize_to(const uint8_t *rgb, int w, int h, int nw, int nh)
{
	if (w <= 0 || h <= 0 || nw <= 0 || nh <= 0) return NULL;
	uint8_t *out = malloc((size_t)nw * nh * 3);
	if (!out) return NULL;
	for (int y = 0; y < nh; y++) {
		int y0 = (int)((int64_t)y * h / nh), y1 = (int)((int64_t)(y + 1) * h / nh);
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < nw; x++) {
			int x0 = (int)((int64_t)x * w / nw), x1 = (int)((int64_t)(x + 1) * w / nw);
			if (x1 <= x0) x1 = x0 + 1;
			uint32_t r = 0, g = 0, b = 0, cnt = 0;
			for (int sy = y0; sy < y1; sy++) {
				const uint8_t *row = rgb + ((size_t)sy * w + x0) * 3;
				for (int sx = x0; sx < x1; sx++, row += 3) {
					r += row[0];
					g += row[1];
					b += row[2];
					cnt++;
				}
			}
			uint8_t *d = out + ((size_t)y * nw + x) * 3;
			d[0] = (uint8_t)(r / cnt);
			d[1] = (uint8_t)(g / cnt);
			d[2] = (uint8_t)(b / cnt);
		}
	}
	return out;
}

// NovelAIに渡せるサイズ（64の倍数、各辺64〜1600）に合わせる。
// すでに条件を満たしていればそのまま。そうでなければ縦横比を保ち、約1024x1024画素以内に収める
void img_fit64(int w, int h, int *nw, int *nh)
{
	if (w % 64 == 0 && h % 64 == 0 && w >= 64 && h >= 64 && w <= 1600 && h <= 1600) {
		*nw = w;
		*nh = h;
		return;
	}
	double s = 1.0, area = (double)w * h;
	if (area > 1024.0 * 1024.0) s = sqrt(1024.0 * 1024.0 / area);
	double fw = w * s, fh = h * s;
	if (fw > 1600) fh *= 1600 / fw, fw = 1600;
	if (fh > 1600) fw *= 1600 / fh, fh = 1600;
	int a = (int)(fw / 64 + 0.5) * 64, b = (int)(fh / 64 + 0.5) * 64;
	*nw = a < 64 ? 64 : a > 1600 ? 1600 : a;
	*nh = b < 64 ? 64 : b > 1600 ? 1600 : b;
}

// RGBAをRGBにする。透明部分は市松模様（checker）か白で埋める
uint8_t *rgba_flatten(const uint8_t *rgba, int w, int h, bool checker)
{
	uint8_t *out = malloc((size_t)w * h * 3);
	if (!out) return NULL;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const uint8_t *s = rgba + ((size_t)y * w + x) * 4;
			uint8_t *d = out + ((size_t)y * w + x) * 3;
			unsigned bg = 255;
			if (checker) bg = (((x >> 4) ^ (y >> 4)) & 1) ? 204 : 238;
			unsigned a = s[3];
			for (int i = 0; i < 3; i++) d[i] = (uint8_t)((s[i] * a + bg * (255 - a) + 127) / 255);
		}
	return out;
}

static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

bool png_info(const uint8_t *p, size_t n, int *w, int *h, int *color_type)
{
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (n < 33 || memcmp(p, sig, 8) || memcmp(p + 12, "IHDR", 4)) return false;
	*w = (int)rd32be(p + 16);
	*h = (int)rd32be(p + 20);
	*color_type = p[25];
	return *w > 0 && *h > 0;
}

static void wr32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

// ch: 3=RGB, 4=RGBA。各行に Sub フィルタを掛けて zlib で圧縮する
bool png_encode(const uint8_t *px, int w, int h, int ch, uint8_t **out, size_t *outlen)
{
	*out = NULL;
	*outlen = 0;
	if (w <= 0 || h <= 0 || (ch != 3 && ch != 4)) return false;
	size_t stride = (size_t)w * ch, rawn = (stride + 1) * h;
	uint8_t *raw = malloc(rawn);
	if (!raw) return false;
	for (int y = 0; y < h; y++) {
		uint8_t *d = raw + (stride + 1) * y;
		const uint8_t *s = px + stride * y;
		d[0] = 1; // Sub
		for (size_t i = 0; i < stride; i++) d[1 + i] = (uint8_t)(s[i] - (i >= (size_t)ch ? s[i - ch] : 0));
	}
	uLongf zn = compressBound((uLong)rawn);
	size_t total = 8 + 25 + 12 + zn + 12;
	uint8_t *buf = malloc(total);
	if (!buf) {
		free(raw);
		return false;
	}
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	memcpy(buf, sig, 8);
	uint8_t *p = buf + 8;
	wr32be(p, 13);
	memcpy(p + 4, "IHDR", 4);
	wr32be(p + 8, (uint32_t)w);
	wr32be(p + 12, (uint32_t)h);
	p[16] = 8;
	p[17] = ch == 4 ? 6 : 2;
	p[18] = p[19] = p[20] = 0;
	wr32be(p + 21, (uint32_t)crc32(0, p + 4, 17));
	p += 25;
	int r = compress2(p + 8, &zn, raw, (uLong)rawn, 6);
	free(raw);
	if (r != Z_OK) {
		free(buf);
		return false;
	}
	wr32be(p, (uint32_t)zn);
	memcpy(p + 4, "IDAT", 4);
	wr32be(p + 8 + zn, (uint32_t)crc32(0, p + 4, (uInt)(zn + 4)));
	p += 12 + zn;
	wr32be(p, 0);
	memcpy(p + 4, "IEND", 4);
	wr32be(p + 8, (uint32_t)crc32(0, p + 4, 4));
	p += 12;
	*out = buf;
	*outlen = (size_t)(p - buf);
	return true;
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
	size_t o = 0, i = 0;
	for (; i + 3 <= n; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = b64tab[v >> 18];
		out[o++] = b64tab[(v >> 12) & 63];
		out[o++] = b64tab[(v >> 6) & 63];
		out[o++] = b64tab[v & 63];
	}
	if (i < n) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
		out[o++] = b64tab[v >> 18];
		out[o++] = b64tab[(v >> 12) & 63];
		out[o++] = i + 1 < n ? b64tab[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	return o;
}

void rgb_to_565(const uint8_t *rgb, size_t n, uint16_t *out)
{
	for (size_t i = 0; i < n; i++, rgb += 3) out[i] = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

static void wr16le(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

// 表示用キャッシュ: "NVW1" + 幅・高さ・元の幅・元の高さ（各16bit）+ zlib で圧縮した RGB565
bool vw_encode(const uint16_t *px, int w, int h, int fw, int fh, uint8_t **out, size_t *outlen)
{
	*out = NULL;
	*outlen = 0;
	if (w <= 0 || h <= 0 || w > 65535 || h > 65535) return false;
	uLong rawn = (uLong)w * h * 2;
	uint8_t *raw = malloc(rawn);
	if (!raw) return false;
	for (size_t i = 0; i < (size_t)w * h; i++) wr16le(raw + i * 2, px[i]);
	uLongf zn = compressBound(rawn);
	uint8_t *buf = malloc(12 + zn);
	if (!buf) {
		free(raw);
		return false;
	}
	memcpy(buf, "NVW1", 4);
	wr16le(buf + 4, (unsigned)w);
	wr16le(buf + 6, (unsigned)h);
	wr16le(buf + 8, (unsigned)fw);
	wr16le(buf + 10, (unsigned)fh);
	int r = compress2(buf + 12, &zn, raw, rawn, 1); // 速さ優先
	free(raw);
	if (r != Z_OK) {
		free(buf);
		return false;
	}
	*out = buf;
	*outlen = 12 + zn;
	return true;
}

bool vw_decode(const uint8_t *buf, size_t n, uint16_t **px, int *w, int *h, int *fw, int *fh)
{
	*px = NULL;
	if (n < 12 || memcmp(buf, "NVW1", 4)) return false;
	*w = (int)rd16(buf + 4);
	*h = (int)rd16(buf + 6);
	*fw = (int)rd16(buf + 8);
	*fh = (int)rd16(buf + 10);
	if (*w <= 0 || *h <= 0 || *w > 1024 || *h > 1024) return false;
	uLongf rawn = (uLongf)*w * *h * 2;
	uint8_t *raw = malloc(rawn);
	uint16_t *p = malloc((size_t)*w * *h * 2);
	bool ok = raw && p && uncompress(raw, &rawn, buf + 12, (uLong)(n - 12)) == Z_OK && rawn == (uLongf)*w * *h * 2;
	if (ok)
		for (size_t i = 0; i < (size_t)*w * *h; i++) p[i] = (uint16_t)rd16(raw + i * 2);
	free(raw);
	if (!ok) {
		free(p);
		return false;
	}
	*px = p;
	return true;
}
