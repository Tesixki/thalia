#include "nai.h"
#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "imgcodec.h"

#define IMAGE_BASE "https://image.novelai.net"
#define API_BASE "https://api.novelai.net"
#define SOC_BUFFERSIZE 0x100000
#define WORKER_STACK (256 * 1024)
#define VIEW_MAX 1024 // 3DSのテクスチャ上限
#define THUMB_W 60
#define THUMB_H 88
#define RESP_LIMIT (32u * 1024 * 1024)

Job g_job;

static u32 *s_soc_buf;
static bool s_net_ok;
static Thread s_thread;
static Config *s_cfg; // ジョブ用のコピー
static char s_arg[256];
static int s_action;
static uint8_t *s_mask; // インペイントのマスク（ジョブ用のコピー）
static int s_mask_w, s_mask_h;
static uint8_t s_secret[32]; // 画像キャッシュキー用の秘密値（起動ごとに作る）

typedef struct {
	uint8_t *p;
	size_t len, cap;
	CURL *h; // 受信の大きさを問い合わせるため
} Mem;

// multipart で JSON より前に付ける画像パート
typedef struct {
	const char *name;
	const uint8_t *data;
	size_t len;
} Part;

// ---------------------------------------------------------------- 乱数

static u64 mix64(u64 t)
{
	t ^= t >> 33;
	t *= 0xFF51AFD7ED558CCDULL;
	t ^= t >> 33;
	t *= 0xC4CEB9FE1A85EC53ULL;
	t ^= t >> 33;
	return t;
}

static u64 s_rng;
static u32 rand32(void)
{
	s_rng += 0x9E3779B97F4A7C15ULL ^ svcGetSystemTick();
	return (u32)(mix64(s_rng) >> 16);
}

// ---------------------------------------------------------------- 初期化

bool nai_init(void)
{
	memset(&g_job, 0, sizeof(g_job));
	s_rng = svcGetSystemTick() ^ (osGetTime() * 0x9E3779B97F4A7C15ULL);
	bool have_secret = false;
	if (R_SUCCEEDED(psInit())) {
		have_secret = R_SUCCEEDED(PS_GenerateRandomBytes(s_secret, sizeof(s_secret)));
		psExit();
	}
	if (!have_secret)
		for (size_t i = 0; i < sizeof(s_secret); i += 4) {
			u32 r = rand32();
			memcpy(s_secret + i, &r, 4);
		}

	s_soc_buf = memalign(0x1000, SOC_BUFFERSIZE);
	if (!s_soc_buf) return false;
	if (R_FAILED(socInit(s_soc_buf, SOC_BUFFERSIZE))) {
		free(s_soc_buf);
		s_soc_buf = NULL;
		return false;
	}
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		socExit();
		free(s_soc_buf);
		s_soc_buf = NULL;
		return false;
	}
	s_net_ok = true;
	return true;
}

void nai_exit(void)
{
	if (g_job.running) {
		g_job.cancel = true;
		nai_finish();
	}
	if (s_net_ok) {
		curl_global_cleanup();
		socExit();
		s_net_ok = false;
	}
	free(s_soc_buf);
	s_soc_buf = NULL;
}

// ---------------------------------------------------------------- HTTP

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
	Mem *m = ud;
	size_t n = sz * nm;
	if (m->len + n + 1 > m->cap) {
		size_t cap = m->cap ? m->cap : 16384;
		curl_off_t cl = -1;
		if (!m->cap && m->h && curl_easy_getinfo(m->h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl > 0 &&
		    (curl_off_t)(m->len + n) <= cl && cl < (curl_off_t)RESP_LIMIT)
			cap = (size_t)cl + 1; // 大きさが分かっていれば倍々で確保しない
		while (cap < m->len + n + 1) cap *= 2;
		if (cap > RESP_LIMIT) return 0;
		uint8_t *np = realloc(m->p, cap);
		if (!np) return 0;
		m->p = np;
		m->cap = cap;
	}
	memcpy(m->p + m->len, ptr, n);
	m->len += n;
	m->p[m->len] = 0;
	return n;
}

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	bool track = ud != NULL;
	if (g_job.cancel) return 1;
	if (!track) return 0;
	if (dlnow > 0) {
		g_job.phase = PH_DOWNLOAD;
		g_job.progress = dltotal > 0 ? (float)dlnow / (float)dltotal : 0.0f;
	} else if (ultotal > 0 && ulnow >= ultotal) {
		g_job.phase = PH_WAIT;
	} else if (ultotal > 32768 && ulnow > 0) {
		// 画像を送っている間（小さいJSONだけのときは出さない）
		g_job.phase = PH_UPLOAD;
		g_job.progress = (float)ulnow / (float)ultotal;
	}
	return 0;
}

