// Thalia - NovelAI 画像生成の非公式クライアント (Nintendo 3DS ホームブリュー)
#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "fx.h"
#include "nai.h"
#include "ui.h"
#include "util.h"

enum { TAB_GEN, TAB_EDIT, TAB_CHR, TAB_TAG, TAB_GAL, TAB_SET, TAB_COUNT };
static const char *const tab_labels[2][TAB_COUNT] = {
	{"生成", "編集", "キャラ", "タグ", "一覧", "設定"},
	{"Create", "Edit", "Chars", "Tags", "Gallery", "Settings"},
};
#define TAB_H 26
#define FOOT_Y (SCR_H - 22)
#define BAR_H 20 // 上画面のステータスバー

static Config cfg;
static bool cfg_dirty;
static int tab;
static bool quit;

// 上画面に出している画像
static Pic view;
static char view_name[48];
static int view_fw, view_fh;
// 拡大ビューア（下画面で操作する）
static bool viewer_on;
static float vz;       // 拡大率（表示用画像の1pxが上画面で何pxか）
static float vcx, vcy; // 上画面の真ん中に来る位置（表示用画像の座標）
static bool viewer_drag_map, viewer_drag_zoom;

// アカウントとキーの状態
enum { KEY_NONE, KEY_CHECKING, KEY_OK, KEY_BAD, KEY_UNKNOWN };
static int key_state;
static bool acct_valid;
static int acct_anlas;
static int acct_tier;
static bool acct_active;
static bool usage_valid, usage_negative; // V5の残量（Opus）
static int usage_percent, usage_purchased, usage_secs;
static u32 last_seed;
static bool has_last_seed;

// 本体の状態（ステータスバー用）
static bool ptm_ok;
static u8 battery_level = 5, battery_charging;

// メッセージ
static char msg[256];
static bool msg_on;
static char flash[96];
static int flash_t;

static void show_msg(const char *s)
{
	str_copy(msg, sizeof(msg), s);
	msg_on = true;
}

static void show_flash(const char *s)
{
	str_copy(flash, sizeof(flash), s);
	flash_t = 120;
}

// 表示の言語を決める。「自動」は本体の言語設定に従う（日本語以外は英語）
static void apply_lang(void)
{
	if (cfg.lang == LANG_JA) g_lang = 0;
	else if (cfg.lang == LANG_EN) g_lang = 1;
	else {
		u8 l = CFG_LANGUAGE_JP;
		if (R_SUCCEEDED(cfguInit())) {
			CFGU_GetSystemLanguage(&l);
			cfguExit();
		}
		g_lang = l == CFG_LANGUAGE_JP ? 0 : 1;
	}
}

static void save_cfg(void)
{
	if (!cfg_dirty) return;
	config_save(&cfg, CONFIG_PATH, true);
	cfg_dirty = false;
}

static int wrapi(int v, int n) { return (v % n + n) % n; }

static u32 alpha_of(u32 c, float a)
{
	a = a < 0 ? 0 : a > 1 ? 1 : a;
	return (c & 0x00FFFFFF) | ((u32)(((c >> 24) & 0xFF) * a + 0.5f) << 24);
}

// 演出のタイマー
#define INTRO_LEN 100
static int intro_f; // 起動からのフレーム数（ロゴの登場）
static int busy_f;  // 通信が始まってからのフレーム数

// ---------------------------------------------------------------- ソフトウェアキーボード

static bool kb_text(char *buf, size_t bufsz, const char *hint, int maxlen)
{
	char *tmp = malloc(bufsz);
	if (!tmp) return false;
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, maxlen);
	swkbdSetInitialText(&kb, buf);
	swkbdSetHintText(&kb, hint);
	swkbdSetFeatures(&kb, SWKBD_PREDICTIVE_INPUT);
	swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
	tmp[0] = 0;
	bool ok = swkbdInputText(&kb, tmp, bufsz) == SWKBD_BUTTON_RIGHT;
	if (ok) str_copy(buf, bufsz, tmp);
	free(tmp);
	return ok;
}

static bool kb_number(unsigned long *val, const char *hint, int digits)
{
	char tmp[16] = "";
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, digits);
	swkbdSetHintText(&kb, hint);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY, 0, 0);
	if (swkbdInputText(&kb, tmp, sizeof(tmp)) != SWKBD_BUTTON_RIGHT) return false;
	*val = strtoul(tmp, NULL, 10);
	return true;
}

// ---------------------------------------------------------------- 保管タグ

#define TAGS_MAX 400
static char tags[TAGS_MAX][TAG_LEN];
static int tag_count;
static Suggestion results[MAX_SUGGEST];
static int result_count;
static int tag_mode; // 0:保管 1:検索結果
static int tag_cur, tag_scroll;
static int tag_target; // 0:プロンプト 1:除外 2:選択中のキャラ
static int chr_cur;

static const char *const default_tags[] = {
	"1girl", "1boy", "solo", "looking at viewer", "smile", "long hair", "short hair", "upper body",
	"full body", "cowboy shot", "from side", "outdoors", "indoors", "night", "sunset", "simple background",
	"white background", "school uniform", "dress", "landscape", "no humans", "chibi", "pixel art", "watercolor (medium)",
};

static void tags_save(void)
{
	FILE *f = fopen(TAGS_PATH, "w");
	if (!f) return;
	for (int i = 0; i < tag_count; i++) fprintf(f, "%s\n", tags[i]);
	fclose(f);
}

static bool tags_add(const char *t)
{
	if (!t[0] || tag_count >= TAGS_MAX) return false;
	for (int i = 0; i < tag_count; i++)
		if (!strcasecmp(tags[i], t)) return false;
	str_copy(tags[tag_count++], TAG_LEN, t);
	return true;
}

static void tags_load(void)
{
	FILE *f = fopen(TAGS_PATH, "r");
	if (!f) {
		for (size_t i = 0; i < sizeof(default_tags) / sizeof(default_tags[0]); i++) tags_add(default_tags[i]);
		tags_save();
		return;
	}
	char line[TAG_LEN + 8];
	while (fgets(line, sizeof(line), f)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
		tags_add(line);
	}
	fclose(f);
}

static int tag_list_count(void) { return tag_mode == 0 ? tag_count : result_count; }
static const char *tag_list_item(int i) { return tag_mode == 0 ? tags[i] : results[i].tag; }

static void tag_append_current(void)
{
	if (tag_cur >= tag_list_count()) return;
	const char *t = tag_list_item(tag_cur);
	char *dst = tag_target == 0 ? cfg.prompt : tag_target == 1 ? cfg.uc : cfg.chars[chr_cur].prompt;
	size_t before = strlen(dst);
	str_append_tag(dst, MAX_PROMPT, t);
	if (strlen(dst) == before) {
		show_flash(L("これ以上入りません", "No more room"));
		return;
	}
	cfg_dirty = true;
	char b[96];
	snprintf(b, sizeof(b), L("追加しました: %.60s", "Added: %.60s"), t);
	show_flash(b);
}

// ---------------------------------------------------------------- ギャラリー

#define GAL_MAX 2000
#define GAL_COLS 5
#define GAL_PER_PAGE 10
#define GAL_NAME 40
static char (*gal_names)[GAL_NAME];
static int gal_count, gal_cur, gal_page = -1;
static Pic gal_thumbs[GAL_PER_PAGE];
static bool gal_dirty = true;
static int gal_del_confirm = -1;

static int cmp_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static void gal_scan(void)
{
	if (!gal_names) gal_names = calloc(GAL_MAX, GAL_NAME);
	gal_count = 0;
	gal_page = -1;
	gal_dirty = false;
	if (!gal_names) return;
	DIR *d = opendir(IMG_DIR);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && gal_count < GAL_MAX) {
		size_t n = strlen(e->d_name), base;
		if (e->d_name[0] == '.') continue;
		if (n > 4 && !strcasecmp(e->d_name + n - 4, ".png")) base = n - 4;
		else if (n > 5 && !strcasecmp(e->d_name + n - 5, ".webp")) base = n - 5;
		else continue;
		if (base >= GAL_NAME) continue;
		memcpy(gal_names[gal_count], e->d_name, base);
		gal_names[gal_count][base] = 0;
		gal_count++;
	}
	closedir(d);
	qsort(gal_names, (size_t)gal_count, GAL_NAME, cmp_desc); // 新しい順
	if (gal_cur >= gal_count) gal_cur = gal_count ? gal_count - 1 : 0;
}

static bool gal_thumb_tried[GAL_PER_PAGE]; // このページで、サムネイル作りを試したか

static void gal_load_thumb(int i)
{
	pic_free(&gal_thumbs[i]);
	int idx = gal_page * GAL_PER_PAGE + i;
	if (idx >= gal_count) return;
	{
		char path[128];
		snprintf(path, sizeof(path), IMG_DIR "/%s.thm", gal_names[idx]);
		FILE *f = fopen(path, "rb");
		if (!f) return;
		uint8_t hd[8];
		if (fread(hd, 1, 8, f) == 8 && !memcmp(hd, "NTH1", 4)) {
			int w = hd[4] | (hd[5] << 8), h = hd[6] | (hd[7] << 8);
			if (w >= 1 && h >= 1 && w <= 128 && h <= 128) {
				uint8_t *rgb = malloc((size_t)w * h * 3);
				if (rgb && fread(rgb, 1, (size_t)w * h * 3, f) == (size_t)w * h * 3) pic_from_rgb(&gal_thumbs[i], rgb, w, h);
				free(rgb);
			}
		}
		fclose(f);
	}
}

static void gal_load_page(void)
{
	gal_page = gal_cur / GAL_PER_PAGE;
	for (int i = 0; i < GAL_PER_PAGE; i++) {
		gal_thumb_tried[i] = false;
		gal_load_thumb(i);
	}
}

// サムネイルが無い画像（PCから入れたPNGなど）は、一覧を開いている間に1枚ずつ作る
static void gal_make_missing_thumb(void)
{
	if (g_job.running || gal_page < 0) return;
	for (int i = 0; i < GAL_PER_PAGE; i++) {
		int idx = gal_page * GAL_PER_PAGE + i;
		if (idx >= gal_count) return;
		if (gal_thumbs[i].valid || gal_thumb_tried[i]) continue;
		gal_thumb_tried[i] = true;
		nai_start_thumb(gal_names[idx]);
		return;
	}
}

// 名前が今のページにあれば、そのサムネイルを読み直す
static void gal_refresh_thumb(const char *name)
{
	if (gal_page < 0) return;
	for (int i = 0; i < GAL_PER_PAGE; i++) {
		int idx = gal_page * GAL_PER_PAGE + i;
		if (idx < gal_count && !strcmp(gal_names[idx], name)) gal_load_thumb(i);
	}
}

static void gal_delete_current(void)
{
	if (gal_cur >= gal_count) return;
	static const char *const exts[] = {".png", ".webp", ".thm", ".txt", ".vw"};
	char path[128];
	for (int i = 0; i < 5; i++) {
		snprintf(path, sizeof(path), IMG_DIR "/%s%s", gal_names[gal_cur], exts[i]);
		remove(path);
	}
	gal_scan();
	show_flash(L("削除しました", "Deleted"));
}

static void gal_load_settings(void)
{
	if (gal_cur >= gal_count) return;
	char path[128];
	snprintf(path, sizeof(path), IMG_DIR "/%s.txt", gal_names[gal_cur]);
	Config *tmp = malloc(sizeof(Config));
	if (!tmp) return;
	// APIキーや編集の設定はそのまま、生成まわりだけ入れ替える
	memcpy(tmp, &cfg, sizeof(Config));
	if (config_load(tmp, path)) {
		str_copy(tmp->api_key, sizeof(tmp->api_key), cfg.api_key);
		tmp->verify_tls = cfg.verify_tls;
		tmp->lang = cfg.lang;
		tmp->recv_webp = cfg.recv_webp;
		memcpy(&cfg, tmp, sizeof(Config));
		cfg_dirty = true;
		show_flash(L("この画像の設定を読み込みました", "Loaded this image's settings"));
	} else {
		show_flash(L("この画像には設定の控えがありません", "No saved settings for this image"));
	}
	free(tmp);
}

// ---------------------------------------------------------------- 編集（マスク）

enum { EM_I2I, EM_INPAINT, EM_DIRECTOR, EM_COUNT };
static const char *const edit_mode_labels[2][EM_COUNT] = {
	{"画像から生成", "インペイント", "ディレクター"},
	{"img2img", "Inpaint", "Director"},
};
static int edit_mode;
static int edit_focus;

static uint8_t *mask;          // 8x8 ピクセル単位
static int mask_w, mask_h;     // マスクのマス数（送るサイズの1/8）
static char mask_name[48];     // どの画像のマスクか
static Pic mask_pic;
static bool mask_editing;
static bool mask_erase;
static int mask_brush = 1;     // 0:細い 1:ふつう 2:太い
static int mask_last_x = -1, mask_last_y = -1;
static int mask_painted;

static void mask_reset(void)
{
	free(mask);
	mask = NULL;
	pic_free(&mask_pic);
	mask_name[0] = 0;
	mask_painted = 0;
}

// 表示中の画像に合ったマスクを用意する
static bool mask_prepare(void)
{
	if (!view.valid || !view_name[0]) return false;
	if (mask && !strcmp(mask_name, view_name)) return true;
	mask_reset();
	int tw, th;
	img_fit64(view_fw, view_fh, &tw, &th);
	mask_w = tw / 8;
	mask_h = th / 8;
	mask = calloc((size_t)mask_w * mask_h, 1);
	if (!mask || !pic_blank(&mask_pic, mask_w, mask_h)) {
		mask_reset();
		return false;
	}
	pic_set_nearest(&mask_pic, true);
	str_copy(mask_name, sizeof(mask_name), view_name);
	return true;
}

