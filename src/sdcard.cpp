// microSD snapshots and the shared SPI bus lock; see sdcard.h

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <soc/spi_struct.h>
#include <miniz.h>
#include "sdcard.h"

static const char *const DIR = "/ircam";
static const uint32_t SD_HZ = 20000000;

static SemaphoreHandle_t busLock;
static int busSck = -1, busMosi = -1;
static bool mounted = false;
static int nextNum = 1;
static File openFile;                       // the file being downloaded

void spiBusInit() {
  busLock = xSemaphoreCreateRecursiveMutex();
  pinMode(SD_CS, OUTPUT);                   // keep the card off the bus until it is mounted
  digitalWrite(SD_CS, HIGH);
}

void spiBusLock()   { xSemaphoreTakeRecursive(busLock, portMAX_DELAY); }
void spiBusUnlock() { xSemaphoreGiveRecursive(busLock); }

// SPI.begin() leaves the peripheral full duplex, which the card needs to read.
// The LCD driver has no MISO and fills the data buffer once to repeat a colour
// (fillRect, fillScreen); full duplex overwrites that buffer with the 0xFF read
// back from the idle MISO line, so black fills came out white. So the card
// gets full duplex only while an sd*() call is running.
static int sdDepth = 0;

static void sdEnter() {
  spiBusLock();
  if (sdDepth++ == 0) {
    GPSPI2.user.usr_miso = 1;
    GPSPI2.user.doutdin = 1;
  }
}

static void sdLeave() {
  if (--sdDepth == 0) {
    GPSPI2.user.usr_miso = 0;
    GPSPI2.user.doutdin = 0;
  }
  spiBusUnlock();
}

// Next free number after the highest IMG_nnnn already on the card
static void scanNumbers() {
  nextNum = 1;
  File dir = SD.open(DIR);
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int n;
    if (sscanf(f.name(), "IMG_%d.", &n) == 1 && n >= nextNum) nextNum = n + 1;
  }
}

bool sdBegin(int sck, int mosi) {
  sdEnter();
  busSck = sck;
  busMosi = mosi;
  SD.end();                                 // no-op unless mounted before
  SPI.begin(sck, SD_MISO, mosi, SD_CS);
  mounted = SD.begin(SD_CS, SPI, SD_HZ);
  if (mounted) {
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    scanNumbers();
  }
  sdLeave();
  Serial.printf("SD card: %s\n", mounted ? "mounted" : "not found");
  return mounted;
}

static void putBE32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// ---------- PNG writer ----------
// Uncompressed: the zlib stream is stored deflate blocks, one per row, which
// is a valid PNG about the size of a BMP (~86 KB). Real deflate (miniz in the
// C6 ROM) needs a ~165 KB compressor, more than the heap can spare next to
// Wi-Fi, and didn't shrink the image even when it got the memory. The ROM
// still provides the checksums (mz_crc32, mz_adler32).

struct PngOut {
  File *f;
  uint8_t buf[4096];                        // collects one IDAT chunk
  size_t n;
  bool ok;
};

static void writeChunk(File &f, const char *type, const uint8_t *data, size_t len, bool &ok) {
  uint8_t hdr[8], crc[4];
  putBE32(hdr, len);
  memcpy(hdr + 4, type, 4);
  putBE32(crc, mz_crc32(mz_crc32(0, hdr + 4, 4), data, len));
  ok = ok && f.write(hdr, 8) == 8 && (len == 0 || f.write(data, len) == len) && f.write(crc, 4) == 4;
}

// zlib stream in, IDAT chunks of up to 4 KB out
static void pngPut(PngOut *o, const void *data, int len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0 && o->ok) {
    const size_t k = min((size_t)len, sizeof(o->buf) - o->n);
    memcpy(o->buf + o->n, p, k);
    o->n += k; p += k; len -= k;
    if (o->n == sizeof(o->buf)) { writeChunk(*o->f, "IDAT", o->buf, o->n, o->ok); o->n = 0; }
  }
}