// post_json が NULL なら GET。parts は JSON より前に付ける画像（PNGそのまま）。
// 成功時 true（HTTPステータスは *code）。失敗時は err に理由
static bool http_do(const Config *c, const char *url, const char *post_json, const Part *parts, int nparts, Mem *resp,
		    long *code, bool track, char *err, size_t errsz)
{
	char curl_err[CURL_ERROR_SIZE] = "";
	char auth[320];
	*code = 0;
	CURL *h = curl_easy_init();
	if (!h) {
		snprintf(err, errsz, L("通信の準備に失敗しました", "Failed to prepare the request"));
		return false;
	}
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->api_key);
	struct curl_slist *hdr = curl_slist_append(NULL, auth);
	hdr = curl_slist_append(hdr, "Accept: */*");
	curl_mime *mime = NULL;

	resp->h = h;
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(h, CURLOPT_USERAGENT, APP_NAME "/" APP_VERSION);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfer_cb);
	curl_easy_setopt(h, CURLOPT_XFERINFODATA, track ? (void *)&g_job : NULL);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curl_err);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(h, CURLOPT_UPLOAD_BUFFERSIZE, 65536L);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	if (c->verify_tls) {
		curl_easy_setopt(h, CURLOPT_CAINFO, "romfs:/cacert.pem");
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
	} else {
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (post_json) {
		// 現行のWebUIと同じく multipart で送る。画像は生のPNGを別パートにし、
		// JSONの中ではそのパート名（"image" / "mask"）で指す
		mime = curl_mime_init(h);
		for (int i = 0; i < nparts; i++) {
			curl_mimepart *pp = curl_mime_addpart(mime);
			curl_mime_name(pp, parts[i].name);
			curl_mime_filename(pp, "blob");
			curl_mime_type(pp, "image/png");
			curl_mime_data(pp, (const char *)parts[i].data, parts[i].len);
		}
		curl_mimepart *part = curl_mime_addpart(mime);
		curl_mime_name(part, "request");
		curl_mime_filename(part, "blob");
		curl_mime_type(part, "application/json");
		curl_mime_data(part, post_json, CURL_ZERO_TERMINATED);
		curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
	}

	CURLcode r = curl_easy_perform(h);
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, code);
	g_job.http_code = *code;
	if (mime) curl_mime_free(mime);
	curl_slist_free_all(hdr);
	curl_easy_cleanup(h);

	if (r == CURLE_OK) return true;
	if (r == CURLE_ABORTED_BY_CALLBACK) snprintf(err, errsz, L("キャンセルしました", "Cancelled"));
	else if (r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CACERT_BADFILE)
		snprintf(err, errsz, L("サーバー証明書を確認できません。本体の日付と時刻が合っているか確認してください (%d)", "Cannot verify the server certificate. Check the console date and time (%d)"), (int)r);
	else if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT)
		snprintf(err, errsz, L("サーバーに接続できません。Wi-Fiを確認してください (%d)", "Cannot reach the server. Check Wi-Fi (%d)"), (int)r);
	else if (r == CURLE_OPERATION_TIMEDOUT) snprintf(err, errsz, L("タイムアウトしました", "Timed out"));
	else snprintf(err, errsz, L("通信エラー %d: %.180s", "Network error %d: %.180s"), (int)r, curl_err[0] ? curl_err : curl_easy_strerror(r));
	return false;
}

static void http_error_message(long code, const Mem *resp, char *err, size_t errsz)
{
	char msg[160] = "";
	if (resp->p) json_find_str((const char *)resp->p, "message", msg, sizeof(msg));
	switch (code) {
	case 401: snprintf(err, errsz, L("APIキーが無効です。設定タブで確認してください", "Invalid API key. Check it in Settings")); break;
	case 402: snprintf(err, errsz, L("Anlasが足りないか、サブスクリプションが有効ではありません", "Not enough Anlas, or no active subscription")); break;
	case 429: snprintf(err, errsz, L("同時に生成できるのは1枚までです。少し待ってからもう一度どうぞ", "Only one generation at a time. Wait a moment and retry")); break;
	case 400: case 409: snprintf(err, errsz, L("リクエストが拒否されました (%ld): %s", "Request rejected (%ld): %s"), code, msg); break;
	default:
		if (code >= 500) snprintf(err, errsz, L("NovelAI側のエラーです (%ld)。時間をおいて試してください", "NovelAI server error (%ld). Try again later"), code);
		else snprintf(err, errsz, L("エラー %ld: %s", "Error %ld: %s"), code, msg);
	}
}

