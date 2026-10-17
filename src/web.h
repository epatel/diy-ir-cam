// Wi-Fi access point with a captive-portal page showing the live image
//
// The server runs in its own FreeRTOS task, which also starts and stops the
// access point when webSetEnabled() changes. loop() hands it a copy of each
// frame with webPublish(), and picks up changes made on the page with the
// webTake*() calls, so all drawing stays in loop().

#pragma once
#include <stdint.h>

#define AP_SSID     "IR-Cam"
#define AP_PASSWORD ""        // empty = open network; otherwise 8+ characters

struct WebSnapshot {
  int16_t t[32 * 24];         // deci-degrees C, display orientation
  float tMin, tMax;           // smoothed colour range
  float centre, spotT, spotX, spotY, fps;
  uint8_t spotMode;           // 0 off, 1 hot, 2 cold (SpotMode)
  bool frozen;
  uint16_t saveSeq;           // counts SD saves; the page refreshes its file list when it changes
  int16_t savedNum;           // last snapshot number, -1 if that save failed
};

void webBegin(bool enabled);
void webSetEnabled(bool enabled);  // AP on/off; applied by the web task
void webPublish(const WebSnapshot &snap);
int webTakeSpotRequest();     // -1 if none, else the requested SpotMode
int webTakeFreezeRequest();   // -1 if none, else 0 or 1
bool webTakeSaveRequest();    // the page asked for a snapshot to the SD card