#define MASK_COLOR 0x9773FFB4u // RGBA

static void mask_paint_at(int cx, int cy)
{
	static const int radius[3] = {1, 2, 4};
	int r = radius[mask_brush];
	for (int y = cy - r; y <= cy + r; y++)
		for (int x = cx - r; x <= cx + r; x++) {
			if (x < 0 || y < 0 || x >= mask_w || y >= mask_h) continue;
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r + r / 2) continue;
			uint8_t *m = &mask[y * mask_w + x];
			uint8_t v = mask_erase ? 0 : 1;
			if (*m == v) continue;
			*m = v;
			mask_painted += v ? 1 : -1;
			pic_set(&mask_pic, x, y, v ? MASK_COLOR : 0);
		}
}

static void mask_clear(void)
{
	if (!mask) return;
	memset(mask, 0, (size_t)mask_w * mask_h);
	for (int y = 0; y < mask_h; y++)
		for (int x = 0; x < mask_w; x++) pic_set(&mask_pic, x, y, 0);
	pic_flush(&mask_pic);
	mask_painted = 0;
}

static void mask_invert(void)
{
	if (!mask) return;
	mask_painted = 0;
	for (int y = 0; y < mask_h; y++)
		for (int x = 0; x < mask_w; x++) {
			uint8_t *m = &mask[y * mask_w + x];
			*m = !*m;
			mask_painted += *m;
			pic_set(&mask_pic, x, y, *m ? MASK_COLOR : 0);
		}
	pic_flush(&mask_pic);
}

// マスク編集画面のキャンバス（下画面の左側）
#define CANVAS_W 248
static void canvas_rect(float *ox, float *oy, float *dw, float *dh)
{
	float s = fminf((float)CANVAS_W / mask_w, (float)SCR_H / mask_h);
	*dw = mask_w * s;
	*dh = mask_h * s;
	*ox = (CANVAS_W - *dw) / 2;
	*oy = (SCR_H - *dh) / 2;
}

// ---------------------------------------------------------------- 生成とジョブ

static bool job_modal(void) { return g_job.running && g_job.type != JOB_ACCOUNT && g_job.type != JOB_THUMB; }

static void start_account_check(void)
{
	if (!cfg.api_key[0]) {
		key_state = KEY_NONE;
		return;
	}
	if (nai_start_account(&cfg)) key_state = KEY_CHECKING;
}

static void start_generate(void)
{
	if (g_job.running) {
		show_flash(L("ほかの通信が終わるまで待ってください", "Please wait for the current request"));
		return;
	}
	if (!cfg.prompt[0]) {
		show_msg(L("プロンプトが空です。生成タブで入力してください。", "The prompt is empty. Enter one on the Create tab."));
		return;
	}
	save_cfg();
	if (!nai_start_generate(&cfg)) show_msg(L("生成を開始できませんでした（メモリ不足）", "Could not start (out of memory)"));
}

static void start_edit(void)
{
	if (g_job.running) {
		show_flash(L("ほかの通信が終わるまで待ってください", "Please wait for the current request"));
		return;
	}
	if (!view.valid || !view_name[0]) {
		show_msg(L("編集する画像がありません。一覧で画像を選んで表示してください。", "No image to edit. Open one from the Gallery."));
		return;
	}
	save_cfg();
	bool ok;
	if (edit_mode == EM_DIRECTOR) {
		ok = nai_start_director(&cfg, view_name);
	} else {
		if (!cfg.prompt[0]) {
			show_msg(L("プロンプトが空です。生成タブで、どんな絵にしたいかを入力してください。", "The prompt is empty. Describe the picture you want on the Create tab."));
			return;
		}
		if (edit_mode == EM_INPAINT) {
			if (!mask_prepare() || mask_painted == 0) {
				show_msg(L("描き直したい部分をマスクで塗ってください。\n「マスクを塗る」から、タッチペンで塗れます。", "Paint the area to redraw first.\nTap Paint Mask and use the stylus."));
				return;
			}
			ok = nai_start_edit(&cfg, ACT_INFILL, view_name, mask, mask_w, mask_h);
		} else {
			ok = nai_start_edit(&cfg, ACT_IMG2IMG, view_name, NULL, 0, 0);
		}
	}
	if (!ok) show_msg(L("開始できませんでした（メモリ不足）", "Could not start (out of memory)"));
}

static void update_account(void)
{
	if (g_job.account_valid) {
		acct_valid = true;
		acct_anlas = g_job.anlas;
		acct_tier = g_job.tier;
		acct_active = g_job.sub_active;
		usage_valid = g_job.usage_valid;
		usage_percent = g_job.usage_percent;
		usage_purchased = g_job.usage_purchased;
		usage_negative = g_job.usage_negative;
		usage_secs = g_job.usage_secs;
		key_state = KEY_OK;
	}
}

// Opusの無料枠が使えるか。V5はV5の残量を使い切っていたら使えない
static bool opus_free_for(int model)
{
	if (!acct_valid || acct_tier < 3 || !acct_active) return false;
	return !(g_models[model].gen == 5 && usage_valid && usage_negative);
}

// 実行ボタンの右端に、必要なAnlasの目安を出す
static void draw_cost(float right, float cy, int cost)
{
	char b[32];
	if (cost < 0) snprintf(b, sizeof(b), L("大きすぎます", "Too large"));
	else if (cost == 0) snprintf(b, sizeof(b), L("無料", "Free"));
	else snprintf(b, sizeof(b), "%d", cost);
	bool short_of = acct_valid && cost > acct_anlas;
	float tw = ui_text_width(F_SMALL, b);
	float w = tw + (cost > 0 ? g_anlas.w + 4 : 0) + 12;
	float x = right - w;
	ui_rrect(x, cy - 8, w, 16, 8, short_of || cost < 0 ? RGBA8(0xE0, 0x48, 0x48, 0xE0) : RGBA8(0x1A, 0x14, 0x40, 0x50));
	x += 6;
	if (cost > 0) {
		pic_draw(&g_anlas, x, cy - g_anlas.h / 2.0f, 1.0f);
		x += g_anlas.w + 4;
	}
	ui_text(F_SMALL, x, ui_text_y(F_SMALL, cy), COL_CREAM, b);
}

static void job_poll(void)
{
	if (!g_job.running || !g_job.done) return;
	switch (g_job.type) {
	case JOB_GENERATE:
	case JOB_EDIT:
	case JOB_DIRECTOR:
	case JOB_LOADIMG:
		if (g_job.ok && g_job.view_px) {
			if (pic_from_rgb565(&view, g_job.view_px, g_job.view_w, g_job.view_h)) {
				str_copy(view_name, sizeof(view_name), g_job.name);
				view_fw = g_job.full_w;
				view_fh = g_job.full_h;
				viewer_on = false;
			} else {
				show_msg(L("画像を表示できません（メモリ不足）", "Cannot show the image (out of memory)"));
			}
		}
		free(g_job.view_px);
		g_job.view_px = NULL;
		if (g_job.type == JOB_LOADIMG && g_job.ok) gal_refresh_thumb(g_job.name);
		if (g_job.type != JOB_LOADIMG && g_job.ok) {
			if (g_job.type != JOB_DIRECTOR) {
				last_seed = g_job.seed;
				has_last_seed = true;
			}
			gal_dirty = true;
			gal_cur = 0;
			if (g_job.saved_count > 1) {
				char b[64];
				snprintf(b, sizeof(b), L("%d枚保存しました（一覧で見られます）", "Saved %d images (see Gallery)"), g_job.saved_count);
				show_flash(b);
			}
		}
		if (g_job.http_code == 401) key_state = KEY_BAD;
		update_account();
		if (g_job.error[0]) show_msg(g_job.error);
		break;
	case JOB_THUMB:
		if (g_job.ok) gal_refresh_thumb(g_job.name);
		break;
	case JOB_SUGGEST:
		if (g_job.ok) {
			memcpy(results, g_job.sug, sizeof(results));
			result_count = g_job.sug_count;
			tag_mode = 1;
			tag_cur = tag_scroll = 0;
			if (!result_count) show_flash(L("候補が見つかりませんでした", "No suggestions found"));
		} else {
			if (g_job.http_code == 401) key_state = KEY_BAD;
			show_msg(g_job.error);
		}
		break;
	case JOB_ACCOUNT:
		if (g_job.ok) {
			update_account();
		} else {
			key_state = g_job.http_code == 401 ? KEY_BAD : KEY_UNKNOWN;
			if (g_job.http_code == 401) show_msg(g_job.error);
		}
		break;
	default: break;
	}
	nai_finish();
}

// ---------------------------------------------------------------- 設定タブの項目

enum {
	SET_MODEL, SET_SIZE, SET_W, SET_H, SET_STEPS, SET_SCALE, SET_SEED, SET_SAMPLER, SET_NOISE,
	SET_RESCALE, SET_VARIETY, SET_QUALITY, SET_UCP, SET_FORMAT, SET_LANG, SET_KEY, SET_TLS, SET_ACCT, SET_QUIT, SET_COUNT
};
static const char *const set_labels[2][SET_COUNT] = {
	{"モデル", "サイズ", "幅", "高さ", "ステップ数", "プロンプト忠実度", "シード", "サンプラー", "ノイズスケジュール",
	 "CFGリスケール", "Variety+", "品質タグ", "除外プリセット", "受信形式", "言語 / Language", "APIキー", "証明書の検証", "Anlas残高を更新", "アプリを終了"},
	{"Model", "Size", "Width", "Height", "Steps", "Prompt Guidance", "Seed", "Sampler", "Noise Schedule",
	 "CFG Rescale", "Variety+", "Quality Tags", "UC Preset", "Image Format", "Language / 言語", "API Key", "Verify TLS", "Refresh Anlas", "Quit"},
};
#define SET_ROW_H 27
#define SET_LIST_Y (TAB_H + 5)
#define SET_LIST_H (FOOT_Y - SET_LIST_Y - 2)
static int set_cur;
static float set_scroll, set_target, set_vel; // スクロール位置(px)、目標、指を離したあとの惰性
static bool set_touch, set_drag;
static int set_t0x, set_t0y, set_tlast;
static float set_scroll0;

static float set_max_scroll(void)
{
	float m = SET_COUNT * SET_ROW_H - 3 - SET_LIST_H;
	return m > 0 ? m : 0;
}

static bool set_locked(int row)
{
	const ModelInfo *m = &g_models[cfg.model];
	if (row == SET_VARIETY && m->gen >= 5) return true; // V5 には Variety+ が無い
	return m->medium && (row == SET_STEPS || row == SET_SAMPLER || row == SET_RESCALE || row == SET_UCP);
}

static bool set_has_arrows(int row) { return row < SET_KEY || row == SET_TLS; }

static void set_value(int row, char *out, size_t n)
{
	const ModelInfo *m = &g_models[cfg.model];
	out[0] = 0;
	switch (row) {
	case SET_MODEL: snprintf(out, n, "%s", m->label); break;
	case SET_SIZE: {
		int p = config_size_preset(&cfg);
		snprintf(out, n, "%s", p >= 0 ? g_sizes[p].label[g_lang] : L("カスタム", "Custom"));
		break;
	}
	case SET_W: snprintf(out, n, "%d", cfg.width); break;
	case SET_H: snprintf(out, n, "%d", cfg.height); break;
	case SET_STEPS: snprintf(out, n, "%d", m->medium ? 14 : cfg.steps); break;
	case SET_SCALE: snprintf(out, n, "%.1f", (double)cfg.scale); break;
	case SET_SEED:
		if (cfg.seed_random) snprintf(out, n, L("ランダム", "Random"));
		else snprintf(out, n, "%lu", (unsigned long)cfg.seed);
		break;
	case SET_SAMPLER: snprintf(out, n, "%s", g_sampler_labels[m->medium ? 0 : cfg.sampler]); break;
	case SET_NOISE: snprintf(out, n, "%s", g_noises[cfg.noise]); break;
	case SET_RESCALE:
		if (m->medium) snprintf(out, n, L("使用不可", "N/A"));
		else snprintf(out, n, "%.2f", (double)cfg.cfg_rescale);
		break;
	case SET_VARIETY:
		if (m->gen >= 5) snprintf(out, n, L("使用不可", "N/A"));
		else snprintf(out, n, "%s", cfg.variety ? "ON" : "OFF");
		break;
	case SET_QUALITY: snprintf(out, n, "%s", cfg.quality ? "ON" : "OFF"); break;
	case SET_UCP: snprintf(out, n, "%s", g_uc_labels[m->medium ? UC_HEAVY : cfg.uc_preset]); break;
	case SET_KEY: {
		size_t l = strlen(cfg.api_key);
		if (!l) snprintf(out, n, L("未設定", "Not set"));
		else snprintf(out, n, L("%.4s…（%d文字）", "%.4s… (%d chars)"), cfg.api_key, (int)l);
		break;
	}
	case SET_TLS: snprintf(out, n, "%s", cfg.verify_tls ? "ON" : "OFF"); break;
	case SET_FORMAT: snprintf(out, n, "%s", cfg.recv_webp ? L("WebP（軽い）", "WebP (lighter)") : "PNG"); break;
	case SET_LANG: snprintf(out, n, "%s", cfg.lang == LANG_JA ? "日本語" : cfg.lang == LANG_EN ? "English" : L("自動", "Auto")); break;
	case SET_ACCT:
		if (acct_valid && usage_valid) snprintf(out, n, "%d ・ V5 %d%%", acct_anlas, usage_negative ? 0 : usage_percent + usage_purchased);
		else if (acct_valid) snprintf(out, n, "%d", acct_anlas);
		else snprintf(out, n, "-");
		break;
	default: break;
	}
}

