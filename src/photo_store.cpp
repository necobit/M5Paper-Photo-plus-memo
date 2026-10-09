#include "photo_store.h"

#include <algorithm>

#include "memo_doc.h"

namespace store {

static const char* kDir = "/p";
static const char* kStateFile = "/state.txt";

bool begin() {
  if (!LittleFS.begin(false)) {
    // 初回はフォーマットが必要（12MB で数十秒かかる）
    if (!LittleFS.begin(true)) return false;
  }
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  return true;
}

String basePath(uint16_t id) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s/%04u.g4", kDir, id);
  return buf;
}

String memoPath(uint16_t id) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s/%04u.memo", kDir, id);
  return buf;
}

std::vector<uint16_t> list() {
  std::vector<uint16_t> ids;
  File dir = LittleFS.open(kDir);
  if (!dir) return ids;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.endsWith(".g4")) {
      int id = name.substring(0, name.length() - 3).toInt();
      if (id > 0) ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

// 8bit グレー → 16 階調へ Floyd-Steinberg。結果は 0..15 を buf に上書き
static void ditherTo4bit(uint8_t* buf, int w, int h) {
  std::vector<int16_t> errCur(w + 2, 0), errNext(w + 2, 0);
  for (int y = 0; y < h; ++y) {
    uint8_t* row = buf + y * w;
    std::fill(errNext.begin(), errNext.end(), 0);
    for (int x = 0; x < w; ++x) {
      int v = row[x] + errCur[x + 1] / 16;
      v = std::min(255, std::max(0, v));
      int q = (v * 15 + 127) / 255;
      int e = v - q * 17;
      row[x] = q;
      errCur[x + 2] += e * 7;
      errNext[x] += e * 3;
      errNext[x + 1] += e * 5;
      errNext[x + 2] += e * 1;
    }
    std::swap(errCur, errNext);
  }
}

uint16_t createFromJpeg(const char* jpgPath, M5Canvas& work, String& err) {
  work.fillScreen(TFT_WHITE);
  if (!work.drawJpgFile(LittleFS, jpgPath, 0, 0, IMG_W, IMG_H, 0, 0, 0.0f, 0.0f, datum_t::middle_center)) {
    err = "JPEGのデコードに失敗しました（プログレッシブJPEGは非対応）";
    return 0;
  }
  auto* buf = (uint8_t*)work.getBuffer();
  ditherTo4bit(buf, IMG_W, IMG_H);

  auto ids = list();
  uint16_t id = ids.empty() ? 1 : ids.back() + 1;
  File f = LittleFS.open(basePath(id), FILE_WRITE);
  if (!f) {
    err = "ファイルを作成できません";
    return 0;
  }
  uint8_t hdr[6] = {'G', '4', IMG_W & 0xFF, IMG_W >> 8, IMG_H & 0xFF, IMG_H >> 8};
  f.write(hdr, 6);
  uint8_t line[IMG_W / 2];
  for (int y = 0; y < IMG_H; ++y) {
    const uint8_t* row = buf + y * IMG_W;
    for (int x = 0; x < IMG_W / 2; ++x) line[x] = (row[x * 2] << 4) | row[x * 2 + 1];
    f.write(line, sizeof(line));
  }
  bool ok = f.getWriteError() == 0;
  f.close();
  if (!ok) {
    LittleFS.remove(basePath(id));
    err = "保存に失敗しました（容量不足？）";
    return 0;
  }
  return id;
}

bool loadBase(uint16_t id, M5Canvas& base) {
  File f = LittleFS.open(basePath(id), FILE_READ);
  if (!f) return false;
  uint8_t hdr[6];
  if (f.read(hdr, 6) != 6 || hdr[0] != 'G' || hdr[1] != '4' || (hdr[2] | hdr[3] << 8) != IMG_W ||
      (hdr[4] | hdr[5] << 8) != IMG_H) {
    f.close();
    return false;
  }
  auto* buf = (uint8_t*)base.getBuffer();
  uint8_t line[IMG_W / 2];
  for (int y = 0; y < IMG_H; ++y) {
    if (f.read(line, sizeof(line)) != sizeof(line)) break;
    uint8_t* row = buf + y * IMG_W;
    for (int x = 0; x < IMG_W / 2; ++x) {
      row[x * 2] = (line[x] >> 4) * 17;
      row[x * 2 + 1] = (line[x] & 0x0F) * 17;
    }
  }
  f.close();
  return true;
}

bool remove(uint16_t id) {
  LittleFS.remove(memoPath(id));
  return LittleFS.remove(basePath(id));
}

uint16_t loadCurrentId() {
  File f = LittleFS.open(kStateFile, FILE_READ);
  if (!f) return 0;
  uint16_t id = f.readString().toInt();
  f.close();
  return id;
}

void saveCurrentId(uint16_t id) {
  File f = LittleFS.open(kStateFile, FILE_WRITE);
  if (!f) return;
  f.print(id);
  f.close();
}

}  // namespace store
