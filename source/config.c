#include "config.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- モデル定義
// 品質タグと除外プリセットの文言は、WebUIと同じ結果になるよう非公式SDK
// (caru-ini/novelai-sdk, 2026-10時点) の定義に合わせている。

#define QT_V5 "very aesthetic, masterpiece, no text"
#define UC_V5_HEAVY "lowres, artistic error, film grain, scan artifacts, worst quality, bad quality, jpeg artifacts, very displeasing, chromatic aberration, dithering, halftone, screentone, multiple views, logo, too many watermarks, negative space, blank page"
#define UC_FURRY_TXT "{worst quality}, distracting watermark, unfinished, bad quality, {widescreen}, upscale, {sequence}, {{grandfathered content}}, blurred foreground, chromatic aberration, sketch, everyone, [sketch background], simple, [flat colors], ych (character), outline, multiple scenes, [[horror (theme)]], comic"
#define UC_V5_SET { \
	UC_V5_HEAVY, \
	"lowres, bad hands, bad anatomy, artistic error, sepia, white haze, worst quality, very displeasing, jpeg artifacts, 0::ai-generated::", \
	UC_V5_HEAVY ", @_@, mismatched pupils, glowing eyes, bad anatomy", \
	UC_FURRY_TXT, "" }

const ModelInfo g_models[] = {
	{"nai-diffusion-5-full", "nai-diffusion-5-full-inpainting", "V5 Full", 5, false, false, QT_V5, UC_V5_SET},
	{"nai-diffusion-5-full-medium", "nai-diffusion-5-full-medium-inpainting", "V5 Full Medium", 5, false, true, QT_V5, UC_V5_SET},
	{"nai-diffusion-5-curated", "nai-diffusion-4-5-curated-inpainting", "V5 Curated", 5, true, false, QT_V5, UC_V5_SET},
	{"nai-diffusion-4-5-full", "nai-diffusion-4-5-full-inpainting", "V4.5 Full", 4, false, false, QT_V5,
		{UC_V5_HEAVY,
		 "lowres, artistic error, scan artifacts, worst quality, bad quality, jpeg artifacts, multiple views, very displeasing, too many watermarks, negative space, blank page",
		 UC_V5_HEAVY ", @_@, mismatched pupils, glowing eyes, bad anatomy",
		 UC_FURRY_TXT, ""}},
	{"nai-diffusion-4-5-curated", "nai-diffusion-4-5-curated-inpainting", "V4.5 Curated", 4, true, false,
		"very aesthetic, masterpiece, no text, -0.8::feet::, rating:general",
		{"blurry, lowres, upscaled, artistic error, film grain, scan artifacts, worst quality, bad quality, jpeg artifacts, very displeasing, chromatic aberration, halftone, multiple views, logo, too many watermarks, negative space, blank page",
		 "blurry, lowres, upscaled, artistic error, scan artifacts, jpeg artifacts, logo, too many watermarks, negative space, blank page",
		 "blurry, lowres, upscaled, artistic error, film grain, scan artifacts, bad anatomy, bad hands, worst quality, bad quality, jpeg artifacts, very displeasing, chromatic aberration, halftone, multiple views, logo, too many watermarks, @_@, mismatched pupils, glowing eyes, negative space, blank page",
		 NULL, ""}},
	{"nai-diffusion-4-full", "nai-diffusion-4-full-inpainting", "V4 Full", 4, false, false,
		"no text, best quality, very aesthetic, absurdres",
		{"blurry, lowres, error, film grain, scan artifacts, worst quality, bad quality, jpeg artifacts, very displeasing, chromatic aberration, multiple views, logo, too many watermarks, white blank page, blank page",
		 "blurry, lowres, error, worst quality, bad quality, jpeg artifacts, very displeasing, white blank page, blank page",
		 NULL, NULL, ""}},
	{"nai-diffusion-4-curated-preview", "nai-diffusion-4-curated-inpainting", "V4 Curated", 4, true, false,
		"rating:general, best quality, very aesthetic, absurdres",
		{"blurry, lowres, error, film grain, scan artifacts, worst quality, bad quality, jpeg artifacts, very displeasing, chromatic aberration, logo, dated, signature, multiple views, gigantic breasts, white blank page, blank page",
		 "blurry, lowres, error, worst quality, bad quality, jpeg artifacts, very displeasing, logo, dated, signature, white blank page, blank page",
		 NULL, NULL, ""}},
	{"nai-diffusion-3", "nai-diffusion-3-inpainting", "V3 Anime", 3, false, false,
		"best quality, amazing quality, very aesthetic, absurdres",
		{"lowres, {bad}, error, fewer, extra, missing, worst quality, jpeg artifacts, bad quality, watermark, unfinished, displeasing, chromatic aberration, signature, extra digits, artistic error, username, scan, [abstract]",
		 "lowres, jpeg artifacts, worst quality, watermark, blurry, very displeasing",
		 "lowres, {bad}, error, fewer, extra, missing, worst quality, jpeg artifacts, bad quality, watermark, unfinished, displeasing, chromatic aberration, signature, extra digits, artistic error, username, scan, [abstract], bad anatomy, bad hands, @_@, mismatched pupils, heart-shaped pupils, glowing eyes",
		 NULL, "lowres"}},
	{"nai-diffusion-furry-3", "nai-diffusion-furry-3-inpainting", "V3 Furry", 3, false, false,
		"{best quality}, {amazing quality}",
		{"{{worst quality}}, [displeasing], {unusual pupils}, guide lines, {{unfinished}}, {bad}, url, artist name, {{tall image}}, mosaic, {sketch page}, comic panel, impact (font), [dated], {logo}, ych, {what}, {where is your god now}, {distorted text}, repeated text, {floating head}, {1994}, {widescreen}, absolutely everyone, sequence, {compression artifacts}, hard translated, {cropped}, {commissioner name}, unknown text, high contrast",
		 "{worst quality}, guide lines, unfinished, bad, url, tall image, widescreen, compression artifacts, unknown text",
		 NULL, NULL, "lowres"}},
};
const int g_model_count = sizeof(g_models) / sizeof(g_models[0]);