// 画像のキャッシュキー: Base64にした画像を、起動ごとの秘密値でHMAC-SHA256したもの（64桁の16進）
static bool cache_key(const uint8_t *data, size_t n, char out[65])
{
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	mbedtls_md_context_t ctx;
	mbedtls_md_init(&ctx);
	bool ok = info && mbedtls_md_setup(&ctx, info, 1) == 0 && mbedtls_md_hmac_starts(&ctx, s_secret, sizeof(s_secret)) == 0;
	char chunk[4096];
	for (size_t i = 0; ok && i < n; i += 3072) { // 3の倍数ずつ区切れば、つなげても同じBase64になる
		size_t len = n - i < 3072 ? n - i : 3072;
		size_t m = b64_encode(data + i, len, chunk);
		ok = mbedtls_md_hmac_update(&ctx, (const unsigned char *)chunk, m) == 0;
	}
	unsigned char mac[32];
	ok = ok && mbedtls_md_hmac_finish(&ctx, mac) == 0;
	mbedtls_md_free(&ctx);
	if (!ok) return false;
	for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", mac[i]);
	return true;
}

// ---------------------------------------------------------------- 画像の保存と表示用データ

static void img_path(char *out, size_t n, const char *name, const char *ext)
{
	snprintf(out, n, IMG_DIR "/%.40s%s", name, ext);
}

static bool write_file(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fwrite(data, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = len > 0 && (unsigned long)len < RESP_LIMIT ? malloc((size_t)len) : NULL;
	bool ok = buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
	fclose(f);
	if (!ok) {
		free(buf);
		return NULL;
	}
	*n = (size_t)len;
	return buf;
}

static void write_thumb(const char *name, const uint8_t *rgb, int w, int h)
{
	int tw, th;
	uint8_t *t = img_resize_fit(rgb, w, h, THUMB_W, THUMB_H, &tw, &th);
	if (!t) return;
	char path[128];
	img_path(path, sizeof(path), name, ".thm");
	FILE *f = fopen(path, "wb");
	if (f) {
		uint8_t hd[8] = {'N', 'T', 'H', '1', (uint8_t)tw, (uint8_t)(tw >> 8), (uint8_t)th, (uint8_t)(th >> 8)};
		fwrite(hd, 1, 8, f);
		fwrite(t, 1, (size_t)tw * th * 3, f);
		fclose(f);
	}
	free(t);
}

// 一覧の名前から元の画像ファイルを探す（.png か .webp）
static bool find_image(const char *name, char *path, size_t n)
{
	struct stat st;
	img_path(path, n, name, ".png");
	if (stat(path, &st) == 0) return true;
	img_path(path, n, name, ".webp");
	return stat(path, &st) == 0;
}

// 表示用に縮小した RGB から、サムネイルと表示用キャッシュ（.vw）を作り、RGB565 を返す。
// 次に一覧から開くときは元の画像を展開せずに、このキャッシュを読むだけで済む
static uint16_t *finish_view(const char *name, const uint8_t *view, int vw, int vh, int fw, int fh, bool make_thumb)
{
	if (make_thumb) write_thumb(name, view, vw, vh);
	uint16_t *px = malloc((size_t)vw * vh * 2);
	if (!px) return NULL;
	rgb_to_565(view, (size_t)vw * vh, px);
	uint8_t *enc;
	size_t enc_len;
	if (vw_encode(px, vw, vh, fw, fh, &enc, &enc_len)) {
		char path[128];
		img_path(path, sizeof(path), name, ".vw");
		write_file(path, enc, enc_len);
		free(enc);
	}
	return px;
}

// 画像（PNG / WebP）を表示用の大きさで展開し、サムネイルとキャッシュも作る。
// 元の大きさの画像は縮小したらすぐ手放す（WebP はそもそも縮小しながら展開する）
static uint16_t *decode_view(const uint8_t *img, size_t n, const char *name, bool make_thumb, int *vw, int *vh, int *fw, int *fh)
{
	uint8_t *view = img_decode_rgb_fit(img, n, VIEW_MAX, VIEW_MAX, true, vw, vh, fw, fh);
	if (!view) {
		snprintf(g_job.error, sizeof(g_job.error), L("画像を展開できません: %s", "Cannot decode image: %s"), img_error());
		return NULL;
	}
	uint16_t *px = finish_view(name, view, *vw, *vh, *fw, *fh, make_thumb);
	free(view);
	if (!px) snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
	return px;
}

// 展開した表示用の画像を g_job に入れる
static bool decode_to_view(const uint8_t *img, size_t n, const char *name, bool make_thumb)
{
	int vw, vh, fw, fh;
	uint16_t *px = decode_view(img, n, name, make_thumb, &vw, &vh, &fw, &fh);
	if (!px) return false;
	g_job.view_px = px;
	g_job.view_w = vw;
	g_job.view_h = vh;
	g_job.full_w = fw;
	g_job.full_h = fh;
	return true;
}

// 表示用キャッシュを読む。元の画像より古いもの・壊れたものは使わない
static bool load_view_cache(const char *name)
{
	char src[128], vwp[128];
	if (!find_image(name, src, sizeof(src))) return false;
	img_path(vwp, sizeof(vwp), name, ".vw");
	struct stat sp, sv;
	if (stat(vwp, &sv) != 0 || stat(src, &sp) != 0 || sv.st_mtime < sp.st_mtime) return false;
	size_t n;
	uint8_t *buf = read_file(vwp, &n);
	if (!buf) return false;
	uint16_t *px;
	int w, h, fw, fh;
	bool ok = vw_decode(buf, n, &px, &w, &h, &fw, &fh);
	free(buf);
	if (!ok) return false;
	g_job.view_px = px;
	g_job.view_w = w;
	g_job.view_h = h;
	g_job.full_w = fw;
	g_job.full_h = fh;
	return true;
}

// 日時からかぶらないファイル名を作る
static void new_name(char *out, size_t n, const char *suffix)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char base[16], path[128];
	strftime(base, sizeof(base), "%Y%m%d_%H%M%S", tm);
	snprintf(out, n, "%.15s%.20s", base, suffix);
	struct stat st;
	for (int i = 2; i < 100; i++) {
		if (!find_image(out, path, sizeof(path))) break;
		snprintf(out, n, "%.15s%.20s_%d", base, suffix, i);
	}
	(void)st;
}

