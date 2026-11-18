// 画像の展開（PNG と WebP）。3DS に依存しないので PC のテストでも使う
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { IMG_UNKNOWN, IMG_PNG, IMG_WEBP } ImgFormat;

ImgFormat img_detect(const uint8_t *p, size_t n);
const char *img_ext(ImgFormat f); // ".png" / ".webp"

// 大きさと透過の有無だけを読む
bool img_info(const uint8_t *p, size_t n, int *w, int *h, bool *alpha);

// 元の大きさの RGB で展開する。透過している部分は checker なら市松模様、そうでなければ白で埋める
uint8_t *img_decode_rgb(const uint8_t *p, size_t n, bool checker, int *w, int *h);

// maxw×maxh に収まる大きさの RGB で展開する（大きいときだけ縮小）。fw, fh には元の大きさ。
// WebP は縮小しながら展開するので、元の大きさの画像をメモリに作らない
uint8_t *img_decode_rgb_fit(const uint8_t *p, size_t n, int maxw, int maxh, bool checker, int *w, int *h, int *fw, int *fh);

const char *img_error(void); // 失敗したときの理由