const char *const g_samplers[] = {"k_euler_ancestral", "k_euler", "k_dpmpp_2s_ancestral", "k_dpmpp_2m", "k_dpmpp_sde", "k_dpm_2", "k_dpm_2_ancestral", "ddim"};
const char *const g_sampler_labels[] = {"Euler Ancestral", "Euler", "DPM++ 2S Ancestral", "DPM++ 2M", "DPM++ SDE", "DPM2", "DPM2 Ancestral", "DDIM"};
const int g_sampler_count = 8;
const char *const g_noises[] = {"karras", "exponential", "polyexponential"};
const int g_noise_count = 3;
const char *const g_uc_labels[UC_COUNT] = {"Heavy", "Light", "Human Focus", "Furry Focus", "None"};
static const int uc_hint[UC_COUNT] = {2, 3, 4, 5, 0};

int g_lang;

const DirTool g_dir_tools[DIR_COUNT] = {
	{"lineart", {"線画", "Line Art"}, {"白黒の線画にします", "Turns the picture into black-and-white line art"}, false},
	{"sketch", {"ラフ", "Sketch"}, {"鉛筆で描いたラフ画のようにします", "Makes it look like a pencil sketch"}, false},
	{"bg-removal", {"背景除去", "Remove BG"}, {"背景を消します（結果は3枚保存されます）", "Removes the background (saves 3 images)"}, false},
	{"declutter", {"文字消し", "Declutter"}, {"文字や効果音を消します", "Removes text and sound effects"}, false},
	{"declutter-keep-bubbles", {"文字消し（吹き出し残す）", "Declutter (keep bubbles)"},
	 {"吹き出しは残して、文字や効果音を消します", "Removes text and effects but keeps speech bubbles"}, false},
	{"colorize", {"色付け", "Colorize"}, {"線画や白黒の画像に色を付けます。プロンプトで色を指定できます", "Colors line art or grayscale images. Add color hints as a prompt"}, true},
	{"emotion", {"表情", "Emotion"}, {"キャラクターの表情を変えます", "Changes the character's expression"}, true},
};
const char *const g_emotions[EMOTION_COUNT] = {
	"neutral", "happy", "sad", "angry", "scared", "surprised", "tired", "excited",
	"nervous", "thinking", "confused", "shy", "disgusted", "smug", "bored", "laughing",
	"irritated", "aroused", "embarrassed", "worried", "love", "determined", "hurt", "playful",
};
const char *const g_emotion_labels[2][EMOTION_COUNT] = {
	{"ふつう", "うれしい", "かなしい", "おこる", "こわがる", "おどろく", "つかれ", "わくわく",
	 "きんちょう", "考え中", "こまる", "てれる", "いやがる", "どや顔", "たいくつ", "大笑い",
	 "いらいら", "興奮", "はずかしい", "しんぱい", "恋", "決意", "いたがる", "いたずら"},
	{"Neutral", "Happy", "Sad", "Angry", "Scared", "Surprised", "Tired", "Excited",
	 "Nervous", "Thinking", "Confused", "Shy", "Disgusted", "Smug", "Bored", "Laughing",
	 "Irritated", "Aroused", "Embarrassed", "Worried", "Love", "Determined", "Hurt", "Playful"},
};
const char *const g_defry_labels[2][6] = {
	{"最大", "強", "やや強", "やや弱", "弱", "最小"},
	{"Max", "Strong", "Medium+", "Medium-", "Weak", "Min"},
};

