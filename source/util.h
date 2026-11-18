// 3DSに依存しない共通処理（文字列バッファ・JSON・ZIP展開・画像縮小）
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	char *p;
	size_t len, cap;
	bool oom;
} StrBuf;

void sb_init(StrBuf *s);
void sb_free(StrBuf *s);
void sb_putn(StrBuf *s, const char *str, size_t n);
void sb_put(StrBuf *s, const char *str);
void sb_printf(StrBuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(StrBuf *s, const char *str); // "..." をエスケープ付きで出力
void sb_urlenc(StrBuf *s, const char *str);

// 簡易JSON読み取り: p以降で "key": を探し、値を取り出す。見つかれば値の直後、なければNULL
const char *json_find_str(const char *p, const char *key, char *out, size_t outsz);
const char *json_find_num(const char *p, const char *key, double *out);
const char *json_find_bool(const char *p, const char *key, bool *out);

// 文字列ヘルパー
void str_copy(char *dst, size_t dstsz, const char *src); // UTF-8の途中で切らない
void str_append_tag(char *dst, size_t dstsz, const char *tag); // ", "区切りで追記
bool str_has_nonascii(const char *s);
void str_ellipsis(char *dst, size_t dstsz, const char *src, size_t maxbytes);

// ZIPから最初の画像ファイルを取り出す（mallocで確保。呼び出し側がfree）
// ZIPでなくPNGそのものが来た場合はそのままコピーする
bool zip_extract_image(const uint8_t *zip, size_t n, uint8_t **out, size_t *outlen);
bool zip_extract_nth(const uint8_t *zip, size_t n, int index, uint8_t **out, size_t *outlen); // index枚目（0から）

// RGB(3バイト/px)画像を、maxw×maxhに収まるよう面積平均で縮小（拡大はしない）
uint8_t *img_resize_fit(const uint8_t *rgb, int w, int h, int maxw, int maxh, int *ow, int *oh);
uint8_t *img_resize_to(const uint8_t *rgb, int w, int h, int nw, int nh); // ちょうど nw×nh に
void img_fit64(int w, int h, int *nw, int *nh); // NovelAIに渡せるサイズ（64の倍数）に合わせる
uint8_t *rgba_flatten(const uint8_t *rgba, int w, int h, bool checker); // 透明部分を白か市松模様で埋めたRGB

// PNGのヘッダを読む（color_type: 2=RGB, 6=RGBA など）
bool png_info(const uint8_t *p, size_t n, int *w, int *h, int *color_type);
// RGB(ch=3) / RGBA(ch=4) をPNGにする（mallocで確保。呼び出し側がfree）
bool png_encode(const uint8_t *px, int w, int h, int ch, uint8_t **out, size_t *outlen);
// Base64（改行なし、=で埋める）。outには 4*((n+2)/3) バイト必要。書いた長さを返す
size_t b64_encode(const uint8_t *in, size_t n, char *out);

// RGB(3バイト/px) → RGB565
void rgb_to_565(const uint8_t *rgb, size_t n, uint16_t *out);
// 一覧から開くときに使う、3DS表示用に縮小した画像のキャッシュ（.vw）
bool vw_encode(const uint16_t *px, int w, int h, int fw, int fh, uint8_t **out, size_t *outlen);
bool vw_decode(const uint8_t *buf, size_t n, uint16_t **px, int *w, int *h, int *fw, int *fh);