static void set_adjust(int row, int dir)
{
	if (set_locked(row)) return;
	switch (row) {
	case SET_MODEL:
		cfg.model = wrapi(cfg.model + dir, g_model_count);
		if (!g_models[cfg.model].uc[cfg.uc_preset]) cfg.uc_preset = UC_LIGHT;
		break;
	case SET_SIZE: {
		int p = config_size_preset(&cfg);
		p = p < 0 ? 0 : wrapi(p + dir, g_size_count);
		cfg.width = g_sizes[p].w;
		cfg.height = g_sizes[p].h;
		break;
	}
	case SET_W: cfg.width += dir * 64; break;
	case SET_H: cfg.height += dir * 64; break;
	case SET_STEPS: cfg.steps += dir; break;
	case SET_SCALE: cfg.scale = (float)((int)(cfg.scale * 10.0f + 0.5f) + dir) / 10.0f; break;
	case SET_SEED: cfg.seed_random = !cfg.seed_random; break;
	case SET_SAMPLER: cfg.sampler = wrapi(cfg.sampler + dir, g_sampler_count); break;
	case SET_NOISE: cfg.noise = wrapi(cfg.noise + dir, g_noise_count); break;
	case SET_RESCALE: cfg.cfg_rescale = (float)((int)(cfg.cfg_rescale * 20.0f + 0.5f) + dir) / 20.0f; break;
	case SET_VARIETY: cfg.variety = !cfg.variety; break;
	case SET_QUALITY: cfg.quality = !cfg.quality; break;
	case SET_UCP: cfg.uc_preset = config_next_uc_preset(&cfg, dir); break;
	case SET_TLS: cfg.verify_tls = !cfg.verify_tls; break;
	case SET_FORMAT:
		cfg.recv_webp = !cfg.recv_webp;
		show_flash(cfg.recv_webp ? L("WebP: 通信量が約3割減ります（画質は同じ）", "WebP: about 30% less data, same quality")
					 : L("PNG: 元の形式で受け取ります", "PNG: the original format"));
		break;
	case SET_LANG:
		cfg.lang = wrapi(cfg.lang + dir, LANG_COUNT);
		apply_lang();
		break;
	default: return;
	}
	config_sanitize(&cfg);
	cfg_dirty = true;
}

static void set_activate(int row)
{
	unsigned long v;
	if (set_locked(row)) {
		show_flash(row == SET_VARIETY ? L("V5 のモデルでは使えません", "Not available on V5 models") : L("V5 Full Medium では固定です", "Fixed on V5 Full Medium"));
		return;
	}
	switch (row) {
	case SET_W:
		if (kb_number(&v, L("幅（64の倍数、64〜1600）", "Width (multiple of 64, 64-1600)"), 4)) cfg.width = (int)v;
		break;
	case SET_H:
		if (kb_number(&v, L("高さ（64の倍数、64〜1600）", "Height (multiple of 64, 64-1600)"), 4)) cfg.height = (int)v;
		break;
	case SET_STEPS:
		if (kb_number(&v, L("ステップ数（1〜50）", "Steps (1-50)"), 2)) cfg.steps = (int)v;
		break;
	case SET_SEED:
		if (kb_number(&v, L("シード値（数字）", "Seed (number)"), 10)) {
			cfg.seed = v > 4294967295UL ? 4294967295UL : (uint32_t)v;
			cfg.seed_random = false;
		}
		break;
	case SET_KEY:
		if (kb_text(cfg.api_key, sizeof(cfg.api_key), "Persistent API Token (pst-...)", 200)) {
			config_sanitize(&cfg);
			cfg_dirty = true;
			save_cfg();
			acct_valid = false;
			start_account_check();
		}
		return;
	case SET_ACCT:
		if (g_job.running) show_flash(L("ほかの通信が終わるまで待ってください", "Please wait for the current request"));
		else start_account_check();
		return;
	case SET_QUIT:
		quit = true;
		return;
	default:
		set_adjust(row, 1);
		return;
	}
	config_sanitize(&cfg);
	cfg_dirty = true;
}

// ---------------------------------------------------------------- 入力

static void edit_prompt(void)
{
	if (kb_text(cfg.prompt, sizeof(cfg.prompt), L("プロンプト（英語のタグをカンマ区切りで）", "Prompt (comma-separated tags)"), 1000)) cfg_dirty = true;
}

static void edit_uc(void)
{
	if (g_models[cfg.model].medium) {
		show_flash(L("V5 Full Medium では除外を指定できません", "UC is not available on V5 Full Medium"));
		return;
	}
	if (kb_text(cfg.uc, sizeof(cfg.uc), L("除外したい要素", "Undesired Content"), 1000)) cfg_dirty = true;
}

// 生成タブ: 0 プロンプト / 1 除外 / 2 モデル / 3 サイズ / 4 生成ボタン
static int gen_focus;
#define GEN_PROMPT_Y 44
#define GEN_PROMPT_H 70
#define GEN_UC_Y 135
#define GEN_UC_H 26
#define GEN_SEL_Y 166
#define GEN_BTN_Y 194

static void gen_activate(int item, int dir)
{
	switch (item) {
	case 0: edit_prompt(); break;
	case 1: edit_uc(); break;
	case 2: set_adjust(SET_MODEL, dir); break;
	case 3: set_adjust(SET_SIZE, dir); break;
	case 4: start_generate(); break;
	}
}

static void input_gen(u32 down, u32 rep, const touchPosition *t, bool tap)
{
	if (rep & KEY_DUP) gen_focus = gen_focus == 3 ? 1 : gen_focus == 4 ? 2 : gen_focus > 0 ? gen_focus - 1 : 0;
	if (rep & KEY_DDOWN) gen_focus = gen_focus == 2 ? 4 : gen_focus < 4 ? gen_focus + 1 : 4;
	if (gen_focus == 2 || gen_focus == 3) {
		if (rep & KEY_DLEFT) gen_activate(gen_focus, -1);
		if (rep & KEY_DRIGHT) gen_activate(gen_focus, 1);
	}
	if (down & KEY_A) gen_activate(gen_focus, 1);
	if (down & KEY_X) edit_uc();
	if (!tap) return;
	if (ui_hit(t, 6, GEN_PROMPT_Y, 308, GEN_PROMPT_H)) gen_focus = 0, edit_prompt();
	else if (ui_hit(t, 6, GEN_UC_Y, 308, GEN_UC_H)) gen_focus = 1, edit_uc();
	else if (ui_hit(t, 6, GEN_SEL_Y, 152, 24)) gen_focus = 2, set_adjust(SET_MODEL, t->px < 82 ? -1 : 1);
	else if (ui_hit(t, 162, GEN_SEL_Y, 152, 24)) gen_focus = 3, set_adjust(SET_SIZE, t->px < 238 ? -1 : 1);
	else if (ui_hit(t, 6, GEN_BTN_Y, 308, 22)) gen_focus = 4, start_generate();
}

static void set_tap(int px, int py)
{
	int row = (int)floorf((py - SET_LIST_Y + set_scroll) / SET_ROW_H);
	if (row < 0 || row >= SET_COUNT || px >= BOT_W - 8) return;
	bool was = row == set_cur;
	set_cur = row;
	if (set_has_arrows(row) && px >= 150 && px < 186) set_adjust(row, -1);
	else if (set_has_arrows(row) && px >= 276) set_adjust(row, 1);
	else if (was || !set_has_arrows(row)) set_activate(row);
}

static void input_set(u32 down, u32 rep, u32 held, const touchPosition *t, bool tap)
{
	int prev = set_cur;
	if (rep & KEY_DUP) set_cur = wrapi(set_cur - 1, SET_COUNT);
	if (rep & KEY_DDOWN) set_cur = wrapi(set_cur + 1, SET_COUNT);
	if (rep & KEY_DLEFT) set_adjust(set_cur, -1);
	if (rep & KEY_DRIGHT) set_adjust(set_cur, 1);
	if (down & KEY_A) set_activate(set_cur);

	// なぞるとスクロール。動かさずに離したときだけタップとして扱う
	if (tap && t->py >= SET_LIST_Y && t->py < FOOT_Y) {
		set_touch = true;
		set_drag = false;
		set_t0x = t->px;
		set_t0y = set_tlast = t->py;
		set_scroll0 = set_scroll;
		set_vel = 0;
	}
	if (set_touch) {
		if (held & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			int dy = tp.py - set_t0y;
			if (!set_drag && abs(dy) > 6) set_drag = true;
			if (set_drag) {
				set_vel = (float)(set_tlast - tp.py);
				set_scroll = set_target = set_scroll0 - dy;
			}
			set_tlast = tp.py;
		} else {
			set_touch = false;
			if (!set_drag) set_tap(set_t0x, set_t0y);
		}
	}

	// 選んだ行が見えるようにする
	if (set_cur != prev) {
		float top = set_cur * SET_ROW_H, bottom = top + SET_ROW_H - 3;
		if (top < set_target) set_target = top;
		if (bottom > set_target + SET_LIST_H) set_target = bottom - SET_LIST_H;
		set_vel = 0;
	}
	// 指を離したあとの惰性と、目標への移動
	if (!set_touch) {
		if (fabsf(set_vel) > 0.4f) {
			set_scroll += set_vel;
			set_vel *= 0.92f;
			set_target = set_scroll;
		} else {
			set_vel = 0;
			set_scroll += (set_target - set_scroll) * 0.3f;
		}
	}
	float mx = set_max_scroll();
	if (set_scroll < 0) set_scroll = 0, set_vel = 0;
	if (set_scroll > mx) set_scroll = mx, set_vel = 0;
	if (set_target < 0) set_target = 0;
	if (set_target > mx) set_target = mx;
}

static void chr_edit(bool uc)
{
	CharSlot *s = &cfg.chars[chr_cur];
	if (uc && g_models[cfg.model].medium) {
		show_flash(L("V5 Full Medium では除外を指定できません", "UC is not available on V5 Full Medium"));
		return;
	}
	if (kb_text(uc ? s->uc : s->prompt, MAX_PROMPT, uc ? L("このキャラの除外したい要素", "Undesired content for this character") : L("このキャラのプロンプト", "Prompt for this character"), 1000)) cfg_dirty = true;
}

#define CHR_ROW_Y 30
#define CHR_ROW_H 30
#define CHR_GRID_X 192
#define CHR_GRID_Y 48
#define CHR_CELL 22

static void input_chr(u32 down, u32 rep, const touchPosition *t, bool tap)
{
	CharSlot *s = &cfg.chars[chr_cur];
	if (rep & KEY_DUP) chr_cur = wrapi(chr_cur - 1, MAX_CHARS);
	if (rep & KEY_DDOWN) chr_cur = wrapi(chr_cur + 1, MAX_CHARS);
	if (down & KEY_A) chr_edit(false);
	if (down & KEY_X) chr_edit(true);
	if (down & KEY_Y) {
		s->enabled = !s->enabled;
		cfg_dirty = true;
	}
	if (!tap) return;
	for (int i = 0; i < MAX_CHARS; i++)
		if (ui_hit(t, 6, CHR_ROW_Y + i * CHR_ROW_H, 176, CHR_ROW_H - 3)) {
			if (t->px >= 146) { // 右端のスイッチ
				cfg.chars[i].enabled = !cfg.chars[i].enabled;
				cfg_dirty = true;
			} else if (i == chr_cur) {
				chr_edit(false);
			}
			chr_cur = i;
			return;
		}
	s = &cfg.chars[chr_cur];
	if (ui_hit(t, CHR_GRID_X, CHR_GRID_Y, CHR_CELL * 5, CHR_CELL * 5)) {
		s->gx = (t->px - CHR_GRID_X) / CHR_CELL;
		s->gy = (t->py - CHR_GRID_Y) / CHR_CELL;
		cfg_dirty = true;
	} else if (ui_hit(t, 190, 164, 124, 22)) {
		s->gx = s->gy = -1;
		cfg_dirty = true;
	} else if (ui_hit(t, 190, 192, 60, 22)) chr_edit(true);
	else if (ui_hit(t, 254, 192, 60, 22)) {
		memset(s, 0, sizeof(*s));
		s->enabled = true;
		s->gx = s->gy = -1;
		cfg_dirty = true;
		show_flash(L("消去しました", "Cleared"));
	}
}

#define TAG_ROWS 6
#define TAG_ROW_H 22
#define TAG_LIST_Y 88

static void tag_search(void)
{
	char q[96] = "";
	if (!kb_text(q, sizeof(q), L("タグの一部を入力（日本語でも検索できます）", "Type part of a tag (Japanese also works)"), 40) || !q[0]) return;
	if (!nai_start_suggest(&cfg, q)) show_flash(L("ほかの通信が終わるまで待ってください", "Please wait for the current request"));
}

static void tag_new(void)
{
	char q[TAG_LEN] = "";
	if (!kb_text(q, sizeof(q), L("保管するタグ", "Tag to save"), 60) || !q[0]) return;
	if (tags_add(q)) {
		tags_save();
		tag_mode = 0;
		tag_cur = tag_count - 1;
		show_flash(L("保管しました", "Saved"));
	} else {
		show_flash(L("すでに保管済みか、上限です", "Already saved, or the list is full"));
	}
}

static void tag_y_action(void)
{
	if (tag_cur >= tag_list_count()) return;
	if (tag_mode == 0) {
		memmove(tags[tag_cur], tags[tag_cur + 1], (size_t)(tag_count - tag_cur - 1) * TAG_LEN);
		tag_count--;
		if (tag_cur >= tag_count && tag_cur > 0) tag_cur--;
		tags_save();
		show_flash(L("保管から外しました", "Removed"));
	} else if (tags_add(results[tag_cur].tag)) {
		tags_save();
		show_flash(L("保管しました", "Saved"));
	} else {
		show_flash(L("すでに保管済みか、上限です", "Already saved, or the list is full"));
	}
}