const SizePreset g_sizes[] = {
	{{"縦長 832x1216", "Portrait 832x1216"}, 832, 1216},
	{{"横長 1216x832", "Landscape 1216x832"}, 1216, 832},
	{{"正方形 1024x1024", "Square 1024x1024"}, 1024, 1024},
	{{"小・縦 512x768", "S Portrait 512x768"}, 512, 768},
	{{"小・横 768x512", "S Landscape 768x512"}, 768, 512},
	{{"小・正方形 640x640", "S Square 640x640"}, 640, 640},
	{{"大・縦 1024x1536", "L Portrait 1024x1536"}, 1024, 1536},
	{{"大・横 1536x1024", "L Landscape 1536x1024"}, 1536, 1024},
};
const int g_size_count = sizeof(g_sizes) / sizeof(g_sizes[0]);

// ---------------------------------------------------------------- 設定

void config_defaults(Config *c)
{
	memset(c, 0, sizeof(*c));
	c->model = 0;
	c->width = 832;
	c->height = 1216;
	c->steps = 23;
	c->scale = 5.0f;
	c->sampler = 0;
	c->noise = 0;
	c->cfg_rescale = 0.0f;
	c->variety = false;
	c->quality = true;
	c->uc_preset = UC_LIGHT;
	c->seed_random = true;
	c->seed = 0;
	c->verify_tls = true;
	for (int i = 0; i < MAX_CHARS; i++) {
		c->chars[i].enabled = true;
		c->chars[i].gx = c->chars[i].gy = -1;
	}
	c->i2i_strength = 0.7f;
	c->i2i_noise = 0.0f;
	c->inpaint_strength = 1.0f;
	c->dir_tool = DIR_LINEART;
	c->dir_emotion = 1;
	c->dir_defry = 0;
	c->lang = LANG_AUTO;
	c->recv_webp = false;
}

static float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void config_sanitize(Config *c)
{
	c->model = clampi(c->model, 0, g_model_count - 1);
	c->width = clampi((c->width + 32) / 64 * 64, 64, 1600);
	c->height = clampi((c->height + 32) / 64 * 64, 64, 1600);
	c->steps = clampi(c->steps, 1, 50);
	if (!(c->scale >= 0.0f)) c->scale = 0.0f;
	if (c->scale > 10.0f) c->scale = 10.0f;
	if (!(c->cfg_rescale >= 0.0f)) c->cfg_rescale = 0.0f;
	if (c->cfg_rescale > 1.0f) c->cfg_rescale = 1.0f;
	c->sampler = clampi(c->sampler, 0, g_sampler_count - 1);
	c->noise = clampi(c->noise, 0, g_noise_count - 1);
	c->uc_preset = clampi(c->uc_preset, 0, UC_COUNT - 1);
	if (!g_models[c->model].uc[c->uc_preset]) c->uc_preset = UC_LIGHT;
	for (int i = 0; i < MAX_CHARS; i++) {
		CharSlot *s = &c->chars[i];
		if (s->gx < 0 || s->gx > 4 || s->gy < 0 || s->gy > 4) s->gx = s->gy = -1;
	}
	c->i2i_strength = clampf(c->i2i_strength, 0.01f, 0.99f);
	c->i2i_noise = clampf(c->i2i_noise, 0.0f, 0.99f);
	c->inpaint_strength = clampf(c->inpaint_strength, 0.01f, 1.0f);
	c->dir_tool = clampi(c->dir_tool, 0, DIR_COUNT - 1);
	c->dir_emotion = clampi(c->dir_emotion, 0, EMOTION_COUNT - 1);
	c->dir_defry = clampi(c->dir_defry, 0, 5);
	c->lang = clampi(c->lang, 0, LANG_COUNT - 1);
}

int config_size_preset(const Config *c)
{
	for (int i = 0; i < g_size_count; i++)
		if (g_sizes[i].w == c->width && g_sizes[i].h == c->height) return i;
	return -1;
}

