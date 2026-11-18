// 設定とモデル定義（3DS非依存）
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "util.h"

#define APP_NAME "Thalia"
#define APP_VERSION "1.0.0"
#define APP_DIR "sdmc:/3ds/thalia"
#define IMG_DIR APP_DIR "/images"
#define CONFIG_PATH APP_DIR "/config.ini"
#define TAGS_PATH APP_DIR "/tags.txt"

#define MAX_PROMPT 2048

// 表示の言語。設定は自動/日本語/English、g_lang は実際に使う言語（0:日本語 1:英語）
enum { LANG_AUTO, LANG_JA, LANG_EN, LANG_COUNT };
extern int g_lang;
#define L(ja, en) (g_lang ? (en) : (ja))
#define MAX_CHARS 6

enum { UC_HEAVY, UC_LIGHT, UC_HUMAN, UC_FURRY, UC_NONE, UC_COUNT };

typedef struct {
	const char *id;      // APIに送るモデル名
	const char *inpaint; // インペイント用のモデル名
	const char *label;   // 画面表示用
	int gen;             // 3 / 4 / 5（4.5は4扱い）
	bool sfw;            // Curated系（除外にnsfwを足さない）
	bool medium;         // V5 Full Medium（設定が固定される）
	const char *quality; // 品質タグ
	const char *uc[UC_COUNT]; // 除外プリセット。NULLはそのモデルで使えない
} ModelInfo;

extern const ModelInfo g_models[];
extern const int g_model_count;
extern const char *const g_samplers[];
extern const char *const g_sampler_labels[];
extern const int g_sampler_count;
extern const char *const g_noises[];
extern const int g_noise_count;
extern const char *const g_uc_labels[UC_COUNT];

typedef struct {
	const char *label[2];
	int w, h;
} SizePreset;
extern const SizePreset g_sizes[];
extern const int g_size_count;

typedef struct {
	char prompt[MAX_PROMPT];
	char uc[MAX_PROMPT];
	bool enabled;
	int gx, gy; // 5x5グリッド上の位置。-1ならAI任せ
} CharSlot;

// ディレクターツール
enum { DIR_LINEART, DIR_SKETCH, DIR_BGREMOVE, DIR_DECLUTTER, DIR_DECLUTTER_BUBBLES, DIR_COLORIZE, DIR_EMOTION, DIR_COUNT };
typedef struct {
	const char *req_type; // /ai/augment-image の req_type
	const char *label[2]; // [言語]
	const char *desc[2];
	bool takes_prompt;    // prompt と defry を送るもの（色付け・表情）
} DirTool;
extern const DirTool g_dir_tools[DIR_COUNT];
#define EMOTION_COUNT 24
extern const char *const g_emotions[EMOTION_COUNT];
extern const char *const g_emotion_labels[2][EMOTION_COUNT];
extern const char *const g_defry_labels[2][6];

typedef struct {
	char api_key[256];
	char prompt[MAX_PROMPT];
	char uc[MAX_PROMPT];
	int model;
	int width, height;
	int steps;
	float scale;
	int sampler;
	int noise;
	float cfg_rescale;
	bool variety;
	bool quality;
	int uc_preset;
	bool seed_random;
	uint32_t seed;
	bool verify_tls;
	CharSlot chars[MAX_CHARS];

	// 編集（img2img・インペイント・ディレクターツール）
	float i2i_strength;     // 0.01〜0.99
	float i2i_noise;        // 0〜0.99
	float inpaint_strength; // 0.01〜1
	int dir_tool;
	int dir_emotion;
	int dir_defry;          // 0（強い）〜5（弱い）
	char dir_prompt[512];

	int lang;       // LANG_AUTO / LANG_JA / LANG_EN
	bool recv_webp; // 生成した画像を WebP（可逆）で受け取る。PNG より通信量が約3割少ない
} Config;

void config_defaults(Config *c);
void config_sanitize(Config *c);
bool config_load(Config *c, const char *path); // 無い項目は現在値のまま
bool config_save(const Config *c, const char *path, bool with_key);

int config_size_preset(const Config *c);          // 一致するプリセット番号。無ければ-1
int config_next_uc_preset(const Config *c, int dir); // そのモデルで使える次のプリセット

// 生成リクエストのJSONを組み立てる
enum { ACT_GENERATE, ACT_IMG2IMG, ACT_INFILL };
typedef struct {
	int action;
	int width, height;        // 元画像のサイズ（img2img / infill）
	uint32_t extra_noise_seed;
	const char *image_key;    // image_cache_secret_key（64桁の16進）。NULLなら付けない
	const char *mask_key;     // mask_cache_secret_key
} ReqExtra;
bool request_build(const Config *c, uint32_t seed, StrBuf *out);
bool request_build_ex(const Config *c, uint32_t seed, const ReqExtra *x, StrBuf *out);
// ディレクターツールのリクエスト
bool director_build(const Config *c, int w, int h, StrBuf *out);

// Anlasの見積もり（非公式SDKの計算式。目安）。is_opus は「Opusで有効、かつ
// V5を選んでいるならV5の残量が残っている」とき true。生成できない大きさなら -1
int anlas_generate(const Config *c, int action, int w, int h, bool is_opus);
int anlas_director(int tool, int w, int h, bool is_opus);
