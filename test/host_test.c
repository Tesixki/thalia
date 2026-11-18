// PC上で動かす確認用プログラム（3DSに依存しない部分のテスト）
//   gcc -std=gnu11 -Wall -Wextra -Isource -Ithird_party/libwebp test/host_test.c source/util.c source/config.c
//       source/imgcodec.c third_party/libwebp/src/*/*.c -lz -lm -lpthread -o /tmp/host_test
//   /tmp/host_test <出力先> <PNG入りのZIP>  （出力先に s_rgb.png / s_rgb.webp などがあれば WebP も確かめる）
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "util.h"

#define STBI_ONLY_PNG
#include "imgcodec.h"
#include "stb_image.h" // 本体は imgcodec.c にある

static int find_model(const char *id)
{
	for (int i = 0; i < g_model_count; i++)
		if (!strcmp(g_models[i].id, id)) return i;
	assert(!"model");
	return 0;
}

static void emit(FILE *f, const Config *c, uint32_t seed)
{
	StrBuf sb;
	sb_init(&sb);
	assert(request_build(c, seed, &sb));
	fprintf(f, "%s\n", sb.p);
	sb_free(&sb);
}

static void requests(const char *path)
{
	FILE *f = fopen(path, "w");
	assert(f);
	Config c;

	config_defaults(&c); // 0: V5 Full 既定値
	strcpy(c.prompt, "1girl, \"quoted\" back\\slash, 日本語");
	strcpy(c.uc, "bad hands");
	emit(f, &c, 12345);

	config_defaults(&c); // 1: V5 Full Medium（除外やステップは無視されるはず）
	c.model = find_model("nai-diffusion-5-full-medium");
	strcpy(c.prompt, "1boy, city");
	c.steps = 40;
	c.sampler = 3;
	c.cfg_rescale = 0.5f;
	c.uc_preset = UC_NONE;
	emit(f, &c, 1);

	config_defaults(&c); // 2: V4.5 Curated + キャラ2人（位置あり・無効1人）
	c.model = find_model("nai-diffusion-4-5-curated");
	strcpy(c.prompt, "2girls, park");
	c.uc_preset = UC_HUMAN;
	strcpy(c.chars[0].prompt, "girl, red hair");
	strcpy(c.chars[0].uc, "hat");
	c.chars[0].gx = 1;
	c.chars[0].gy = 2;
	strcpy(c.chars[1].prompt, "girl, blue hair");
	strcpy(c.chars[2].prompt, "boy");
	c.chars[2].enabled = false;
	c.variety = true;
	c.width = 1216;
	c.height = 832;
	emit(f, &c, 4294967294u);

	config_defaults(&c); // 3: V3、"|" 区切りと重み
	c.model = find_model("nai-diffusion-3");
	strcpy(c.prompt, "1girl, smile:1.2|landscape");
	strcpy(c.uc, "text");
	c.uc_preset = UC_HEAVY;
	c.sampler = 4;
	c.noise = 1;
	c.steps = 28;
	c.scale = 6.5f;
	emit(f, &c, 7);

	config_defaults(&c); // 4: V4 Full、品質タグなし・プリセットなし
	c.model = find_model("nai-diffusion-4-full");
	strcpy(c.prompt, "nsfw, 1girl");
	c.quality = false;
	c.uc_preset = UC_NONE;
	c.cfg_rescale = 0.3f;
	emit(f, &c, 99);

	config_defaults(&c); // 5: V4.5 Full、"|" を含むプロンプト
	c.model = find_model("nai-diffusion-4-5-full");
	strcpy(c.prompt, "1girl, ||a|b||, tree | extra part");
	c.uc_preset = UC_FURRY;
	emit(f, &c, 5);

	config_defaults(&c); // 6: V3 除外プリセットなし + 入力あり / 7: 入力なし
	c.model = find_model("nai-diffusion-3");
	strcpy(c.prompt, "cat");
	c.uc_preset = UC_NONE;
	strcpy(c.uc, "dog");
	emit(f, &c, 6);
	c.uc[0] = 0;
	emit(f, &c, 6);

	config_defaults(&c); // 8: V5 Curated、Heavy
	c.model = find_model("nai-diffusion-5-curated");
	strcpy(c.prompt, "scenery");
	c.uc_preset = UC_HEAVY;
	emit(f, &c, 8);

	config_defaults(&c); // 9: V4.5 Full、WebP で受け取る
	c.model = find_model("nai-diffusion-4-5-full");
	strcpy(c.prompt, "1girl");
	c.recv_webp = true;
	emit(f, &c, 10);
	fclose(f);
}

