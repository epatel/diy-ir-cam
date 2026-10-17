// Access point, captive-portal DNS and the web server; see web.h
//
// Every DNS name resolves to the camera and every unknown URL redirects to
// "/", so phones open the page as the network's sign-in page. The page polls
// /frame.json and draws the image itself from the raw temperatures. The
// snapshots on the SD card are listed, downloaded and deleted through the
// sd*() calls, which take the SPI bus lock themselves.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "web.h"
#include "web_page.h"
#include "sdcard.h"

static WebServer server(80);
static DNSServer dns;

static WebSnapshot snap;                  // latest frame, guarded by snapLock
static uint32_t snapSeq = 0;
static SemaphoreHandle_t snapLock;
static volatile int spotRequest = -1, freezeRequest = -1;
static volatile bool wantOn = false, saveRequest = false;

static char json[6144];                   // 768 values of up to 6 chars + fields

static void handleFrame() {
  static WebSnapshot s;                   // copy out so loop() never waits on the network
  uint32_t seq;
  xSemaphoreTake(snapLock, portMAX_DELAY);
  s = snap;
  seq = snapSeq;
  xSemaphoreGive(snapLock);

  int n = snprintf(json, sizeof(json),
                   "{\"seq\":%lu,\"min\":%.1f,\"max\":%.1f,\"centre\":%.1f,\"spot\":%u,"
                   "\"spotT\":%.1f,\"spotX\":%.2f,\"spotY\":%.2f,\"fps\":%.1f,\"frozen\":%s,"
                   "\"saveSeq\":%u,\"saved\":%d,\"t\":[",
                   (unsigned long)seq, s.tMin, s.tMax, s.centre, s.spotMode,
                   s.spotT, s.spotX, s.spotY, s.fps, s.frozen ? "true" : "false",
                   s.saveSeq, s.savedNum);
  for (int i = 0; i < 32 * 24; i++)
    n += snprintf(json + n, sizeof(json) - n, i ? ",%d" : "%d", s.t[i]);
  snprintf(json + n, sizeof(json) - n, "]}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void handleSet() {
  if (server.hasArg("spot")) {
    int v = server.arg("spot").toInt();
    if (v >= 0 && v <= 2) spotRequest = v;
  }
  if (server.hasArg("freeze")) freezeRequest = server.arg("freeze").toInt() ? 1 : 0;
  server.send(204);
}

static void handleSave() {
  saveRequest = true;
  server.send(204);
}

static void handleFiles() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", sdListJson());
}

// /file?name=IMG_0001.png, streamed in chunks so loop() gets the bus in between
static void handleFile() {
  static uint8_t buf[2048];
  const String name = server.arg("name");
  size_t size;
  if (!sdValidName(name) || !sdOpen(name, size)) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.setContentLength(size);
  server.send(200, name.endsWith(".csv") ? "text/csv" : name.endsWith(".png") ? "image/png" : "image/bmp", "");
  WiFiClient client = server.client();
  size_t n;
  while ((n = sdRead(buf, sizeof(buf))) > 0)
    if (client.write(buf, n) != n) break;
  sdClose();
}

// /delete?name=IMG_0001 removes the .png and the .csv (and a .bmp from older firmware)
static void handleDelete() {
  const String base = server.arg("name");
  bool ok = sdValidName(base) && base.indexOf('.') < 0;
  ok = ok && (sdRemove(base + ".png") | sdRemove(base + ".csv") | sdRemove(base + ".bmp"));
  server.send(ok ? 204 : 404);
}

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", WEB_PAGE);
}

// Captive-portal probes (generate_204, hotspot-detect.html, connecttest.txt,
// ...) land here; redirecting them is what makes the phone show the page
static void handleOther() {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

static void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : nullptr);
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
}

static void stopAP() {
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Starting and stopping happen here too, so the server is never torn down
// while it is handling a request
static void webTask(void *) {
  bool on = false;
  for (;;) {
    if (wantOn != on) {
      on = wantOn;
      if (on) startAP(); else stopAP();
    }
    if (on) {
      dns.processNextRequest();
      server.handleClient();
      delay(2);
    } else {
      delay(50);
    }
  }
}

void webBegin(bool enabled) {
  snapLock = xSemaphoreCreateMutex();
  server.on("/", handleRoot);
  server.on("/frame.json", handleFrame);
  server.on("/set", handleSet);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/files.json", handleFiles);
  server.on("/file", handleFile);
  server.on("/delete", HTTP_POST, handleDelete);
  server.onNotFound(handleOther);
  wantOn = enabled;

  // Same priority as loop(), so the two time-slice while loop() waits on I2C
  xTaskCreate(webTask, "web", 6144, nullptr, 1, nullptr);
}

void webSetEnabled(bool enabled) {
  wantOn = enabled;
}

void webPublish(const WebSnapshot &s) {
  xSemaphoreTake(snapLock, portMAX_DELAY);
  snap = s;
  snapSeq++;
  xSemaphoreGive(snapLock);
}

int webTakeSpotRequest() {
  int v = spotRequest;
  spotRequest = -1;
  return v;
}

int webTakeFreezeRequest() {
  int v = freezeRequest;
  freezeRequest = -1;
  return v;
}

bool webTakeSaveRequest() {
  bool v = saveRequest;
  saveRequest = false;
  return v;
}