// 受け取った画像（PNG / WebP）をそのまま保存して表示用にする。settings が NULL でなければ設定の控えも書く
static bool save_result(const uint8_t *img, size_t img_len, const char *suffix, const Config *settings, bool show)
{
	char name[48], path[128];
	new_name(name, sizeof(name), suffix);
	img_path(path, sizeof(path), name, img_ext(img_detect(img, img_len)));
	bool saved = write_file(path, img, img_len);
	if (settings) {
		img_path(path, sizeof(path), name, ".txt");
		config_save(settings, path, false);
	}
	if (saved) g_job.saved_count++;
	if (!show) {
		// 表示しない分（背景除去の2枚目など）もサムネイルとキャッシュは作っておく
		int vw, vh, fw, fh;
		free(decode_view(img, img_len, name, true, &vw, &vh, &fw, &fh));
		g_job.error[0] = 0;
		return saved;
	}
	str_copy(g_job.name, sizeof(g_job.name), name);
	g_job.phase = PH_DECODE;
	if (!decode_to_view(img, img_len, name, true)) return false;
	if (!saved) snprintf(g_job.error, sizeof(g_job.error), L("画像は表示できますが、SDカードに保存できませんでした", "Shown, but could not save to the SD card"));
	return true;
}

// ---------------------------------------------------------------- 各ジョブ

static void parse_account(const char *body)
{
	double fixed = 0, purchased = 0, tier = 0;
	bool active = false;
	if (!json_find_num(body, "fixedTrainingStepsLeft", &fixed)) return;
	json_find_num(body, "purchasedTrainingSteps", &purchased);
	json_find_num(body, "tier", &tier);
	json_find_bool(body, "active", &active);
	g_job.anlas = (int)(fixed + purchased);
	g_job.tier = (int)tier;
	g_job.sub_active = active;
	g_job.account_valid = true;
	// "usage":{"percent":20,"purchasedPercent":0,"isNegative":false,"timeUntilNextPercent":6048}
	const char *u = strstr(body, "\"usage\"");
	double pct, pur = 0, secs = 0;
	if (u && json_find_num(u, "percent", &pct)) {
		bool neg = false;
		json_find_num(u, "purchasedPercent", &pur);
		json_find_bool(u, "isNegative", &neg);
		json_find_num(u, "timeUntilNextPercent", &secs);
		g_job.usage_valid = true;
		g_job.usage_percent = (int)pct;
		g_job.usage_purchased = (int)pur;
		g_job.usage_negative = neg;
		g_job.usage_secs = (int)secs;
	}
}

