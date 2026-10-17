// DIY thermal camera
// Waveshare ESP32-C6-LCD-1.47 (ST7789 172x320) + Pimoroni MLX90640 (32x24)
//
// Wiring (header pins 11-14, side by side):
//   MLX GND  -> pin 11 (GND)    MLX 3-6V -> pin 12 (3V3)
//   MLX SDA  -> pin 13 (GPIO0)  MLX SCL  -> pin 14 (GPIO1)
//
// Screen layout (portrait 172x320, held upright):
//   172x129 thermal image on top, colour bar + min/max, large centre temp.
// Also a Wi-Fi access point ("IR-Cam") whose sign-in page shows the live
// image and the options; see web.cpp. Snapshots go to the microSD card
// (menu item Save, or the web page); see sdcard.cpp.
// The sensor's 32-pixel side runs along the board's short edge, so in
// portrait the native 32x24 image fits without rotating it.

#include <Arduino.h>
#include <Wire.h>
#include <Ticker.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <MLX90640_API.h>
#include <MLX90640_I2C_Driver.h>
#include "web.h"
#include "sdcard.h"

// ---------- Pins (from Waveshare schematic) ----------
#define LCD_MOSI 6
#define LCD_SCLK 7
#define LCD_CS   14
#define LCD_DC   15
#define LCD_RST  21
#define LCD_BL   22

#define BTN_HOLD 9   // BOOT button: 10k pull-up, pressed = low

#define I2C_SDA  0   // XTAL_32K pins; free since the board has no 32 kHz crystal
#define I2C_SCL  1

// ---------- Options ----------
#define SCREEN_ROTATION 0  // 0 or 2: which end of the board is up
#define FLIP_X          1  // sensor orientation for SCREEN_ROTATION 0;
#define FLIP_Y          0  //   rotation 2 flips both automatically
#define I2C_KHZ         1000

// ---------- MLX90640 settings ----------
#define MLX_ADDR     0x33
#define MLX_RATE     0x05    // subpage rate: 0x00 = 0.5 Hz, 0x04 = 8 Hz, 0x05 = 16 Hz, 0x07 = 64 Hz
#define MLX_ADC_18   0x02    // 0x00 = 16 bit ... 0x03 = 19 bit
#define EMISSIVITY   0.95f   // skin and most matte surfaces are 0.95-0.98
#define TA_SHIFT     8.0f    // Melexis: reflected temp = Ta - 8 for open air

// ---------- Geometry ----------
static const int SENS_W = 32;
static const int SENS_H = 24;
static const int SCR_W  = 172;
static const int SCR_H  = 320;
static const int IMG_W  = SCR_W;                      // 172
static const int IMG_H  = SCR_W * SENS_H / SENS_W;    // 129 (5.375x)
static const int IMG_X  = 0;
static const int IMG_Y  = 4;
static const int BAR_X  = 10;
static const int BAR_W  = SCR_W - 2 * BAR_X;
static const int BAR_Y  = IMG_Y + IMG_H + 14;
static const int BAR_H  = 10;

// ---------- Hardware objects ----------
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
// 172x320 panel sits in a 240x320 controller RAM, hence the 34px column offset
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0 /*rotation*/, true /*IPS*/,
                                      172, 320, 34, 0, 34, 0);
static paramsMLX90640 mlxParams;
static uint16_t mlxEE[832];
static uint16_t mlxData[834];               // one subpage + control/status words

static float frame[SENS_W * SENS_H];
static uint16_t *imgBuf;                    // IMG_W * IMG_H RGB565
static uint16_t palette[256];

