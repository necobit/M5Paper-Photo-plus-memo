#pragma once
#include <Arduino.h>

namespace web {

void begin();  // Wi-Fi 接続（失敗時はアクセスポイント）+ HTTP サーバ起動
void loop();
bool isAccessPoint();
String ssid();
String apPassword();
String url();  // http://<IP>/

}  // namespace web