static void input_tag(u32 down, u32 rep, const touchPosition *t, bool tap)
{
	int n = tag_list_count();
	if (n && (rep & KEY_DUP)) tag_cur = wrapi(tag_cur - 1, n);
	if (n && (rep & KEY_DDOWN)) tag_cur = wrapi(tag_cur + 1, n);
	if (n && (rep & KEY_DLEFT)) tag_cur = tag_cur >= TAG_ROWS ? tag_cur - TAG_ROWS : 0;
	if (n && (rep & KEY_DRIGHT)) tag_cur = tag_cur + TAG_ROWS < n ? tag_cur + TAG_ROWS : n - 1;
	if (down & KEY_A) tag_append_current();
	if (down & KEY_Y) tag_y_action();
	if (down & KEY_X) tag_search();
	if (tap) {
		if (ui_hit(t, 6, 30, 75, 24)) tag_mode = 0, tag_cur = tag_scroll = 0;
		else if (ui_hit(t, 81, 30, 75, 24)) tag_mode = 1, tag_cur = tag_scroll = 0;
		else if (ui_hit(t, 162, 30, 72, 24)) tag_search();
		else if (ui_hit(t, 240, 30, 74, 24)) tag_new();
		else if (ui_hit(t, 6, 60, 160, 22)) tag_target = wrapi(tag_target + (t->px < 86 ? -1 : 1), 3);
		else if (t->py >= TAG_LIST_Y && t->py < TAG_LIST_Y + TAG_ROWS * TAG_ROW_H) {
			int row = tag_scroll + (t->py - TAG_LIST_Y) / TAG_ROW_H;
			if (row < tag_list_count()) {
				if (row == tag_cur) tag_append_current();
				tag_cur = row;
			}
		}
	}
	n = tag_list_count();
	if (tag_cur >= n) tag_cur = n ? n - 1 : 0;
	if (tag_cur < tag_scroll) tag_scroll = tag_cur;
	if (tag_cur >= tag_scroll + TAG_ROWS) tag_scroll = tag_cur - TAG_ROWS + 1;
}

static void viewer_open(void);

static bool gal_cur_shown(void) { return gal_cur < gal_count && view.valid && !strcmp(view_name, gal_names[gal_cur]); }

static void gal_open_current(void)
{
	if (gal_cur >= gal_count) return;
	if (gal_cur_shown()) {
		viewer_open();
		return;
	}
	if (!nai_start_loadimg(gal_names[gal_cur])) show_flash(L("ほかの通信が終わるまで待ってください", "Please wait for the current request"));
}

#define GAL_X 9
#define GAL_Y 31
#define GAL_CW 58
#define GAL_CH 88
#define GAL_GX 62
#define GAL_GY 94

static void input_gal(u32 down, u32 rep, const touchPosition *t, bool tap)
{
	int prev = gal_cur;
	if (gal_count) {
		if ((rep & KEY_DLEFT) && gal_cur > 0) gal_cur--;
		if ((rep & KEY_DRIGHT) && gal_cur + 1 < gal_count) gal_cur++;
		if ((rep & KEY_DUP) && gal_cur >= GAL_COLS) gal_cur -= GAL_COLS;
		if ((rep & KEY_DDOWN) && gal_cur + GAL_COLS < gal_count) gal_cur += GAL_COLS;
		if (down & KEY_A) gal_open_current();
		if (down & KEY_X) gal_load_settings();
		if (down & KEY_Y) {
			if (gal_del_confirm == gal_cur) {
				gal_delete_current();
				gal_del_confirm = -1;
			} else {
				gal_del_confirm = gal_cur;
				show_flash(L("もう一度 Y で削除します", "Press Y again to delete"));
			}
		}
		if (tap) {
			for (int i = 0; i < GAL_PER_PAGE; i++) {
				int idx = (gal_cur / GAL_PER_PAGE) * GAL_PER_PAGE + i;
				if (idx >= gal_count) break;
				if (ui_hit(t, GAL_X + (i % GAL_COLS) * GAL_GX, GAL_Y + (i / GAL_COLS) * GAL_GY, GAL_CW, GAL_CH)) {
					if (idx == gal_cur) gal_open_current();
					gal_cur = idx;
					break;
				}
			}
		}
	}
	if (gal_cur != prev) gal_del_confirm = -1;
}

// 編集タブ ---------------------------------------------------------
// 項目: 0 モード切り替え / 以降はモードごと。最後が実行ボタン
enum { EF_MODE, EF_A, EF_B, EF_C, EF_D, EF_RUN };
#define EDIT_MODE_Y 72
#define EDIT_ROW1_Y 103
#define EDIT_ROW2_Y 131
#define EDIT_ROW3_Y 159
#define EDIT_RUN_Y 194

static bool edit_item_exists(int it)
{
	if (it == EF_MODE || it == EF_RUN) return true;
	switch (edit_mode) {
	case EM_I2I: return it == EF_A || it == EF_B;
	case EM_INPAINT: return it == EF_A || it == EF_B;
	default:
		if (it == EF_A) return true;
		if (it == EF_B) return cfg.dir_tool == DIR_EMOTION;
		return g_dir_tools[cfg.dir_tool].takes_prompt && (it == EF_C || it == EF_D);
	}
}

static void edit_move(int dir)
{
	int it = edit_focus;
	do it = wrapi(it + dir, EF_RUN + 1);
	while (!edit_item_exists(it));
	edit_focus = it;
}

static void edit_adjust(int it, int dir)
{
	switch (edit_mode) {
	case EM_I2I:
		if (it == EF_A) cfg.i2i_strength = (float)((int)(cfg.i2i_strength * 20.0f + 0.5f) + dir) / 20.0f;
		if (it == EF_B) cfg.i2i_noise = (float)((int)(cfg.i2i_noise * 20.0f + 0.5f) + dir) / 20.0f;
		if (cfg.i2i_strength < 0.05f) cfg.i2i_strength = 0.05f;
		break;
	case EM_INPAINT:
		if (it == EF_B) cfg.inpaint_strength = (float)((int)(cfg.inpaint_strength * 20.0f + 0.5f) + dir) / 20.0f;
		if (cfg.inpaint_strength < 0.05f) cfg.inpaint_strength = 0.05f;
		break;
	default:
		if (it == EF_A) cfg.dir_tool = wrapi(cfg.dir_tool + dir, DIR_COUNT);
		if (it == EF_B) cfg.dir_emotion = wrapi(cfg.dir_emotion + dir, EMOTION_COUNT);
		if (it == EF_C) cfg.dir_defry = wrapi(cfg.dir_defry + dir, 6);
		break;
	}
	config_sanitize(&cfg);
	cfg_dirty = true;
}

static void mask_open(void)
{
	if (!mask_prepare()) {
		show_msg(L("マスクを用意できません。画像を表示してからもう一度どうぞ。", "Cannot prepare a mask. Show an image and try again."));
		return;
	}
	mask_editing = true;
	mask_last_x = mask_last_y = -1;
}

static void edit_activate(int it)
{
	if (it == EF_RUN) {
		start_edit();
		return;
	}
	if (it == EF_MODE) {
		edit_mode = wrapi(edit_mode + 1, EM_COUNT);
		return;
	}
	if (edit_mode == EM_INPAINT && it == EF_A) {
		mask_open();
		return;
	}
	if (edit_mode == EM_DIRECTOR && it == EF_D) {
		const char *hint = cfg.dir_tool == DIR_EMOTION ? L("追加のタグ（なくても大丈夫）", "Extra tags (optional)") : L("色の指示など（例: red hair, blue dress）", "Color hints (e.g. red hair, blue dress)");
		if (kb_text(cfg.dir_prompt, sizeof(cfg.dir_prompt), hint, 400)) cfg_dirty = true;
		return;
	}
	edit_adjust(it, 1);
}

static void input_edit(u32 down, u32 rep, const touchPosition *t, bool tap)
{
	if (!view.valid) {
		if ((down & KEY_A) || (tap && ui_hit(t, 80, 150, 160, 28))) tab = TAB_GAL;
		return;
	}
	if (!edit_item_exists(edit_focus)) edit_focus = EF_MODE;
	if (rep & KEY_DUP) edit_move(-1);
	if (rep & KEY_DDOWN) edit_move(1);
	if (rep & (KEY_DLEFT | KEY_DRIGHT)) {
		int dir = (rep & KEY_DLEFT) ? -1 : 1;
		if (edit_focus == EF_MODE) edit_mode = wrapi(edit_mode + dir, EM_COUNT);
		else if (edit_focus != EF_RUN && !(edit_mode == EM_INPAINT && edit_focus == EF_A) && !(edit_mode == EM_DIRECTOR && edit_focus == EF_D))
			edit_adjust(edit_focus, dir);
	}
	if (down & KEY_A) edit_activate(edit_focus);
	if (!tap) return;
	float sw = 308.0f / EM_COUNT;
	if (ui_hit(t, 6, EDIT_MODE_Y, 308, 26)) {
		edit_mode = (int)((t->px - 6) / sw);
		if (edit_mode >= EM_COUNT) edit_mode = EM_COUNT - 1;
		edit_focus = EF_MODE;
		return;
	}
	if (ui_hit(t, 6, EDIT_RUN_Y, 308, 22)) {
		edit_focus = EF_RUN;
		start_edit();
		return;
	}
	const int rows[3] = {EDIT_ROW1_Y, EDIT_ROW2_Y, EDIT_ROW3_Y};
	for (int r = 0; r < 3; r++) {
		if (t->py < rows[r] || t->py >= rows[r] + 24) continue;
		int it = -1;
		if (edit_mode == EM_DIRECTOR && r == 2) {
			if (t->px < 160) it = EF_C;
			else it = EF_D;
		} else if (edit_mode == EM_DIRECTOR && r == 1) {
			it = cfg.dir_tool == DIR_EMOTION ? EF_B : -1;
		} else {
			it = r == 0 ? EF_A : r == 1 ? EF_B : -1;
		}
		if (it < 0 || !edit_item_exists(it)) return;
		edit_focus = it;
		bool selector = !(edit_mode == EM_INPAINT && it == EF_A) && !(edit_mode == EM_DIRECTOR && it == EF_D);
		if (!selector) edit_activate(it);
		else {
			float w0 = (edit_mode == EM_DIRECTOR && it == EF_C) ? 150 : 308;
			edit_adjust(it, t->px < 6 + w0 / 2 ? -1 : 1);
		}
		return;
	}
}

// マスク編集画面
#define TOOL_X 254
static void input_mask(u32 down, u32 held, const touchPosition *t, bool tap)
{
	if (down & (KEY_B | KEY_A | KEY_START)) {
		mask_editing = false;
		return;
	}
	if (down & KEY_Y) mask_erase = !mask_erase;
	if (down & KEY_L) mask_brush = mask_brush > 0 ? mask_brush - 1 : 0;
	if (down & KEY_R) mask_brush = mask_brush < 2 ? mask_brush + 1 : 2;
	if (down & KEY_X) mask_clear();
	if (tap && t->px >= TOOL_X) {
		if (ui_hit(t, TOOL_X, 8, 60, 28)) mask_erase = false;
		else if (ui_hit(t, TOOL_X, 40, 60, 28)) mask_erase = true;
		else if (ui_hit(t, TOOL_X, 92, 60, 24)) mask_brush = (t->px - TOOL_X) / 20 > 2 ? 2 : (t->px - TOOL_X) / 20;
		else if (ui_hit(t, TOOL_X, 126, 60, 26)) mask_clear();
		else if (ui_hit(t, TOOL_X, 158, 60, 26)) mask_invert();
		else if (ui_hit(t, TOOL_X, 204, 60, 30)) mask_editing = false;
		return;
	}
	if (!(held & KEY_TOUCH)) {
		mask_last_x = mask_last_y = -1;
		return;
	}
	touchPosition tp;
	hidTouchRead(&tp);
	if (tp.px >= TOOL_X - 4) {
		mask_last_x = mask_last_y = -1;
		return;
	}
	float ox, oy, dw, dh;
	canvas_rect(&ox, &oy, &dw, &dh);
	int gx = (int)floorf((tp.px - ox) / dw * mask_w), gy = (int)floorf((tp.py - oy) / dh * mask_h);
	if (mask_last_x < 0) {
		mask_paint_at(gx, gy);
	} else {
		// 前の点からの線をなぞる（速く動かしても途切れないように）
		int dx = gx - mask_last_x, dy = gy - mask_last_y;
		int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
		if (steps < 1) steps = 1;
		for (int i = 1; i <= steps; i++) mask_paint_at(mask_last_x + dx * i / steps, mask_last_y + dy * i / steps);
	}
	mask_last_x = gx;
	mask_last_y = gy;
	pic_flush(&mask_pic);
}

// ---------------------------------------------------------------- 描画: 上画面

static void draw_wifi(float x, float y, int level)
{
	for (int i = 0; i < 3; i++) {
		float h = 4 + i * 3;
		ui_rect(x + i * 5, y + 11 - h, 3, h, i < level ? COL_WHITE : RGBA8(0xFF, 0xFF, 0xFF, 0x40));
	}
}

static void draw_battery(float x, float y)
{
	u32 c = battery_level <= 1 && !battery_charging ? COL_RED : COL_WHITE;
	ui_rring(x, y, 20, 11, 2, 1, c);
	ui_rect(x + 20, y + 3, 2, 5, c);
	int lv = battery_level > 5 ? 5 : battery_level;
	int bars = (lv * 4 + 4) / 5;
	for (int i = 0; i < bars; i++) ui_rect(x + 2 + i * 4, y + 2, 3, 7, battery_charging ? COL_GREEN : c);
}

