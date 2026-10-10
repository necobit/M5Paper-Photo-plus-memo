#include "memo_doc.h"

#include <algorithm>

static const uint8_t kMagic[4] = {'M', '5', 'P', 'M'};
static constexpr uint8_t kVersion = 2;  // v1: 座標・太さが px 単位

void Rect::unite(const Rect& o) {
  if (o.empty()) return;
  if (empty()) {
    *this = o;
    return;
  }
  int16_t x1 = std::min(x, o.x), y1 = std::min(y, o.y);
  int16_t x2 = std::max<int16_t>(x + w, o.x + o.w), y2 = std::max<int16_t>(y + h, o.y + o.h);
  x = x1;
  y = y1;
  w = x2 - x1;
  h = y2 - y1;
}

Rect Rect::clipped(int maxW, int maxH) const {
  int x1 = std::max<int>(0, x), y1 = std::max<int>(0, y);
  int x2 = std::min<int>(maxW, x + w), y2 = std::min<int>(maxH, y + h);
  Rect r;
  r.x = x1;
  r.y = y1;
  r.w = std::max(0, x2 - x1);
  r.h = std::max(0, y2 - y1);
  return r;
}

void Stroke::updateBounds() {
  if (pts.empty()) {
    bounds = Rect();
    return;
  }
  int16_t x1 = pts[0].x, y1 = pts[0].y, x2 = x1, y2 = y1;
  for (auto& p : pts) {
    x1 = std::min(x1, p.x);
    y1 = std::min(y1, p.y);
    x2 = std::max(x2, p.x);
    y2 = std::max(y2, p.y);
  }
  int m = width / SUB / 2 + 2;
  bounds.x = x1 / SUB - m;
  bounds.y = y1 / SUB - m;
  bounds.w = (x2 - x1) / SUB + m * 2 + 2;
  bounds.h = (y2 - y1) / SUB + m * 2 + 2;
}

static int distSqToSegment(int px, int py, const Point& a, const Point& b) {
  int dx = b.x - a.x, dy = b.y - a.y;
  int len2 = dx * dx + dy * dy;
  float t = len2 ? std::min(1.0f, std::max(0.0f, ((px - a.x) * dx + (py - a.y) * dy) / (float)len2)) : 0.0f;
  float cx = a.x + t * dx - px, cy = a.y + t * dy - py;
  return (int)(cx * cx + cy * cy);
}

bool Stroke::hitTest(int px, int py, int radius) const {
  Rect probe;
  probe.x = px - radius;
  probe.y = py - radius;
  probe.w = probe.h = radius * 2 + 1;
  if (!bounds.intersects(probe)) return false;
  int sx = px * SUB, sy = py * SUB;
  int r = radius * SUB + width / 2;
  int r2 = r * r;
  if (pts.size() == 1) return distSqToSegment(sx, sy, pts[0], pts[0]) <= r2;
  for (size_t i = 1; i < pts.size(); ++i) {
    if (distSqToSegment(sx, sy, pts[i - 1], pts[i]) <= r2) return true;
  }
  return false;
}

void MemoDoc::reset() {
  baseVisible = true;
  layers.clear();
  layers.push_back({1, true});
  strokes.clear();
  undoStack.clear();
  activeIndex = 0;
  dirty = false;
}

bool MemoDoc::isLayerVisible(uint8_t id) const {
  for (auto& l : layers)
    if (l.id == id) return l.visible;
  return false;
}

int MemoDoc::addLayer() {
  if ((int)layers.size() >= MAX_MEMO_LAYERS) return -1;
  uint8_t id = 1;
  for (auto& l : layers) id = std::max<uint8_t>(id, l.id + 1);
  layers.push_back({id, true});
  activeIndex = layers.size() - 1;
  dirty = true;
  return activeIndex;
}

Rect MemoDoc::removeLayer(int index) {
  Rect dirtyRect;
  if (index < 0 || index >= (int)layers.size()) return dirtyRect;
  uint8_t id = layers[index].id;
  for (auto& s : strokes)
    if (s.layerId == id && !s.deleted) dirtyRect.unite(s.bounds);
  // 物理削除すると Undo の index がずれるので履歴ごと破棄する
  strokes.erase(std::remove_if(strokes.begin(), strokes.end(), [id](const Stroke& s) { return s.layerId == id; }),
                strokes.end());
  undoStack.clear();
  if (layers.size() > 1) {
    layers.erase(layers.begin() + index);
  }
  activeIndex = std::min<int>(activeIndex, layers.size() - 1);
  dirty = true;
  return dirtyRect;
}

void MemoDoc::commitStroke(Stroke&& s) {
  s.updateBounds();
  strokes.push_back(std::move(s));
  undoStack.push_back({Action::Add, {(uint32_t)(strokes.size() - 1)}});
  dirty = true;
}