int config_next_uc_preset(const Config *c, int dir)
{
	int p = c->uc_preset;
	for (int i = 0; i < UC_COUNT; i++) {
		p = (p + dir + UC_COUNT) % UC_COUNT;
		if (g_models[c->model].uc[p]) return p;
	}
	return c->uc_preset;
}

// 値の中の改行とバックスラッシュをエスケープして1行に収める
static void write_kv(FILE *f, const char *key, const char *val)
{
	fprintf(f, "%s=", key);
	for (; *val; val++) {
		if (*val == '\n') fputs("\\n", f);
		else if (*val == '\r') continue;
		else if (*val == '\\') fputs("\\\\", f);
		else fputc(*val, f);
	}
	fputc('\n', f);
}

static void unescape(char *s)
{
	char *o = s;
	for (; *s; s++) {
		if (*s == '\\' && s[1] == 'n') {
			*o++ = '\n';
			s++;
		} else if (*s == '\\' && s[1] == '\\') {
			*o++ = '\\';
			s++;
		} else
			*o++ = *s;
	}
	*o = 0;
}

bool config_save(const Config *c, const char *path, bool with_key)
{
	FILE *f = fopen(path, "w");
	if (!f) return false;
	char key[32];
	if (with_key) {
		fputs("# Thalia 設定ファイル。api_key に NovelAI の Persistent API Token (pst-...) を書く\n", f);
		write_kv(f, "api_key", c->api_key);
		fprintf(f, "verify_tls=%d\nlang=%d\nimage_format=%s\n", c->verify_tls, c->lang, c->recv_webp ? "webp" : "png");
	}
	write_kv(f, "prompt", c->prompt);
	write_kv(f, "uc", c->uc);
	write_kv(f, "model", g_models[c->model].id);
	fprintf(f, "width=%d\nheight=%d\nsteps=%d\nscale=%.2f\n", c->width, c->height, c->steps, (double)c->scale);
	write_kv(f, "sampler", g_samplers[c->sampler]);
	write_kv(f, "noise_schedule", g_noises[c->noise]);
	fprintf(f, "cfg_rescale=%.2f\nvariety=%d\nquality=%d\nuc_preset=%d\nseed_random=%d\nseed=%lu\n",
		(double)c->cfg_rescale, c->variety, c->quality, c->uc_preset, c->seed_random, (unsigned long)c->seed);
	fprintf(f, "i2i_strength=%.2f\ni2i_noise=%.2f\ninpaint_strength=%.2f\ndir_tool=%d\ndir_emotion=%d\ndir_defry=%d\n",
		(double)c->i2i_strength, (double)c->i2i_noise, (double)c->inpaint_strength, c->dir_tool, c->dir_emotion, c->dir_defry);
	write_kv(f, "dir_prompt", c->dir_prompt);
	for (int i = 0; i < MAX_CHARS; i++) {
		const CharSlot *s = &c->chars[i];
		if (!s->prompt[0] && !s->uc[0]) continue;
		snprintf(key, sizeof(key), "char%d_prompt", i + 1);
		write_kv(f, key, s->prompt);
		snprintf(key, sizeof(key), "char%d_uc", i + 1);
		write_kv(f, key, s->uc);
		fprintf(f, "char%d_enabled=%d\nchar%d_pos=%d,%d\n", i + 1, s->enabled, i + 1, s->gx, s->gy);
	}
	bool ok = !ferror(f);
	return fclose(f) == 0 && ok;
}