// ---------- Ironbow palette ----------
static void buildPalette() {
  struct Stop { float pos; uint8_t r, g, b; };
  static const Stop stops[] = {
    {0.00f,   0,   0,   0},
    {0.15f,  32,   0, 140},
    {0.35f, 145,   0, 160},
    {0.55f, 230,  50,  50},
    {0.75f, 250, 150,   0},
    {0.90f, 255, 230,  60},
    {1.00f, 255, 255, 255},
  };
  const int n = sizeof(stops) / sizeof(stops[0]);
  for (int i = 0; i < 256; i++) {
    float t = i / 255.0f;
    int s = 0;
    while (s < n - 2 && t > stops[s + 1].pos) s++;
    float u = (t - stops[s].pos) / (stops[s + 1].pos - stops[s].pos);
    uint8_t r = stops[s].r + u * (stops[s + 1].r - stops[s].r);
    uint8_t g = stops[s].g + u * (stops[s + 1].g - stops[s].g);
    uint8_t b = stops[s].b + u * (stops[s + 1].b - stops[s].b);
    palette[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
}

static inline float px(int x, int y) {
  if (FLIP_X != (SCREEN_ROTATION == 2)) x = SENS_W - 1 - x;
  if (FLIP_Y != (SCREEN_ROTATION == 2)) y = SENS_H - 1 - y;
  return frame[y * SENS_W + x];
}

// ---------- Bilinear upscale 32x24 -> 172x129 into imgBuf ----------
// The ESP32-C6 has no FPU, so this runs in fixed point: sensor values are
// mapped to palette index * 256 once (768 float ops), then all 22188 output
// pixels are interpolated with integers using precomputed coordinate tables.

struct Tap { uint8_t i0, i1; uint16_t w; };   // w = weight of i1, 0..256
static Tap tapX[IMG_W], tapY[IMG_H];

static void buildTaps(Tap *taps, int outLen, int inLen) {
  for (int o = 0; o < outLen; o++) {
    float f = (o + 0.5f) * inLen / outLen - 0.5f;
    if (f < 0) f = 0;
    if (f > inLen - 1) f = inLen - 1;
    int i0 = (int)f;
    taps[o].i0 = i0;
    taps[o].i1 = min(i0 + 1, inLen - 1);
    taps[o].w = (uint16_t)((f - i0) * 256 + 0.5f);
  }
}

static void renderImage(float tMin, float tMax) {
  static int32_t q[SENS_W * SENS_H];          // palette index << 8
  const float k = 255.0f * 256.0f / max(tMax - tMin, 1.0f);
  for (int y = 0; y < SENS_H; y++)
    for (int x = 0; x < SENS_W; x++)
      q[y * SENS_W + x] = (int32_t)((px(x, y) - tMin) * k);

  int32_t col[SENS_W];
  for (int oy = 0; oy < IMG_H; oy++) {
    const Tap ty = tapY[oy];
    const int32_t *r0 = q + ty.i0 * SENS_W, *r1 = q + ty.i1 * SENS_W;
    for (int x = 0; x < SENS_W; x++) col[x] = r0[x] + (((r1[x] - r0[x]) * ty.w) >> 8);

    uint16_t *row = imgBuf + oy * IMG_W;
    for (int ox = 0; ox < IMG_W; ox++) {
      const Tap tx = tapX[ox];
      int32_t v = col[tx.i0] + (((col[tx.i1] - col[tx.i0]) * tx.w) >> 8);
      int idx = v >> 8;
      row[ox] = palette[idx < 0 ? 0 : idx > 255 ? 255 : idx];
    }
  }
}

// ---------- Overlays, drawn into imgBuf so they clip to the image ----------
static const uint16_t SPOT_COLOUR = RGB565_CYAN;  // not in the ironbow palette; green looked too dark on the panel

static inline void bufPixel(int x, int y, uint16_t c) {
  if (x >= 0 && x < IMG_W && y >= 0 && y < IMG_H) imgBuf[y * IMG_W + x] = c;
}

static void drawCrosshair() {
  const int cx = IMG_W / 2, cy = IMG_H / 2;
  for (int i = -8; i <= 8; i++) {
    bufPixel(cx + i, cy, RGB565_WHITE);
    bufPixel(cx, cy + i, RGB565_WHITE);
  }
}

// Ring of radius r, with a black ring just outside so it reads on white-hot
static void drawSpotMarker(int cx, int cy, uint16_t colour) {
  const int r = 5;
  for (int dy = -r - 1; dy <= r + 1; dy++) {
    for (int dx = -r - 1; dx <= r + 1; dx++) {
      int d2 = dx * dx + dy * dy;
      if (d2 >= r * r - r && d2 <= r * r + r) bufPixel(cx + dx, cy + dy, colour);
      else if (d2 > r * r + r && d2 <= (r + 1) * (r + 1) + r + 1) bufPixel(cx + dx, cy + dy, RGB565_BLACK);
    }
  }
}

// Hottest (sign +1) or coldest (sign -1) sensor pixel in display orientation,
// refined to sub-pixel with a weighted centroid of its 3x3 neighbourhood so
// the marker doesn't jump
static float findSpot(float sign, float &hx, float &hy) {
  int mx = 0, my = 0;
  float best = -1e6;
  for (int y = 0; y < SENS_H; y++)
    for (int x = 0; x < SENS_W; x++)
      if (sign * px(x, y) > best) { best = sign * px(x, y); mx = x; my = y; }

  float sw = 0, sx = 0, sy = 0;
  for (int y = max(my - 1, 0); y <= min(my + 1, SENS_H - 1); y++)
    for (int x = max(mx - 1, 0); x <= min(mx + 1, SENS_W - 1); x++) {
      float w = sign * px(x, y) - (best - 2.0f);   // only pixels within 2 degrees count
      if (w > 0) { sw += w; sx += w * x; sy += w * y; }
    }
  hx = sx / sw;
  hy = sy / sw;
  return sign * best;
}

static int spotToImgX(float sx) { return (int)((sx + 0.5f) * IMG_W / SENS_W); }
static int spotToImgY(float sy) { return (int)((sy + 0.5f) * IMG_H / SENS_H); }

// ---------- Live state, kept between frames so the screen can be redrawn ----------
static float sMin = 20, sMax = 30;      // smoothed range, avoids flicker
static float sCenter = NAN;
static float spotT, spotX, spotY;       // marked spot: temperature, sensor position
static float fps = 0;
static bool frozen = false;

// One spot marker at a time: none, the hottest or the coldest pixel
enum SpotMode { SPOT_OFF, SPOT_HOT, SPOT_COLD };
static SpotMode spotMode = SPOT_HOT;
static bool wifiOn = true;

// Spot mode and Wi-Fi survive a restart (NVS, written only when they change);
// freeze does not. "ver" is the layout of the stored keys: bump
// SETTINGS_VERSION when a key changes meaning, and add a step to
// migrateSettings() that converts from the previous version.
static const uint8_t SETTINGS_VERSION = 1;
static Preferences prefs;

static void saveSettings() {
  prefs.putUChar("ver", SETTINGS_VERSION);
  prefs.putUChar("spot", spotMode);
  prefs.putBool("wifi", wifiOn);
}

static void migrateSettings() {
  uint8_t ver = prefs.getUChar("ver", 0);
  if (ver == SETTINGS_VERSION) return;
  if (ver == 0 || ver > SETTINGS_VERSION) {  // nothing saved yet, or from newer firmware
    prefs.clear();
    saveSettings();                          // the defaults
    return;
  }
  // Future steps go here, e.g. if (ver == 1) { ...convert...; ver = 2; }
  prefs.putUChar("ver", SETTINGS_VERSION);
}

static void loadSettings() {
  prefs.begin("ircam");
  migrateSettings();
  uint8_t m = prefs.getUChar("spot", SPOT_HOT);
  spotMode = m <= SPOT_COLD ? (SpotMode)m : SPOT_HOT;
  wifiOn = prefs.getBool("wifi", true);
}

static const int SPOT_X = (SCR_W - 10 * 12) / 2, SPOT_Y = 268;  // 10 chars at size 2

// A short message ("saved 0007") replaces the spot readout for MSG_MS
static const uint32_t MSG_MS = 2000;
static uint32_t msgUntilMs = 0;

static void drawStaticPanel() {
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_YELLOW, RGB565_BLACK);
  gfx->setCursor((SCR_W - 6 * 6) / 2, 206);
  gfx->print("centre");
}

static void drawColourBar() {
  for (int i = 0; i < BAR_W; i++) {          // cold left, hot right
    gfx->drawFastVLine(BAR_X + i, BAR_Y, BAR_H, palette[i * 255 / (BAR_W - 1)]);
  }
}

// Min/max and fps always; the centre and spot readouts share the area the
// menu uses, so they're skipped while it is open
static void drawPanel(bool readouts) {
  char buf[16];
  const int labelY = BAR_Y + BAR_H + 6;

  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%-5.1f", sMin);
  gfx->setCursor(BAR_X, labelY);                  gfx->print(buf);
  snprintf(buf, sizeof(buf), "%5.1f", sMax);
  gfx->setCursor(BAR_X + BAR_W - 5 * 12, labelY); gfx->print(buf);

  if (readouts) {
    gfx->setTextSize(4);
    gfx->setTextColor(RGB565_YELLOW, RGB565_BLACK);
    snprintf(buf, sizeof(buf), "%5.1f", sCenter);
    gfx->setCursor(20, 222); gfx->print(buf);
    gfx->setTextSize(2);
    gfx->setCursor(20 + 5 * 24, 222); gfx->print("C");

    if (msgUntilMs) {
      // leave the message up
    } else if (spotMode == SPOT_OFF) {
      gfx->fillRect(SPOT_X, SPOT_Y, 10 * 12, 16, RGB565_BLACK);
    } else {
      const bool hot = spotMode == SPOT_HOT;
      gfx->setTextColor(SPOT_COLOUR, RGB565_BLACK);
      snprintf(buf, sizeof(buf), hot ? "hot  %5.1f" : "cold %5.1f", spotT);
      gfx->setCursor(SPOT_X, SPOT_Y); gfx->print(buf);
    }
  }

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%.1f fps ", fps);
  gfx->setCursor(BAR_X, SCR_H - 14); gfx->print(buf);
}

// Image plus overlays from the current frame[]; also used to redraw a
// frozen frame when the spot mode changes
static void drawThermal() {
  renderImage(sMin, sMax);
  drawCrosshair();
  if (spotMode != SPOT_OFF) {
    const bool hot = spotMode == SPOT_HOT;
    spotT = findSpot(hot ? 1 : -1, spotX, spotY);
    drawSpotMarker(spotToImgX(spotX), spotToImgY(spotY), SPOT_COLOUR);
  }
  gfx->draw16bitRGBBitmap(IMG_X, IMG_Y, imgBuf, IMG_W, IMG_H);
}

// ---------- BOOT button: short and long presses ----------
// readFrame() blocks for most of each frame, so the button is sampled every
// 5 ms by a background timer instead of in loop(). A press counts after 8
// low samples in a row (40 ms), and the button must read high for 8 samples
// before the next press can count, so bounce on press or release is ignored.
// A long press fires as soon as it has been held long enough; a short press
// fires on release.
static const int BTN_TICK_MS = 5;
static const int LONG_TICKS  = 600 / BTN_TICK_MS;
static volatile bool shortPress = false, longPress = false;
static Ticker buttonTicker;

static void pollButton() {
  static uint8_t history = 0xFF;
  static bool pressed = false;
  static int heldTicks = 0;
  history = (history << 1) | (digitalRead(BTN_HOLD) ? 1 : 0);
  if (!pressed && history == 0x00) {
    pressed = true;
    heldTicks = 0;
  } else if (pressed && history == 0xFF) {
    pressed = false;
    if (heldTicks < LONG_TICKS) shortPress = true;
  } else if (pressed && heldTicks < LONG_TICKS && ++heldTicks == LONG_TICKS) {
    longPress = true;
  }
}

static void drawHoldBadge() {
  const int w = 4 * 12, h = 16, x = SCR_W - BAR_X - w, y = SCR_H - 20;
  if (frozen) {
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_RED, RGB565_BLACK);
    gfx->setCursor(x, y);
    gfx->print("HOLD");
  } else {
    gfx->fillRect(x, y, w, h, RGB565_BLACK);
  }
}