static void draw_status_bar(void)
{
	char b[48];
	ui_grad(0, 0, TOP_W, BAR_H, RGBA8(0x2C, 0x2E, 0x46, 0xF0), RGBA8(0x16, 0x18, 0x2A, 0xF0));
	ui_rect(0, BAR_H, TOP_W, 1, RGBA8(0, 0, 0, 0x30));
	pic_draw(&g_nib_s, 6, 3, 1.0f);
	ui_text(F_SMALL, 21, ui_text_y(F_SMALL, BAR_H / 2.0f), COL_CREAM, APP_NAME);

	float x = TOP_W - 28;
	draw_battery(x, 5);
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	snprintf(b, sizeof(b), "%d:%02d", tm->tm_hour, tm->tm_min);
	float tw = ui_text_width(F_SMALL, b);
	x -= tw + 8;
	ui_text(F_SMALL, x, ui_text_y(F_SMALL, BAR_H / 2.0f), COL_WHITE, b);
	x -= 20;
	draw_wifi(x, 4, osGetWifiStrength());

	// APIキーの状態
	const char *ks;
	u32 kc;
	switch (key_state) {
	case KEY_NONE: ks = L("キー未設定", "No key"); kc = COL_RED; break;
	case KEY_CHECKING: ks = L("キー確認中", "Checking"); kc = COL_ORANGE; break;
	case KEY_OK: ks = L("キーOK", "Key OK"); kc = RGBA8(0x6C, 0xD8, 0x5E, 0xFF); break;
	case KEY_BAD: ks = L("キー無効", "Bad key"); kc = COL_RED; break;
	default: ks = L("キー未確認", "Unverified"); kc = COL_FAINT; break;
	}
	tw = ui_text_width(F_SMALL, ks);
	x -= tw + 22;
	ui_rrect(x - 4, 3, tw + 18, BAR_H - 6, 6, RGBA8(0xFF, 0xFF, 0xFF, 0x18));
	ui_circle(x + 3, BAR_H / 2.0f, 3.5f, kc);
	ui_text(F_SMALL, x + 10, ui_text_y(F_SMALL, BAR_H / 2.0f), COL_WHITE, ks);

	// V5の残量（Opusのとき）。使い切ったら赤
	if (usage_valid) {
		int pct = usage_negative ? 0 : usage_percent + usage_purchased;
		snprintf(b, sizeof(b), "V5 %d%%", pct);
		tw = ui_text_width(F_SMALL, b);
		x -= tw + 16;
		ui_rrect(x - 4, 3, tw + 8, BAR_H - 6, 6, usage_negative || pct <= 0 ? RGBA8(0xE0, 0x48, 0x48, 0xC0) : RGBA8(0x97, 0x73, 0xFF, 0x60));
		ui_text(F_SMALL, x, ui_text_y(F_SMALL, BAR_H / 2.0f), COL_WHITE, b);
	}

	// Anlas残高（NovelAIのWebUIと同じアイコン）
	if (acct_valid) snprintf(b, sizeof(b), "%d", acct_anlas);
	else snprintf(b, sizeof(b), "-");
	tw = ui_text_width(F_SMALL, b);
	x -= tw + 26;
	pic_draw(&g_anlas, x, (BAR_H - g_anlas.h) / 2, 1.0f);
	ui_text(F_SMALL, x + g_anlas.w + 4, ui_text_y(F_SMALL, BAR_H / 2.0f), COL_CREAM, b);
}

static void draw_view(float area_y)
{
	float area_h = SCR_H - area_y;
	float sc = fminf((float)TOP_W / view.w, area_h / view.h);
	float dw = view.w * sc, dh = view.h * sc;
	float x = (TOP_W - dw) / 2, y = area_y + (area_h - dh) / 2;
	pic_draw(&view, x, y, sc);
	// インペイントのマスクを重ねる
	if (mask && !strcmp(mask_name, view_name) && (mask_editing || (tab == TAB_EDIT && edit_mode == EM_INPAINT)))
		pic_draw(&mask_pic, x, y, dw / mask_w);
}

// ---------------------------------------------------------------- 拡大ビューア

#define VMAP_X 6
#define VMAP_Y 6
#define VMAP_W 228
#define VMAP_H 206
#define VPANEL_X 242
#define VSLIDER_Y0 44
#define VSLIDER_Y1 134

static float viewer_fit(void) { return fminf((float)TOP_W / view.w, (float)SCR_H / view.h); }
static float viewer_zmax(void) { return fmaxf(viewer_fit() * 8.0f, 3.0f); }

static void viewer_clamp(void)
{
	float zmin = viewer_fit(), zmax = viewer_zmax();
	if (vz < zmin) vz = zmin;
	if (vz > zmax) vz = zmax;
	float hw = TOP_W / 2.0f / vz, hh = SCR_H / 2.0f / vz;
	vcx = view.w <= hw * 2 ? view.w / 2.0f : fminf(fmaxf(vcx, hw), view.w - hw);
	vcy = view.h <= hh * 2 ? view.h / 2.0f : fminf(fmaxf(vcy, hh), view.h - hh);
}

static void viewer_open(void)
{
	if (!view.valid) return;
	viewer_on = true;
	viewer_drag_map = viewer_drag_zoom = false;
	vz = viewer_fit() * 2.0f;
	vcx = view.w / 2.0f;
	vcy = view.h / 3.0f; // 顔のあたりから見られるように、少し上から
	viewer_clamp();
}

// 下画面の縮図の位置と大きさ
static void viewer_map(float *ox, float *oy, float *sc)
{
	*sc = fminf((float)VMAP_W / view.w, (float)VMAP_H / view.h);
	*ox = VMAP_X + (VMAP_W - view.w * *sc) / 2;
	*oy = VMAP_Y + (VMAP_H - view.h * *sc) / 2;
}

// スライダーの位置 0..1（対数で等間隔）
static float viewer_zoom_t(void) { return logf(vz / viewer_fit()) / logf(viewer_zmax() / viewer_fit()); }

static void input_viewer(u32 down, u32 held, const touchPosition *t, bool tap)
{
	if (down & (KEY_B | KEY_SELECT)) {
		viewer_on = false;
		return;
	}
	if (down & KEY_X) vz = viewer_fit();
	if (held & (KEY_R | KEY_A)) vz *= 1.035f;
	if (held & (KEY_L | KEY_Y)) vz /= 1.035f;
	// 十字ボタンとスライドパッドで動かす
	float mx = 0, my = 0;
	if (held & KEY_DLEFT) mx -= 5;
	if (held & KEY_DRIGHT) mx += 5;
	if (held & KEY_DUP) my -= 5;
	if (held & KEY_DDOWN) my += 5;
	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dx) > 15) mx += cp.dx / 18.0f;
	if (abs(cp.dy) > 15) my -= cp.dy / 18.0f;
	vcx += mx / vz;
	vcy += my / vz;
	if (tap) {
		if (ui_hit(t, VPANEL_X, 6, 72, 28)) vz *= 1.5f;
		else if (ui_hit(t, VPANEL_X, 156, 72, 26)) vz /= 1.5f;
		else if (ui_hit(t, VPANEL_X, 188, 72, 26)) {
			viewer_on = false;
			return;
		}
		viewer_drag_map = ui_hit(t, VMAP_X, VMAP_Y, VMAP_W, VMAP_H);
		viewer_drag_zoom = ui_hit(t, VPANEL_X, VSLIDER_Y0 - 8, 72, VSLIDER_Y1 - VSLIDER_Y0 + 16);
	}
	if (!(held & KEY_TOUCH)) viewer_drag_map = viewer_drag_zoom = false;
	if (held & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		if (viewer_drag_map) {
			// 縮図をなぞった場所を上画面の真ん中に
			float ox, oy, sc;
			viewer_map(&ox, &oy, &sc);
			vcx = (tp.px - ox) / sc;
			vcy = (tp.py - oy) / sc;
		} else if (viewer_drag_zoom) {
			float k = fminf(fmaxf((VSLIDER_Y1 - tp.py) / (float)(VSLIDER_Y1 - VSLIDER_Y0), 0), 1);
			vz = viewer_fit() * expf(k * logf(viewer_zmax() / viewer_fit()));
		}
	}
	viewer_clamp();
}

static void draw_viewer_top(void)
{
	char b[32];
	ui_grad(0, 0, TOP_W, SCR_H, RGBA8(0x2A, 0x2B, 0x36, 0xFF), RGBA8(0x16, 0x17, 0x1F, 0xFF));
	float x = TOP_W / 2.0f - vcx * vz, y = SCR_H / 2.0f - vcy * vz;
	pic_draw(&view, x, y, vz);
	if (mask && !strcmp(mask_name, view_name) && tab == TAB_EDIT && edit_mode == EM_INPAINT) pic_draw(&mask_pic, x, y, vz * view.w / mask_w);
	snprintf(b, sizeof(b), "×%.1f", (double)(vz / viewer_fit()));
	float w = ui_text_width(F_SMALL, b) + 18;
	ui_rrect(6, SCR_H - 24, w, 18, 9, RGBA8(0x10, 0x11, 0x1C, 0xB0));
	ui_text(F_SMALL, 15, ui_text_y(F_SMALL, SCR_H - 15), RGBA8(0xE8, 0xE8, 0xF0, 0xFF), b);
}

static void draw_viewer_bottom(void)
{
	char b[32];
	ui_rect(0, 0, BOT_W, SCR_H, RGBA8(0x24, 0x25, 0x31, 0xFF));
	float ox, oy, sc;
	viewer_map(&ox, &oy, &sc);
	pic_draw(&view, ox, oy, sc);
	// 上画面に見えている範囲を枠で示し、外側を暗くする
	float hw = TOP_W / 2.0f / vz, hh = SCR_H / 2.0f / vz;
	float x0 = ox + fmaxf(0, vcx - hw) * sc, y0 = oy + fmaxf(0, vcy - hh) * sc;
	float x1 = ox + fminf(view.w, vcx + hw) * sc, y1 = oy + fminf(view.h, vcy + hh) * sc;
	float ex = ox + view.w * sc, ey = oy + view.h * sc;
	u32 dim = RGBA8(0x0C, 0x0C, 0x16, 0x99);
	ui_rect(ox, oy, ex - ox, y0 - oy, dim);
	ui_rect(ox, y1, ex - ox, ey - y1, dim);
	ui_rect(ox, y0, x0 - ox, y1 - y0, dim);
	ui_rect(x1, y0, ex - x1, y1 - y0, dim);
	ui_rring(x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4, 4, 2, COL_CURSOR);
	// 右の操作パネル
	ui_grad(VPANEL_X - 4, 0, BOT_W - VPANEL_X + 4, FOOT_Y, RGBA8(0xF6, 0xF6, 0xF8, 0xFF), RGBA8(0xE0, 0xE1, 0xE6, 0xFF));
	ui_rect(VPANEL_X - 5, 0, 1, FOOT_Y, COL_LINE_D);
	ui_button(VPANEL_X, 6, 72, 28, "＋", BTN_NORMAL);
	float cx = VPANEL_X + 36, t = viewer_zoom_t();
	float ky = VSLIDER_Y1 - t * (VSLIDER_Y1 - VSLIDER_Y0);
	ui_rrect(cx - 3, VSLIDER_Y0, 6, VSLIDER_Y1 - VSLIDER_Y0, 3, RGBA8(0xC8, 0xC9, 0xD2, 0xFF));
	ui_rrect(cx - 3, ky, 6, VSLIDER_Y1 - ky, 3, COL_PURPLE);
	ui_circle(cx, ky, 9, COL_LINE_D);
	ui_circle(cx, ky, 8, COL_WHITE);
	ui_circle(cx, ky, 3, COL_PURPLE);
	snprintf(b, sizeof(b), "×%.1f", (double)(vz / viewer_fit()));
	ui_text_center(F_SMALL, cx, 138, COL_TEXT, b);
	ui_button(VPANEL_X, 156, 72, 26, "－", BTN_NORMAL);
	ui_button(VPANEL_X, 188, 72, 26, L("もどる", "Back"), BTN_ON);
	ui_footer(L("L/R:縮小・拡大|+:移動|X:全体|B:もどる", "L/R:Zoom|+:Move|X:Fit|B:Back"));
}

