// Access point, captive-portal DNS and the web server; see web.h
//
// Every DNS name resolves to the camera and every unknown URL redirects to
// "/", so phones open the page as the network's sign-in page. The page polls
// /frame.json and draws the image itself from the raw temperatures.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "web.h"
#include "web_page.h"

static WebServer server(80);
static DNSServer dns;

static WebSnapshot snap;                  // latest frame, guarded by snapLock
static uint32_t snapSeq = 0;
static SemaphoreHandle_t snapLock;
static volatile int spotRequest = -1, freezeRequest = -1;

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
                   "\"spotT\":%.1f,\"spotX\":%.2f,\"spotY\":%.2f,\"fps\":%.1f,\"frozen\":%s,\"t\":[",
                   (unsigned long)seq, s.tMin, s.tMax, s.centre, s.spotMode,
                   s.spotT, s.spotX, s.spotY, s.fps, s.frozen ? "true" : "false");
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

static void webTask(void *) {
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

void webBegin() {
  snapLock = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : nullptr);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/frame.json", handleFrame);
  server.on("/set", handleSet);
  server.onNotFound(handleOther);
  server.begin();

  // Same priority as loop(), so the two time-slice while loop() waits on I2C
  xTaskCreate(webTask, "web", 6144, nullptr, 1, nullptr);
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
