#include "web_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "app.h"
#include "memo_doc.h"
#include "photo_store.h"
#include "settings.h"
#include "web_page.h"

namespace web {

static const char* kWifiFile = "/wifi.txt";
static const char* kUploadTmp = "/upload.tmp";
static const char* kApSsid = "M5Paper-Memo";
static const char* kApPass = "m5papermemo";
static const char* kHostname = "m5paper";

static WebServer server(80);
static constexpr uint32_t kConnectTimeoutMs = 15000;
static bool apMode = false;
static State wifiState = State::Off;
static uint32_t connectStartMs = 0;
static String staSsid, staPass;
static File uploadFile;
static String uploadErr;

bool isAccessPoint() { return apMode; }
State state() { return wifiState; }
String ssid() { return apMode ? String(kApSsid) : staSsid; }
String apPassword() { return kApPass; }
String url() { return "http://" + (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString() + "/"; }

static void sendJsonError(int code, const String& msg) {
  server.send(code, "application/json", "{\"ok\":false,\"error\":\"" + msg + "\"}");
}

// ---- 8bit グレー BMP（下の行から送る） ----
static void sendBmpHeader(int w, int h) {
  uint32_t rowSize = (w + 3) & ~3;
  uint32_t offset = 14 + 40 + 1024;
  uint32_t fileSize = offset + rowSize * h;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int pos, uint32_t v) {
    for (int i = 0; i < 4; ++i) hdr[pos + i] = v >> (i * 8);
  };
  put32(2, fileSize);
  put32(10, offset);
  put32(14, 40);
  put32(18, w);
  put32(22, h);
  hdr[26] = 1;   // planes
  hdr[28] = 8;   // bpp
  put32(34, rowSize * h);
  put32(46, 256);
  server.setContentLength(fileSize);
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, sizeof(hdr));
  uint8_t pal[1024];
  for (int i = 0; i < 256; ++i) {
    pal[i * 4] = pal[i * 4 + 1] = pal[i * 4 + 2] = i;
    pal[i * 4 + 3] = 0;
  }
  server.sendContent((const char*)pal, sizeof(pal));
}

static void handleSnapshot() {
  auto& c = appComposeCanvas();
  auto* buf = (const uint8_t*)c.getBuffer();
  sendBmpHeader(IMG_W, IMG_H);
  for (int y = IMG_H - 1; y >= 0; --y) server.sendContent((const char*)buf + y * IMG_W, IMG_W);
}

// 元画像を 1/4 に縮小したサムネイル（4x4 平均）
static void handleThumb() {
  constexpr int TW = IMG_W / 4, TH = IMG_H / 4;
  File f = LittleFS.open(store::basePath(server.arg("id").toInt()), FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "not found");
    return;
  }
  sendBmpHeader(TW, TH);
  static uint8_t rows[4][IMG_W / 2];
  uint8_t out[TW];
  for (int ty = TH - 1; ty >= 0; --ty) {
    f.seek(6 + ty * 4 * (IMG_W / 2));
    f.read(&rows[0][0], sizeof(rows));
    for (int tx = 0; tx < TW; ++tx) {
      int sum = 0;
      for (int r = 0; r < 4; ++r) {
        const uint8_t* p = &rows[r][tx * 2];
        sum += (p[0] >> 4) + (p[0] & 15) + (p[1] >> 4) + (p[1] & 15);
      }
      out[tx] = sum * 17 / 16;
    }
    server.sendContent((const char*)out, TW);
  }
  f.close();
}

static void handleList() {
  String json = "{\"current\":" + String(appCurrentId()) + ",\"used\":" + String(LittleFS.usedBytes()) +
                ",\"total\":" + String(LittleFS.totalBytes()) + ",\"photos\":[";
  auto ids = store::list();
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) json += ',';
    json += ids[i];
  }
  auto& st = settings::get();
  json += "],\"sleep\":" + String(st.sleepSec) + ",\"poweroff\":" + String(st.powerOffSec) + "}";
  server.send(200, "application/json", json);
}

static void handleSettings() {
  auto& st = settings::get();
  if (server.hasArg("sleep")) st.sleepSec = std::max(10L, server.arg("sleep").toInt());
  if (server.hasArg("poweroff")) st.powerOffSec = std::max(0L, server.arg("poweroff").toInt());
  settings::save();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleUploadData() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      uploadErr = "";
      LittleFS.remove(kUploadTmp);
      uploadFile = LittleFS.open(kUploadTmp, FILE_WRITE);
      if (!uploadFile) uploadErr = "一時ファイルを作れません";
      appShowBusy("受信中…");
      break;
    case UPLOAD_FILE_WRITE:
      if (uploadFile && uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
        uploadErr = "容量が足りません。古い写真を削除してください";
        uploadFile.close();
      }
      break;
    case UPLOAD_FILE_END:
      if (uploadFile) uploadFile.close();
      break;
    case UPLOAD_FILE_ABORTED:
      if (uploadFile) uploadFile.close();
      uploadErr = "受信が中断されました";
      break;
  }
}

