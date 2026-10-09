// M5Paper 写真 + 手書きメモ
//   画面 960x540（横向き）: 左 880x540 が写真、右 80px がツールバー
//   メモはレイヤー（メモ1, メモ2 …）ごとにストロークで保持し、元画像の上に重ねて表示する
#include <LittleFS.h>
#include <M5Unified.h>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include <algorithm>

#include "app.h"
#include "memo_doc.h"
#include "photo_store.h"
#include "web_server.h"

namespace {

constexpr int SCREEN_W = 960;
constexpr int TB_X = IMG_W, TB_W = SCREEN_W - IMG_W, BTN_H = 60;
constexpr int PANEL_W = 400, PANEL_X = IMG_W - PANEL_W, ROW_H = 60;
constexpr int COL_VIS = PANEL_X + 200, COL_DEL = PANEL_X + 300;
constexpr int PANEL_LAST_ROW = IMG_H / ROW_H - 1;
constexpr uint8_t kPenWidths[] = {2, 5, 10};
constexpr int kEraserRadius = 14;
constexpr uint32_t kSaveDelayMs = 3000;

enum Btn { B_TOOL, B_WIDTH, B_LAYER, B_UNDO, B_PREV, B_NEXT, B_REFRESH, B_WIFI, B_STATUS, BTN_COUNT };
enum class Tool { Pen, Eraser };
enum class Overlay { None, Layers, Info };

M5Canvas baseCanvas(&M5.Display);     // 元画像
M5Canvas composeCanvas(&M5.Display);  // 元画像 + 表示中レイヤー
MemoDoc doc;
uint16_t currentId = 0;
Tool tool = Tool::Pen;
int widthIdx = 1;
Overlay overlay = Overlay::None;
int confirmDeleteIndex = -1;

bool drawing = false;
Stroke curStroke;
Point lastPt;

// 画面更新は待たずに溜めておき、コントローラが空いた時に出す（タッチの読み取りを止めないため）
struct Seg {
  Point a, b;
  uint8_t width;
};
std::vector<Seg> pendingSegs;  // まだ画面に描いていないペンの線分
Rect pendingErase;             // 消しゴムで消えたがまだ画面に反映していない範囲
Rect pendingRefresh;           // 書き終えた範囲。手が止まったら階調を整えて描き直す
uint32_t refreshDueMs = 0;
bool undoButtonStale = false;
constexpr uint32_t kRefreshIdleMs = 800;

// 操作が無いとライトスリープ（RAM の状態は保持、e-ink は表示を保持）。タッチか側面スイッチで復帰
constexpr uint32_t kSleepAfterMs = 30000;
constexpr uint32_t kSleepAfterInfoMs = 180000;  // 「送る」画面を表示中は iPhone 操作を待つので長め
constexpr gpio_num_t kButtonPins[] = {GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39};  // 側面スイッチ 上/押し/下
uint32_t lastActivityMs = 0;

// Wi-Fi は「送る」画面を開いた時だけ付け、iPhone からの通信が途絶えたら切る
constexpr uint32_t kWifiOffAfterMs = 60000;
uint32_t lastWebMs = 0;
bool infoStale = false;  // Wi-Fi の状態が変わり「送る」画面の描き直しが必要
bool sleeping = false;
bool wakeGuard = false;  // 起こしたタッチ / スイッチ操作を離すまで無視する
uint32_t saveDueMs = 0;
uint32_t statusDueMs = 0;

const lgfx::IFont* const fontL = &fonts::efontJA_24;
const lgfx::IFont* const fontS = &fonts::efontJA_16;

Rect makeRect(int x, int y, int w, int h) {
  Rect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

const Rect kImageRect = makeRect(0, 0, IMG_W, IMG_H);

// ---------------------------------------------------------------- 描画

void drawStroke(LovyanGFX& g, const Stroke& s) {
  if (s.pts.size() == 1) {
    g.fillCircle(s.pts[0].x, s.pts[0].y, std::max(1, s.width / 2), TFT_BLACK);
    return;
  }
  float r = s.width / 2.0f;
  for (size_t i = 1; i < s.pts.size(); ++i) {
    g.drawWideLine(s.pts[i - 1].x, s.pts[i - 1].y, s.pts[i].x, s.pts[i].y, r, TFT_BLACK);
  }
}

void compose(Rect r) {
  r = r.clipped(IMG_W, IMG_H);
  if (r.empty()) return;
  composeCanvas.setClipRect(r.x, r.y, r.w, r.h);
  if (currentId && doc.baseVisible) {
    baseCanvas.pushSprite(&composeCanvas, 0, 0);
  } else {
    composeCanvas.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
  }
  if (currentId) {
    for (auto& l : doc.layers) {
      if (!l.visible) continue;
      for (auto& s : doc.strokes) {
        if (!s.deleted && s.layerId == l.id && s.bounds.intersects(r)) drawStroke(composeCanvas, s);
      }
    }
  }
  composeCanvas.clearClipRect();
}

void pushImage(Rect r) {
  r = r.clipped(IMG_W, IMG_H);
  if (r.empty()) return;
  // 実行中の高速更新と範囲が重なると IT8951 が画素を取りこぼすので、終わるのを待つ
  M5.Display.waitDisplay();
  M5.Display.setClipRect(r.x, r.y, r.w, r.h);
  composeCanvas.pushSprite(&M5.Display, 0, 0);
  M5.Display.clearClipRect();
}

void drawLabel(const char* text, int cx, int cy, const lgfx::IFont* font, uint16_t fg, uint16_t bg) {
  auto& d = M5.Display;
  d.setFont(font);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(fg, bg);
  d.drawString(text, cx, cy);
}

void drawButton(int i) {
  auto& d = M5.Display;
  int y = i * BTN_H, cx = TB_X + TB_W / 2, cy = y + BTN_H / 2;
  bool inv = (i == B_TOOL && tool == Tool::Eraser) || (i == B_LAYER && overlay == Overlay::Layers) ||
             (i == B_WIFI && overlay == Overlay::Info);
  uint16_t bg = inv ? TFT_BLACK : TFT_WHITE, fg = inv ? TFT_WHITE : TFT_BLACK;
  d.fillRect(TB_X, y, TB_W, BTN_H, bg);
  d.drawFastHLine(TB_X, y + BTN_H - 1, TB_W, TFT_DARKGREY);
  char buf[24];
  switch (i) {
    case B_TOOL:
      if (tool == Tool::Pen) drawLabel("ペン", cx, cy, fontL, fg, bg);
      else drawLabel("消しゴム", cx, cy, fontS, fg, bg);
      break;
    case B_WIDTH:
      drawLabel("太さ", cx, y + 16, fontS, fg, bg);
      d.drawWideLine(TB_X + 14, y + 40, TB_X + TB_W - 14, y + 40, kPenWidths[widthIdx] / 2.0f, fg);
      break;
    case B_LAYER:
      drawLabel("レイヤー", cx, y + 18, fontS, fg, bg);
      snprintf(buf, sizeof(buf), "メモ%u", doc.activeLayer().id);
      drawLabel(buf, cx, y + 42, fontS, fg, bg);
      break;
    case B_UNDO:
      drawLabel("戻す", cx, cy, fontL, doc.undoStack.empty() ? TFT_LIGHTGREY : fg, bg);
      break;
    case B_PREV: drawLabel("前へ", cx, cy, fontL, fg, bg); break;
    case B_NEXT: drawLabel("次へ", cx, cy, fontL, fg, bg); break;
    case B_REFRESH: drawLabel("更新", cx, cy, fontL, fg, bg); break;
    case B_WIFI: drawLabel("送る", cx, cy, fontL, fg, bg); break;
    case B_STATUS: {
      if (sleeping) {
        drawLabel("スリープ", cx, y + 18, fontS, fg, bg);
        drawLabel("中", cx, y + 42, fontS, fg, bg);  // 幅 80px に収まる表記
        break;
      }
      auto ids = store::list();
      int pos = std::find(ids.begin(), ids.end(), currentId) - ids.begin();
      if (currentId && pos < (int)ids.size()) snprintf(buf, sizeof(buf), "%d/%d", pos + 1, (int)ids.size());
      else snprintf(buf, sizeof(buf), "-/%d", (int)ids.size());
      drawLabel(buf, cx, y + 18, fontS, fg, bg);
      snprintf(buf, sizeof(buf), "%d%%", (int)M5.Power.getBatteryLevel());
      drawLabel(buf, cx, y + 42, fontS, TFT_DARKGREY, bg);
      break;
    }
  }
}

void drawToolbar() {
  M5.Display.drawFastVLine(TB_X, 0, IMG_H, TFT_BLACK);
  for (int i = 0; i < BTN_COUNT; ++i) drawButton(i);
}

void updateButton(int i) {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  drawButton(i);
}

void drawPanel() {
  auto& d = M5.Display;
  d.fillRect(PANEL_X, 0, PANEL_W, IMG_H, TFT_WHITE);
  d.fillRect(PANEL_X, 0, 3, IMG_H, TFT_BLACK);
  char buf[32];

  drawLabel("レイヤー", PANEL_X + 70, ROW_H / 2, fontL, TFT_BLACK, TFT_WHITE);
  drawLabel("名前をタップで描く先を選択", PANEL_X + 270, ROW_H / 2, fontS, TFT_DARKGREY, TFT_WHITE);

  auto visCell = [&](int row, bool visible) {
    drawLabel(visible ? "表示" : "非表示", COL_VIS + 50, row * ROW_H + ROW_H / 2, fontS,
              visible ? TFT_BLACK : TFT_LIGHTGREY, TFT_WHITE);
    d.drawRoundRect(COL_VIS + 8, row * ROW_H + 10, 84, ROW_H - 20, 6, visible ? TFT_BLACK : TFT_LIGHTGREY);
  };

  // 元画像
  d.drawFastHLine(PANEL_X, ROW_H, PANEL_W, TFT_DARKGREY);
  drawLabel("元画像", PANEL_X + 100, ROW_H + ROW_H / 2, fontL, TFT_BLACK, TFT_WHITE);
  visCell(1, doc.baseVisible);

  for (size_t i = 0; i < doc.layers.size(); ++i) {
    int row = 2 + i, y = row * ROW_H;
    auto& l = doc.layers[i];
    bool active = (int)i == doc.activeIndex;
    d.drawFastHLine(PANEL_X, y, PANEL_W, TFT_DARKGREY);
    if (active) d.fillRect(PANEL_X + 3, y + 1, 197, ROW_H - 1, TFT_BLACK);
    uint16_t fg = active ? TFT_WHITE : TFT_BLACK, bg = active ? TFT_BLACK : TFT_WHITE;
    snprintf(buf, sizeof(buf), "メモ%u", l.id);
    drawLabel(buf, PANEL_X + 70, y + ROW_H / 2, fontL, fg, bg);
    snprintf(buf, sizeof(buf), "%d本", doc.visibleStrokeCount(l.id));
    drawLabel(buf, PANEL_X + 160, y + ROW_H / 2, fontS, active ? TFT_LIGHTGREY : TFT_DARKGREY, bg);
    visCell(row, l.visible);
    bool confirm = confirmDeleteIndex == (int)i;
    if (confirm) d.fillRoundRect(COL_DEL + 8, y + 10, 84, ROW_H - 20, 6, TFT_BLACK);
    else d.drawRoundRect(COL_DEL + 8, y + 10, 84, ROW_H - 20, 6, TFT_BLACK);
    const char* delLabel = confirm ? "本当に?" : (doc.layers.size() == 1 ? "全消去" : "削除");
    drawLabel(delLabel, COL_DEL + 50, y + ROW_H / 2, fontS, confirm ? TFT_WHITE : TFT_BLACK,
              confirm ? TFT_BLACK : TFT_WHITE);
  }

  int y = PANEL_LAST_ROW * ROW_H;
  d.drawFastHLine(PANEL_X, y, PANEL_W, TFT_BLACK);
  bool canAdd = (int)doc.layers.size() < MAX_MEMO_LAYERS;
  drawLabel("+ レイヤー追加", PANEL_X + 100, y + ROW_H / 2, fontS, canAdd ? TFT_BLACK : TFT_LIGHTGREY, TFT_WHITE);
  d.drawFastVLine(COL_VIS, y, ROW_H, TFT_DARKGREY);
  drawLabel("閉じる", COL_VIS + 100, y + ROW_H / 2, fontL, TFT_BLACK, TFT_WHITE);
}

void drawInfo() {
  auto& d = M5.Display;
  d.fillRect(0, 0, IMG_W, IMG_H, TFT_WHITE);
  d.setFont(fontL);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.drawString("iPhone から写真を送る", 30, 24);
  d.setFont(fontS);
  String url = web::url();
  int y = 70;
  auto line = [&](const String& s) {
    d.drawString(s, 30, y);
    y += 26;
  };
  if (web::state() != web::State::Ready) {
    line("Wi-Fi に接続しています…");
    line("家の Wi-Fi に繋がらない場合は、15 秒後に M5Paper 自身がアクセスポイントになります");
    return;
  }
  if (web::isAccessPoint()) {
    line("1. iPhone の Wi-Fi で「" + web::ssid() + "」に接続");
    line("   パスワード: " + web::apPassword() + "（左の QR をカメラで読んでも OK）");
    line("2. Safari で " + url + " を開く（右の QR）");
    String wifiQr = "WIFI:T:WPA;S:" + web::ssid() + ";P:" + web::apPassword() + ";;";
    d.qrcode(wifiQr.c_str(), 120, 190, 230, 4);
    d.qrcode(url.c_str(), 520, 190, 230, 4);
    d.drawString("Wi-Fi に接続", 175, 430);
    d.drawString("ページを開く", 580, 430);
  } else {
    line("1. iPhone を Wi-Fi「" + web::ssid() + "」に接続");
    line("2. Safari で " + url + " を開く（右の QR）");
    line("   または http://m5paper.local/");
    d.qrcode(url.c_str(), 520, 190, 230, 4);
  }
  y = 470;
  line(currentId ? "画面をタップすると閉じます" : "写真が届くとここに表示されます");
}

// 表示待ちの範囲をまとめて取り出す（呼び出し側がその範囲を描き直す）
Rect takePending() {
  Rect r = pendingErase;
  r.unite(pendingRefresh);
  for (auto& s : pendingSegs) {
    int m = s.width / 2 + 2;
    r.unite(makeRect(std::min(s.a.x, s.b.x) - m, std::min(s.a.y, s.b.y) - m, std::abs(s.a.x - s.b.x) + m * 2 + 1,
                     std::abs(s.a.y - s.b.y) + m * 2 + 1));
  }
  pendingSegs.clear();
  pendingErase = Rect();
  pendingRefresh = Rect();
  undoButtonStale = false;
  return r;
}

void redrawAll(epd_mode_t mode = epd_mode_t::epd_quality) {
  takePending();
  infoStale = false;
  compose(kImageRect);
  M5.Display.waitDisplay();
  M5.Display.setEpdMode(mode);
  M5.Display.startWrite();
  if (overlay == Overlay::Info) {
    drawInfo();
  } else {
    composeCanvas.pushSprite(&M5.Display, 0, 0);
    if (overlay == Overlay::Layers) drawPanel();
  }
  drawToolbar();
  M5.Display.endWrite();
}

void showBusy(const char* msg) {
  auto& d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_fast);
  d.startWrite();
  int w = 360, h = 80, x = (IMG_W - w) / 2, y = (IMG_H - h) / 2;
  d.fillRect(x, y, w, h, TFT_WHITE);
  d.drawRect(x, y, w, h, TFT_BLACK);
  d.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
  drawLabel(msg, x + w / 2, y + h / 2, fontL, TFT_BLACK, TFT_WHITE);
  d.endWrite();
}

// ---------------------------------------------------------------- データ

void startWifi() {
  lastWebMs = millis();
  web::start();
}

void saveNow() {
  saveDueMs = 0;
  if (currentId && doc.dirty) doc.save(LittleFS, store::memoPath(currentId).c_str());
}

void scheduleSave() { saveDueMs = millis() + kSaveDelayMs; }

void openPhoto(uint16_t id) {
  saveNow();
  overlay = Overlay::None;
  confirmDeleteIndex = -1;
  if (id && store::loadBase(id, baseCanvas)) {
    currentId = id;
    if (!doc.load(LittleFS, store::memoPath(id).c_str())) doc.reset();
    store::saveCurrentId(id);
  } else {
    currentId = 0;
    doc.reset();
    overlay = Overlay::Info;
    startWifi();
  }
  redrawAll();
}

void stepPhoto(int dir) {
  auto ids = store::list();
  if (ids.empty()) return;
  auto it = std::find(ids.begin(), ids.end(), currentId);
  int n = ids.size(), i = it == ids.end() ? 0 : (it - ids.begin() + dir + n) % n;
  if (ids[i] != currentId || overlay != Overlay::None) openPhoto(ids[i]);
}

// ---------------------------------------------------------------- 入力

void closeOverlayAndRedraw() {
  overlay = Overlay::None;
  confirmDeleteIndex = -1;
  redrawAll();
}

void onPanelTap(int x, int y) {
  int row = y / ROW_H;
  int col = x < COL_VIS ? 0 : (x < COL_DEL ? 1 : 2);
  int prevConfirm = confirmDeleteIndex;
  confirmDeleteIndex = -1;
  bool imageChanged = false;

  if (row == 1) {
    doc.baseVisible = !doc.baseVisible;
    doc.dirty = imageChanged = true;
  } else if (row >= 2 && row < 2 + (int)doc.layers.size()) {
    int i = row - 2;
    auto& l = doc.layers[i];
    if (col == 0) {
      doc.activeIndex = i;
      if (!l.visible) l.visible = imageChanged = true;
      doc.dirty = true;
    } else if (col == 1) {
      l.visible = !l.visible;
      doc.dirty = imageChanged = true;
    } else if (prevConfirm == i) {
      doc.removeLayer(i);
      imageChanged = true;
    } else {
      confirmDeleteIndex = i;
    }
  } else if (row == PANEL_LAST_ROW) {
    if (col == 0) {
      doc.addLayer();
    } else {
      scheduleSave();
      closeOverlayAndRedraw();
      return;
    }
  }

  if (doc.dirty) scheduleSave();
  if (imageChanged) {
    takePending();
    compose(kImageRect);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.startWrite();
    pushImage(makeRect(0, 0, PANEL_X, IMG_H));
    drawPanel();
    drawButton(B_LAYER);
    drawButton(B_UNDO);
    M5.Display.endWrite();
  } else {
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    M5.Display.startWrite();
    drawPanel();
    drawButton(B_LAYER);
    M5.Display.endWrite();
  }
}

void onToolbar(int i) {
  switch (i) {
    case B_TOOL:
      tool = tool == Tool::Pen ? Tool::Eraser : Tool::Pen;
      updateButton(B_TOOL);
      break;
    case B_WIDTH:
      widthIdx = (widthIdx + 1) % (int)sizeof(kPenWidths);
      if (tool == Tool::Eraser) {
        tool = Tool::Pen;
        updateButton(B_TOOL);
      }
      updateButton(B_WIDTH);
      break;
    case B_LAYER:
      if (!currentId) break;
      if (overlay == Overlay::Layers) {
        closeOverlayAndRedraw();
      } else {
        overlay = Overlay::Layers;
        confirmDeleteIndex = -1;
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.startWrite();
        drawPanel();
        drawButton(B_LAYER);
        M5.Display.endWrite();
      }
      break;
    case B_UNDO: {
      if (!currentId || doc.undoStack.empty()) break;
      Rect r = doc.undo();
      r.unite(takePending());
      scheduleSave();
      compose(r);
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
      M5.Display.startWrite();
      pushImage(r);
      if (overlay == Overlay::Layers) drawPanel();
      drawButton(B_UNDO);
      M5.Display.endWrite();
      break;
    }
    case B_PREV: stepPhoto(-1); break;
    case B_NEXT: stepPhoto(1); break;
    case B_REFRESH:
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
      M5.Display.clear(TFT_WHITE);  // 残像を消すため一度白で塗る
      redrawAll();
      break;
    case B_WIFI:
      if (overlay == Overlay::Info) {
        if (currentId) closeOverlayAndRedraw();
      } else {
        overlay = Overlay::Info;
        startWifi();
        redrawAll(epd_mode_t::epd_fast);
      }
      break;
  }
}

// 溜まった線分・消去範囲を、コントローラが空いている時だけまとめて描く。
// 更新中の範囲に重ねて描くと IT8951 が画素を取りこぼすため待つ必要があるが、待つとタッチを取りこぼす
void flushPending() {
  if (pendingSegs.empty() && pendingErase.empty()) return;
  if (M5.Display.displayBusy()) return;
  // DU4(fastest) は灰色の上の画素を取りこぼすことがあるので白黒 2 値の DU(fast) で描く
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  M5.Display.startWrite();
  for (auto& s : pendingSegs) M5.Display.drawWideLine(s.a.x, s.a.y, s.b.x, s.b.y, s.width / 2.0f, TFT_BLACK);
  Rect e = pendingErase.clipped(IMG_W, IMG_H);
  if (!e.empty()) {
    M5.Display.setClipRect(e.x, e.y, e.w, e.h);
    composeCanvas.pushSprite(&M5.Display, 0, 0);
    M5.Display.clearClipRect();
  }
  M5.Display.endWrite();
  pendingSegs.clear();
  pendingErase = Rect();
}

// 書くのを 0.8 秒休んだら、高速更新で荒れた範囲を階調付きで描き直す
void refreshWhenIdle() {
  if (drawing || (pendingRefresh.empty() && !undoButtonStale)) return;
  if ((int32_t)(millis() - refreshDueMs) < 0) return;
  if (!pendingSegs.empty() || !pendingErase.empty() || M5.Display.displayBusy()) return;
  Rect r = pendingRefresh.clipped(overlay == Overlay::Layers ? PANEL_X : IMG_W, IMG_H);
  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.startWrite();
  if (!r.empty() && overlay != Overlay::Info) {
    M5.Display.setClipRect(r.x, r.y, r.w, r.h);
    composeCanvas.pushSprite(&M5.Display, 0, 0);
    M5.Display.clearClipRect();
  }
  if (undoButtonStale) drawButton(B_UNDO);
  M5.Display.endWrite();
  pendingRefresh = Rect();
  undoButtonStale = false;
}

void penSegment(Point a, Point b) {
  composeCanvas.drawWideLine(a.x, a.y, b.x, b.y, curStroke.width / 2.0f, TFT_BLACK);
  pendingSegs.push_back({a, b, curStroke.width});
  flushPending();
}

void eraseAlong(Point a, Point b) {
  int dx = b.x - a.x, dy = b.y - a.y;
  int steps = std::max(1, (int)(sqrtf(dx * dx + dy * dy) / (kEraserRadius / 2)));
  Rect hit;
  for (int i = 1; i <= steps; ++i) {
    hit.unite(doc.eraseAt(a.x + dx * i / steps, a.y + dy * i / steps, kEraserRadius));
  }
  if (hit.empty()) return;
  compose(hit);
  pendingErase.unite(hit);
  pendingRefresh.unite(hit);
  flushPending();
}

void beginStroke(int x, int y) {
  if (!doc.activeLayer().visible) {
    doc.activeLayer().visible = true;
    doc.dirty = true;
    redrawAll();
  }
  drawing = true;
  lastPt = {(int16_t)x, (int16_t)y};
  if (tool == Tool::Pen) {
    curStroke = Stroke();
    curStroke.layerId = doc.activeLayer().id;
    curStroke.width = kPenWidths[widthIdx];
    curStroke.pts.push_back(lastPt);
    penSegment(lastPt, lastPt);
  } else {
    doc.beginErase();
    eraseAlong(lastPt, lastPt);
  }
}

void continueStroke(int x, int y) {
  Point p = {(int16_t)std::min(std::max(x, 0), IMG_W - 1), (int16_t)std::min(std::max(y, 0), IMG_H - 1)};
  int dx = p.x - lastPt.x, dy = p.y - lastPt.y;
  if (dx * dx + dy * dy < 4) return;
  if (tool == Tool::Pen) {
    curStroke.pts.push_back(p);
    penSegment(lastPt, p);
  } else {
    eraseAlong(lastPt, p);
  }
  lastPt = p;
}

// ここでは画面を待たない。続けて次の線を書けるよう、描き直しは手が止まってから行う
void endStroke() {
  drawing = false;
  bool hadUndo = !doc.undoStack.empty();
  if (tool == Tool::Pen) {
    doc.commitStroke(std::move(curStroke));
    pendingRefresh.unite(doc.strokes.back().bounds);
  } else {
    doc.endErase();
  }
  if (hadUndo == doc.undoStack.empty()) undoButtonStale = true;  // 「戻す」の有効/無効が変わった
  refreshDueMs = millis() + kRefreshIdleMs;
  if (doc.dirty) scheduleSave();
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (t.isPressed()) lastActivityMs = millis();
  if (drawing) {
    if (t.isPressed()) {
      continueStroke(t.x, t.y);
    } else {
      endStroke();
    }
    return;
  }
  if (!t.wasPressed()) return;
  int x = t.x, y = t.y;
  if (x >= TB_X) {
    onToolbar(std::min(y / BTN_H, BTN_COUNT - 1));
  } else if (overlay == Overlay::Layers) {
    if (x >= PANEL_X) onPanelTap(x, y);
    else {
      scheduleSave();
      closeOverlayAndRedraw();
    }
  } else if (overlay == Overlay::Info) {
    if (currentId) closeOverlayAndRedraw();
  } else if (currentId) {
    beginStroke(x, y);
  }
}

void enterSleep() {
  // 表示待ちを出し切り、高速更新で荒れた部分も整えてから眠る
  Rect r = takePending();
  if (overlay == Overlay::Layers) r = r.clipped(PANEL_X, IMG_H);
  if (overlay != Overlay::Info && !r.empty()) {
    M5.Display.setEpdMode(epd_mode_t::epd_text);
    pushImage(r);
  }
  saveNow();
  sleeping = true;
  M5.Display.waitDisplay();
  updateButton(B_STATUS);
  M5.Display.waitDisplay();
  Serial.println("sleep");
  Serial.flush();

  web::stop();
  M5.Display.sleep();
  for (auto pin : kButtonPins) gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  M5.Power.lightSleep(m5::Power_Class::sleep_no_timer, true);  // タッチ INT(GPIO36) でも復帰
  for (auto pin : kButtonPins) gpio_wakeup_disable(pin);

  Serial.printf("wakeup cause=%d\n", (int)esp_sleep_get_wakeup_cause());
  M5.Display.wakeup();
  sleeping = false;
  wakeGuard = true;
  if (overlay == Overlay::Info) startWifi();  // 「送る」画面のまま眠った場合は Wi-Fi を付け直す
  updateButton(B_STATUS);
  lastActivityMs = millis();
}

void fatal(const char* msg) {
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  M5.Display.clear(TFT_WHITE);
  drawLabel(msg, SCREEN_W / 2, IMG_H / 2, fontL, TFT_BLACK, TFT_WHITE);
  for (;;) delay(1000);
}

}  // namespace

