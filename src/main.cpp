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
#include "settings.h"
#include "web_server.h"

namespace {

constexpr int SCREEN_W = 960;
constexpr int TB_X = IMG_W, TB_W = SCREEN_W - IMG_W, BTN_H = 60;
constexpr int PANEL_W = 400, PANEL_X = IMG_W - PANEL_W, ROW_H = 60;
constexpr int COL_VIS = PANEL_X + 200, COL_DEL = PANEL_X + 300;
constexpr int PANEL_LAST_ROW = IMG_H / ROW_H - 1;
constexpr uint8_t kPenWidths[] = {2, 5, 10};
constexpr uint8_t kStampSizes[] = {12, 20, 30};  // 丸ポチの直径。「太さ」と同じ 3 段階
static_assert(sizeof(kPenWidths) == sizeof(kStampSizes), "太さの段階数を揃える");
constexpr int kEraserRadius = 14;
constexpr uint32_t kSaveDelayMs = 3000;

// 設定パネルの選択肢（秒）
constexpr uint32_t kSleepChoices[] = {15, 30, 60, 120, 300};
const char* const kSleepLabels[] = {"15秒", "30秒", "1分", "2分", "5分"};
constexpr uint32_t kOffChoices[] = {300, 600, 1800, 3600, 0};
const char* const kOffLabels[] = {"5分", "10分", "30分", "1時間", "しない"};
constexpr int kChoiceCount = 5;
constexpr int CHOICE_W = PANEL_W / kChoiceCount;

enum Btn { B_TOOL, B_WIDTH, B_LAYER, B_UNDO, B_PREV, B_NEXT, B_REFRESH, B_WIFI, B_STATUS, BTN_COUNT };
enum class Tool { Pen, Eraser, Stamp };  // Stamp: 触れた所に丸ポチを 1 つ置く（滑っても線にならない）
enum class Overlay { None, Layers, Info, Settings };

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
Point lastScreen;  // 直前のタッチ位置（画面 px）

// 拡大表示。G39 で拡大、G37 で縮小、G38 で等倍。拡大中は左端の矢印ボタンで移動する
constexpr float kZoomLevels[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr int kZoomCount = sizeof(kZoomLevels) / sizeof(kZoomLevels[0]);
int zoomIdx = 0;
float zoom = 1.0f;
float viewX = 0, viewY = 0;           // 画面左上に来る元画像の座標
M5Canvas viewCanvas(&M5.Display);     // 拡大時の表示内容
constexpr int PAN_W = 64, PAN_H = 64, PAN_CELLS = 5;  // 倍率表示 + ↑←→↓
constexpr int PAN_Y0 = (IMG_H - PAN_H * PAN_CELLS) / 2;

// 画面更新は待たずに溜めておき、コントローラが空いた時に出す（タッチの読み取りを止めないため）
struct Seg {
  Point a, b;  // SUB 単位の元画像座標。描く時に今の表示倍率で画面座標にする
  uint8_t width;
};
std::vector<Seg> pendingSegs;  // まだ画面に描いていないペンの線分
Rect pendingErase;             // 消しゴムで消えたがまだ画面に反映していない範囲
Rect pendingRefresh;           // 消しゴムで 2 値表示になった範囲。手が止まったら階調付きで描き直す
uint32_t refreshDueMs = 0;
bool undoButtonStale = false;
constexpr uint32_t kRefreshIdleMs = 800;

// 操作が無いと 2 段階で省電力（時間は settings で変更可。どちらも最後の操作から数える）
//   1. ライトスリープ: RAM の状態を保持。タッチか側面スイッチで即復帰
//   2. 電源オフ: 状態は Flash に保存済み。電源ボタン(G38)長押しで起動し、同じ画面から再開
constexpr uint32_t kSleepAfterInfoMs = 180000;  // 「送る」画面を表示中は iPhone 操作を待つので長め
constexpr const char* kResumeFlag = "/resume.flag";  // 電源オフからの起動の印
constexpr gpio_num_t kButtonPins[] = {GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39};  // 側面スイッチ 上/押し/下
uint32_t lastActivityMs = 0;

// Wi-Fi は「送る」画面を開いた時だけ付け、iPhone からの通信が途絶えたら切る
constexpr uint32_t kWifiOffAfterMs = 60000;
uint32_t lastWebMs = 0;
bool infoStale = false;  // Wi-Fi の状態が変わり「送る」画面の描き直しが必要
enum class PowerState { Awake, Sleeping, Off };
PowerState powerState = PowerState::Awake;
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

bool zoomed() { return zoomIdx > 0; }

Rect panRect() { return makeRect(0, PAN_Y0, PAN_W, PAN_H * PAN_CELLS); }
void drawPanButtons();

int dotRadius(float r) { return std::max(1, (int)lroundf(r)); }

// 元画像 px（SUB 単位）を、原点 (ox, oy)・倍率 scale の座標系に描く。ピクセル中心どうしを合わせる
void drawSeg(LovyanGFX& g, Point a, Point b, uint8_t width, float ox = 0, float oy = 0, float scale = 1) {
  auto X = [&](const Point& p) { return (p.x / (float)SUB + 0.5f - ox) * scale - 0.5f; };
  auto Y = [&](const Point& p) { return (p.y / (float)SUB + 0.5f - oy) * scale - 0.5f; };
  float r = width * scale / SUB / 2;
  if (a.x == b.x && a.y == b.y) g.fillCircle(lroundf(X(a)), lroundf(Y(a)), dotRadius(r), TFT_BLACK);
  else g.drawWideLine(X(a), Y(a), X(b), Y(b), r, TFT_BLACK);
}

void drawStroke(LovyanGFX& g, const Stroke& s, float ox = 0, float oy = 0, float scale = 1) {
  if (s.pts.size() == 1) {
    drawSeg(g, s.pts[0], s.pts[0], s.width, ox, oy, scale);
    return;
  }
  for (size_t i = 1; i < s.pts.size(); ++i) drawSeg(g, s.pts[i - 1], s.pts[i], s.width, ox, oy, scale);
}

void drawLayers(LovyanGFX& g, const Rect& imgRect, float ox = 0, float oy = 0, float scale = 1) {
  if (!currentId) return;
  for (auto& l : doc.layers) {
    if (!l.visible) continue;
    for (auto& s : doc.strokes) {
      if (!s.deleted && s.layerId == l.id && s.bounds.intersects(imgRect)) drawStroke(g, s, ox, oy, scale);
    }
  }
}

// 等倍の合成画像（保存・スナップショット・等倍表示に使う）
void compose(Rect r) {
  r = r.clipped(IMG_W, IMG_H);
  if (r.empty()) return;
  composeCanvas.setClipRect(r.x, r.y, r.w, r.h);
  if (currentId && doc.baseVisible) {
    baseCanvas.pushSprite(&composeCanvas, 0, 0);
  } else {
    composeCanvas.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
  }
  drawLayers(composeCanvas, r);
  composeCanvas.clearClipRect();
}

// 画面 px ⇔ 元画像
Point screenToSub(int sx, int sy) {
  float ix = (sx + 0.5f) / zoom + viewX - 0.5f, iy = (sy + 0.5f) / zoom + viewY - 0.5f;
  int x = std::min(std::max((int)lroundf(ix * SUB), 0), IMG_W * SUB - 1);
  int y = std::min(std::max((int)lroundf(iy * SUB), 0), IMG_H * SUB - 1);
  return {(int16_t)x, (int16_t)y};
}

Rect imageToScreenRect(const Rect& r) {
  if (!zoomed() || r.empty()) return r;
  int x = floorf((r.x - viewX) * zoom) - 1, y = floorf((r.y - viewY) * zoom) - 1;
  return makeRect(x, y, ceilf(r.w * zoom) + 3, ceilf(r.h * zoom) + 3);
}

Rect screenToImageRect(const Rect& r) {
  if (!zoomed() || r.empty()) return r;
  int x = floorf(viewX + r.x / zoom) - 1, y = floorf(viewY + r.y / zoom) - 1;
  return makeRect(x, y, ceilf(r.w / zoom) + 3, ceilf(r.h / zoom) + 3);
}

// 拡大表示を画面 px の範囲 sr だけ作る。写真は補間して拡大し、線は拡大後の座標で描き直すのでぼけない
void renderView(const Rect& sr) {
  viewCanvas.setClipRect(sr.x, sr.y, sr.w, sr.h);
  if (currentId && doc.baseVisible) {
    float ox = (0.5f - viewX) * zoom - 0.5f, oy = (0.5f - viewY) * zoom - 0.5f;
    baseCanvas.pushRotateZoomWithAA(&viewCanvas, ox, oy, 0, zoom, zoom);  // pivot は (0,0)
  } else {
    viewCanvas.fillRect(sr.x, sr.y, sr.w, sr.h, TFT_WHITE);
  }
  drawLayers(viewCanvas, screenToImageRect(sr), viewX, viewY, zoom);
  viewCanvas.clearClipRect();
}

// 画面 px の範囲を今の表示（等倍 or 拡大）で描く。完了待ちはしない
void pushScreenRect(Rect sr) {
  if (sr.empty()) return;
  LGFX_Sprite* src = &composeCanvas;
  if (zoomed()) {
    renderView(sr);
    src = &viewCanvas;
  }
  M5.Display.setClipRect(sr.x, sr.y, sr.w, sr.h);
  src->pushSprite(&M5.Display, 0, 0);
  M5.Display.clearClipRect();
  if (zoomed() && overlay != Overlay::Info && sr.intersects(panRect())) drawPanButtons();
}

// 元画像の範囲を描き直す。clipW より右（パネル）は描かない
void pushView(const Rect& imgRect, int clipW = IMG_W) {
  Rect sr = imageToScreenRect(imgRect).clipped(clipW, IMG_H);
  if (sr.empty()) return;
  // 実行中の高速更新と範囲が重なると IT8951 が画素を取りこぼすので、終わるのを待つ
  M5.Display.waitDisplay();
  pushScreenRect(sr);
}

void drawLabel(const char* text, int cx, int cy, const lgfx::IFont* font, uint16_t fg, uint16_t bg) {
  auto& d = M5.Display;
  d.setFont(font);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(fg, bg);
  d.drawString(text, cx, cy);
}

void drawPanButtons() {
  auto& d = M5.Display;
  Rect r = panRect();
  d.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
  char buf[16];
  if (zoom == floorf(zoom)) snprintf(buf, sizeof(buf), "%d倍", (int)zoom);
  else snprintf(buf, sizeof(buf), "%.1f倍", zoom);
  drawLabel(buf, PAN_W / 2, PAN_Y0 + PAN_H / 2, fontS, TFT_BLACK, TFT_WHITE);
  const char* arrows[] = {"↑", "←", "→", "↓"};
  for (int i = 0; i < 4; ++i) {
    int y = PAN_Y0 + (i + 1) * PAN_H;
    d.drawRoundRect(4, y + 4, PAN_W - 8, PAN_H - 8, 8, TFT_BLACK);
    drawLabel(arrows[i], PAN_W / 2, y + PAN_H / 2, fontL, TFT_BLACK, TFT_WHITE);
  }
  d.drawRect(r.x, r.y, r.w, r.h, TFT_BLACK);
}

void drawButton(int i) {
  auto& d = M5.Display;
  int y = i * BTN_H, cx = TB_X + TB_W / 2, cy = y + BTN_H / 2;
  bool inv = (i == B_TOOL && tool != Tool::Pen) || (i == B_LAYER && overlay == Overlay::Layers) ||
             (i == B_WIFI && overlay == Overlay::Info) || (i == B_STATUS && overlay == Overlay::Settings);
  uint16_t bg = inv ? TFT_BLACK : TFT_WHITE, fg = inv ? TFT_WHITE : TFT_BLACK;
  d.fillRect(TB_X, y, TB_W, BTN_H, bg);
  d.drawFastHLine(TB_X, y + BTN_H - 1, TB_W, TFT_DARKGREY);
  char buf[24];
  switch (i) {
    case B_TOOL:
      if (tool == Tool::Pen) drawLabel("ペン", cx, cy, fontL, fg, bg);
      else if (tool == Tool::Eraser) drawLabel("消しゴム", cx, cy, fontS, fg, bg);
      else drawLabel("丸ポチ", cx, cy, fontS, fg, bg);
      break;
    case B_WIDTH:
      if (tool == Tool::Stamp) {
        drawLabel("大きさ", cx, y + 12, fontS, fg, bg);
        d.fillCircle(cx, y + 38, kStampSizes[widthIdx] / 2, fg);
      } else {
        drawLabel("太さ", cx, y + 16, fontS, fg, bg);
        d.drawWideLine(TB_X + 14, y + 40, TB_X + TB_W - 14, y + 40, kPenWidths[widthIdx] / 2.0f, fg);
      }
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
      if (powerState == PowerState::Sleeping) {
        drawLabel("スリープ", cx, y + 18, fontS, fg, bg);
        drawLabel("中", cx, y + 42, fontS, fg, bg);  // 幅 80px に収まる表記
        break;
      }
      if (powerState == PowerState::Off) {
        drawLabel("電源オフ", cx, y + 18, fontS, fg, bg);
        drawLabel("長押し起動", cx, y + 42, fontS, fg, bg);  // 電源ボタン(G38)の長押し
        break;
      }
      auto ids = store::list();
      int pos = std::find(ids.begin(), ids.end(), currentId) - ids.begin();
      if (currentId && pos < (int)ids.size()) snprintf(buf, sizeof(buf), "%d/%d", pos + 1, (int)ids.size());
      else snprintf(buf, sizeof(buf), "-/%d", (int)ids.size());
      drawLabel(buf, cx, y + 12, fontS, fg, bg);
      snprintf(buf, sizeof(buf), "%d%%", (int)M5.Power.getBatteryLevel());
      drawLabel(buf, cx, y + 30, fontS, inv ? TFT_LIGHTGREY : TFT_DARKGREY, bg);
      drawLabel("設定", cx, y + 48, fontS, fg, bg);  // タップで設定パネル
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
  // まとめて 1 回の画面更新にする（図形ごとに更新すると前の更新完了を何度も待つ）
  M5.Display.startWrite();
  drawButton(i);
  M5.Display.endWrite();
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

void drawSettingsPanel() {
  auto& d = M5.Display;
  auto& st = settings::get();
  d.fillRect(PANEL_X, 0, PANEL_W, IMG_H, TFT_WHITE);
  d.fillRect(PANEL_X, 0, 3, IMG_H, TFT_BLACK);
  drawLabel("設定", PANEL_X + 40, ROW_H / 2, fontL, TFT_BLACK, TFT_WHITE);
  drawLabel("どちらも最後の操作から", PANEL_X + 250, ROW_H / 2, fontS, TFT_DARKGREY, TFT_WHITE);

  auto choiceRow = [&](int row, const char* title, const uint32_t* values, const char* const* labels,
                       uint32_t current) {
    int y = row * ROW_H;
    d.drawFastHLine(PANEL_X, y, PANEL_W, TFT_DARKGREY);
    d.setFont(fontS);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.drawString(title, PANEL_X + 16, y + ROW_H / 2);
    y += ROW_H;
    for (int i = 0; i < kChoiceCount; ++i) {
      int x = PANEL_X + i * CHOICE_W;
      bool sel = values[i] == current;
      if (sel) d.fillRoundRect(x + 6, y + 8, CHOICE_W - 12, ROW_H - 16, 6, TFT_BLACK);
      else d.drawRoundRect(x + 6, y + 8, CHOICE_W - 12, ROW_H - 16, 6, TFT_BLACK);
      drawLabel(labels[i], x + CHOICE_W / 2, y + ROW_H / 2, fontS, sel ? TFT_WHITE : TFT_BLACK,
                sel ? TFT_BLACK : TFT_WHITE);
    }
  };
  choiceRow(1, "スリープまで（タッチで復帰）", kSleepChoices, kSleepLabels, st.sleepSec);
  choiceRow(3, "電源オフまで（ボタンで起動）", kOffChoices, kOffLabels, st.powerOffSec);

  int y = 5 * ROW_H;
  d.drawFastHLine(PANEL_X, y, PANEL_W, TFT_DARKGREY);
  d.setFont(fontS);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextColor(TFT_DARKGREY, TFT_WHITE);
  d.drawString("電源オフ中も画面は表示されたまま。", PANEL_X + 16, y + 16);
  d.drawString("起動すると約2秒で続きから書ける。", PANEL_X + 16, y + 40);
  d.drawString("Wi-Fi は「送る」を押した時だけ付く。", PANEL_X + 16, y + 64);

  y = PANEL_LAST_ROW * ROW_H;
  d.drawFastHLine(PANEL_X, y, PANEL_W, TFT_BLACK);
  drawLabel("閉じる", PANEL_X + PANEL_W / 2, y + ROW_H / 2, fontL, TFT_BLACK, TFT_WHITE);
}

void drawOverlayPanel() {
  if (overlay == Overlay::Layers) drawPanel();
  else if (overlay == Overlay::Settings) drawSettingsPanel();
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
    Stroke tmp;
    tmp.width = s.width;
    tmp.pts = {s.a, s.b};
    tmp.updateBounds();
    r.unite(tmp.bounds);
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
  } else if (zoomed()) {
    renderView(kImageRect);
    viewCanvas.pushSprite(&M5.Display, 0, 0);
    drawPanButtons();
    drawOverlayPanel();
  } else {
    composeCanvas.pushSprite(&M5.Display, 0, 0);
    drawOverlayPanel();
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
  zoomIdx = 0;
  zoom = 1.0f;
  viewX = viewY = 0;
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
  overlay = currentId ? Overlay::None : Overlay::Info;  // 写真が無ければ「送る」画面に戻る
  if (overlay == Overlay::Info) startWifi();
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
    pushView(kImageRect, PANEL_X);
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

// 右側のパネル（レイヤー / 設定）を開く。パネル同士の切り替えは描き足すだけで済ませる
void openPanel(Overlay o) {
  bool wasInfo = overlay == Overlay::Info;
  overlay = o;
  confirmDeleteIndex = -1;
  if (wasInfo) {
    redrawAll(epd_mode_t::epd_fast);
    return;
  }
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  M5.Display.startWrite();
  drawOverlayPanel();
  drawButton(B_LAYER);
  drawButton(B_STATUS);
  M5.Display.endWrite();
}

void onSettingsTap(int x, int y) {
  int row = y / ROW_H;
  int i = std::min((x - PANEL_X) / CHOICE_W, kChoiceCount - 1);
  auto& st = settings::get();
  if (row == 2) st.sleepSec = kSleepChoices[i];
  else if (row == 4) st.powerOffSec = kOffChoices[i];
  else if (row == PANEL_LAST_ROW) {
    closeOverlayAndRedraw();
    return;
  } else {
    return;
  }
  settings::save();
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  M5.Display.startWrite();
  drawSettingsPanel();
  M5.Display.endWrite();
}

void onToolbar(int i) {
  switch (i) {
    case B_TOOL:
      // ペン → 消しゴム → 丸ポチ → ペン
      tool = tool == Tool::Pen ? Tool::Eraser : (tool == Tool::Eraser ? Tool::Stamp : Tool::Pen);
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
      M5.Display.startWrite();
      drawButton(B_TOOL);
      drawButton(B_WIDTH);  // 丸ポチでは「大きさ」の見本になる
      M5.Display.endWrite();
      break;
    case B_WIDTH:
      widthIdx = (widthIdx + 1) % (int)sizeof(kPenWidths);
      if (tool == Tool::Eraser) {  // 消しゴムに太さは無いのでペンに戻す
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
        openPanel(Overlay::Layers);
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
      pushView(r);
      drawOverlayPanel();
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
    case B_STATUS:
      if (overlay == Overlay::Settings) closeOverlayAndRedraw();
      else openPanel(Overlay::Settings);
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
  for (auto& s : pendingSegs) drawSeg(M5.Display, s.a, s.b, s.width, viewX, viewY, zoom);
  pushScreenRect(imageToScreenRect(pendingErase).clipped(IMG_W, IMG_H));
  M5.Display.endWrite();
  pendingSegs.clear();
  pendingErase = Rect();
}

// 消しゴムを 0.8 秒休んだら、高速更新で荒れた写真の階調を描き直す
void refreshWhenIdle() {
  if (drawing || (pendingRefresh.empty() && !undoButtonStale)) return;
  if ((int32_t)(millis() - refreshDueMs) < 0) return;
  if (!pendingSegs.empty() || !pendingErase.empty() || M5.Display.displayBusy()) return;
  bool panel = overlay == Overlay::Layers || overlay == Overlay::Settings;
  Rect sr = imageToScreenRect(pendingRefresh).clipped(panel ? PANEL_X : IMG_W, IMG_H);
  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.startWrite();
  if (overlay != Overlay::Info) pushScreenRect(sr);
  if (undoButtonStale) drawButton(B_UNDO);
  M5.Display.endWrite();
  pendingRefresh = Rect();
  undoButtonStale = false;
}

// a→b（SUB 単位）を等倍の合成画像に描き、画面には溜めて後で描く
void penSegment(Point a, Point b) {
  drawSeg(composeCanvas, a, b, curStroke.width);
  pendingSegs.push_back({a, b, curStroke.width});
  flushPending();
}

// 画面 px の a→b をなぞって、触れた線を消す
void eraseAlong(Point a, Point b) {
  int dx = b.x - a.x, dy = b.y - a.y;
  int steps = std::max(1, (int)(sqrtf(dx * dx + dy * dy) / (kEraserRadius / 2)));
  int radius = std::max(2, (int)lroundf(kEraserRadius / zoom));  // 画面上で同じ大きさにする
  Rect hit;
  for (int i = 1; i <= steps; ++i) {
    Point p = screenToSub(a.x + dx * i / steps, a.y + dy * i / steps);
    hit.unite(doc.eraseAt(p.x / SUB, p.y / SUB, radius));
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
  lastScreen = {(int16_t)x, (int16_t)y};
  if (tool == Tool::Pen || tool == Tool::Stamp) {
    // 丸ポチは 1 点だけの線として保存する（消しゴム・戻す・レイヤーがそのまま効く）
    // 太さは画面上で同じに見えるよう、拡大中は細く保存する
    uint8_t screenWidth = tool == Tool::Stamp ? kStampSizes[widthIdx] : kPenWidths[widthIdx];
    curStroke = Stroke();
    curStroke.layerId = doc.activeLayer().id;
    curStroke.width = std::min(255, std::max(1, (int)lroundf(screenWidth * SUB / zoom)));
    Point p = screenToSub(x, y);
    curStroke.pts.push_back(p);
    penSegment(p, p);
  } else {
    doc.beginErase();
    eraseAlong(lastScreen, lastScreen);
  }
}

void continueStroke(int x, int y) {
  if (tool == Tool::Stamp) return;  // 指が滑っても線にしない
  Point p = {(int16_t)std::min(std::max(x, 0), IMG_W - 1), (int16_t)std::min(std::max(y, 0), IMG_H - 1)};
  int dx = p.x - lastScreen.x, dy = p.y - lastScreen.y;
  if (dx * dx + dy * dy < 4) return;
  if (tool == Tool::Pen) {
    Point prev = curStroke.pts.back(), sp = screenToSub(p.x, p.y);
    curStroke.pts.push_back(sp);
    penSegment(prev, sp);
  } else {
    eraseAlong(lastScreen, p);
  }
  lastScreen = p;
}

// ここでは画面を待たない。続けて次の線を書けるよう、描き直しは手が止まってから行う
void endStroke() {
  drawing = false;
  bool hadUndo = !doc.undoStack.empty();
  if (tool == Tool::Pen || tool == Tool::Stamp) {
    // 線は溜めて空いた時に描く方式で掠れなくなったので、描き直しはしない
    doc.commitStroke(std::move(curStroke));
  } else {
    doc.endErase();
  }
  if (hadUndo == doc.undoStack.empty()) undoButtonStale = true;  // 「戻す」の有効/無効が変わった
  refreshDueMs = millis() + kRefreshIdleMs;
  if (doc.dirty) scheduleSave();
}

void clampView() {
  viewX = std::min(std::max(viewX, 0.0f), IMG_W - IMG_W / zoom);
  viewY = std::min(std::max(viewY, 0.0f), IMG_H - IMG_H / zoom);
}

// 表示中の中心を保ったまま倍率を変える
void setZoom(int idx) {
  idx = std::min(std::max(idx, 0), kZoomCount - 1);
  if (idx == zoomIdx || !currentId || overlay == Overlay::Info) return;
  float cx = viewX + IMG_W / zoom / 2, cy = viewY + IMG_H / zoom / 2;
  zoomIdx = idx;
  zoom = kZoomLevels[idx];
  viewX = cx - IMG_W / zoom / 2;
  viewY = cy - IMG_H / zoom / 2;
  clampView();
  uint32_t t0 = millis();
  redrawAll();
  Serial.printf("zoom %.1f redraw %lu ms\n", zoom, millis() - t0);
}

// 表示範囲の半分ずつ動かす
void panBy(int dx, int dy) {
  float ox = viewX, oy = viewY;
  viewX += dx * IMG_W / zoom / 2;
  viewY += dy * IMG_H / zoom / 2;
  clampView();
  if (viewX != ox || viewY != oy) redrawAll();
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
  } else if (overlay == Overlay::Settings) {
    if (x >= PANEL_X) onSettingsTap(x, y);
    else closeOverlayAndRedraw();
  } else if (overlay == Overlay::Info) {
    if (currentId) closeOverlayAndRedraw();
  } else if (zoomed() && panRect().contains(x, y)) {
    int cell = (y - PAN_Y0) / PAN_H;  // 0: 倍率表示, 1: ↑, 2: ←, 3: →, 4: ↓
    const int dirs[][2] = {{0, 0}, {0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    if (cell > 0) panBy(dirs[cell][0], dirs[cell][1]);
  } else if (currentId) {
    beginStroke(x, y);
  }
}

// 表示待ちを出し切り、高速更新で荒れた部分も整える
void settleDisplay() {
  Rect r = takePending();
  if (overlay != Overlay::Info && !r.empty()) {
    M5.Display.setEpdMode(epd_mode_t::epd_text);
    pushView(r, overlay == Overlay::None ? IMG_W : PANEL_X);
  }
  M5.Display.waitDisplay();
}

void powerOff() {
  Serial.println("power off");
  M5.Display.wakeup();
  if (overlay != Overlay::None) {
    overlay = Overlay::None;  // 電源オフ中に見える画面は写真だけにしておく
    confirmDeleteIndex = -1;
    redrawAll();
  } else {
    settleDisplay();
  }
  saveNow();
  auto& st = settings::get();
  st.tool = (uint8_t)tool;
  st.widthIdx = widthIdx;
  settings::save();
  File f = LittleFS.open(kResumeFlag, FILE_WRITE);
  if (f) f.close();

  powerState = PowerState::Off;
  updateButton(B_STATUS);
  M5.Display.waitDisplay();
  web::stop();
  Serial.flush();
  // バッテリー駆動なら電源が落ちる。USB 給電中は落ちずにディープスリープになるので、押し込みで起こす
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
  M5.Power.powerOff();
}

// ライトスリープ。remainMs 後に起きなければ（タイマーで起きたら）電源オフへ進む
void enterSleep(uint32_t remainMs) {
  settleDisplay();
  saveNow();
  powerState = PowerState::Sleeping;
  updateButton(B_STATUS);
  M5.Display.waitDisplay();
  Serial.println("sleep");
  Serial.flush();

  web::stop();
  M5.Display.sleep();
  for (auto pin : kButtonPins) gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  // タッチ INT(GPIO36) でも復帰
  M5.Power.lightSleep(remainMs ? (uint64_t)remainMs * 1000 : m5::Power_Class::sleep_no_timer, true);
  for (auto pin : kButtonPins) gpio_wakeup_disable(pin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  auto cause = esp_sleep_get_wakeup_cause();
  Serial.printf("wakeup cause=%d\n", (int)cause);
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    powerOff();
    return;
  }
  M5.Display.wakeup();
  powerState = PowerState::Awake;
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
  Serial.printf("M5.begin done %lu ms\n", millis());

  if (!store::begin()) fatal("ストレージを初期化できません");
  settings::load();
  tool = settings::get().tool <= (uint8_t)Tool::Stamp ? (Tool)settings::get().tool : Tool::Pen;
  widthIdx = settings::get().widthIdx % sizeof(kPenWidths);
  // 電源オフからの再開。画面は残っているので「起動中」表示は出さない。
  // ただし IT8951 は電源断で表示状態を忘れており、部分更新すると掠れるため全面を描き直す
  bool resumed = LittleFS.exists(kResumeFlag);
  LittleFS.remove(kResumeFlag);
  if (!resumed) showBusy("起動中…");

  for (auto* c : {&baseCanvas, &composeCanvas, &viewCanvas}) {
    c->setPsram(true);
    c->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (!c->createSprite(IMG_W, IMG_H)) fatal("メモリ不足");
    c->fillScreen(TFT_WHITE);
  }
  baseCanvas.setPivot(0, 0);  // 拡大描画の基準を左上に（createSprite で中央に戻るので後で設定）

  web::begin();

  uint16_t id = store::loadCurrentId();
  auto ids = store::list();
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) id = ids.empty() ? 0 : ids.front();
  openPhoto(id);
  Serial.printf("ready %lu ms%s\n", millis(), resumed ? " (resumed)" : "");
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
  if (M5.BtnA.wasPressed()) setZoom(zoomIdx - 1);  // G37: 縮小
  if (M5.BtnC.wasPressed()) setZoom(zoomIdx + 1);  // G39: 拡大
  if (M5.BtnB.wasPressed()) setZoom(0);            // G38: 等倍

  uint32_t now = millis();
  if (infoStale) {
    infoStale = false;
    if (overlay == Overlay::Info) redrawAll(epd_mode_t::epd_fast);
  }
  if (web::state() != web::State::Off && overlay != Overlay::Info && now - lastWebMs >= kWifiOffAfterMs) {
    web::stop();
  }
  auto& st = settings::get();
  uint32_t idle = now - lastActivityMs;
  uint32_t sleepAfter = st.sleepSec * 1000;
  if (overlay == Overlay::Info) sleepAfter = std::max(sleepAfter, kSleepAfterInfoMs);
  uint32_t offAfter = st.powerOffSec * 1000;
  if (offAfter && idle >= offAfter) {
    powerOff();
    return;
  }
  if (idle >= sleepAfter) {
    enterSleep(offAfter ? offAfter - idle : 0);
    return;
  }
  if (saveDueMs && (int32_t)(now - saveDueMs) >= 0) saveNow();
  if ((int32_t)(now - statusDueMs) >= 0) {
    statusDueMs = now + 60000;
    updateButton(B_STATUS);
  }
}
