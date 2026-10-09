#pragma once
// 写真1枚分のメモ（レイヤー + ストローク）データモデル
#include <Arduino.h>
#include <FS.h>
#include <vector>

constexpr int IMG_W = 880;  // 画像領域（右 80px はツールバー）
constexpr int IMG_H = 540;
constexpr int MAX_MEMO_LAYERS = 6;

struct Point {
  int16_t x, y;
};

struct Rect {
  int16_t x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  bool intersects(const Rect& o) const {
    return !empty() && !o.empty() && x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
  }
  void unite(const Rect& o);
  Rect clipped(int maxW, int maxH) const;
};

struct Stroke {
  uint8_t layerId = 0;
  uint8_t width = 3;
  bool deleted = false;
  std::vector<Point> pts;
  Rect bounds;  // 線幅込みの外接矩形
  void updateBounds();
  bool hitTest(int px, int py, int radius) const;
};

struct Layer {
  uint8_t id;  // 表示名「メモ<id>」に使う。削除しても他レイヤーの番号は変わらない
  bool visible;
};

struct Action {
  enum Type : uint8_t { Add, Erase };
  Type type;
  std::vector<uint32_t> strokes;  // 対象ストロークの index
};

class MemoDoc {
 public:
  bool baseVisible = true;
  std::vector<Layer> layers;
  std::vector<Stroke> strokes;
  std::vector<Action> undoStack;
  int activeIndex = 0;  // layers 内の index
  bool dirty = false;

  void reset();  // 「メモ1」だけの空ドキュメント
  bool load(fs::FS& fs, const char* path);
  bool save(fs::FS& fs, const char* path);

  Layer& activeLayer() { return layers[activeIndex]; }
  bool isLayerVisible(uint8_t id) const;
  int addLayer();                // 追加した index、上限なら -1
  Rect removeLayer(int index);   // 再描画が必要な範囲を返す
  void commitStroke(Stroke&& s);
  void beginErase();
  Rect eraseAt(int x, int y, int radius);  // 消したストロークの範囲を返す
  void endErase();
  Rect undo();
  int visibleStrokeCount(uint8_t layerId) const;

 private:
  Action pendingErase_;
};