bool config_load(Config *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return false;
	size_t cap = MAX_PROMPT * 2 + 64;
	char *line = malloc(cap);
	if (!line) {
		fclose(f);
		return false;
	}
	while (fgets(line, (int)cap, f)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
		if (line[0] == '#' || line[0] == ';') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*k == ' ' || *k == '\t' || (unsigned char)*k == 0xEF || (unsigned char)*k == 0xBB || (unsigned char)*k == 0xBF) k++; // BOMも飛ばす
		size_t kn = strlen(k);
		while (kn && (k[kn - 1] == ' ' || k[kn - 1] == '\t')) k[--kn] = 0;
		while (*v == ' ' || *v == '\t') v++;
		unescape(v);

		if (!strcmp(k, "api_key")) {
			size_t vn = strlen(v);
			while (vn && isspace((unsigned char)v[vn - 1])) v[--vn] = 0;
			str_copy(c->api_key, sizeof(c->api_key), v);
		} else if (!strcmp(k, "verify_tls")) c->verify_tls = atoi(v) != 0;
		else if (!strcmp(k, "lang")) c->lang = atoi(v);
		else if (!strcmp(k, "image_format")) c->recv_webp = !strcmp(v, "webp");
		else if (!strcmp(k, "prompt")) str_copy(c->prompt, sizeof(c->prompt), v);
		else if (!strcmp(k, "uc")) str_copy(c->uc, sizeof(c->uc), v);
		else if (!strcmp(k, "model")) {
			for (int i = 0; i < g_model_count; i++)
				if (!strcmp(v, g_models[i].id)) c->model = i;
		} else if (!strcmp(k, "width")) c->width = atoi(v);
		else if (!strcmp(k, "height")) c->height = atoi(v);
		else if (!strcmp(k, "steps")) c->steps = atoi(v);
		else if (!strcmp(k, "scale")) c->scale = (float)atof(v);
		else if (!strcmp(k, "sampler")) {
			for (int i = 0; i < g_sampler_count; i++)
				if (!strcmp(v, g_samplers[i])) c->sampler = i;
		} else if (!strcmp(k, "noise_schedule")) {
			for (int i = 0; i < g_noise_count; i++)
				if (!strcmp(v, g_noises[i])) c->noise = i;
		} else if (!strcmp(k, "cfg_rescale")) c->cfg_rescale = (float)atof(v);
		else if (!strcmp(k, "variety")) c->variety = atoi(v) != 0;
		else if (!strcmp(k, "quality")) c->quality = atoi(v) != 0;
		else if (!strcmp(k, "uc_preset")) c->uc_preset = atoi(v);
		else if (!strcmp(k, "seed_random")) c->seed_random = atoi(v) != 0;
		else if (!strcmp(k, "seed")) c->seed = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(k, "i2i_strength")) c->i2i_strength = (float)atof(v);
		else if (!strcmp(k, "i2i_noise")) c->i2i_noise = (float)atof(v);
		else if (!strcmp(k, "inpaint_strength")) c->inpaint_strength = (float)atof(v);
		else if (!strcmp(k, "dir_tool")) c->dir_tool = atoi(v);
		else if (!strcmp(k, "dir_emotion")) c->dir_emotion = atoi(v);
		else if (!strcmp(k, "dir_defry")) c->dir_defry = atoi(v);
		else if (!strcmp(k, "dir_prompt")) str_copy(c->dir_prompt, sizeof(c->dir_prompt), v);
		else if (!strncmp(k, "char", 4) && k[4] >= '1' && k[4] <= '0' + MAX_CHARS && k[5] == '_') {
			CharSlot *s = &c->chars[k[4] - '1'];
			const char *sub = k + 6;
			if (!strcmp(sub, "prompt")) str_copy(s->prompt, sizeof(s->prompt), v);
			else if (!strcmp(sub, "uc")) str_copy(s->uc, sizeof(s->uc), v);
			else if (!strcmp(sub, "enabled")) s->enabled = atoi(v) != 0;
			else if (!strcmp(sub, "pos")) {
				if (sscanf(v, "%d,%d", &s->gx, &s->gy) != 2) s->gx = s->gy = -1;
			}
		}
	}
	free(line);
	fclose(f);
	config_sanitize(c);
	return true;
}

// ---------------------------------------------------------------- リクエスト組み立て

static void join_tags(StrBuf *s, const char *a, size_t an, const char *b)
{
	while (an && (a[an - 1] == ' ' || a[an - 1] == ',')) an--;
	sb_putn(s, a, an);
	if (an && b[0]) sb_put(s, ", ");
	sb_put(s, b);
}

// "text:" の位置（引用テキスト指定の開始）を探す。無ければNULL
static const char *find_text_marker(const char *base, size_t n)
{
	for (size_t i = 0; i + 5 <= n; i++) {
		if (strncasecmp(base + i, "text:", 5)) continue;
		if (i + 5 < n && base[i + 5] == ':') continue; // 重み記法 "text::" は対象外
		if (i == 0 || strchr(" \t,.:[]{}", base[i - 1])) return base + i;
	}
	return NULL;
}

