// 電池の 0% 電圧を測るための消費加速テスト
//   - Wi-Fi スキャン連続 + BLE アドバタイズ / スキャン + CPU 全開で電池を早く減らす
//   - 電池駆動中は 30 秒ごとに電圧を /drain.csv に追記（電源が落ちる直前の値 = 0% の電圧）
//   - 起動時に前回の記録を /drain_N.csv（N は空いている番号）に移し、要約を画面とシリアルに出す。過去の記録は消さない
//     （空になった電池を USB で充電し始めると電圧が給電判定より低く、記録が始まってしまうため。上書きすると本番の記録が消える）
//   - USB 給電中は記録しない
//   - 基板の温湿度センサー(SHT30)で筐体内温度を監視し、50℃で負荷を止め 45℃で再開する（電池の保護）
#include <BLEDevice.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>

namespace {

constexpr int kUsbPowerMv = 4100;  // 本体アプリと同じ判定
constexpr uint32_t kLogIntervalMs = 30000;
constexpr uint32_t kDisplayIntervalMs = 60000;
const char* kLogFile = "/drain.csv";
constexpr uint32_t kBleRestartMs = 60000;  // BLE スキャン結果はメモリに溜まり続けるので定期的に捨てる

bool fsOk = false;
bool logging = false;
uint32_t logStartMs = 0;
uint32_t nextLogMs = 0;
uint32_t nextDisplayMs = 0;
uint32_t nextBleRestartMs = 0;
String prevFile;
int startMv = 0;
int lastLoggedMv = 0;
volatile uint32_t burnCount = 0;
volatile bool paused = false;  // 温度が高い間は負荷を止める
constexpr float kPauseTempC = 50.0f, kResumeTempC = 45.0f;
float lastTempC = NAN;

struct Summary {
  bool valid = false;
  int firstMv = 0, lastMv = 0, minMv = 0;
  uint32_t lastSec = 0;
  int samples = 0;
} prev;

int readBatteryMv() {
  int sum = 0;
  for (int i = 0; i < 16; ++i) sum += M5.Power.getBatteryVoltage();
  return sum / 16;
}

// 基板上の SHT30（I2C 0x44）で温度を読む。失敗時は NAN
float readTempC() {
  static constexpr uint8_t kAddr = 0x44;
  static constexpr uint32_t kFreq = 100000;
  const uint8_t cmd[2] = {0x24, 0x00};  // 単発測定・高精度・クロックストレッチ無し
  if (!M5.In_I2C.start(kAddr, false, kFreq) || !M5.In_I2C.write(cmd, 2) || !M5.In_I2C.stop()) return NAN;
  delay(20);
  uint8_t buf[6];
  if (!M5.In_I2C.start(kAddr, true, kFreq) || !M5.In_I2C.read(buf, 6, true) || !M5.In_I2C.stop()) return NAN;
  return -45.0f + 175.0f * ((buf[0] << 8) | buf[1]) / 65535.0f;
}

// 前回の記録を空いている番号のファイルに移して要約を作る（記録が無ければ最新の退避分を読む）
void rotatePreviousLog() {
  if (!fsOk) return;
  int n = 1;
  while (LittleFS.exists("/drain_" + String(n) + ".csv")) ++n;
  if (LittleFS.exists(kLogFile)) {
    prevFile = "/drain_" + String(n) + ".csv";
    LittleFS.rename(kLogFile, prevFile);
  } else if (n > 1) {
    prevFile = "/drain_" + String(n - 1) + ".csv";
  } else {
    return;
  }
  File f = LittleFS.open(prevFile, FILE_READ);
  if (!f) return;
  Serial.printf("---- previous drain log %s (sec,mV,degC) ----\n", prevFile.c_str());
  while (f.available()) {
    String line = f.readStringUntil('\n');
    Serial.println(line);
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    uint32_t sec = line.substring(0, comma).toInt();
    int mv = line.substring(comma + 1).toInt();  // toInt() は次のカンマで止まる
    if (!prev.valid) {
      prev.valid = true;
      prev.firstMv = prev.minMv = mv;
    }
    prev.lastSec = sec;
    prev.lastMv = mv;
    prev.minMv = std::min(prev.minMv, mv);
    ++prev.samples;
  }
  f.close();
  Serial.println("---- end ----");
}

void appendLog(int mv) {
  if (!fsOk) return;
  File f = LittleFS.open(kLogFile, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%d,%.1f\n", (unsigned long)((millis() - logStartMs) / 1000), mv, lastTempC);
  f.close();  // 毎回閉じて、突然電源が落ちても直前までを残す
}

// ---- 電力を使うもの ----

void burnTask(void*) {
  volatile float x = 1.0f;
  for (;;) {
    if (paused) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    for (int i = 0; i < 20000; ++i) x = x * 1.000001f + 0.5f;
    ++burnCount;
    vTaskDelay(1);  // コア 0 のアイドルタスクを止めるとウォッチドッグで再起動するため
  }
}

void startBurners() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.scanNetworks(true);

  BLEDevice::init("M5Paper-drain");
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEDevice::getAdvertising()->start();
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, nullptr, false);  // 時間無制限・非同期