static void draw_top(void)
{
	char b[160];
	if (view.valid && viewer_on) {
		draw_viewer_top();
	} else if (view.valid) {
		ui_grad(0, 0, TOP_W, SCR_H, RGBA8(0x2A, 0x2B, 0x36, 0xFF), RGBA8(0x16, 0x17, 0x1F, 0xFF));
		draw_view(BAR_H + 1);
		if (!g_job.running && !mask_editing) {
			snprintf(b, sizeof(b), "%s   %d×%d", view_name, view_fw, view_fh);
			const char *hint = L("SELECT:拡大", "SELECT:Zoom");
			float w = ui_text_width(F_SMALL, b) + 16, hw = ui_hints(0, 0, hint, 0, 0, false);
			ui_rrect(6, SCR_H - 24, w + hw + 14, 18, 9, RGBA8(0x10, 0x11, 0x1C, 0xB0));
			ui_text(F_SMALL, 14, ui_text_y(F_SMALL, SCR_H - 15), RGBA8(0xE8, 0xE8, 0xF0, 0xFF), b);
			ui_hints(6 + w + 4, SCR_H - 15, hint, RGBA8(0xE8, 0xE8, 0xF0, 0xFF), COL_FAINT, true);
		}
	} else {
		ui_grad(0, 0, TOP_W, SCR_H, COL_WHITE, RGBA8(0xE2, 0xE3, 0xEA, 0xFF));
		for (int y = BAR_H + 2; y < SCR_H; y += 4) ui_rect(0, (float)y, TOP_W, 2, RGBA8(0x00, 0x00, 0x30, 0x07));
		// ロゴ（起動直後は、ペン先が弾んで現れ、波紋が広がり、文字が1字ずつ流れ込む）
		float t = (float)intro_f / INTRO_LEN;
		float tw = ui_text_width(F_TITLE, APP_NAME);
		float lx = 200 - (g_nib_l.w + 12 + tw) / 2;
		float ncx = lx + g_nib_l.w / 2.0f, ncy = 52 + g_nib_l.h / 2.0f;
		for (int i = 0; i < 2; i++) {
			float tr = fx_clamp01((t - 0.12f - i * 0.14f) / 0.5f);
			if (tr > 0 && tr < 1) fx_ring(ncx, ncy, 26 + 120 * fx_ease_out(tr), 3.0f - 2.0f * tr, alpha_of(COL_PURPLE, (1 - tr) * 0.55f));
		}
		if (t >= 1) fx_sparkles(200, ncy, 150, 40, 9, COL_PURPLE, 7);
		float tn = fx_clamp01(t / 0.32f);
		fx_logo(ncx, ncy, false, fx_ease_out(tn), 0.55f + 0.45f * fx_ease_out_back(tn), 1.0f);
		float x = lx + g_nib_l.w + 12, ty = ui_text_y(F_TITLE, ncy + 3);
		for (int i = 0; APP_NAME[i]; i++) {
			char ch[2] = {APP_NAME[i], 0};
			float e = fx_ease_out((t - 0.28f - i * 0.06f) / 0.22f);
			if (e > 0) ui_text(F_TITLE, x + (1 - e) * 18, ty, alpha_of(COL_NAVY, e), ch);
			x += ui_text_width(F_TITLE, ch);
		}
		ui_text_center(F_SMALL, 200, 116, alpha_of(COL_SUB, fx_ease_out((t - 0.55f) / 0.3f)), L("for NovelAI ・ 画像生成クライアント（非公式）", "for NovelAI · Unofficial image generator"));
		// 操作の案内カード（下から滑り込む）
		float cy0 = 140 + (1 - fx_ease_out((t - 0.62f) / 0.38f)) * 110;
		ui_shadow(40, cy0, 320, 80, 10);
		ui_rrect(40, cy0, 320, 80, 10, COL_LINE);
		ui_rrect_grad(41, cy0 + 1, 318, 78, 9, COL_WHITE, RGBA8(0xF4, 0xF4, 0xF7, 0xFF));
		float y = cy0 + 8;
		x = 58;
		x += ui_key_icon(x, y + 4, "START") + 6;
		ui_text(F_BODY, x, y, COL_TEXT, L("生成する", "Generate"));
		x = 214;
		x += ui_key_icon(x, y + 4, "L") + 2;
		x += ui_key_icon(x, y + 4, "R") + 6;
		ui_text(F_BODY, x, y, COL_TEXT, L("タブ切り替え", "Switch tabs"));
		y += 24;
		x = 58;
		x += ui_key_icon(x, y + 4, "SELECT") + 6;
		ui_text(F_BODY, x, y, COL_TEXT, L("拡大", "Zoom"));
		x = 214;
		x += ui_key_icon(x, y + 4, "+") + 6;
		ui_text(F_BODY, x, y, COL_TEXT, L("項目を選ぶ", "Select"));
		y += 24;
		ui_text_center(F_SMALL, 200, y, COL_SUB, L("編集タブで img2img・インペイント・ディレクターツール", "Edit tab: img2img, Inpaint and Director Tools"));
		ui_text_right(F_SMALL, TOP_W - 8, SCR_H - 18, COL_FAINT, "v" APP_VERSION);
	}

	if (mask_editing) {
		const char *h = L("Y:ペン/消しゴム|L/R:太さ|X:全部消す|B:完了", "Y:Pen/Eraser|L/R:Size|X:Clear|B:Done");
		float w = ui_hints(0, 0, h, 0, 0, false) + 20;
		ui_rrect((TOP_W - w) / 2, SCR_H - 28, w, 22, 11, RGBA8(0xF6, 0xF6, 0xF8, 0xE8));
		ui_hints((TOP_W - w) / 2 + 10, SCR_H - 17, h, COL_TEXT, COL_SUB, true);
		const char *t = L("紫に塗った部分が描き直されます", "The purple area will be redrawn");
		float tw = ui_text_width(F_SMALL, t) + 20;
		ui_rrect((TOP_W - tw) / 2, BAR_H + 6, tw, 20, 10, RGBA8(0x10, 0x11, 0x1C, 0xC0));
		ui_text_center(F_SMALL, 200, ui_text_y(F_SMALL, BAR_H + 16), COL_WHITE, t);
	}

	if (job_modal()) {
		const char *label = "";
		switch (g_job.phase) {
		case PH_PREPARE: label = L("画像を用意しています", "Preparing image"); break;
		case PH_CONNECT: label = g_job.type == JOB_SUGGEST ? L("タグを検索しています", "Searching tags") : L("サーバーに接続しています", "Connecting"); break;
		case PH_UPLOAD: label = L("画像を送っています", "Uploading image"); break;
		case PH_WAIT: label = g_job.type == JOB_DIRECTOR ? L("加工しています", "Processing") : L("生成しています", "Generating"); break;
		case PH_DOWNLOAD: label = L("画像を受け取っています", "Downloading"); break;
		case PH_SAVE: label = L("保存しています", "Saving"); break;
		case PH_DECODE: label = L("画像を展開しています", "Decoding image"); break;
		default: break;
		}
		// 紺で暗くして、光るペン先・回る点の輪・きらめきを出す
		float ta = fx_clamp01(busy_f / 12.0f);
		// 画像があるときは少し透けて見えるように、待機画面のときはしっかり暗く
		ui_rect(0, 0, TOP_W, SCR_H, RGBA8(0x12, 0x13, 0x24, (u8)((view.valid ? 0xC8 : 0xF0) * ta)));
		float cx = 200, cy = 98;
		fx_sparkles(cx, cy, 170, 80, 12, COL_CREAM, 3);
		fx_ring(cx, cy, 58, 1, alpha_of(COL_CREAM, 0.12f * ta));
		fx_spinner(cx, cy, 48, alpha_of(COL_CREAM, ta));
		fx_logo(cx, cy, true, ta, 0.8f + 0.04f * sinf(ui_frame() * 0.1f), 1.0f);
		snprintf(b, sizeof(b), "%s%.*s", label, (int)((ui_frame() / 20) % 4), "...");
		float lw = ui_text_width(F_BODY, "...") ;
		ui_text(F_BODY, 200 - (ui_text_width(F_BODY, label) + lw) / 2, 160, alpha_of(COL_WHITE, ta), b);
		bool known = (g_job.phase == PH_DOWNLOAD || g_job.phase == PH_UPLOAD) && g_job.progress > 0;
		fx_bar(80, 190, 240, 12, known ? g_job.progress : -1.0f);
		if (known) snprintf(b, sizeof(b), "%d%%", (int)(g_job.progress * 100));
		else snprintf(b, sizeof(b), L("%d秒", "%ds"), busy_f / 60);
		ui_text_center(F_SMALL, 200, 208, alpha_of(RGBA8(0xB8, 0xBA, 0xD6, 0xFF), ta), b);
	}

	draw_status_bar();
}

// ---------------------------------------------------------------- 描画: 下画面

static void draw_flash_or(const char *footer)
{
	ui_footer(footer);
	if (flash_t > 0) {
		float w = ui_text_width(F_SMALL, flash) + 24;
		if (w > BOT_W - 16) w = BOT_W - 16;
		float a = flash_t < 20 ? flash_t / 20.0f : 1.0f;
		ui_rrect((BOT_W - w) / 2, FOOT_Y - 30, w, 24, 12, RGBA8(0x22, 0x24, 0x38, (u8)(0xE0 * a)));
		ui_text_fit(F_SMALL, (BOT_W - w) / 2 + 12, ui_text_y(F_SMALL, FOOT_Y - 18), w - 24,
			    ui_mix(RGBA8(0xFF, 0xFF, 0xFF, 0), COL_WHITE, a), flash);
	}
}

static void draw_label(float x, float y, const char *s) { ui_text(F_SMALL, x, y, COL_SUB, s); }

static void draw_gen(void)
{
	char b[64];
	const ModelInfo *m = &g_models[cfg.model];

	draw_label(10, 28, L("プロンプト", "Prompt"));
	snprintf(b, sizeof(b), L("%d文字", "%d chars"), (int)strlen(cfg.prompt));
	ui_text_right(F_SMALL, 310, 28, COL_FAINT, b);
	ui_field(6, GEN_PROMPT_Y, 308, GEN_PROMPT_H);
	if (cfg.prompt[0]) ui_text_wrap(F_BODY, 12, GEN_PROMPT_Y + 3, COL_TEXT, 296, cfg.prompt, 3);
	else ui_text(F_BODY, 12, GEN_PROMPT_Y + 3, COL_FAINT, L("タッチして入力（英語のタグをカンマ区切りで）", "Tap to enter (comma-separated tags)"));

	draw_label(10, GEN_UC_Y - 17, L("除外したい要素", "Undesired Content"));
	ui_field(6, GEN_UC_Y, 308, GEN_UC_H);
	float ty = ui_text_y(F_BODY, GEN_UC_Y + GEN_UC_H / 2.0f);
	if (m->medium) ui_text_fit(F_SMALL, 12, ui_text_y(F_SMALL, GEN_UC_Y + GEN_UC_H / 2.0f), 296, COL_FAINT, L("このモデルでは指定できません（プリセット固定）", "Not available on this model (preset only)"));
	else if (cfg.uc[0]) ui_text_fit(F_BODY, 12, ty, 296, COL_TEXT, cfg.uc);
	else {
		snprintf(b, sizeof(b), L("プリセット「%s」のみ", "Preset %s only"), g_uc_labels[cfg.uc_preset]);
		ui_text(F_BODY, 12, ty, COL_FAINT, b);
	}

	ui_selector(6, GEN_SEL_Y, 152, 24, m->label, true);
	int p = config_size_preset(&cfg);
	if (p >= 0) snprintf(b, sizeof(b), "%s", g_sizes[p].label[g_lang]);
	else snprintf(b, sizeof(b), "%d×%d", cfg.width, cfg.height);
	ui_selector(162, GEN_SEL_Y, 152, 24, b, true);

	ui_button(6, GEN_BTN_Y, 308, 22, L("生成する", "Generate"), BTN_PRIMARY);
	draw_cost(310, GEN_BTN_Y + 11, anlas_generate(&cfg, ACT_GENERATE, 0, 0, opus_free_for(cfg.model)));

	switch (gen_focus) {
	case 0: ui_cursor(6, GEN_PROMPT_Y, 308, GEN_PROMPT_H, 5); break;
	case 1: ui_cursor(6, GEN_UC_Y, 308, GEN_UC_H, 5); break;
	case 2: ui_cursor(6, GEN_SEL_Y, 152, 24, 7); break;
	case 3: ui_cursor(162, GEN_SEL_Y, 152, 24, 7); break;
	default: ui_cursor(6, GEN_BTN_Y, 308, 22, 7); break;
	}
	if (gen_focus == 2 || gen_focus == 3) draw_flash_or(L("+:選ぶ|A:変更|START:生成", "+:Select|A:Change|START:Generate"));
	else draw_flash_or(L("A:入力|X:除外|START:生成|L/R:タブ", "A:Edit|X:UC|START:Generate|L/R:Tabs"));
}

static void draw_value_row(float x, float y, float w, const char *name, const char *value, bool arrows, bool enabled, bool chevron)
{
	ui_button(x, y, w, 24, NULL, enabled ? BTN_NORMAL : BTN_DISABLED);
	float ty = ui_text_y(F_BODY, y + 12);
	int nf = ui_text_width(F_BODY, name) <= w * 0.44f - 4 ? F_BODY : F_SMALL;
	ui_text_fit(nf, x + 10, nf == F_BODY ? ty : ui_text_y(F_SMALL, y + 12), w * 0.44f, enabled ? COL_TEXT : COL_FAINT, name);
	float vx0 = x + w * 0.46f, vx1 = x + w - 8;
	if (arrows && enabled) {
		ui_tri(vx0 + 6, y + 12, 8, 0, COL_PURPLE);
		ui_tri(vx1 - 4, y + 12, 8, 1, COL_PURPLE);
	}
	if (chevron) ui_tri(vx1 - 4, y + 12, 7, 1, COL_LINE_D);
	float vw = vx1 - vx0 - 28;
	int f = ui_text_width(F_BODY, value) <= vw ? F_BODY : F_SMALL;
	float tw = fminf(ui_text_width(f, value), vw);
	ui_text_fit(f, (vx0 + vx1) / 2 - tw / 2, ui_text_y(f, y + 12), vw, enabled ? COL_NAVY : COL_FAINT, value);
}

static void draw_set(void)
{
	char b[96];
	for (int row = 0; row < SET_COUNT; row++) {
		float y = SET_LIST_Y + row * SET_ROW_H - set_scroll;
		if (y + 30 < SET_LIST_Y || y > FOOT_Y) continue;
		set_value(row, b, sizeof(b));
		bool locked = set_locked(row);
		draw_value_row(6, y, BOT_W - 20, set_labels[g_lang][row], b, set_has_arrows(row), !locked, !set_has_arrows(row));
		if (row == SET_KEY && !cfg.api_key[0]) ui_circle(BOT_W - 24, y + 12, 3, COL_RED);
		if (row == set_cur) ui_cursor(6, y, BOT_W - 20, 24, 7);
	}
	// スクロールバー
	float total = SET_COUNT * SET_ROW_H - 3, track = SET_LIST_H, bar = track * track / total, mx = set_max_scroll();
	ui_rrect(BOT_W - 8, SET_LIST_Y, 4, track, 2, RGBA8(0xD6, 0xD7, 0xDE, 0xFF));
	ui_rrect(BOT_W - 8, SET_LIST_Y + (mx > 0 ? (track - bar) * set_scroll / mx : 0), 4, bar, 2, COL_PURPLE);
	draw_flash_or(set_has_arrows(set_cur) ? L("+:選ぶ・変える|A:決定|L/R:タブ", "+:Select/Change|A:OK|L/R:Tabs") : L("+:選ぶ|A:決定|L/R:タブ", "+:Select|A:OK|L/R:Tabs"));
}