// ---------------------------------------------------------------- app.h

uint16_t appCurrentId() { return currentId; }
void appShowPhoto(uint16_t id) { openPhoto(id); }
void appShowBusy(const char* msg) { showBusy(msg); }
void appRedrawAll() { redrawAll(epd_mode_t::epd_fast); }
void appNoteWebActivity() { lastActivityMs = lastWebMs = millis(); }
void appWifiStateChanged() { infoStale = true; }
M5Canvas& appComposeCanvas() { return composeCanvas; }

void appPhotoDeleted(uint16_t id) {
  if (id != currentId) {
    updateButton(B_STATUS);
    return;
  }
  // 削除済みファイルに保存し直さないよう、先に切り離す
  currentId = 0;
  doc.reset();
  auto ids = store::list();
  auto next = std::upper_bound(ids.begin(), ids.end(), id);
  if (next == ids.end() && !ids.empty()) next = ids.end() - 1;
  openPhoto(next == ids.end() ? 0 : *next);
}

// ---------------------------------------------------------------- Arduino

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = false;  // 全面クリアは約1.4秒かかる。直後に写真を描き直すので不要
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  Serial.printf("display %dx%d\n", M5.Display.width(), M5.Display.height());

  showBusy("起動中…");
  if (!store::begin()) fatal("ストレージを初期化できません");

  for (auto* c : {&baseCanvas, &composeCanvas}) {
    c->setPsram(true);
    c->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (!c->createSprite(IMG_W, IMG_H)) fatal("メモリ不足");
    c->fillScreen(TFT_WHITE);
  }

  web::begin();

  uint16_t id = store::loadCurrentId();
  auto ids = store::list();
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) id = ids.empty() ? 0 : ids.front();
  openPhoto(id);
  statusDueMs = millis() + 60000;
  lastActivityMs = millis();
}

