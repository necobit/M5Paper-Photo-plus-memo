#pragma once
#include <Arduino.h>

namespace web {

// Wi-Fi は電力を食うので必要な時だけ付ける
enum class State { Off, Connecting, Ready };

void begin();  // HTTP のルート登録のみ（Wi-Fi は付けない）
void start();  // Wi-Fi を付ける。家の Wi-Fi に繋がらなければアクセスポイントになる（待たずに戻る）
void stop();
void loop();
State state();
bool isAccessPoint();
String ssid();
String apPassword();
String url();  // http://<IP>/

}  // namespace web