// ---------- Menu: long press opens, short press steps, long press selects ----------
// Drawn over the centre/spot readouts; the image keeps running behind it.
// Save comes first, so hold-hold takes a snapshot; then the spot modes, in
// SpotMode order, then Wi-Fi. A '*' marks the current spot mode and whether
// Wi-Fi is on. Selecting Save saves, a spot mode sets it, Wi-Fi toggles it;
// any of them closes the menu. It also closes after MENU_TIMEOUT_MS without
// a press.
static const int MENU_SAVE = 0, MENU_SPOT = 1, MENU_WIFI = 4, MENU_COUNT = 5;
static const char *const MENU_LABELS[MENU_COUNT] = {"Save", "Spot off", "Hot spot", "Cold spot", "Wi-Fi"};
static const int MENU_PITCH = 18;
static const int MENU_Y = 196, MENU_H = 102;   // between the min/max labels and HOLD
static const uint32_t MENU_TIMEOUT_MS = 6000;
static bool menuOpen = false;
static int menuSel = 0;
static uint32_t menuLastMs = 0;

static void drawMenu() {
  char buf[16];
  gfx->setTextSize(2);
  for (int i = 0; i < MENU_COUNT; i++) {
    const int y = MENU_Y + 6 + i * MENU_PITCH;
    const uint16_t fg = i == menuSel ? RGB565_BLACK : RGB565_WHITE;
    const uint16_t bg = i == menuSel ? RGB565_YELLOW : RGB565_BLACK;
    gfx->fillRect(4, y - 1, SCR_W - 8, MENU_PITCH, bg);
    gfx->setTextColor(fg, bg);
    const bool active = i == MENU_SAVE ? false : i == MENU_WIFI ? wifiOn : spotMode == i - MENU_SPOT;
    snprintf(buf, sizeof(buf), "%c %s", active ? '*' : ' ', MENU_LABELS[i]);
    gfx->setCursor(14, y); gfx->print(buf);
  }
}