// 品質タグを足したプロンプトを作る
static void build_prompt(const Config *c, const ModelInfo *m, StrBuf *out)
{
	const char *p = c->prompt;
	const char *tags = c->quality ? m->quality : "";
	if (!tags[0]) {
		sb_put(out, p);
		return;
	}
	if (m->gen == 3) {
		// V3: "|" で区切った各部分の末尾（:重み の前）に足す
		while (1) {
			const char *bar = strchr(p, '|');
			size_t n = bar ? (size_t)(bar - p) : strlen(p);
			size_t w = n;
			while (w > 0 && (isdigit((unsigned char)p[w - 1]) || p[w - 1] == '.')) w--;
			if (w > 0 && w < n && p[w - 1] == ':') w--;
			else w = n;
			join_tags(out, p, w, tags);
			sb_putn(out, p + w, n - w);
			if (!bar) break;
			sb_put(out, "|");
			p = bar + 1;
		}
		return;
	}
	// V4以降: 最初の単独 "|" より前を本文とみなし、そこに足す（"||" は対象外）
	size_t n = strlen(p), cut = n;
	bool in_pair = false;
	for (size_t i = 0; i < n; i++) {
		if (p[i] != '|') continue;
		if (i + 1 < n && p[i + 1] == '|') {
			in_pair = !in_pair;
			i++;
		} else if (!in_pair) {
			cut = i;
			break;
		}
	}
	const char *mk = find_text_marker(p, cut);
	if (mk) {
		join_tags(out, p, (size_t)(mk - p), tags);
		sb_put(out, ", ");
		sb_putn(out, mk, cut - (size_t)(mk - p));
	} else {
		join_tags(out, p, cut, tags);
	}
	sb_putn(out, p + cut, n - cut);
}

static bool contains_nocase(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n)) return true;
	return false;
}

// プリセットとユーザー入力から除外プロンプトを作る
static void build_uc(const Config *c, const ModelInfo *m, int preset, const char *user_uc, StrBuf *out)
{
	const char *pre = m->uc[preset] ? m->uc[preset] : "";
	bool add_nsfw = !m->sfw && preset != UC_NONE && pre[0] && !contains_nocase(c->prompt, "nsfw");
	if (m->gen == 3) {
		StrBuf t;
		sb_init(&t);
		if (user_uc[0]) join_tags(&t, preset == UC_NONE ? "" : pre, preset == UC_NONE ? 0 : strlen(pre), user_uc);
		else sb_put(&t, pre);
		if (add_nsfw) join_tags(out, "nsfw", 4, t.p ? t.p : "");
		else sb_put(out, t.p ? t.p : "");
		sb_free(&t);
		return;
	}
	if (add_nsfw) sb_put(out, "nsfw, ");
	join_tags(out, pre, strlen(pre), user_uc);
}

static void put_center(StrBuf *o, const CharSlot *s)
{
	static const double cell[5] = {0.1, 0.3, 0.5, 0.7, 0.9};
	double x = 0.5, y = 0.5;
	if (s->gx >= 0 && s->gy >= 0) {
		x = cell[s->gx];
		y = cell[s->gy];
	}
	sb_printf(o, "{\"x\":%.1f,\"y\":%.1f}", x, y);
}

static void put_char_captions(StrBuf *o, const Config *c, bool negative, bool allow_uc)
{
	sb_put(o, "[");
	bool first = true;
	for (int i = 0; i < MAX_CHARS; i++) {
		const CharSlot *s = &c->chars[i];
		if (!s->enabled || !s->prompt[0]) continue;
		if (!first) sb_put(o, ",");
		first = false;
		sb_put(o, "{\"char_caption\":");
		sb_json_str(o, negative ? (allow_uc ? s->uc : "") : s->prompt);
		sb_put(o, ",\"centers\":[");
		put_center(o, s);
		sb_put(o, "]}");
	}
	sb_put(o, "]");
}

bool request_build(const Config *c, uint32_t seed, StrBuf *o) { return request_build_ex(c, seed, NULL, o); }