static void draw_switch(float x, float y, bool on)
{
	ui_rrect(x, y, 30, 14, 7, on ? COL_GREEN : RGBA8(0xBE, 0xBF, 0xC8, 0xFF));
	ui_circle(on ? x + 23 : x + 7, y + 7, 5.5f, COL_WHITE);
}

static void draw_chr(void)
{
	char b[96];
	const ModelInfo *m = &g_models[cfg.model];
	for (int i = 0; i < MAX_CHARS; i++) {
		const CharSlot *s = &cfg.chars[i];
		float y = CHR_ROW_Y + i * CHR_ROW_H;
		ui_button(6, y, 176, CHR_ROW_H - 4, NULL, BTN_NORMAL);
		ui_circle(19, y + 13, 8, s->prompt[0] && s->enabled ? COL_PURPLE : RGBA8(0xBE, 0xBF, 0xC8, 0xFF));
		snprintf(b, sizeof(b), "%d", i + 1);
		ui_text_center(F_SMALL, 19, ui_text_y(F_SMALL, y + 13), COL_WHITE, b);
		float ty = ui_text_y(F_BODY, y + (CHR_ROW_H - 4) / 2.0f);
		if (s->prompt[0]) ui_text_fit(F_BODY, 32, ty, 110, s->enabled ? COL_TEXT : COL_FAINT, s->prompt);
		else ui_text(F_BODY, 32, ty, COL_FAINT, L("（空き）", "(empty)"));
		draw_switch(148, y + 6, s->enabled);
		if (i == chr_cur) ui_cursor(6, y, 176, CHR_ROW_H - 4, 7);
	}
	const CharSlot *s = &cfg.chars[chr_cur];
	draw_label(CHR_GRID_X, 29, L("画面の中の位置", "Position"));
	ui_rrect(CHR_GRID_X - 2, CHR_GRID_Y - 2, CHR_CELL * 5 + 4, CHR_CELL * 5 + 4, 5, COL_LINE_D);
	for (int gy = 0; gy < 5; gy++)
		for (int gx = 0; gx < 5; gx++) {
			bool on = s->gx == gx && s->gy == gy;
			float cx = CHR_GRID_X + gx * CHR_CELL, cy = CHR_GRID_Y + gy * CHR_CELL;
			ui_rect(cx, cy, CHR_CELL - 1, CHR_CELL - 1, (gx + gy) & 1 ? COL_WHITE : RGBA8(0xF2, 0xF2, 0xF6, 0xFF));
			if (on) ui_rrect_grad(cx + 2, cy + 2, CHR_CELL - 5, CHR_CELL - 5, 4, RGBA8(0xB0, 0x96, 0xFF, 0xFF), COL_PURPLE_D);
		}
	ui_button(190, 164, 124, 22, L("AIにまかせる", "AI's choice"), s->gx < 0 ? BTN_ON : BTN_NORMAL);
	ui_button(190, 192, 60, 22, L("除外", "UC"), m->medium ? BTN_DISABLED : BTN_NORMAL);
	ui_button(254, 192, 60, 22, L("消去", "Clear"), BTN_NORMAL);
	if (m->gen < 4) {
		ui_footer("");
		ui_text(F_SMALL, 8, ui_text_y(F_SMALL, FOOT_Y + 11), COL_RED, L("このモデルはキャラ別プロンプトに対応していません", "This model has no character prompts"));
	} else {
		draw_flash_or(L("A:プロンプト|X:除外|Y:オン/オフ|+:選ぶ", "A:Prompt|X:UC|Y:On/Off|+:Select"));
	}
}

static void draw_tag(void)
{
	char b[160];
	// 保管 / 検索結果（つながったボタン）
	const char *seg[2] = {L("保管", "Saved"), L("検索結果", "Results")};
	int counts[2] = {tag_count, result_count};
	for (int i = 0; i < 2; i++) {
		float x = 6 + i * 75;
		bool on = tag_mode == i;
		ui_rrect_corners(x, 30, 75, 24, 7, i == 0 ? 5 : 10, on ? COL_NAVY_HI : COL_WHITE, on ? COL_NAVY : RGBA8(0xE2, 0xE2, 0xE6, 0xFF));
		snprintf(b, sizeof(b), "%s %d", seg[i], counts[i]);
		ui_text_center(F_SMALL, x + 37.5f, ui_text_y(F_SMALL, 42), on ? COL_CREAM : COL_TEXT, b);
	}
	ui_rring(6, 30, 150, 24, 7, 1, COL_LINE_D);
	ui_button(162, 30, 72, 24, L("検索", "Search"), BTN_NORMAL);
	ui_button(240, 30, 74, 24, L("＋追加", "+ Add"), BTN_NORMAL);

	if (tag_target == 2) snprintf(b, sizeof(b), L("追加先: キャラ%d", "Add to: Char %d"), chr_cur + 1);
	else snprintf(b, sizeof(b), L("追加先: %s", "Add to: %s"), tag_target == 0 ? L("プロンプト", "Prompt") : L("除外", "UC"));
	ui_selector(6, 60, 160, 22, b, true);

	int n = tag_list_count();
	if (!n) {
		ui_text_center(F_BODY, BOT_W / 2, 136, COL_FAINT, tag_mode == 0 ? L("保管したタグはありません", "No saved tags") : L("「検索」でDanbooruタグの候補を探せます", "Use Search to find Danbooru tags"));
	} else {
		ui_rrect(6, TAG_LIST_Y - 2, BOT_W - 18, TAG_ROWS * TAG_ROW_H + 4, 6, COL_LINE);
		ui_rrect(7, TAG_LIST_Y - 1, BOT_W - 20, TAG_ROWS * TAG_ROW_H + 2, 5, COL_WHITE);
		for (int i = 0; i < TAG_ROWS; i++) {
			int row = tag_scroll + i;
			if (row >= n) break;
			float y = TAG_LIST_Y + i * TAG_ROW_H;
			bool sel = row == tag_cur;
			if (i > 0) ui_rect(12, y, BOT_W - 30, 1, RGBA8(0xE6, 0xE6, 0xEC, 0xFF));
			if (sel) ui_rrect(8, y + 1, BOT_W - 22, TAG_ROW_H - 1, 4, COL_PURPLE_L);
			if (tag_mode == 1 && results[row].label[0]) snprintf(b, sizeof(b), "%.40s → %.60s", results[row].label, results[row].tag);
			else snprintf(b, sizeof(b), "%s", tag_list_item(row));
			ui_text_fit(F_BODY, 14, ui_text_y(F_BODY, y + TAG_ROW_H / 2.0f), 230, sel ? COL_NAVY : COL_TEXT, b);
			if (tag_mode == 1 && results[row].count > 0) {
				snprintf(b, sizeof(b), "%d", results[row].count);
				ui_text_right(F_SMALL, BOT_W - 20, ui_text_y(F_SMALL, y + TAG_ROW_H / 2.0f), COL_FAINT, b);
			}
		}
		if (n > TAG_ROWS) {
			float track = TAG_ROWS * TAG_ROW_H, bar = track * TAG_ROWS / n;
			ui_rrect(BOT_W - 8, TAG_LIST_Y, 4, track, 2, RGBA8(0xD6, 0xD7, 0xDE, 0xFF));
			ui_rrect(BOT_W - 8, TAG_LIST_Y + (track - bar) * tag_scroll / (n - TAG_ROWS), 4, bar, 2, COL_PURPLE);
		}
	}
	draw_flash_or(tag_mode == 0 ? L("A:追加|X:検索|Y:保管から外す", "A:Add|X:Search|Y:Remove") : L("A:追加|X:検索|Y:保管する", "A:Add|X:Search|Y:Save"));
}

static void draw_gal(void)
{
	char b[96];
	if (!gal_count) {
		ui_text_center(F_BODY, BOT_W / 2, 100, COL_FAINT, L("まだ画像がありません", "No images yet"));
		ui_text_center(F_SMALL, BOT_W / 2, 124, COL_FAINT, L("生成した画像はここに並びます", "Generated images appear here"));
		draw_flash_or(L("L/R:タブ", "L/R:Tabs"));
		return;
	}
	int page = gal_cur / GAL_PER_PAGE;
	for (int i = 0; i < GAL_PER_PAGE; i++) {
		int idx = page * GAL_PER_PAGE + i;
		if (idx >= gal_count) break;
		float x = GAL_X + (i % GAL_COLS) * GAL_GX, y = GAL_Y + (i / GAL_COLS) * GAL_GY;
		ui_shadow(x, y, GAL_CW, GAL_CH, 5);
		ui_rrect(x, y, GAL_CW, GAL_CH, 5, COL_LINE);
		ui_rrect(x + 1, y + 1, GAL_CW - 2, GAL_CH - 2, 4, COL_WHITE);
		const Pic *t = &gal_thumbs[i];
		if (t->valid) {
			float s = fminf((GAL_CW - 6.0f) / t->w, (GAL_CH - 6.0f) / t->h);
			if (s > 1.0f) s = 1.0f;
			pic_draw(t, x + (GAL_CW - t->w * s) / 2, y + (GAL_CH - t->h * s) / 2, s);
		} else {
			ui_text_center(F_SMALL, x + GAL_CW / 2, y + GAL_CH / 2 - 8, COL_FAINT, "PNG");
		}
		if (idx == gal_del_confirm) {
			ui_rrect(x + 1, y + 1, GAL_CW - 2, GAL_CH - 2, 4, RGBA8(0xE0, 0x48, 0x48, 0x60));
			ui_text_center(F_SMALL, x + GAL_CW / 2, y + GAL_CH / 2 - 8, COL_WHITE, L("削除?", "Delete?"));
		}
		if (idx == gal_cur) ui_cursor(x, y, GAL_CW, GAL_CH, 5);
	}
	if (gal_cur_shown()) snprintf(b, sizeof(b), L("A:拡大|X:設定を読込|Y:削除|%d/%d", "A:Zoom|X:Load settings|Y:Delete|%d/%d"), gal_cur + 1, gal_count);
	else snprintf(b, sizeof(b), L("A:表示|X:設定を読込|Y:削除|%d/%d", "A:View|X:Load settings|Y:Delete|%d/%d"), gal_cur + 1, gal_count);
	draw_flash_or(b);
}

