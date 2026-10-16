// DIY thermal camera
// Waveshare ESP32-C6-LCD-1.47 (ST7789 172x320) + Pimoroni MLX90640 (32x24)
//
// Wiring (header pins 11-14, side by side):
//   MLX GND  -> pin 11 (GND)    MLX 3-6V -> pin 12 (3V3)
//   MLX SDA  -> pin 13 (GPIO0)  MLX SCL  -> pin 14 (GPIO1)
//
// Screen layout (portrait 172x320, held upright):
//   172x129 thermal image on top, colour bar + min/max, large centre temp.
// The sensor's 32-pixel side runs along the board's short edge, so in
// portrait the native 32x24 image fits without rotating it.

#include <Arduino.h>
#include <Wire.h>
#include <Ticker.h>
#include <Arduino_GFX_Library.h>
#include <MLX90640_API.h>
#include <MLX90640_I2C_Driver.h>

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
static const uint16_t HOT_COLOUR = RGB565_CYAN;  // not in the ironbow palette

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
static void drawHotMarker(int cx, int cy) {
  const int r = 5;
  for (int dy = -r - 1; dy <= r + 1; dy++) {
    for (int dx = -r - 1; dx <= r + 1; dx++) {
      int d2 = dx * dx + dy * dy;
      if (d2 >= r * r - r && d2 <= r * r + r) bufPixel(cx + dx, cy + dy, HOT_COLOUR);
      else if (d2 > r * r + r && d2 <= (r + 1) * (r + 1) + r + 1) bufPixel(cx + dx, cy + dy, RGB565_BLACK);
    }
  }
}

// Hottest sensor pixel (in display orientation), refined to sub-pixel with a
// weighted centroid of its 3x3 neighbourhood so the marker doesn't jump
static float findHotSpot(float &hx, float &hy) {
  int mx = 0, my = 0;
  float best = -1e6;
  for (int y = 0; y < SENS_H; y++)
    for (int x = 0; x < SENS_W; x++)
      if (px(x, y) > best) { best = px(x, y); mx = x; my = y; }

  float sw = 0, sx = 0, sy = 0;
  for (int y = max(my - 1, 0); y <= min(my + 1, SENS_H - 1); y++)
    for (int x = max(mx - 1, 0); x <= min(mx + 1, SENS_W - 1); x++) {
      float w = px(x, y) - (best - 2.0f);   // only pixels within 2 degrees count
      if (w > 0) { sw += w; sx += w * x; sy += w * y; }
    }
  hx = sx / sw;
  hy = sy / sw;
  return best;
}

static void drawStaticPanel() {
  for (int i = 0; i < BAR_W; i++) {          // cold left, hot right
    gfx->drawFastVLine(BAR_X + i, BAR_Y, BAR_H, palette[i * 255 / (BAR_W - 1)]);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_YELLOW, RGB565_BLACK);
  gfx->setCursor((SCR_W - 6 * 6) / 2, 206);
  gfx->print("centre");
}

static void drawPanel(float tMin, float tMax, float tCenter, float tHot, float fps) {
  char buf[16];
  const int labelY = BAR_Y + BAR_H + 6;

  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%-5.1f", tMin);
  gfx->setCursor(BAR_X, labelY);                  gfx->print(buf);
  snprintf(buf, sizeof(buf), "%5.1f", tMax);
  gfx->setCursor(BAR_X + BAR_W - 5 * 12, labelY); gfx->print(buf);

  gfx->setTextSize(4);
  gfx->setTextColor(RGB565_YELLOW, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%5.1f", tCenter);
  gfx->setCursor(20, 222); gfx->print(buf);
  gfx->setTextSize(2);
  gfx->setCursor(20 + 5 * 24, 222); gfx->print("C");

  gfx->setTextColor(HOT_COLOUR, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "hot %5.1f", tHot);
  gfx->setCursor((SCR_W - 9 * 12) / 2, 268); gfx->print(buf);

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY, RGB565_BLACK);
  snprintf(buf, sizeof(buf), "%.1f fps ", fps);
  gfx->setCursor(BAR_X, SCR_H - 14); gfx->print(buf);
}

// ---------- Freeze frame (BOOT button) ----------
// readFrame() blocks for most of each frame, so the button is sampled every
// 5 ms by a background timer instead of in loop(). A press counts after 8
// low samples in a row (40 ms), and the button must read high for 8 samples
// before the next press can count, so bounce on press or release is ignored.
static volatile bool holdToggled = false;
static Ticker buttonTicker;

static void pollHoldButton() {
  static uint8_t history = 0xFF;
  static bool pressed = false;
  history = (history << 1) | (digitalRead(BTN_HOLD) ? 1 : 0);
  if (!pressed && history == 0x00) {
    pressed = true;
    holdToggled = true;
  } else if (pressed && history == 0xFF) {
    pressed = false;
  }
}

static void drawHoldBadge(bool frozen) {
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

  if (!gfx->begin(80000000)) Serial.println("gfx->begin() failed");
  gfx->setRotation(SCREEN_ROTATION);  // portrait 172x320
  gfx->fillScreen(RGB565_BLACK);

  buildPalette();
  buildTaps(tapX, IMG_W, SENS_W);
  buildTaps(tapY, IMG_H, SENS_H);
  drawStaticPanel();

  imgBuf = (uint16_t *)malloc(IMG_W * IMG_H * sizeof(uint16_t));
  if (!imgBuf) fatal("No RAM for image");

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
  buttonTicker.attach_ms(5, pollHoldButton);

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

void loop() {
  static uint32_t lastMs = 0;
  static float fps = 0;
  static float sMin = 20, sMax = 30;   // smoothed range, avoids flicker
  static bool frozen = false;

  if (holdToggled) {
    holdToggled = false;
    frozen = !frozen;
    drawHoldBadge(frozen);
    if (!frozen) lastMs = 0;           // don't count the pause in the fps
  }
  if (frozen) {                        // leave the last frame on screen
    delay(20);
    return;
  }

  if (!readFrame()) {
    Serial.println("MLX90640 frame read failed");
    delay(100);
    return;
  }

  float tMin = 1e6, tMax = -1e6;
  for (float t : frame) {
    if (t < tMin) tMin = t;
    if (t > tMax) tMax = t;
  }
  if (tMax - tMin < 5) {               // keep at least a 5 degree span
    float mid = (tMax + tMin) / 2;
    tMin = mid - 2.5f;
    tMax = mid + 2.5f;
  }
  sMin += 0.3f * (tMin - sMin);
  sMax += 0.3f * (tMax - sMax);

  // Centre reading is smoothed (~0.5 s at 8 fps); the image stays raw
  static float sCenter = NAN;
  float tCenter = (px(15, 11) + px(16, 11) + px(15, 12) + px(16, 12)) / 4;
  sCenter = isnan(sCenter) ? tCenter : sCenter + 0.25f * (tCenter - sCenter);

  float hx, hy;
  float tHot = findHotSpot(hx, hy);

  renderImage(sMin, sMax);
  drawCrosshair();
  drawHotMarker((int)((hx + 0.5f) * IMG_W / SENS_W), (int)((hy + 0.5f) * IMG_H / SENS_H));
  gfx->draw16bitRGBBitmap(IMG_X, IMG_Y, imgBuf, IMG_W, IMG_H);

  uint32_t now = millis();
  if (lastMs) fps += 0.2f * (1000.0f / (now - lastMs) - fps);
  lastMs = now;

  drawPanel(sMin, sMax, sCenter, tHot, fps);
}