bool request_build_ex(const Config *c, uint32_t seed, const ReqExtra *x, StrBuf *o)
{
	const ModelInfo *m = &g_models[c->model];
	int act = x ? x->action : ACT_GENERATE;
	int width = act != ACT_GENERATE ? x->width : c->width;
	int height = act != ACT_GENERATE ? x->height : c->height;
	bool v4 = m->gen >= 4;

	// V5 Full Medium は ステップ14 / Euler Ancestral / 除外Heavy 固定で、
	// CFGリスケールと除外プロンプトの入力は使えない
	int steps = m->medium ? 14 : c->steps;
	int sampler = m->medium ? 0 : c->sampler;
	int preset = m->medium ? UC_HEAVY : c->uc_preset;
	if (!m->uc[preset]) preset = UC_LIGHT;
	const char *user_uc = m->medium ? "" : c->uc;

	StrBuf prompt, uc;
	sb_init(&prompt);
	sb_init(&uc);
	build_prompt(c, m, &prompt);
	build_uc(c, m, preset, user_uc, &uc);
	const char *ps = prompt.p ? prompt.p : "";
	const char *us = uc.p ? uc.p : "";

	bool use_coords = false;
	if (v4)
		for (int i = 0; i < MAX_CHARS; i++) {
			const CharSlot *s = &c->chars[i];
			if (s->enabled && s->prompt[0] && s->gx >= 0 && s->gy >= 0) use_coords = true;
		}

	sb_printf(o, "{\"action\":\"%s\",\"input\":", act == ACT_IMG2IMG ? "img2img" : act == ACT_INFILL ? "infill" : "generate");
	sb_json_str(o, ps);
	sb_put(o, ",\"model\":");
	sb_json_str(o, act == ACT_INFILL ? m->inpaint : m->id);
	sb_put(o, ",\"use_new_shared_trial\":true");
	sb_printf(o, ",\"parameters\":{\"params_version\":%d", m->gen == 5 ? 4 : 3);
	sb_put(o, ",\"legacy\":false,\"legacy_v3_extend\":false,\"deliberate_euler_ancestral_bug\":false"
		  ",\"prefer_brownian\":true,\"autoSmea\":false,\"sm\":false,\"sm_dyn\":false"
		  ",\"add_original_image\":false,\"dynamic_thresholding\":false,\"legacy_uc\":false"
		  ",\"normalize_reference_strength_multiple\":false");
	sb_printf(o, ",\"use_coords\":%s", use_coords ? "true" : "false");
	sb_printf(o, ",\"width\":%d,\"height\":%d,\"steps\":%d,\"scale\":%.2f", width, height, steps, (double)c->scale);
	sb_put(o, ",\"sampler\":");
	sb_json_str(o, g_samplers[sampler]);
	sb_printf(o, ",\"seed\":%lu,\"n_samples\":1,\"noise_schedule\":", (unsigned long)seed);
	sb_json_str(o, g_noises[c->noise]);
	if (!v4) {
		sb_put(o, ",\"prompt\":");
		sb_json_str(o, ps);
	}
	sb_put(o, ",\"negative_prompt\":");
	sb_json_str(o, us);
	if (v4) {
		sb_put(o, ",\"v4_prompt\":{\"caption\":{\"base_caption\":");
		sb_json_str(o, ps);
		sb_put(o, ",\"char_captions\":");
		put_char_captions(o, c, false, true);
		sb_printf(o, "},\"use_coords\":%s,\"use_order\":true}", use_coords ? "true" : "false");
		sb_put(o, ",\"v4_negative_prompt\":{\"caption\":{\"base_caption\":");
		sb_json_str(o, us);
		sb_put(o, ",\"char_captions\":");
		put_char_captions(o, c, true, !m->medium);
		sb_put(o, "},\"legacy_uc\":false}");
	}
	sb_printf(o, ",\"tag_hint_qt\":%d,\"tag_hint_uc_preset\":%d", c->quality ? 1 : 0, uc_hint[preset]);
	if (!m->medium) sb_printf(o, ",\"cfg_rescale\":%.2f", (double)c->cfg_rescale);
	if (c->variety && m->gen < 5) // V5 には Variety+ が無い
		sb_put(o, ",\"skip_cfg_above_sigma\":58");
	if (act == ACT_GENERATE) {
		sb_put(o, ",\"strength\":0.7");
	} else {
		// img2img: 元画像をどれだけ変えるか / infill: 塗った部分をどれだけ変えるか
		float st = act == ACT_IMG2IMG ? c->i2i_strength : c->inpaint_strength;
		sb_printf(o, ",\"strength\":%.2f,\"noise\":%.2f,\"extra_noise_seed\":%lu",
			  act == ACT_IMG2IMG ? (double)c->i2i_strength : 0.7, act == ACT_IMG2IMG ? (double)c->i2i_noise : 0.0,
			  (unsigned long)x->extra_noise_seed);
		sb_printf(o, ",\"img2img\":{\"color_correct\":true,\"strength\":%.2f},\"inpaintImg2ImgStrength\":%.2f", (double)st, (double)st);
		sb_put(o, ",\"image\":\"image\"");
		if (x->image_key) sb_printf(o, ",\"image_cache_secret_key\":\"%s\"", x->image_key);
		if (act == ACT_INFILL) {
			sb_put(o, ",\"mask\":\"mask\"");
			if (x->mask_key) sb_printf(o, ",\"mask_cache_secret_key\":\"%s\"", x->mask_key);
		}
	}
	if (c->recv_webp) sb_put(o, ",\"image_format\":\"webp\""); // 可逆の WebP で受け取る
	sb_put(o, ",\"controlnet_strength\":1,\"characterPrompts\":[");
	if (v4) {
		bool first = true;
		for (int i = 0; i < MAX_CHARS; i++) {
			const CharSlot *s = &c->chars[i];
			if (!s->prompt[0]) continue;
			if (!first) sb_put(o, ",");
			first = false;
			sb_put(o, "{\"prompt\":");
			sb_json_str(o, s->prompt);
			sb_put(o, ",\"uc\":");
			sb_json_str(o, m->medium ? "" : s->uc);
			sb_put(o, ",\"center\":");
			put_center(o, s);
			sb_printf(o, ",\"enabled\":%s}", s->enabled ? "true" : "false");
		}
	}
	sb_put(o, "]}}");

	bool ok = !o->oom && !prompt.oom && !uc.oom;
	sb_free(&prompt);
	sb_free(&uc);
	return ok;
}