static void openMenu() {
  menuOpen = true;
  menuSel = 0;
  gfx->fillRect(0, MENU_Y, SCR_W, MENU_H, RGB565_BLACK);
  drawMenu();
}

static void closeMenu() {
  menuOpen = false;
  gfx->fillRect(0, MENU_Y, SCR_W, MENU_H, RGB565_BLACK);
  drawStaticPanel();
  drawPanel(true);
}

static uint16_t saveSeq = 0;            // counts save attempts, for the web page
static int16_t savedNum = 0;            // last snapshot number, -1 if that save failed

// Hand the web page a copy of the current state, after every frame and
// after any change made while frozen
static void publishWeb() {
  static WebSnapshot s;
  for (int y = 0; y < SENS_H; y++)
    for (int x = 0; x < SENS_W; x++) s.t[y * SENS_W + x] = (int16_t)lroundf(px(x, y) * 10);
  s.tMin = sMin; s.tMax = sMax; s.centre = sCenter;
  s.spotT = spotT; s.spotX = spotX; s.spotY = spotY; s.fps = fps;
  s.spotMode = spotMode;
  s.frozen = frozen;
  s.saveSeq = saveSeq;
  s.savedNum = savedNum;
  webPublish(s);
}

static void setFrozen(bool on) {
  frozen = on;
  drawHoldBadge();
  publishWeb();
}