static uint8_t *load(const char *dir, const char *name, size_t *n)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)len);
	assert(buf && fread(buf, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len;
	return buf;
}

// 可逆 WebP と PNG が同じ画素に展開されること、縮小しながらの展開、ZIP の中の WebP
static void webp_tests(const char *dir)
{
	const char *pairs[2][2] = {{"s_rgb.png", "s_rgb.webp"}, {"s_rgba.png", "s_rgba.webp"}};
	for (int k = 0; k < 2; k++) {
		size_t pn, wn;
		uint8_t *png = load(dir, pairs[k][0], &pn), *webp = load(dir, pairs[k][1], &wn);
		if (!png || !webp) {
			printf("(WebP の見本が無いので WebP のテストは飛ばします)\n");
			free(png);
			free(webp);
			return;
		}
		assert(img_detect(png, pn) == IMG_PNG && img_detect(webp, wn) == IMG_WEBP);
		assert(!strcmp(img_ext(IMG_WEBP), ".webp"));
		int w1, h1, w2, h2;
		bool alpha;
		assert(img_info(webp, wn, &w1, &h1, &alpha) && w1 == 333 && h1 == 217 && alpha == (k == 1));
		uint8_t *a = img_decode_rgb(png, pn, true, &w1, &h1);
		uint8_t *b = img_decode_rgb(webp, wn, true, &w2, &h2);
		assert(a && b && w1 == w2 && h1 == h2 && !memcmp(a, b, (size_t)w1 * h1 * 3));
		free(a);
		free(b);
		int vw, vh, fw, fh;
		uint8_t *v = img_decode_rgb_fit(webp, wn, 100, 100, true, &vw, &vh, &fw, &fh);
		assert(v && vw == 100 && vh == 65 && fw == 333 && fh == 217);
		free(v);
		v = img_decode_rgb_fit(png, pn, 100, 100, true, &vw, &vh, &fw, &fh);
		assert(v && vw == 100 && vh == 65 && fw == 333 && fh == 217);
		free(v);
		v = img_decode_rgb_fit(webp, wn, 1024, 1024, true, &vw, &vh, &fw, &fh); // 小さい画像は縮小しない
		assert(v && vw == 333 && vh == 217);
		free(v);
		assert(!img_decode_rgb(webp, wn / 2, true, &w1, &h1)); // 途中で切れたもの
		free(png);
		free(webp);
	}
	size_t zn;
	uint8_t *zip = load(dir, "img_webp.zip", &zn);
	assert(zip);
	uint8_t *img;
	size_t len;
	assert(zip_extract_image(zip, zn, &img, &len) && img_detect(img, len) == IMG_WEBP);
	free(img);
	free(zip);
	printf("WebP tests OK\n");
}

// img2img / infill（compare_sdk.py の EDIT_CASES と同じ順番）
static void edit_requests(const char *path)
{
	FILE *f = fopen(path, "w");
	assert(f);
	Config c;
	StrBuf sb;
	ReqExtra x;

	config_defaults(&c); // 0: V5 Full img2img
	strcpy(c.prompt, "1girl, smile");
	c.i2i_strength = 0.55f;
	c.i2i_noise = 0.1f;
	x = (ReqExtra){ACT_IMG2IMG, 1024, 704, 777, NULL, NULL};
	sb_init(&sb);
	assert(request_build_ex(&c, 42, &x, &sb));
	fprintf(f, "%s\n", sb.p);
	sb_free(&sb);

	config_defaults(&c); // 1: V5 Curated インペイント
	c.model = find_model("nai-diffusion-5-curated");
	strcpy(c.prompt, "cat");
	c.inpaint_strength = 0.8f;
	x = (ReqExtra){ACT_INFILL, 832, 1216, 888, NULL, NULL};
	sb_init(&sb);
	assert(request_build_ex(&c, 43, &x, &sb));
	fprintf(f, "%s\n", sb.p);
	sb_free(&sb);

	config_defaults(&c); // 2: V3 img2img
	c.model = find_model("nai-diffusion-3");
	strcpy(c.prompt, "landscape");
	x = (ReqExtra){ACT_IMG2IMG, 832, 1216, 5, NULL, NULL};
	sb_init(&sb);
	assert(request_build_ex(&c, 44, &x, &sb));
	fprintf(f, "%s\n", sb.p);
	sb_free(&sb);

	config_defaults(&c); // 3: V4 Curated インペイント
	c.model = find_model("nai-diffusion-4-curated-preview");
	strcpy(c.prompt, "dog");
	strcpy(c.uc, "blurry");
	x = (ReqExtra){ACT_INFILL, 1216, 832, 6, NULL, NULL};
	sb_init(&sb);
	assert(request_build_ex(&c, 45, &x, &sb));
	fprintf(f, "%s\n", sb.p);
	sb_free(&sb);
	fclose(f);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	const char *dir = argv[1], *zip_path = argv[2];
	char path[512];

	// --- リクエスト
	snprintf(path, sizeof(path), "%s/requests.jsonl", dir);
	requests(path);
	snprintf(path, sizeof(path), "%s/requests_edit.jsonl", dir);
	edit_requests(path);

	// --- JSON読み取り
	char s[64];
	const char *js = "{\"tags\":[{\"tag\":\"cat \\\"x\\\" \\u732b\",\"count\":12,\"confidence\":0.5},{\"tag\":\"dog\",\"count\":3}]}";
	const char *p = json_find_str(js, "tag", s, sizeof(s));
	assert(p && !strcmp(s, "cat \"x\" 猫"));
	double d;
	assert(json_find_num(p, "count", &d) && d == 12);
	p = json_find_str(p, "tag", s, sizeof(s));
	assert(p && !strcmp(s, "dog"));
	assert(!json_find_str(p, "tag", s, sizeof(s)));
	bool b = false;
	assert(json_find_bool("{\"active\" : true}", "active", &b) && b);
	assert(json_find_num("{\"trainingStepsLeft\":{\"fixedTrainingStepsLeft\":9500,\"purchasedTrainingSteps\":20}}", "purchasedTrainingSteps", &d) && d == 20);

	// --- 文字列
	char t[16] = "a, ";
	str_append_tag(t, sizeof(t), "bb");
	assert(!strcmp(t, "a, bb"));
	str_append_tag(t, sizeof(t), "this is too long");
	assert(!strcmp(t, "a, bb"));
	char u[8];
	str_copy(u, sizeof(u), "あいう"); // 9バイト→文字の途中で切らない
	assert(!strcmp(u, "あい"));
	StrBuf sb;
	sb_init(&sb);
	sb_urlenc(&sb, "a b/猫");
	assert(!strcmp(sb.p, "a%20b%2F%E7%8C%AB"));
	sb_free(&sb);

	// --- 設定の保存と読み込み
	Config *c1 = malloc(sizeof(Config)), *c2 = malloc(sizeof(Config));
	config_defaults(c1);
	strcpy(c1->api_key, "pst-abc");
	strcpy(c1->prompt, "line1\nline2 \\ back=slash");
	strcpy(c1->chars[3].prompt, "girl");
	c1->chars[3].gx = 4;
	c1->chars[3].gy = 0;
	c1->chars[3].enabled = false;
	c1->model = 4;
	c1->seed = 4000000000u;
	c1->seed_random = false;
	c1->scale = 7.5f;
	c1->cfg_rescale = 0.35f;
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	assert(config_save(c1, path, true));
	config_defaults(c2);
	assert(config_load(c2, path));
	assert(!memcmp(c1, c2, sizeof(Config)));
	free(c1);
	free(c2);

	// --- ZIP展開 → PNG展開 → 縮小
	FILE *f = fopen(zip_path, "rb");
	assert(f);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *zip = malloc((size_t)n);
	assert(fread(zip, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	uint8_t *png;
	size_t png_len;
	assert(zip_extract_image(zip, (size_t)n, &png, &png_len));
	snprintf(path, sizeof(path), "%s/extracted.png", dir);
	f = fopen(path, "wb");
	fwrite(png, 1, png_len, f);
	fclose(f);
	int w, h, comp;
	uint8_t *rgb = stbi_load_from_memory(png, (int)png_len, &w, &h, &comp, 3);
	assert(rgb && w == 832 && h == 1216);
	int vw, vh;
	uint8_t *view = img_resize_fit(rgb, w, h, 1024, 1024, &vw, &vh);
	assert(view && vw == 701 && vh == 1024);
	int tw, th;
	uint8_t *thumb = img_resize_fit(rgb, w, h, 60, 88, &tw, &th);
	assert(thumb && tw == 60 && th == 88);
	snprintf(path, sizeof(path), "%s/view.ppm", dir);
	f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", vw, vh);
	fwrite(view, 1, (size_t)vw * vh * 3, f);
	fclose(f);
	// 壊れたデータで落ちないこと
	uint8_t *o;
	size_t ol;
	for (long cut = 0; cut < n; cut += n / 37 + 1) assert(!zip_extract_image(zip, (size_t)cut, &o, &ol) || (free(o), 1));
	zip[n / 2] ^= 0xFF;
	if (zip_extract_image(zip, (size_t)n, &o, &ol)) free(o);
	assert(!zip_extract_nth(zip, (size_t)n, 1, &o, &ol)); // 1枚しか入っていない
	int iw, ih, ct;
	assert(png_info(png, png_len, &iw, &ih, &ct) && iw == 832 && ih == 1216 && ct == 2);
	free(zip);
	free(png);

	// --- PNGの書き出し → 読み戻し（RGB / RGBA）
	for (int ch = 3; ch <= 4; ch++) {
		int ew = 37, eh = 23;
		uint8_t *src = malloc((size_t)ew * eh * ch);
		for (int i = 0; i < ew * eh * ch; i++) src[i] = (uint8_t)(i * 7 + i / 5);
		uint8_t *enc;
		size_t enc_len;
		assert(png_encode(src, ew, eh, ch, &enc, &enc_len));
		assert(png_info(enc, enc_len, &iw, &ih, &ct) && iw == ew && ih == eh && ct == (ch == 4 ? 6 : 2));
		int dw, dh, dc;
		uint8_t *dec = stbi_load_from_memory(enc, (int)enc_len, &dw, &dh, &dc, ch);
		assert(dec && dw == ew && dh == eh && !memcmp(dec, src, (size_t)ew * eh * ch));
		stbi_image_free(dec);
		free(enc);
		free(src);
	}

	// --- Base64
	char b64[16];
	assert(b64_encode((const uint8_t *)"", 0, b64) == 0);
	b64[b64_encode((const uint8_t *)"f", 1, b64)] = 0;
	assert(!strcmp(b64, "Zg=="));
	b64[b64_encode((const uint8_t *)"fo", 2, b64)] = 0;
	assert(!strcmp(b64, "Zm8="));
	b64[b64_encode((const uint8_t *)"foobar", 6, b64)] = 0;
	assert(!strcmp(b64, "Zm9vYmFy"));

	// --- サイズ合わせ
	int fw, fh;
	img_fit64(832, 1216, &fw, &fh);
	assert(fw == 832 && fh == 1216);
	img_fit64(1000, 700, &fw, &fh);
	assert(fw % 64 == 0 && fh % 64 == 0 && fw == 1024 && fh == 704);
	img_fit64(3000, 2000, &fw, &fh);
	assert(fw % 64 == 0 && fh % 64 == 0 && (double)fw * fh <= 1100000.0);
	img_fit64(10, 10, &fw, &fh);
	assert(fw == 64 && fh == 64);
	uint8_t px3[2 * 2 * 3] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 0};
	uint8_t *up = img_resize_to(px3, 2, 2, 4, 4);
	assert(up && up[0] == 0 && up[(4 * 0 + 3) * 3] == 255);
	free(up);

	// --- img2img / infill / ディレクターツールのリクエスト
	Config ce;
	config_defaults(&ce);
	strcpy(ce.prompt, "1girl");
	ReqExtra x = {ACT_IMG2IMG, 1024, 704, 123, "ab12", NULL};
	StrBuf sbx;
	sb_init(&sbx);
	assert(request_build_ex(&ce, 5, &x, &sbx));
	assert(strstr(sbx.p, "\"action\":\"img2img\"") && strstr(sbx.p, "\"model\":\"nai-diffusion-5-full\""));
	assert(strstr(sbx.p, "\"width\":1024,\"height\":704") && strstr(sbx.p, "\"strength\":0.70,\"noise\":0.00,\"extra_noise_seed\":123"));
	assert(strstr(sbx.p, "\"img2img\":{\"color_correct\":true,\"strength\":0.70}") && strstr(sbx.p, "\"image\":\"image\",\"image_cache_secret_key\":\"ab12\""));
	assert(!strstr(sbx.p, "\"mask\""));
	sb_free(&sbx);
	ce.model = find_model("nai-diffusion-5-curated");
	x.action = ACT_INFILL;
	x.mask_key = "cd34";
	sb_init(&sbx);
	assert(request_build_ex(&ce, 5, &x, &sbx));
	assert(strstr(sbx.p, "\"action\":\"infill\"") && strstr(sbx.p, "\"model\":\"nai-diffusion-4-5-curated-inpainting\""));
	assert(strstr(sbx.p, "\"strength\":0.70,\"noise\":0.00") && strstr(sbx.p, "\"inpaintImg2ImgStrength\":1.00"));
	assert(strstr(sbx.p, "\"mask\":\"mask\",\"mask_cache_secret_key\":\"cd34\"") && strstr(sbx.p, "\"params_version\":4"));
	sb_free(&sbx);
	ce.dir_tool = DIR_EMOTION;
	ce.dir_emotion = 2;
	ce.dir_defry = 3;
	sb_init(&sbx);
	assert(director_build(&ce, 832, 1216, &sbx));
	assert(!strcmp(sbx.p, "{\"req_type\":\"emotion\",\"width\":832,\"height\":1216,\"image\":\"image\",\"prompt\":\"sad;;\",\"defry\":3}"));
	sb_free(&sbx);
	ce.dir_tool = DIR_BGREMOVE;
	sb_init(&sbx);
	assert(director_build(&ce, 64, 128, &sbx));
	assert(!strcmp(sbx.p, "{\"req_type\":\"bg-removal\",\"width\":64,\"height\":128,\"image\":\"image\"}"));
	sb_free(&sbx);

	webp_tests(dir);

	// --- V5 には Variety+ を送らない（V4.5 には送る）
	config_defaults(&ce);
	strcpy(ce.prompt, "x");
	ce.variety = true;
	sb_init(&sbx);
	assert(request_build(&ce, 1, &sbx) && !strstr(sbx.p, "skip_cfg_above_sigma"));
	sb_free(&sbx);
	ce.model = find_model("nai-diffusion-4-5-full");
	sb_init(&sbx);
	assert(request_build(&ce, 1, &sbx) && strstr(sbx.p, "\"skip_cfg_above_sigma\":58"));
	sb_free(&sbx);

	// --- RGB565 と表示用キャッシュ（.vw）の往復
	{
		uint8_t px3b[3 * 3] = {255, 0, 0, 0, 255, 0, 8, 4, 255};
		uint16_t c565[3];
		rgb_to_565(px3b, 3, c565);
		assert(c565[0] == 0xF800 && c565[1] == 0x07E0 && c565[2] == ((1 << 11) | (1 << 5) | 31));
		int vw = 37, vh = 21;
		uint16_t *img = malloc((size_t)vw * vh * 2);
		for (int i = 0; i < vw * vh; i++) img[i] = (uint16_t)(i * 2654435761u >> 7);
		uint8_t *enc;
		size_t enc_len;
		assert(vw_encode(img, vw, vh, 832, 1216, &enc, &enc_len));
		uint16_t *dec;
		int dw, dh, dfw, dfh;
		assert(vw_decode(enc, enc_len, &dec, &dw, &dh, &dfw, &dfh));
		assert(dw == vw && dh == vh && dfw == 832 && dfh == 1216 && !memcmp(dec, img, (size_t)vw * vh * 2));
		free(dec);
		assert(!vw_decode(enc, enc_len / 2, &dec, &dw, &dh, &dfw, &dfh)); // 途中で切れたものは使わない
		enc[0] = 'X';
		assert(!vw_decode(enc, enc_len, &dec, &dw, &dh, &dfw, &dfh));
		free(enc);
		free(img);
	}

	// --- Anlasの見積もり（値は非公式SDKの tests/test_anlas.py・test_v5.py と同じ）
	Config ca;
	config_defaults(&ca);
	ca.model = find_model("nai-diffusion-4-5-full");
	ca.width = ca.height = 1024;
	ca.steps = 28;
	assert(anlas_generate(&ca, ACT_GENERATE, 0, 0, false) == 20);
	assert(anlas_generate(&ca, ACT_GENERATE, 0, 0, true) == 0);
	ca.steps = 23;
	ca.i2i_strength = 0.7f;
	assert(anlas_generate(&ca, ACT_IMG2IMG, 1024, 1024, false) == 12);
	ca.steps = 28;
	ca.inpaint_strength = 0.5f;
	assert(anlas_generate(&ca, ACT_INFILL, 1024, 1024, false) == 10);
	ca.model = find_model("nai-diffusion-5-full-medium");
	ca.width = 1024;
	ca.height = 1536;
	assert(anlas_generate(&ca, ACT_GENERATE, 0, 0, true) == 17); // 1MP超えはOpusでも有料
	ca.model = find_model("nai-diffusion-5-full");
	ca.steps = 14;
	assert(anlas_generate(&ca, ACT_GENERATE, 0, 0, false) == 18);
	ca.width = ca.height = 1600;
	ca.steps = 50;
	assert(anlas_generate(&ca, ACT_GENERATE, 0, 0, false) == 82); // 設定できる最大（上限の140には届かない）
	assert(anlas_director(DIR_LINEART, 1024, 1024, false) == 20);
	assert(anlas_director(DIR_LINEART, 1024, 1024, true) == 0);
	assert(anlas_director(DIR_BGREMOVE, 1024, 1024, true) == 65);
	assert(anlas_director(DIR_SKETCH, 1536, 2048, true) == 60);
	assert(anlas_director(DIR_BGREMOVE, 1536, 2048, false) == 185);

	stbi_image_free(rgb);
	free(view);
	free(thumb);
	puts("host tests OK");
	return 0;
}
