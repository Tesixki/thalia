// 通信とバックグラウンド処理（生成・編集・画像読み込み・タグ候補・アカウント情報）
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define MAX_SUGGEST 32
#define TAG_LEN 96

typedef enum { JOB_NONE, JOB_GENERATE, JOB_EDIT, JOB_DIRECTOR, JOB_LOADIMG, JOB_THUMB, JOB_SUGGEST, JOB_ACCOUNT } JobType;
typedef enum { PH_IDLE, PH_PREPARE, PH_CONNECT, PH_UPLOAD, PH_WAIT, PH_DOWNLOAD, PH_DECODE, PH_SAVE } JobPhase;

typedef struct {
	char tag[TAG_LEN];   // プロンプトに入れる英語タグ
	char label[TAG_LEN]; // 日本語検索のときの日本語名（無ければ空）
	int count;
} Suggestion;

typedef struct {
	volatile JobType type;
	volatile bool running;
	volatile bool done; // ワーカーが最後に立てる。メイン側が結果を回収したら下ろす
	volatile bool cancel;
	volatile JobPhase phase;
	volatile float progress; // 送受信の進み具合 0..1
	bool ok;
	long http_code; // 最後に受け取ったHTTPステータス（通信できなかったら0）
	char error[256];

	// 画像を返すジョブの結果: 表示用に縮小したRGB565画像（メイン側でfree）
	uint16_t *view_px;
	int view_w, view_h;
	int full_w, full_h;
	uint32_t seed;
	char name[48];   // 保存したファイル名（拡張子なし）。複数保存したときは1枚目
	int saved_count; // 保存した枚数

	// JOB_SUGGEST の結果
	Suggestion sug[MAX_SUGGEST];
	int sug_count;

	// JOB_ACCOUNT の結果（生成の後にも更新される）
	bool account_valid;
	int anlas;
	int tier;
	bool sub_active;
	// V5の残量（Opus。/user/subscription の usage）。残り枚数は返らず、パーセントだけ
	bool usage_valid;
	int usage_percent;   // 残り（100を超えることもある）
	int usage_purchased; // 追加購入分
	bool usage_negative; // 使い切った（V5もAnlasがかかる）
	int usage_secs;      // 1%回復するまでの秒数
} Job;

extern Job g_job;

bool nai_init(void); // 通信の初期化。失敗時はfalse（オフラインでもギャラリーは使える）
void nai_exit(void);

// どれも、すでに処理中ならfalseを返す
bool nai_start_generate(const Config *c);
// img2img（action=ACT_IMG2IMG）かインペイント（ACT_INFILL）。src は一覧の名前（拡張子なし）。
// mask は 8x8 ピクセル単位のマスク（gw×gh、0以外=描き直す）。gw,gh は送るサイズの1/8
bool nai_start_edit(const Config *c, int action, const char *src, const uint8_t *mask, int gw, int gh);
bool nai_start_director(const Config *c, const char *src);
bool nai_start_loadimg(const char *name);
bool nai_start_thumb(const char *name); // 一覧用のサムネイルだけ作る（画面は止めない）
bool nai_start_suggest(const Config *c, const char *query);
bool nai_start_account(const Config *c);
void nai_finish(void); // 結果を回収し終えたら呼ぶ（スレッドを片付ける）