static void setSpotMode(SpotMode m) {
  if (m != spotMode) {
    spotMode = m;
    saveSettings();
  }
  if (frozen) {                        // show the change on the held frame
    drawThermal();
    if (!menuOpen) drawPanel(true);
    publishWeb();
  }
}

// The AP name on the fps line doubles as the Wi-Fi indicator
static void drawWifiLabel() {
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY, RGB565_BLACK);
  gfx->setCursor(70, SCR_H - 14);
  if (wifiOn) gfx->print(AP_SSID);
  else gfx->fillRect(70, SCR_H - 14, 6 * strlen(AP_SSID), 8, RGB565_BLACK);
}

static void setWifi(bool on) {
  wifiOn = on;
  webSetEnabled(on);
  drawWifiLabel();
  saveSettings();
}

static void showMessage(const char *text, uint16_t colour) {
  if (menuOpen) return;                // the menu covers the spot line
  char buf[16];
  snprintf(buf, sizeof(buf), "%-10s", text);
  gfx->setTextSize(2);
  gfx->setTextColor(colour, RGB565_BLACK);
  gfx->setCursor(SPOT_X, SPOT_Y); gfx->print(buf);
  msgUntilMs = millis() + MSG_MS;
}

// Strip under the image in saved PNGs: colour bar, min/max, centre and the
// spot, like the screen. Drawn into a canvas in RAM, so it never touches SPI.
static const int FOOT_H = 35;
static Arduino_Canvas *footer;          // IMG_W x FOOT_H, framebuffer only

