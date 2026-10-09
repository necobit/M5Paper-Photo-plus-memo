#pragma once
// 本体設定（/settings.txt に key=value で保存）
#include <Arduino.h>

struct Settings {
  uint32_t sleepSec = 30;      // 最後の操作からライトスリープまで（タッチで即復帰）
  uint32_t powerOffSec = 600; // 最後の操作から電源オフまで（0 = 切らない。ボタンで起動）
  uint8_t tool = 0;            // 電源オフ前のツール / 太さを起動時に戻す
  uint8_t widthIdx = 1;
};

namespace settings {

Settings& get();
void load();
void save();

}  // namespace settings
