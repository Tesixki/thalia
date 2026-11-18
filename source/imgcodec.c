#include "imgcodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "src/webp/decode.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

static char s_err[96];

const char *img_error(void) { return s_err; }

ImgFormat img_detect(const uint8_t *p, size_t n)
{
	static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (n >= 8 && !memcmp(p, png_sig, 8)) return IMG_PNG;
	if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return IMG_WEBP;
	return IMG_UNKNOWN;
}

const char *img_ext(ImgFormat f) { return f == IMG_WEBP ? ".webp" : ".png"; }

bool img_info(const uint8_t *p, size_t n, int *w, int *h, bool *alpha)
{
	switch (img_detect(p, n)) {
	case IMG_PNG: {
		int comp;
		if (!stbi_info_from_memory(p, (int)n, w, h, &comp)) return false;
		*alpha = comp == 2 || comp == 4;
		return true;
	}
	case IMG_WEBP: {
		WebPBitstreamFeatures f;
		if (WebPGetFeatures(p, n, &f) != VP8_STATUS_OK) return false;
		*w = f.width;
		*h = f.height;
		*alpha = f.has_alpha != 0;
		return true;
	}
	default:
		snprintf(s_err, sizeof(s_err), "unsupported image");
		return false;
	}
}

// 大きいときだけ、縦横比を保って maxw×maxh に収める
static void fit_size(int w, int h, int maxw, int maxh, int *nw, int *nh)
{
	*nw = w;
	*nh = h;
	if (w <= maxw && h <= maxh) return;
	if ((int64_t)w * maxh >= (int64_t)h * maxw) {
		*nw = maxw;
		*nh = (int)(((int64_t)h * maxw + w / 2) / w);
	} else {
		*nh = maxh;
		*nw = (int)(((int64_t)w * maxh + h / 2) / h);
	}
	if (*nw < 1) *nw = 1;
	if (*nh < 1) *nh = 1;
}

static uint8_t *png_decode(const uint8_t *p, size_t n, bool checker, int *w, int *h)
{
	int comp;
	if (!stbi_info_from_memory(p, (int)n, w, h, &comp)) {
		snprintf(s_err, sizeof(s_err), "%s", stbi_failure_reason());
		return NULL;
	}
	if (comp == 1 || comp == 3) {
		// 透過が無ければ最初から RGB で展開する（RGBA を作ってから変換するより約4MB少ない）
		uint8_t *rgb = stbi_load_from_memory(p, (int)n, w, h, &comp, 3);
		if (!rgb) snprintf(s_err, sizeof(s_err), "%s", stbi_failure_reason());
		return rgb;
	}
	uint8_t *rgba = stbi_load_from_memory(p, (int)n, w, h, &comp, 4);
	if (!rgba) {
		snprintf(s_err, sizeof(s_err), "%s", stbi_failure_reason());
		return NULL;
	}
	uint8_t *rgb = rgba_flatten(rgba, *w, *h, checker);
	stbi_image_free(rgba);
	if (!rgb) snprintf(s_err, sizeof(s_err), "out of memory");
	return rgb;
}

// WebP を ow×oh で展開する（元の大きさと違えば libwebp が縮小しながら展開する）
static uint8_t *webp_decode(const uint8_t *p, size_t n, int ow, int oh, bool checker)
{
	WebPDecoderConfig cfg;
	if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(p, n, &cfg.input) != VP8_STATUS_OK) {
		snprintf(s_err, sizeof(s_err), "bad webp");
		return NULL;
	}
	bool alpha = cfg.input.has_alpha != 0;
	if (ow != cfg.input.width || oh != cfg.input.height) {
		cfg.options.use_scaling = 1;
		cfg.options.scaled_width = ow;
		cfg.options.scaled_height = oh;
	}
	int bpp = alpha ? 4 : 3;
	size_t size = (size_t)ow * oh * bpp;
	uint8_t *buf = malloc(size);
	if (!buf) {
		snprintf(s_err, sizeof(s_err), "out of memory");
		return NULL;
	}
	cfg.output.colorspace = alpha ? MODE_RGBA : MODE_RGB;
	cfg.output.is_external_memory = 1;
	cfg.output.u.RGBA.rgba = buf;
	cfg.output.u.RGBA.stride = ow * bpp;
	cfg.output.u.RGBA.size = size;
	VP8StatusCode st = WebPDecode(p, n, &cfg);
	WebPFreeDecBuffer(&cfg.output);
	if (st != VP8_STATUS_OK) {
		free(buf);
		snprintf(s_err, sizeof(s_err), "webp decode error %d", (int)st);
		return NULL;
	}
	if (!alpha) return buf;
	uint8_t *rgb = rgba_flatten(buf, ow, oh, checker);
	free(buf);
	if (!rgb) snprintf(s_err, sizeof(s_err), "out of memory");
	return rgb;
}

uint8_t *img_decode_rgb(const uint8_t *p, size_t n, bool checker, int *w, int *h)
{
	switch (img_detect(p, n)) {
	case IMG_PNG: return png_decode(p, n, checker, w, h);
	case IMG_WEBP: {
		bool alpha;
		if (!img_info(p, n, w, h, &alpha)) return NULL;
		return webp_decode(p, n, *w, *h, checker);
	}
	default:
		snprintf(s_err, sizeof(s_err), "unsupported image");
		return NULL;
	}
}

uint8_t *img_decode_rgb_fit(const uint8_t *p, size_t n, int maxw, int maxh, bool checker, int *w, int *h, int *fw, int *fh)
{
	bool alpha;
	if (!img_info(p, n, fw, fh, &alpha)) return NULL;
	int nw, nh;
	fit_size(*fw, *fh, maxw, maxh, &nw, &nh);
	*w = nw;
	*h = nh;
	if (img_detect(p, n) == IMG_WEBP) return webp_decode(p, n, nw, nh, checker);
	// PNG は元の大きさで展開してから縮小し、元の大きさの画像はすぐに手放す
	int ow, oh;
	uint8_t *full = png_decode(p, n, checker, &ow, &oh);
	if (!full) return NULL;
	if (ow == nw && oh == nh) return full;
	int rw, rh;
	uint8_t *small = img_resize_fit(full, ow, oh, maxw, maxh, &rw, &rh);
	free(full);
	if (!small) {
		snprintf(s_err, sizeof(s_err), "out of memory");
		return NULL;
	}
	*w = rw;
	*h = rh;
	return small;
}