  xTaskCreatePinnedToCore(burnTask, "burn", 2048, nullptr, 1, nullptr, 0);
}

void setPaused(bool p) {
  if (p == paused) return;
  paused = p;
  Serial.printf("%s at %.1f C\n", p ? "PAUSE" : "RESUME", lastTempC);
  if (p) {
    BLEDevice::getScan()->stop();
    BLEDevice::getAdvertising()->stop();
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.scanNetworks(true);
    BLEDevice::getAdvertising()->start();
    BLEDevice::getScan()->start(0, nullptr, false);
  }
}

void keepWifiScanning() {
  if (paused) return;
  int n = WiFi.scanComplete();
  if (n >= 0 || n == WIFI_SCAN_FAILED) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
}

// ---- 表示 ----

String hms(uint32_t sec) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60),
           (unsigned long)(sec % 60));
  return buf;
}

String volts(int mv) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d.%02dV", mv / 1000, mv % 1000 / 10);
  return buf;
}

void drawStatus(int mv) {
  auto& d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_quality);
  d.startWrite();
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(textdatum_t::top_left);

  d.setFont(&fonts::efontJA_24);
  d.drawString("電池 消費テスト（0% 電圧の計測）", 30, 24);

  d.setFont(&fonts::efontJA_24);
  d.setTextSize(3);
  d.drawString(volts(mv), 30, 80);
  d.setTextSize(1);

  int y = 180;
  auto line = [&](const String& s) {
    d.drawString(s, 30, y);
    y += 36;
  };
  if (mv >= kUsbPowerMv) {
    line("USB 給電中: 記録していません。抜くと記録を始めます");
  } else if (logging) {
    line("記録中  経過 " + hms((millis() - logStartMs) / 1000) + "  開始 " + volts(startMv));
    line("最後に記録した電圧 " + volts(lastLoggedMv) + "（30 秒ごと）");
  }
  String temp = isnan(lastTempC) ? String("温度 不明") : "筐体内 " + String(lastTempC, 1) + "℃";
  if (paused) line(temp + "  高温のため負荷を止めて冷却中（" + String(kResumeTempC, 0) + "℃で再開）");
  else line(temp + "  Wi-Fi スキャン連続 / BLE 送受信 / CPU 全開で消費中");

  y = 360;
  d.drawFastHLine(30, y - 12, 900, TFT_DARKGREY);
  if (prev.valid) {
    line(prevFile.substring(1) + ": " + volts(prev.firstMv) + " → 最後 " + volts(prev.lastMv) + "（最低 " + volts(prev.minMv) +
         "）");
    line("持続 " + hms(prev.lastSec) + "  記録 " + String(prev.samples) + " 点");
    line("最後の電圧 ≒ 動かなくなる電圧（0%）");
  } else {
    line("前回の記録はありません");
  }
  d.endWrite();
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation(M5.Display.getRotation() ^ 1);

  fsOk = LittleFS.begin(false);  // フォーマットはしない（本体アプリの写真を守る）
  Serial.printf("littlefs %s\n", fsOk ? "ok" : "NG");
  rotatePreviousLog();

  startBurners();
  lastTempC = readTempC();
  int mv = readBatteryMv();
  drawStatus(mv);
  nextDisplayMs = millis() + kDisplayIntervalMs;
  nextLogMs = millis();
}

void loop() {
  M5.update();
  keepWifiScanning();

  uint32_t now = millis();
  if ((int32_t)(now - nextLogMs) >= 0) {
    nextLogMs = now + kLogIntervalMs;
    lastTempC = readTempC();
    if (!isnan(lastTempC)) {
      if (lastTempC >= kPauseTempC) setPaused(true);
      else if (lastTempC <= kResumeTempC) setPaused(false);
    }
    int mv = readBatteryMv();
    if (mv < kUsbPowerMv) {
      if (!logging) {
        logging = true;
        logStartMs = now;
        startMv = mv;
      }
      appendLog(mv);
      lastLoggedMv = mv;
    }
    Serial.printf("t=%lus %d mV %.1fC %s%s burn=%lu\n", (unsigned long)(now / 1000), mv, lastTempC,
                  logging ? "logging" : "usb", paused ? " paused" : "", (unsigned long)burnCount);
  }
  if ((int32_t)(now - nextDisplayMs) >= 0) {
    nextDisplayMs = now + kDisplayIntervalMs;
    drawStatus(readBatteryMv());
  }

  if (!paused && (int32_t)(now - nextBleRestartMs) >= 0) {
    nextBleRestartMs = now + kBleRestartMs;
    BLEScan* scan = BLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->start(0, nullptr, false);
  }

  // コア 1 も回し続ける
  if (paused) {
    delay(100);
    return;
  }
  volatile float x = 1.0f;
  for (int i = 0; i < 20000; ++i) x = x * 1.000001f + 0.5f;
}
