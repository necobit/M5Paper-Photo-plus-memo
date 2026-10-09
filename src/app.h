#pragma once
// main.cpp（画面/UI）が Web サーバ側に公開する操作
#include <M5Unified.h>

uint16_t appCurrentId();
void appShowPhoto(uint16_t id);      // 現在のメモを保存してから切り替え
void appPhotoDeleted(uint16_t id);
void appShowBusy(const char* msg);   // 処理中メッセージ
void appRedrawAll();
M5Canvas& appComposeCanvas();        // 現在の表示内容（元画像 + 表示中レイヤー）