bool director_build(const Config *c, int w, int h, StrBuf *o)
{
	const DirTool *t = &g_dir_tools[c->dir_tool];
	sb_put(o, "{\"req_type\":");
	sb_json_str(o, t->req_type);
	sb_printf(o, ",\"width\":%d,\"height\":%d,\"image\":\"image\"", w, h);
	if (t->takes_prompt) {
		sb_put(o, ",\"prompt\":");
		if (c->dir_tool == DIR_EMOTION) {
			// 表情は "気分;;追加のタグ" の形
			StrBuf p;
			sb_init(&p);
			sb_put(&p, g_emotions[c->dir_emotion]);
			sb_put(&p, ";;");
			sb_put(&p, c->dir_prompt);
			sb_json_str(o, p.p ? p.p : "");
			if (p.oom) o->oom = true;
			sb_free(&p);
		} else {
			sb_json_str(o, c->dir_prompt);
		}
		sb_printf(o, ",\"defry\":%d", c->dir_defry);
	}
	sb_put(o, "}");
	return !o->oom;
}

// ---------------------------------------------------------------- Anlasの見積もり
// 非公式SDK (caru-ini/novelai-sdk, utils/anlas.py) の計算式。WebUIからの推定なので目安。

#define AREA_C 2.951823174884865e-6
#define STEP_C 5.753298233447344e-7
#define OPUS_MAX_PIXELS 1048576.0

int anlas_generate(const Config *c, int action, int w, int h, bool is_opus)
{
	const ModelInfo *m = &g_models[c->model];
	if (action == ACT_GENERATE) {
		w = c->width;
		h = c->height;
	}
	int steps = m->medium ? 14 : c->steps;
	double strength = action == ACT_IMG2IMG ? c->i2i_strength : action == ACT_INFILL ? c->inpaint_strength : 1.0;
	if (!(strength > 0)) strength = 1.0;
	double area = (double)w * h;
	double sf = m->medium ? 1.0 / 1.06521739 : 1.0;
	double base = ceil(AREA_C * area + STEP_C * area * steps * sf);
	int per = (int)ceil(base * strength);
	if (per < 2) per = 2;
	if (per > 140) return -1; // WebUIでも生成できない大きさ
	bool free1 = is_opus && area <= OPUS_MAX_PIXELS && steps <= 28;
	return free1 ? 0 : per; // 1枚ずつ生成するので、Opusの無料枠に入れば0
}

int anlas_director(int tool, int w, int h, bool is_opus)
{
	double a = (double)w * h;
	if (a <= 0) return 0;
	if (a > 3145728.0) {
		double f = sqrt(3145728.0 / a);
		w = (int)floor(w * f);
		h = (int)floor(h * f);
	}
	a = (double)w * h;
	if (a < 1048576.0) {
		double f = sqrt(1048576.0 / a);
		w = (int)floor(w * f);
		h = (int)floor(h * f);
	}
	a = (double)w * h;
	int base = (int)ceil(AREA_C * a + STEP_C * a * 28);
	if (base < 2) base = 2;
	if (tool == DIR_BGREMOVE) return 3 * base + 5; // 背景除去はOpusでも有料
	return is_opus && a <= OPUS_MAX_PIXELS ? 0 : base;
}