static bool do_account(char *err, size_t errsz)
{
	static const char *const urls[] = {IMAGE_BASE "/user/subscription", API_BASE "/user/subscription"};
	for (int i = 0; i < 2; i++) {
		Mem resp = {0};
		long code;
		bool ok = http_do(s_cfg, urls[i], NULL, NULL, 0, &resp, &code, false, err, errsz);
		if (ok && code == 200 && resp.p) {
			parse_account((const char *)resp.p);
			free(resp.p);
			if (g_job.account_valid) return true;
			snprintf(err, errsz, L("アカウント情報を読み取れません", "Cannot read account info"));
			continue;
		}
		if (ok) http_error_message(code, &resp, err, errsz);
		free(resp.p);
		if (!ok || code == 401) return false;
	}
	return false;
}

// 生成系の共通部分: 送信 → ZIPから画像を取り出す → 保存・表示 → 残高更新
static bool post_and_save(const char *url, const char *json, const Part *parts, int nparts, const Config *settings,
			  const char *suffix, bool all_images)
{
	g_job.phase = PH_CONNECT;
	Mem resp = {0};
	long code;
	bool ok = http_do(s_cfg, url, json, parts, nparts, &resp, &code, true, g_job.error, sizeof(g_job.error));
	if (!ok) {
		free(resp.p);
		return false;
	}
	if (code != 200 && code != 201) {
		http_error_message(code, &resp, g_job.error, sizeof(g_job.error));
		free(resp.p);
		return false;
	}

	g_job.phase = PH_SAVE;
	bool shown = false;
	if (!all_images) {
		uint8_t *img;
		size_t img_len;
		bool got = zip_extract_nth(resp.p, resp.len, 0, &img, &img_len);
		free(resp.p); // 展開の前に ZIP を手放してメモリを空ける
		resp.p = NULL;
		if (got) {
			bool r = save_result(img, img_len, suffix, settings, true);
			free(img);
			if (!r) return false;
			shown = true;
		}
	}
	for (int i = 0; all_images && i < 8; i++) {
		uint8_t *png;
		size_t png_len;
		if (!zip_extract_nth(resp.p, resp.len, i, &png, &png_len)) break;
		char sfx[24];
		snprintf(sfx, sizeof(sfx), i ? "%s_%d" : "%s", suffix, i + 1);
		bool r = save_result(png, png_len, sfx, settings, !shown);
		free(png);
		if (!shown) {
			if (!r) {
				free(resp.p);
				return false;
			}
			shown = true;
		}
		if (!all_images) break;
	}
	free(resp.p);
	if (!shown) {
		snprintf(g_job.error, sizeof(g_job.error), L("受け取ったデータから画像を取り出せません", "No image in the response"));
		return false;
	}
	// 残りAnlasを更新（失敗しても結果には影響させない）
	char tmp[128];
	do_account(tmp, sizeof(tmp));
	return true;
}

static bool do_generate(void)
{
	StrBuf req;
	sb_init(&req);
	if (!request_build(s_cfg, s_cfg->seed, &req)) {
		sb_free(&req);
		snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
		return false;
	}
	Config *settings = malloc(sizeof(Config));
	if (settings) {
		memcpy(settings, s_cfg, sizeof(Config));
		settings->seed_random = false; // 控えには実際に使ったシードを残す
	}
	bool ok = post_and_save(IMAGE_BASE "/ai/generate-image", req.p, NULL, 0, settings, "", false);
	free(settings);
	sb_free(&req);
	return ok;
}