static void drawFooter() {
  char buf[24];
  footer->fillScreen(RGB565_BLACK);
  for (int i = 0; i < BAR_W; i++)
    footer->drawFastVLine(BAR_X + i, 3, 6, palette[i * 255 / (BAR_W - 1)]);
  footer->setTextSize(1);
  footer->setTextColor(RGB565_WHITE, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%.1f C", sMin);
  footer->setCursor(BAR_X, 13); footer->print(buf);
  snprintf(buf, sizeof(buf), "%.1f C", sMax);
  footer->setCursor(BAR_X + BAR_W - 6 * strlen(buf) + 1, 13); footer->print(buf);

  footer->setTextColor(RGB565_YELLOW, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "centre %.1f C", sCenter);
  footer->setCursor(BAR_X, 25); footer->print(buf);
  if (spotMode != SPOT_OFF) {
    footer->setTextColor(SPOT_COLOUR, RGB565_BLACK);
    snprintf(buf, sizeof(buf), "%s %.1f C", spotMode == SPOT_HOT ? "hot" : "cold", spotT);
    footer->setCursor(BAR_X + BAR_W - 6 * strlen(buf) + 1, 25); footer->print(buf);
  }
}

// The current (or frozen) frame to the SD card, as shown on screen, with the
// readouts in a strip below it and in the PNG's Comment
static void saveSnapshot() {
  static float t[SENS_W * SENS_H];      // display orientation
  for (int y = 0; y < SENS_H; y++)
    for (int x = 0; x < SENS_W; x++) t[y * SENS_W + x] = px(x, y);
  drawFooter();
  char comment[128];
  int n = snprintf(comment, sizeof(comment), "min %.1f C, max %.1f C, centre %.1f C", sMin, sMax, sCenter);
  if (spotMode != SPOT_OFF)
    snprintf(comment + n, sizeof(comment) - n, ", %s spot %.1f C at %.1f,%.1f",
             spotMode == SPOT_HOT ? "hot" : "cold", spotT, spotX, spotY);
  const SdSnapshot snap = {imgBuf, IMG_W, IMG_H, footer->getFramebuffer(), FOOT_H, t, SENS_W, SENS_H, comment};
  savedNum = sdSave(snap);
  saveSeq++;
  char buf[16];
  if (savedNum >= 0) snprintf(buf, sizeof(buf), "saved %04d", savedNum);
  showMessage(savedNum >= 0 ? buf : "no SD card", savedNum >= 0 ? RGB565_GREEN : RGB565_RED);
  publishWeb();
}

static void selectMenuItem() {
  const int sel = menuSel;
  closeMenu();
  if (sel == MENU_SAVE) saveSnapshot();
  else if (sel == MENU_WIFI) setWifi(!wifiOn);
  else setSpotMode((SpotMode)(sel - MENU_SPOT));
}

// Changes made on the web page
static void handleWeb() {
  int v = webTakeSpotRequest();
  if (v >= 0) {
    setSpotMode((SpotMode)v);
    if (menuOpen) drawMenu();          // keep the '*' right
  }
  v = webTakeFreezeRequest();
  if (v >= 0 && (bool)v != frozen) setFrozen(v);
  if (webTakeSaveRequest()) saveSnapshot();
}

// Outside the menu a short press still toggles freeze directly
static void handleButton() {
  const bool s = shortPress, l = longPress;
  shortPress = false;
  longPress = false;
  if (s || l) menuLastMs = millis();

  if (!menuOpen) {
    if (l) openMenu();
    else if (s) setFrozen(!frozen);
  } else if (l) {
    selectMenuItem();
  } else if (s) {
    menuSel = (menuSel + 1) % MENU_COUNT;
    drawMenu();
  } else if (millis() - menuLastMs > MENU_TIMEOUT_MS) {
    closeMenu();
  }

  if (msgUntilMs && (int32_t)(millis() - msgUntilMs) >= 0) {
    msgUntilMs = 0;
    if (!menuOpen) drawPanel(true);    // back to the spot readout
  }
}

static void fatal(const char *msg) {
  Serial.println(msg);
  gfx->setTextColor(RGB565_RED);
  gfx->setTextSize(2);
  gfx->setCursor(10, 70);
  gfx->print(msg);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  spiBusInit();

  if (!gfx->begin(80000000)) Serial.println("gfx->begin() failed");
  gfx->setRotation(SCREEN_ROTATION);  // portrait 172x320
  gfx->fillScreen(RGB565_BLACK);
  sdBegin(LCD_SCLK, LCD_MOSI);        // after gfx->begin(), which resets the SPI peripheral

  buildPalette();
  buildTaps(tapX, IMG_W, SENS_W);
  buildTaps(tapY, IMG_H, SENS_H);
  drawColourBar();
  drawStaticPanel();

  imgBuf = (uint16_t *)malloc(IMG_W * IMG_H * sizeof(uint16_t));
  if (!imgBuf) fatal("No RAM for image");
  footer = new Arduino_Canvas(IMG_W, FOOT_H, nullptr);
  if (!footer->begin(GFX_SKIP_OUTPUT_BEGIN)) fatal("No RAM for footer");

  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  MLX90640_I2CFreqSet(400);           // EEPROM dump at 400 kHz, then speed up

  Wire.beginTransmission(MLX_ADDR);
  if (Wire.endTransmission() != 0) fatal("MLX90640 not found");

  if (MLX90640_DumpEE(MLX_ADDR, mlxEE) != 0) fatal("EEPROM read failed");
  if (MLX90640_ExtractParameters(mlxEE, &mlxParams) != 0) fatal("Bad calibration");
  MLX90640_I2CFreqSet(I2C_KHZ);

  MLX90640_SetChessMode(MLX_ADDR);
  MLX90640_SetResolution(MLX_ADDR, MLX_ADC_18);
  MLX90640_SetRefreshRate(MLX_ADDR, MLX_RATE);  // 16 Hz subpages = ~8 full frames/s

  pinMode(BTN_HOLD, INPUT_PULLUP);
  buttonTicker.attach_ms(BTN_TICK_MS, pollButton);

  loadSettings();
  webBegin(wifiOn);
  drawWifiLabel();

  Serial.println("Thermal camera ready");
}

// A full image is two subpages (chess pattern); each call to CalculateTo
// fills in the pixels of the subpage that was just read.
static bool readFrame() {
  for (int i = 0; i < 2; i++) {
    if (MLX90640_GetFrameData(MLX_ADDR, mlxData) < 0) return false;
    float ta = MLX90640_GetTa(mlxData, &mlxParams);
    MLX90640_CalculateTo(mlxData, &mlxParams, EMISSIVITY, ta - TA_SHIFT, frame);
  }
  MLX90640_BadPixelsCorrection(mlxParams.brokenPixels, frame, 1, &mlxParams);
  MLX90640_BadPixelsCorrection(mlxParams.outlierPixels, frame, 1, &mlxParams);
  return true;
}

// loop() holds the SPI bus (LCD and SD card) except while it waits on the
// sensor, which is when the web task gets to read the card
void loop() {
  static uint32_t lastMs = 0;

  spiBusLock();
  const bool wasFrozen = frozen;
  handleButton();
  handleWeb();
  spiBusUnlock();
  if (wasFrozen && !frozen) lastMs = 0;  // don't count the pause in the fps
  if (frozen) {                          // leave the last frame on screen
    delay(20);
    return;
  }

  if (!readFrame()) {
    Serial.println("MLX90640 frame read failed");
    delay(100);
    return;
  }
  spiBusLock();

  float tMin = 1e6, tMax = -1e6;
  for (float t : frame) {
    if (t < tMin) tMin = t;
    if (t > tMax) tMax = t;
  }
  if (tMax - tMin < 5) {                 // keep at least a 5 degree span
    float mid = (tMax + tMin) / 2;
    tMin = mid - 2.5f;
    tMax = mid + 2.5f;
  }
  sMin += 0.3f * (tMin - sMin);
  sMax += 0.3f * (tMax - sMax);

  // Centre reading is smoothed (~0.5 s at 8 fps); the image stays raw
  float tCenter = (px(15, 11) + px(16, 11) + px(15, 12) + px(16, 12)) / 4;
  sCenter = isnan(sCenter) ? tCenter : sCenter + 0.25f * (tCenter - sCenter);

  drawThermal();

  uint32_t now = millis();
  if (lastMs) fps += 0.2f * (1000.0f / (now - lastMs) - fps);
  lastMs = now;

  drawPanel(!menuOpen);
  publishWeb();
  spiBusUnlock();
}
