#include "settings.h"

#include <LittleFS.h>

namespace settings {

static const char* kFile = "/settings.txt";
static Settings current;

Settings& get() { return current; }

void load() {
  current = Settings();
  File f = LittleFS.open(kFile, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    long v = line.substring(eq + 1).toInt();
    if (key == "sleep") current.sleepSec = v;
    else if (key == "poweroff") current.powerOffSec = v;
    else if (key == "tool") current.tool = v;
    else if (key == "width") current.widthIdx = v;
  }
  f.close();
  if (current.sleepSec < 10) current.sleepSec = 10;
}

void save() {
  File f = LittleFS.open(kFile, FILE_WRITE);
  if (!f) return;
  f.printf("sleep=%u\npoweroff=%u\ntool=%u\nwidth=%u\n", (unsigned)current.sleepSec, (unsigned)current.powerOffSec,
           current.tool, current.widthIdx);
  f.close();
}

}  // namespace settings