// 送る元画像を用意する。サイズが64の倍数でない・RGBでない・WebP の画像は PNG に作り直す
static uint8_t *prepare_source(const char *name, bool fit64, size_t *out_len, int *out_w, int *out_h)
{
	char path[128];
	size_t n;
	uint8_t *src = find_image(name, path, sizeof(path)) ? read_file(path, &n) : NULL;
	if (!src) {
		snprintf(g_job.error, sizeof(g_job.error), L("元の画像を読み込めません", "Cannot read the source image"));
		return NULL;
	}
	int w, h, ct = 0;
	bool alpha;
	if (!img_info(src, n, &w, &h, &alpha)) {
		free(src);
		snprintf(g_job.error, sizeof(g_job.error), L("この画像は使えません", "This image is not supported"));
		return NULL;
	}
	int tw = w, th = h;
	if (fit64) img_fit64(w, h, &tw, &th);
	bool is_png = img_detect(src, n) == IMG_PNG && png_info(src, n, &w, &h, &ct);
	if (is_png && (!fit64 || (tw == w && th == h && ct == 2 && src[24] == 8))) {
		*out_len = n;
		*out_w = w;
		*out_h = h;
		return src;
	}
	g_job.phase = PH_PREPARE;
	int dw, dh;
	uint8_t *rgb = img_decode_rgb(src, n, false, &dw, &dh); // 透明部分は白にする（WebUIと同じ）
	free(src);
	if (!rgb) {
		snprintf(g_job.error, sizeof(g_job.error), L("画像を展開できません: %s", "Cannot decode image: %s"), img_error());
		return NULL;
	}
	uint8_t *sized = tw != dw || th != dh ? img_resize_to(rgb, dw, dh, tw, th) : rgb;
	if (sized != rgb) free(rgb);
	uint8_t *enc = NULL;
	size_t enc_len = 0;
	bool ok = sized && png_encode(sized, tw, th, 3, &enc, &enc_len);
	free(sized);
	if (!ok) {
		snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
		return NULL;
	}
	*out_len = enc_len;
	*out_w = tw;
	*out_h = th;
	return enc;
}

// 8x8単位のマスクを、送るサイズのRGBA PNG（白=描き直す）にする
static uint8_t *build_mask_png(int w, int h, size_t *out_len)
{
	uint8_t *px = malloc((size_t)w * h * 4);
	if (!px) return NULL;
	for (int y = 0; y < h; y++) {
		int gy = y / 8 < s_mask_h ? y / 8 : s_mask_h - 1;
		for (int x = 0; x < w; x++) {
			int gx = x / 8 < s_mask_w ? x / 8 : s_mask_w - 1;
			uint8_t v = s_mask[gy * s_mask_w + gx] ? 255 : 0;
			uint8_t *d = px + ((size_t)y * w + x) * 4;
			d[0] = d[1] = d[2] = v;
			d[3] = 255;
		}
	}
	uint8_t *png = NULL;
	bool ok = png_encode(px, w, h, 4, &png, out_len);
	free(px);
	return ok ? png : NULL;
}

static bool do_edit(void)
{
	g_job.phase = PH_PREPARE;
	size_t img_len, mask_len = 0;
	int w, h;
	uint8_t *img = prepare_source(s_arg, true, &img_len, &w, &h);
	if (!img) return false;
	uint8_t *mask = NULL;
	if (s_action == ACT_INFILL) {
		mask = build_mask_png(w, h, &mask_len);
		if (!mask) {
			free(img);
			snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
			return false;
		}
	}
	char ikey[65], mkey[65];
	bool keys = cache_key(img, img_len, ikey) && (!mask || cache_key(mask, mask_len, mkey));
	ReqExtra x = {s_action, w, h, rand32() % 1000000000u, keys ? ikey : NULL, keys && mask ? mkey : NULL};
	StrBuf req;
	sb_init(&req);
	bool built = request_build_ex(s_cfg, s_cfg->seed, &x, &req);
	bool ok = false;
	if (!built) {
		snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
	} else {
		Part parts[2] = {{"image", img, img_len}, {"mask", mask, mask_len}};
		Config *settings = malloc(sizeof(Config));
		if (settings) {
			memcpy(settings, s_cfg, sizeof(Config));
			settings->seed_random = false;
		}
		ok = post_and_save(IMAGE_BASE "/ai/generate-image", req.p, parts, mask ? 2 : 1, settings,
				   s_action == ACT_INFILL ? "_inpaint" : "_i2i", false);
		free(settings);
	}
	sb_free(&req);
	free(img);
	free(mask);
	return ok;
}

