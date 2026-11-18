// ロゴと読み込み中の演出
#pragma once
#include "ui.h"

void fx_init(void);
void fx_exit(void);

float fx_ease_out(float t);      // 0..1 → 0..1（だんだんゆっくり）
float fx_ease_out_back(float t); // 少し行き過ぎて戻る
float fx_clamp01(float t);

// ペン先マーク（中心 cx,cy）。dark なら暗い背景用のクリーム色。
// appear: 0..1 で現れる途中、scale: 大きさ、pulse: にじみの強さ 0..1
void fx_logo(float cx, float cy, bool dark, float appear, float scale, float pulse);
void fx_spinner(float cx, float cy, float r, u32 color);             // 3DS風の回る点の輪
void fx_sparkles(float cx, float cy, float rx, float ry, int n, u32 color, u32 seed); // 瞬くきらめき
void fx_ring(float cx, float cy, float r, float thick, u32 color);  // 細い輪（波紋）
void fx_bar(float x, float y, float w, float h, float frac);         // しま模様が流れる進捗バー（frac<0 は不明）