static void handleUploadDone() {
  if (uploadErr.length() || !LittleFS.exists(kUploadTmp)) {
    LittleFS.remove(kUploadTmp);
    appRedrawAll();
    sendJsonError(400, uploadErr.length() ? uploadErr : String("ファイルがありません"));
    return;
  }
  appShowBusy("変換中…");
  String err;
  uint16_t id = store::createFromJpeg(kUploadTmp, appComposeCanvas(), err);
  LittleFS.remove(kUploadTmp);
  if (!id) {
    appRedrawAll();
    sendJsonError(400, err);
    return;
  }
  // 先に応答してから描画（e-ink の更新に時間がかかるため）
  server.send(200, "application/json", "{\"ok\":true,\"id\":" + String(id) + "}");
  appShowPhoto(id);
}

static void handleShow() {
  uint16_t id = server.arg("id").toInt();
  if (!LittleFS.exists(store::basePath(id))) return sendJsonError(404, "写真がありません");
  server.send(200, "application/json", "{\"ok\":true}");
  appShowPhoto(id);
}

static void handleDelete() {
  uint16_t id = server.arg("id").toInt();
  if (!store::remove(id)) return sendJsonError(404, "写真がありません");
  server.send(200, "application/json", "{\"ok\":true}");
  appPhotoDeleted(id);
}

static void handleWifi() {
  String s = server.arg("ssid"), p = server.arg("pass");
  s.trim();
  if (s.length()) {
    File f = LittleFS.open(kWifiFile, FILE_WRITE);
    f.print(s + "\n" + p + "\n");
    f.close();
  } else {
    LittleFS.remove(kWifiFile);
  }
  server.send(200, "text/html; charset=utf-8",
              "<meta name=viewport content='width=device-width'><p style='font-family:sans-serif'>"
              "保存しました。M5Paper を再起動します。画面に表示される新しいアドレスを開いてください。</p>");
  delay(500);
  ESP.restart();
}

static void loadStationConfig() {
  staSsid = staPass = "";
  File f = LittleFS.open(kWifiFile, FILE_READ);
  if (!f) return;
  staSsid = f.readStringUntil('\n');
  staPass = f.readStringUntil('\n');
  f.close();
  staSsid.trim();
  staPass.trim();
}

static void startAccessPoint() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPass);
}

static void setState(State s) {
  wifiState = s;
  appWifiStateChanged();
}

static void becomeReady() {
  server.begin();
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
  setState(State::Ready);
}

void begin() {
  // 起動時は Wi-Fi を付けない。ルートだけ登録しておく
  WiFi.mode(WIFI_OFF);

  // iPhone からの操作中に Wi-Fi やスリープが切れないよう、どのリクエストでも時間をリセットする
  auto on = [](const char* path, HTTPMethod method, WebServer::THandlerFunction fn) {
    server.on(path, method, [fn] {
      appNoteWebActivity();
      fn();
    });
  };
  on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kIndexHtml); });
  on("/api/list", HTTP_GET, handleList);
  on("/api/thumb", HTTP_GET, handleThumb);
  on("/api/snapshot.bmp", HTTP_GET, handleSnapshot);
  server.on("/api/upload", HTTP_POST, handleUploadDone, [] {
    appNoteWebActivity();
    handleUploadData();
  });
  on("/api/show", HTTP_POST, handleShow);
  on("/api/delete", HTTP_POST, handleDelete);
  on("/api/wifi", HTTP_POST, handleWifi);
  on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
}

void start() {
  if (wifiState != State::Off) return;
  loadStationConfig();
  if (staSsid.length()) {
    // 家の Wi-Fi へ。接続は待たずに戻り、loop() で完了 / タイムアウトを見る
    apMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kHostname);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    connectStartMs = millis();
    setState(State::Connecting);
  } else {
    startAccessPoint();
    becomeReady();
  }
}

void stop() {
  if (wifiState == State::Off) return;
  server.stop();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  setState(State::Off);
}

void loop() {
  if (wifiState == State::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      becomeReady();
    } else if (millis() - connectStartMs > kConnectTimeoutMs) {
      // 家の Wi-Fi が見つからなければアクセスポイントとして待つ
      WiFi.disconnect(true);
      startAccessPoint();
      becomeReady();
    }
  }
  if (wifiState == State::Ready) server.handleClient();
}

}  // namespace web