// 8-bit RGB PNG of the image with the footer below it (both RGB565, same
// width), plus a tEXt Comment
static bool writePng(const char *path, const SdSnapshot &s) {
  const int w = s.w, h = s.h + s.footerH;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  uint8_t ihdr[13] = {0};
  putBE32(ihdr, w);
  putBE32(ihdr + 4, h);
  ihdr[8] = 8;                              // bits per channel
  ihdr[9] = 2;                              // truecolour RGB
  static PngOut o;
  o.f = &f; o.n = 0;
  o.ok = f.write(SIG, sizeof(SIG)) == sizeof(SIG);
  writeChunk(f, "IHDR", ihdr, sizeof(ihdr), o.ok);
  if (s.comment) {
    static uint8_t text[256];
    const size_t n = snprintf((char *)text, sizeof(text), "Comment%c%s", 0, s.comment);
    writeChunk(f, "tEXt", text, min(n, sizeof(text) - 1), o.ok);
  }

  static const uint8_t ZHDR[2] = {0x78, 0x01};   // zlib header, no compression
  pngPut(&o, ZHDR, 2);
  uint32_t adler = 1;

  const int rowLen = 1 + w * 3;
  uint8_t row[rowLen];
  for (int y = 0; o.ok && y < h; y++) {
    const uint16_t *src = y < s.h ? s.image + y * w : s.footer + (y - s.h) * w;
    uint8_t *px = row + 1;
    for (int x = 0; x < w; x++) {
      const uint16_t c = src[x];
      const uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      px[x * 3 + 0] = (r << 3) | (r >> 2);
      px[x * 3 + 1] = (g << 2) | (g >> 4);
      px[x * 3 + 2] = (b << 3) | (b >> 2);
    }
    row[0] = 0;                             // PNG filter: none

    const bool last = y == h - 1;           // stored block header: final flag, length, ~length
    const uint8_t blk[5] = {(uint8_t)last, (uint8_t)rowLen, (uint8_t)(rowLen >> 8),
                            (uint8_t)~rowLen, (uint8_t)(~rowLen >> 8)};
    pngPut(&o, blk, 5);
    pngPut(&o, row, rowLen);
    adler = mz_adler32(adler, row, rowLen);
  }
  uint8_t a[4];
  putBE32(a, adler);
  pngPut(&o, a, 4);
  if (o.n) writeChunk(f, "IDAT", o.buf, o.n, o.ok);
  writeChunk(f, "IEND", nullptr, 0, o.ok);
  f.close();
  return o.ok;
}

// One line per sensor row, in deg C
static bool writeCsv(const char *path, const float *temps, int tw, int th) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  bool ok = true;
  char line[8 * 32 + 2];
  for (int y = 0; ok && y < th; y++) {
    int n = 0;
    for (int x = 0; x < tw; x++)
      n += snprintf(line + n, sizeof(line) - n, x ? ",%.2f" : "%.2f", temps[y * tw + x]);
    n += snprintf(line + n, sizeof(line) - n, "\n");
    ok = f.write((const uint8_t *)line, n) == (size_t)n;
  }
  f.close();
  return ok;
}

int sdSave(const SdSnapshot &s) {
  sdEnter();
  if (!mounted && busSck >= 0) sdBegin(busSck, busMosi);   // card inserted since boot
  int num = -1;
  if (mounted) {
    char path[32];
    snprintf(path, sizeof(path), "%s/IMG_%04d.png", DIR, nextNum);
    bool ok = writePng(path, s);
    snprintf(path, sizeof(path), "%s/IMG_%04d.csv", DIR, nextNum);
    ok = ok && writeCsv(path, s.temps, s.tw, s.th);
    if (ok) num = nextNum++;
    else mounted = false;                   // card removed or full; remount on the next save
  }
  sdLeave();
  return num;
}

String sdListJson() {
  String s = "[";
  sdEnter();
  File dir = mounted ? SD.open(DIR) : File();
  if (dir) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      if (s.length() > 1) s += ',';
      s += "{\"n\":\"";
      s += f.name();
      s += "\",\"s\":";
      s += (unsigned long)f.size();
      s += '}';
    }
    dir.close();                            // while the bus is still ours
  }
  sdLeave();
  return s + "]";
}

// Plain file names only, so a request can't leave the snapshot folder
bool sdValidName(const String &name) {
  if (name.length() == 0 || name.length() > 32 || name[0] == '.') return false;
  for (char c : name)
    if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
  return true;
}

bool sdOpen(const String &name, size_t &size) {
  sdEnter();
  openFile = mounted ? SD.open(String(DIR) + "/" + name) : File();
  const bool ok = openFile && !openFile.isDirectory();
  size = ok ? openFile.size() : 0;
  sdLeave();
  return ok;
}

size_t sdRead(uint8_t *buf, size_t len) {
  sdEnter();
  size_t n = openFile ? openFile.read(buf, len) : 0;
  sdLeave();
  return n;
}

void sdClose() {
  sdEnter();
  openFile.close();
  sdLeave();
}

bool sdRemove(const String &name) {
  sdEnter();
  const bool ok = mounted && SD.remove(String(DIR) + "/" + name);
  sdLeave();
  return ok;
}
