# wifi

Access point and captive-portal web page in `src/web.cpp` / `src/web_page.h`, interface in `src/web.h`.

## What it does

- `webBegin(enabled)` (end of `setup()`) registers the handlers and starts the web task; the task starts or stops an open access point `AP_SSID` = "IR-Cam" whenever `webSetEnabled()` changes it (menu item Wi-Fi, saved in NVS; see the display card). `AP_PASSWORD` is empty; set 8+ characters to secure it. The camera is `192.168.4.1`.
- `DNSServer` answers every name with the camera's IP, and `onNotFound` redirects every unknown URL to `/`. The OS connectivity probes (`/generate_204`, `/hotspot-detect.html`, ...) therefore fail, so phones open `/` as the network's sign-in page.
- `/` serves `WEB_PAGE`: a self-contained page (no external files: there's no internet on that network). It polls `/frame.json` every ~80 ms and draws the image itself: same ironbow stops as `buildPalette()`, browser-smoothed 32×24 → 640×480, crosshair, cyan spot ring. Buttons: spot Off/Hot/Cold, Freeze, Save image (PNG via `toDataURL`; downloads may not work inside an iOS sign-in sheet, they do in a normal browser at `http://192.168.4.1`).
- `/frame.json`: `seq`, smoothed `min`/`max`, `centre`, `spot` (SpotMode), `spotT`, `spotX`/`spotY` (sensor coords, display orientation), `fps`, `frozen`, and `t`: 768 temperatures in deci-°C, row-major in display orientation (already through `px()`). About 4 KB.
- `/set?spot=0|1|2` and `/set?freeze=0|1` → 204.

## Threading

The server runs in its own FreeRTOS task (`webTask`, priority 1 like `loop()`, so they time-slice while `loop()` waits on I²C). The same task does `startAP()`/`stopAP()`, so the server is never torn down mid-request; with Wi-Fi off it just sleeps 50 ms at a time. It never touches the display or `frame[]`:

- `loop()` calls `publishWeb()` after every frame and after any change while frozen; it copies into a `WebSnapshot` that `webPublish()` stores under a mutex. `handleFrame()` copies it out again before formatting, so `loop()` never waits on the network.
- Page actions set `volatile` request ints; `handleWeb()` in `loop()` takes them with `webTake*()` and applies them like button events (`setSpotMode`, `setFrozen`).

## Budget

With Wi-Fi the app is ~1.09 MB of the default 1.25 MB app partition (83 %); RAM 72 KB static. If it outgrows that, add `board_build.partitions = huge_app.csv` (no OTA is used). The SSID is shown in dark grey at x 70 on the fps line.