void loop() {
  M5.update();
  if (wakeGuard) {
    // 起こした操作で線を引いたり写真を送ったりしないよう、全部離されるまで入力を読み捨てる
    bool held = M5.Touch.getCount() > 0 || M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();
    if (!held) wakeGuard = false;
    lastActivityMs = millis();
    web::loop();
    return;
  }
  handleTouch();
  flushPending();  // 指が止まっていても溜まった線を出す
  if (drawing) return;  // 描画中はタッチを最優先
  refreshWhenIdle();

  web::loop();

  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) lastActivityMs = millis();
  if (M5.BtnA.wasPressed()) stepPhoto(-1);
  if (M5.BtnC.wasPressed()) stepPhoto(1);
  if (M5.BtnB.wasPressed()) onToolbar(B_REFRESH);

  uint32_t now = millis();
  if (infoStale) {
    infoStale = false;
    if (overlay == Overlay::Info) redrawAll(epd_mode_t::epd_fast);
  }
  if (web::state() != web::State::Off && overlay != Overlay::Info && now - lastWebMs >= kWifiOffAfterMs) {
    web::stop();
  }
  uint32_t sleepAfter = overlay == Overlay::Info ? kSleepAfterInfoMs : kSleepAfterMs;
  if (now - lastActivityMs >= sleepAfter) {
    enterSleep();
    return;
  }
  if (saveDueMs && (int32_t)(now - saveDueMs) >= 0) saveNow();
  if ((int32_t)(now - statusDueMs) >= 0) {
    statusDueMs = now + 60000;
    updateButton(B_STATUS);
  }
}