static void draw_edit(void)
{
	char b[160];
	if (!view.valid) {
		ui_text_center(F_BODY, BOT_W / 2, 70, COL_TEXT, L("編集する画像がありません", "No image to edit"));
		ui_text_center(F_SMALL, BOT_W / 2, 98, COL_SUB, L("一覧で画像を選んで A で表示すると、", "Open an image from the Gallery with A"));
		ui_text_center(F_SMALL, BOT_W / 2, 116, COL_SUB, L("その画像をここで加工できます。", "to edit it here."));
		ui_button(80, 150, 160, 28, L("一覧を開く", "Open Gallery"), BTN_NORMAL);
		ui_cursor(80, 150, 160, 28, 7);
		draw_flash_or(L("A:一覧を開く", "A:Open Gallery"));
		return;
	}
	// 元画像
	ui_rrect(6, 29, 308, 38, 7, COL_LINE);
	ui_rrect_grad(7, 30, 306, 36, 6, COL_WHITE, RGBA8(0xF2, 0xF2, 0xF5, 0xFF));
	float s = fminf(26.0f / view.w, 32.0f / view.h);
	pic_draw(&view, 12 + (26 - view.w * s) / 2, 32 + (32 - view.h * s) / 2, s);
	ui_text_fit(F_BODY, 46, 29, 262, COL_TEXT, view_name);
	int tw, th;
	img_fit64(view_fw, view_fh, &tw, &th);
	if (edit_mode == EM_DIRECTOR || (tw == view_fw && th == view_fh)) snprintf(b, sizeof(b), "%d×%d", view_fw, view_fh);
	else snprintf(b, sizeof(b), L("%d×%d（%d×%dにして送ります）", "%d×%d (sent as %d×%d)"), view_fw, view_fh, tw, th);
	ui_text_fit(F_SMALL, 46, 48, 262, COL_SUB, b);

	// モード切り替え
	float sw = 308.0f / EM_COUNT;
	for (int i = 0; i < EM_COUNT; i++) {
		bool on = edit_mode == i;
		ui_rrect_corners(6 + i * sw, EDIT_MODE_Y, sw, 26, 7, i == 0 ? 5 : i == EM_COUNT - 1 ? 10 : 0, on ? COL_NAVY_HI : COL_WHITE,
				 on ? COL_NAVY : RGBA8(0xE2, 0xE2, 0xE6, 0xFF));
		if (i > 0) ui_rect(6 + i * sw, EDIT_MODE_Y, 1, 26, COL_LINE_D);
		ui_text_center(F_SMALL, 6 + i * sw + sw / 2, ui_text_y(F_SMALL, EDIT_MODE_Y + 13), on ? COL_CREAM : COL_TEXT, edit_mode_labels[g_lang][i]);
	}
	ui_rring(6, EDIT_MODE_Y, 308, 26, 7, 1, COL_LINE_D);

	const char *run = L("この画像から生成", "Generate from this image");
	const char *foot = L("+:選ぶ|A:決定|START:生成", "+:Select|A:OK|START:Generate");
	switch (edit_mode) {
	case EM_I2I:
		snprintf(b, sizeof(b), "%.2f", (double)cfg.i2i_strength);
		draw_value_row(6, EDIT_ROW1_Y, 308, L("変える強さ", "Strength"), b, true, true, false);
		snprintf(b, sizeof(b), "%.2f", (double)cfg.i2i_noise);
		draw_value_row(6, EDIT_ROW2_Y, 308, L("ノイズ", "Noise"), b, true, true, false);
		ui_text_wrap(F_SMALL, 10, EDIT_ROW3_Y, COL_SUB, 300, L("強さが大きいほど元の絵から離れます。プロンプトなどは生成タブの内容を使います。", "Higher strength changes the picture more. Uses the prompt and settings from the Create tab."), 2);
		break;
	case EM_INPAINT: {
		int total = mask && !strcmp(mask_name, view_name) ? mask_w * mask_h : 0;
		int pct = total ? (mask_painted * 100 + total / 2) / total : 0;
		snprintf(b, sizeof(b), L("塗った範囲 %d%%", "Painted %d%%"), pct);
		ui_button(6, EDIT_ROW1_Y, 150, 24, L("マスクを塗る", "Paint Mask"), BTN_NORMAL);
		ui_text(F_SMALL, 166, ui_text_y(F_SMALL, EDIT_ROW1_Y + 12), pct ? COL_PURPLE_D : COL_FAINT, b);
		snprintf(b, sizeof(b), "%.2f", (double)cfg.inpaint_strength);
		draw_value_row(6, EDIT_ROW2_Y, 308, L("描き直す強さ", "Strength"), b, true, true, false);
		ui_text_wrap(F_SMALL, 10, EDIT_ROW3_Y, COL_SUB, 300, L("塗った部分を、生成タブのプロンプトで描き直します", "Redraws the painted area with the Create tab prompt"), 2);
		run = L("インペイント", "Inpaint");
		break;
	}
	default: {
		const DirTool *t = &g_dir_tools[cfg.dir_tool];
		draw_value_row(6, EDIT_ROW1_Y, 308, L("ツール", "Tool"), t->label[g_lang], true, true, false);
		if (t->takes_prompt) {
			if (cfg.dir_tool == DIR_EMOTION) draw_value_row(6, EDIT_ROW2_Y, 308, L("表情", "Emotion"), g_emotion_labels[g_lang][cfg.dir_emotion], true, true, false);
			else ui_text_fit(F_SMALL, 10, EDIT_ROW2_Y + 4, 300, COL_SUB, t->desc[g_lang]);
			snprintf(b, sizeof(b), L("効き %s", "Effect: %s"), g_defry_labels[g_lang][cfg.dir_defry]);
			ui_selector(6, EDIT_ROW3_Y, 150, 24, b, true);
			ui_button(162, EDIT_ROW3_Y, 152, 24, cfg.dir_prompt[0] ? cfg.dir_prompt : L("追加の指示…", "Extra prompt…"), BTN_NORMAL);
		} else {
			ui_text_wrap(F_SMALL, 10, EDIT_ROW2_Y + 2, COL_SUB, 300, t->desc[g_lang], 2);
		}
		run = L("加工する", "Apply");
		foot = L("+:選ぶ|A:決定|START:実行", "+:Select|A:OK|START:Run");
		break;
	}
	}
	ui_button(6, EDIT_RUN_Y, 308, 22, run, BTN_PRIMARY);
	{
		int tw2, th2;
		img_fit64(view_fw, view_fh, &tw2, &th2);
		int cost;
		if (edit_mode == EM_DIRECTOR) cost = anlas_director(cfg.dir_tool, view_fw, view_fh, acct_valid && acct_tier >= 3 && acct_active);
		else cost = anlas_generate(&cfg, edit_mode == EM_INPAINT ? ACT_INFILL : ACT_IMG2IMG, tw2, th2, opus_free_for(cfg.model));
		draw_cost(310, EDIT_RUN_Y + 11, cost);
	}

	switch (edit_focus) {
	case EF_MODE: ui_cursor(6, EDIT_MODE_Y, 308, 26, 7); break;
	case EF_A:
		if (edit_mode == EM_INPAINT) ui_cursor(6, EDIT_ROW1_Y, 150, 24, 7);
		else ui_cursor(6, EDIT_ROW1_Y, 308, 24, 7);
		break;
	case EF_B: ui_cursor(6, EDIT_ROW2_Y, 308, 24, 7); break;
	case EF_C: ui_cursor(6, EDIT_ROW3_Y, 150, 24, 7); break;
	case EF_D: ui_cursor(162, EDIT_ROW3_Y, 152, 24, 7); break;
	default: ui_cursor(6, EDIT_RUN_Y, 308, 22, 7); break;
	}
	draw_flash_or(foot);
}

static void draw_mask_editor(void)
{
	ui_rect(0, 0, BOT_W, SCR_H, RGBA8(0x2A, 0x2B, 0x36, 0xFF));
	float ox, oy, dw, dh;
	canvas_rect(&ox, &oy, &dw, &dh);
	pic_draw(&view, ox, oy, dw / view.w);
	pic_draw(&mask_pic, ox, oy, dw / mask_w);
	// 道具
	ui_grad(TOOL_X - 4, 0, BOT_W - TOOL_X + 4, SCR_H, RGBA8(0xF6, 0xF6, 0xF8, 0xFF), RGBA8(0xE0, 0xE1, 0xE6, 0xFF));
	ui_rect(TOOL_X - 5, 0, 1, SCR_H, COL_LINE_D);
	ui_button(TOOL_X, 8, 60, 28, L("ペン", "Pen"), mask_erase ? BTN_NORMAL : BTN_ON);
	ui_button(TOOL_X, 40, 60, 28, L("消しゴム", "Eraser"), mask_erase ? BTN_ON : BTN_NORMAL);
	ui_text_center(F_SMALL, TOOL_X + 30, 74, COL_SUB, L("太さ", "Size"));
	for (int i = 0; i < 3; i++) {
		float cx = TOOL_X + 10 + i * 20, cy = 104;
		if (i == mask_brush) ui_rrect(cx - 9, cy - 9, 18, 18, 5, COL_PURPLE_L);
		ui_circle(cx, cy, 2.5f + i * 2.0f, i == mask_brush ? COL_PURPLE_D : COL_LINE_D);
	}
	ui_button(TOOL_X, 126, 60, 26, L("全部消す", "Clear"), BTN_NORMAL);
	ui_button(TOOL_X, 158, 60, 26, L("反転", "Invert"), BTN_NORMAL);
	ui_button(TOOL_X, 204, 60, 30, L("完了", "Done"), BTN_PRIMARY);
}

static void draw_dialogs(void)
{
	if (job_modal()) {
		ui_rect(0, 0, BOT_W, SCR_H, COL_SHADE);
		ui_shadow(30, 70, 260, 100, 12);
		ui_rrect(30, 70, 260, 100, 12, COL_LINE_D);
		ui_rrect_grad(31, 71, 258, 98, 11, COL_WHITE, RGBA8(0xF0, 0xF0, 0xF4, 0xFF));
		const char *t = g_job.type == JOB_LOADIMG ? L("画像を読み込んでいます", "Loading image") : g_job.type == JOB_SUGGEST ? L("タグを探しています", "Searching tags") : L("通信しています", "Please wait");
		float ttw = ui_text_width(F_BODY, t);
		ui_text(F_BODY, BOT_W / 2 - ttw / 2 + 10, 86, COL_TEXT, t);
		fx_spinner(BOT_W / 2 - ttw / 2 - 6, ui_text_y(F_BODY, 0) * -1 + 86 + 0.5f, 7, COL_PURPLE_D);
		bool known = (g_job.phase == PH_DOWNLOAD || g_job.phase == PH_UPLOAD) && g_job.progress > 0;
		fx_bar(56, 113, 208, 11, known ? g_job.progress : -1.0f);
		if (g_job.type != JOB_LOADIMG) {
			float w = ui_key_icon(0, -100, "B") + 6 + ui_text_width(F_SMALL, L("キャンセル", "Cancel"));
			float x = BOT_W / 2 - w / 2;
			x += ui_key_icon(x, 138, "B") + 6;
			ui_text(F_SMALL, x, ui_text_y(F_SMALL, 138 + 8), COL_SUB, L("キャンセル", "Cancel"));
		}
	}
	if (msg_on) {
		ui_rect(0, 0, BOT_W, SCR_H, COL_SHADE);
		ui_shadow(16, 34, 288, 172, 12);
		ui_rrect(16, 34, 288, 172, 12, COL_LINE_D);
		ui_rrect_grad(17, 35, 286, 170, 11, COL_WHITE, RGBA8(0xF0, 0xF0, 0xF4, 0xFF));
		ui_text_wrap(F_BODY, 30, 48, COL_TEXT, 260, msg, 5);
		ui_button(110, 166, 100, 28, "OK", BTN_NORMAL);
		ui_cursor(110, 166, 100, 28, 7);
	}
}

// ---------------------------------------------------------------- メイン

static void switch_tab(int t)
{
	tab = wrapi(t, TAB_COUNT);
	save_cfg();
	gal_del_confirm = -1;
}

static void poll_system(void)
{
	static int t;
	if (t-- > 0) return;
	t = 120;
	if (ptm_ok) {
		PTMU_GetBatteryLevel(&battery_level);
		PTMU_GetBatteryChargeState(&battery_charging);
	}
}

int main(void)
{
	gfxInitDefault();
	romfsInit();
	osSetSpeedupEnable(true); // New 3DSでは画像の展開が速くなる
	ptm_ok = R_SUCCEEDED(ptmuInit());
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(8192);
	C2D_Prepare();
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	ui_init();
	fx_init();
	hidSetRepeatParameters(20, 5);

	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	mkdir(IMG_DIR, 0777);
	config_defaults(&cfg);
	if (!config_load(&cfg, CONFIG_PATH)) config_save(&cfg, CONFIG_PATH, true); // 書き換え用のひな形を作る
	tags_load();
	apply_lang();

	key_state = cfg.api_key[0] ? KEY_UNKNOWN : KEY_NONE;
	bool net = nai_init();
	if (!net) show_msg(L("通信機能を初期化できませんでした。一覧の閲覧だけ使えます。", "Network init failed. Only the Gallery is available."));
	else if (!cfg.api_key[0])
		show_msg(L("APIキーが未設定です。\n設定タブの「APIキー」で入力するか、SDカードの /3ds/thalia/config.ini に書いてください。", "No API key.\nEnter it under Settings > API Key, or write it in /3ds/thalia/config.ini on the SD card."));
	else start_account_check();

	while (aptMainLoop() && !quit) {
		hidScanInput();
		u32 down = hidKeysDown(), rep = hidKeysDownRepeat(), held = hidKeysHeld();
		touchPosition tp = {0};
		bool tap = (down & KEY_TOUCH) != 0;
		if (viewer_on && !view.valid) viewer_on = false;
		if (tap) hidTouchRead(&tp);
		if (flash_t > 0) flash_t--;
		poll_system();
		if (intro_f < INTRO_LEN) intro_f = (down & ~KEY_TOUCH) || tap ? INTRO_LEN : intro_f + 1; // ボタンで飛ばせる
		busy_f = job_modal() ? busy_f + 1 : 0;

		if (msg_on) {
			if (down & (KEY_A | KEY_B | KEY_TOUCH)) msg_on = false;
		} else if (job_modal()) {
			if (down & KEY_B) g_job.cancel = true;
		} else if (viewer_on) {
			input_viewer(down, held, &tp, tap);
		} else if (mask_editing) {
			input_mask(down, held, &tp, tap);
		} else {
			if (down & KEY_L) switch_tab(tab - 1);
			if (down & KEY_R) switch_tab(tab + 1);
			if ((down & KEY_SELECT) && view.valid) viewer_open();
			if (down & KEY_START) {
				if (tab == TAB_EDIT) start_edit();
				else start_generate();
			}
			if (tap && tp.py < TAB_H) {
				switch_tab(tp.px * TAB_COUNT / BOT_W);
			} else {
				switch (tab) {
				case TAB_GEN: input_gen(down, rep, &tp, tap); break;
				case TAB_EDIT: input_edit(down, rep, &tp, tap); break;
				case TAB_CHR: input_chr(down, rep, &tp, tap); break;
				case TAB_TAG: input_tag(down, rep, &tp, tap); break;
				case TAB_GAL: input_gal(down, rep, &tp, tap); break;
				case TAB_SET: input_set(down, rep, held, &tp, tap); break;
				}
			}
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		// テクスチャの差し替えは、前のフレームの描画が済んだここで行う
		job_poll();
		if (tab == TAB_GAL) {
			if (gal_dirty) gal_scan();
			if (gal_page != gal_cur / GAL_PER_PAGE) gal_load_page();
			gal_make_missing_thumb();
		}
		if (mask_editing && (!view.valid || strcmp(mask_name, view_name))) mask_editing = false;
		ui_frame_begin();

		C2D_TargetClear(top, COL_BG);
		C2D_SceneBegin(top);
		draw_top();

		C2D_TargetClear(bot, COL_BG);
		C2D_SceneBegin(bot);
		if (viewer_on) {
			draw_viewer_bottom();
		} else if (mask_editing) {
			draw_mask_editor();
		} else {
			ui_stripes(BOT_W);
			switch (tab) {
			case TAB_GEN: draw_gen(); break;
			case TAB_EDIT: draw_edit(); break;
			case TAB_CHR: draw_chr(); break;
			case TAB_TAG: draw_tag(); break;
			case TAB_GAL: draw_gal(); break;
			case TAB_SET: draw_set(); break;
			}
			ui_tabs(tab_labels[g_lang], TAB_COUNT, tab, TAB_H); // スクロールではみ出した分を隠すため後から描く
		}
		draw_dialogs();
		C3D_FrameEnd(0);
	}

	save_cfg();
	nai_exit();
	mask_reset();
	pic_free(&view);
	for (int i = 0; i < GAL_PER_PAGE; i++) pic_free(&gal_thumbs[i]);
	fx_exit();
	ui_exit();
	C2D_Fini();
	C3D_Fini();
	if (ptm_ok) ptmuExit();
	romfsExit();
	gfxExit();
	return 0;
}
