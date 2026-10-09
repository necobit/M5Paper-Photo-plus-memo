#pragma once
// 写真の保存（LittleFS）
//   /p/NNNN.g4   元画像（880x540, 4bit グレー, Floyd-Steinberg ディザ済み）
//   /p/NNNN.memo メモレイヤー（MemoDoc 形式）
#include <LittleFS.h>
#include <M5Unified.h>
#include <vector>

namespace store {

bool begin();
std::vector<uint16_t> list();  // 昇順の写真 ID
String basePath(uint16_t id);
String memoPath(uint16_t id);

// JPEG を画面サイズに収めてグレー化・ディザして保存。work は IMG_W x IMG_H の 8bit グレーキャンバス
uint16_t createFromJpeg(const char* jpgPath, M5Canvas& work, String& err);
bool loadBase(uint16_t id, M5Canvas& base);
bool remove(uint16_t id);

uint16_t loadCurrentId();
void saveCurrentId(uint16_t id);

}  // namespace store