static bool do_director(void)
{
	g_job.phase = PH_PREPARE;
	size_t img_len;
	int w, h;
	uint8_t *img = prepare_source(s_arg, false, &img_len, &w, &h);
	if (!img) return false;
	StrBuf req;
	sb_init(&req);
	bool ok = false;
	if (!director_build(s_cfg, w, h, &req)) {
		snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
	} else {
		// ファイル名の後ろに付ける印（一覧の名前は40文字までなので短く）
		static const char *const sfx[DIR_COUNT] = {"_line", "_sketch", "_bg", "_clean", "_clean2", "_color", "_emo"};
		Part part = {"image", img, img_len};
		ok = post_and_save(IMAGE_BASE "/ai/augment-image", req.p, &part, 1, NULL, sfx[s_cfg->dir_tool], true);
	}
	sb_free(&req);
	free(img);
	return ok;
}

static bool do_loadimg(void)
{
	char path[128];
	g_job.phase = PH_DECODE;
	str_copy(g_job.name, sizeof(g_job.name), s_arg);
	if (load_view_cache(s_arg)) return true;
	size_t n;
	uint8_t *buf = find_image(s_arg, path, sizeof(path)) ? read_file(path, &n) : NULL;
	if (!buf) {
		snprintf(g_job.error, sizeof(g_job.error), L("ファイルを読み込めません", "Cannot read the file"));
		return false;
	}
	// サムネイルが無い画像（PCから入れたものなど）はここで作る
	img_path(path, sizeof(path), s_arg, ".thm");
	struct stat st;
	bool dec = decode_to_view(buf, n, s_arg, stat(path, &st) != 0);
	free(buf);
	return dec;
}

static bool do_thumb(void)
{
	char path[128];
	size_t n;
	uint8_t *buf = find_image(s_arg, path, sizeof(path)) ? read_file(path, &n) : NULL;
	if (!buf) return false;
	int vw, vh, fw, fh;
	uint16_t *px = decode_view(buf, n, s_arg, true, &vw, &vh, &fw, &fh);
	free(buf);
	if (!px) return false;
	free(px);
	str_copy(g_job.name, sizeof(g_job.name), s_arg);
	g_job.error[0] = 0;
	return true;
}

static bool do_suggest(void)
{
	bool jp = str_has_nonascii(s_arg); // 日本語で入力されたら日本語タグ検索
	StrBuf url;
	sb_init(&url);
	sb_put(&url, IMAGE_BASE "/ai/generate-image/suggest-tags?model=");
	sb_urlenc(&url, g_models[s_cfg->model].id);
	sb_put(&url, "&prompt=");
	sb_urlenc(&url, s_arg);
	if (jp) sb_put(&url, "&lang=jp");
	if (url.oom) {
		sb_free(&url);
		snprintf(g_job.error, sizeof(g_job.error), L("メモリが足りません", "Out of memory"));
		return false;
	}
	Mem resp = {0};
	long code;
	bool ok = http_do(s_cfg, url.p, NULL, NULL, 0, &resp, &code, false, g_job.error, sizeof(g_job.error));
	sb_free(&url);
	if (!ok) {
		free(resp.p);
		return false;
	}
	if (code != 200 || !resp.p) {
		http_error_message(code, &resp, g_job.error, sizeof(g_job.error));
		free(resp.p);
		return false;
	}
	const char *p = (const char *)resp.p;
	if (!jp) {
		const char *arr = strstr(p, "\"tags\"");
		if (arr) p = arr + 6;
	}
	g_job.sug_count = 0;
	while (g_job.sug_count < MAX_SUGGEST) {
		Suggestion *s = &g_job.sug[g_job.sug_count];
		memset(s, 0, sizeof(*s));
		double num = 0;
		const char *next;
		if (jp) {
			// [{"jp_tag":"..","en_tag":"..","power":N}, ...]
			next = json_find_str(p, "jp_tag", s->label, sizeof(s->label));
			if (!next) break;
			const char *e = json_find_str(p, "en_tag", s->tag, sizeof(s->tag));
			if (!e) break;
			if (e > next) next = e;
			json_find_num(p, "power", &num);
		} else {
			// {"tags":[{"tag":"..","count":N,"confidence":F}, ...]}
			next = json_find_str(p, "tag", s->tag, sizeof(s->tag));
			if (!next) break;
			json_find_num(next, "count", &num);
		}
		s->count = (int)num;
		if (s->tag[0]) g_job.sug_count++;
		p = next;
	}
	free(resp.p);
	return true;
}