void MemoDoc::beginErase() {
  pendingErase_ = {Action::Erase, {}};
}

Rect MemoDoc::eraseAt(int x, int y, int radius) {
  Rect r;
  uint8_t id = activeLayer().id;
  for (size_t i = 0; i < strokes.size(); ++i) {
    auto& s = strokes[i];
    if (s.deleted || s.layerId != id) continue;
    if (s.hitTest(x, y, radius)) {
      s.deleted = true;
      pendingErase_.strokes.push_back(i);
      r.unite(s.bounds);
    }
  }
  return r;
}

void MemoDoc::endErase() {
  if (!pendingErase_.strokes.empty()) {
    undoStack.push_back(std::move(pendingErase_));
    dirty = true;
  }
  pendingErase_ = {Action::Erase, {}};
}

Rect MemoDoc::undo() {
  Rect r;
  if (undoStack.empty()) return r;
  Action a = std::move(undoStack.back());
  undoStack.pop_back();
  for (auto i : a.strokes) {
    if (i >= strokes.size()) continue;
    strokes[i].deleted = (a.type == Action::Add);
    r.unite(strokes[i].bounds);
  }
  dirty = true;
  return r;
}

int MemoDoc::visibleStrokeCount(uint8_t layerId) const {
  int n = 0;
  for (auto& s : strokes)
    if (!s.deleted && s.layerId == layerId) ++n;
  return n;
}

// ---- 永続化 ----
// "M5PM" ver baseVisible activeIndex layerCount {id visible}*
// u32 strokeCount {layerId width u16 n {i16 x i16 y}*n}*

bool MemoDoc::save(fs::FS& fs, const char* path) {
  String tmp = String(path) + ".tmp";
  File f = fs.open(tmp, FILE_WRITE);
  if (!f) return false;
  f.write(kMagic, 4);
  uint8_t hdr[4] = {kVersion, (uint8_t)baseVisible, (uint8_t)activeIndex, (uint8_t)layers.size()};
  f.write(hdr, 4);
  for (auto& l : layers) {
    uint8_t b[2] = {l.id, (uint8_t)l.visible};
    f.write(b, 2);
  }
  uint32_t count = 0;
  for (auto& s : strokes)
    if (!s.deleted) ++count;
  f.write((uint8_t*)&count, 4);
  for (auto& s : strokes) {
    if (s.deleted) continue;
    uint16_t n = s.pts.size();
    uint8_t b[2] = {s.layerId, s.width};
    f.write(b, 2);
    f.write((uint8_t*)&n, 2);
    f.write((uint8_t*)s.pts.data(), n * sizeof(Point));
  }
  bool ok = f.getWriteError() == 0;
  f.close();
  if (!ok) {
    fs.remove(tmp);
    return false;
  }
  fs.remove(path);
  fs.rename(tmp, path);
  dirty = false;
  return true;
}

bool MemoDoc::load(fs::FS& fs, const char* path) {
  reset();
  File f = fs.open(path, FILE_READ);
  if (!f) return false;
  uint8_t magic[4], hdr[4];
  if (f.read(magic, 4) != 4 || memcmp(magic, kMagic, 4) != 0 || f.read(hdr, 4) != 4 || hdr[0] < 1 ||
      hdr[0] > kVersion || hdr[3] == 0) {
    f.close();
    return false;
  }
  baseVisible = hdr[1];
  layers.clear();
  for (int i = 0; i < hdr[3]; ++i) {
    uint8_t b[2];
    if (f.read(b, 2) != 2) break;
    layers.push_back({b[0], (bool)b[1]});
  }
  if (layers.empty()) layers.push_back({1, true});
  activeIndex = std::min<int>(hdr[2], layers.size() - 1);
  uint32_t count = 0;
  f.read((uint8_t*)&count, 4);
  strokes.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t b[2];
    uint16_t n;
    if (f.read(b, 2) != 2 || f.read((uint8_t*)&n, 2) != 2) break;
    Stroke s;
    s.layerId = b[0];
    s.width = b[1];
    s.pts.resize(n);
    if (f.read((uint8_t*)s.pts.data(), n * sizeof(Point)) != n * sizeof(Point)) break;
    if (hdr[0] == 1) {  // px 単位から SUB 単位へ
      s.width = std::min(255, s.width * SUB);
      for (auto& p : s.pts) {
        p.x *= SUB;
        p.y *= SUB;
      }
    }
    s.updateBounds();
    strokes.push_back(std::move(s));
  }
  f.close();
  dirty = hdr[0] != kVersion;  // 旧形式は次の保存で新形式に書き換える
  return true;
}