static void worker(void *arg)
{
	(void)arg;
	bool ok = false;
	switch (g_job.type) {
	case JOB_GENERATE: ok = do_generate(); break;
	case JOB_EDIT: ok = do_edit(); break;
	case JOB_DIRECTOR: ok = do_director(); break;
	case JOB_LOADIMG: ok = do_loadimg(); break;
	case JOB_THUMB: ok = do_thumb(); break;
	case JOB_SUGGEST: ok = do_suggest(); break;
	case JOB_ACCOUNT: ok = do_account(g_job.error, sizeof(g_job.error)); break;
	default: break;
	}
	g_job.ok = ok;
	g_job.done = true;
}

// ---------------------------------------------------------------- 開始と後片付け

static bool start(JobType type, const Config *c, const char *arg, bool need_net)
{
	if (g_job.running) return false;
	memset(&g_job, 0, sizeof(g_job));
	g_job.type = type;
	if (need_net && !s_net_ok) {
		snprintf(g_job.error, sizeof(g_job.error), L("通信機能を初期化できませんでした", "Network is not available"));
		g_job.running = g_job.done = true;
		return true;
	}
	if (need_net && !c->api_key[0]) {
		snprintf(g_job.error, sizeof(g_job.error), L("APIキーが未設定です。設定タブか config.ini で入力してください", "No API key. Set it in Settings or config.ini"));
		g_job.running = g_job.done = true;
		return true;
	}
	free(s_cfg);
	s_cfg = NULL;
	if (c) {
		s_cfg = malloc(sizeof(Config));
		if (!s_cfg) return false;
		memcpy(s_cfg, c, sizeof(Config));
	}
	str_copy(s_arg, sizeof(s_arg), arg ? arg : "");

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (prio < 0x3F) prio++; // 画面描画より少し低い優先度で動かす
	g_job.running = true;
	g_job.phase = PH_CONNECT;
	s_thread = threadCreate(worker, NULL, WORKER_STACK, prio, -1, false);
	if (!s_thread) {
		snprintf(g_job.error, sizeof(g_job.error), L("スレッドを作れません（メモリ不足）", "Cannot create a thread (out of memory)"));
		g_job.done = true;
	}
	return true;
}

// ランダムシードならここで決める（控えに残すため）
static Config *with_seed(const Config *c)
{
	Config *tmp = malloc(sizeof(Config));
	if (!tmp) return NULL;
	memcpy(tmp, c, sizeof(Config));
	if (tmp->seed_random) tmp->seed = rand32() % 4294967295u;
	return tmp;
}

bool nai_start_generate(const Config *c)
{
	Config *tmp = with_seed(c);
	if (!tmp) return false;
	bool r = start(JOB_GENERATE, tmp, NULL, true);
	if (r) g_job.seed = tmp->seed;
	free(tmp);
	return r;
}

bool nai_start_edit(const Config *c, int action, const char *src, const uint8_t *mask, int gw, int gh)
{
	if (g_job.running) return false;
	free(s_mask);
	s_mask = NULL;
	if (action == ACT_INFILL) {
		if (!mask || gw <= 0 || gh <= 0) return false;
		s_mask = malloc((size_t)gw * gh);
		if (!s_mask) return false;
		memcpy(s_mask, mask, (size_t)gw * gh);
		s_mask_w = gw;
		s_mask_h = gh;
	}
	s_action = action;
	Config *tmp = with_seed(c);
	if (!tmp) return false;
	bool r = start(JOB_EDIT, tmp, src, true);
	if (r) g_job.seed = tmp->seed;
	free(tmp);
	return r;
}

bool nai_start_director(const Config *c, const char *src) { return start(JOB_DIRECTOR, c, src, true); }
bool nai_start_loadimg(const char *name) { return start(JOB_LOADIMG, NULL, name, false); }
bool nai_start_thumb(const char *name) { return start(JOB_THUMB, NULL, name, false); }
bool nai_start_suggest(const Config *c, const char *query) { return start(JOB_SUGGEST, c, query, true); }
bool nai_start_account(const Config *c) { return start(JOB_ACCOUNT, c, NULL, true); }

void nai_finish(void)
{
	if (s_thread) {
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
	}
	free(s_cfg);
	s_cfg = NULL;
	free(s_mask);
	s_mask = NULL;
	g_job.running = false;
	g_job.done = false;
	g_job.type = JOB_NONE;
	g_job.phase = PH_IDLE;
}
