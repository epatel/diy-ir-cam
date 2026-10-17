// Snapshots on the microSD card (FAT32), in /ircam as IMG_nnnn.png (the
// image as on screen) plus IMG_nnnn.csv (the 32x24 temperatures)
//
// The card shares the SPI bus with the LCD, and the display library keeps its
// own bus lock, separate from the SD library's. So every user of the bus
// holds spiBusLock(): loop() while it draws, the sd* calls themselves (from
// the web task). The lock is recursive, so loop() can call sdSave() while
// holding it.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <WString.h>

#define SD_CS   4
#define SD_MISO 5

void spiBusInit();            // before anything touches the bus
void spiBusLock();
void spiBusUnlock();

bool sdBegin(int sck, int mosi);  // mount; after gfx->begin(). Retried by sdSave() if no card
struct SdSnapshot {
  const uint16_t *image; int w, h;    // RGB565, as on screen
  const uint16_t *footer; int footerH;  // RGB565, w wide, stacked under the image in the PNG
  const float *temps; int tw, th;     // deg C, row-major, for the CSV
  const char *comment;                // PNG tEXt "Comment", or nullptr
};
// Writes the next IMG_nnnn.png/.csv. Returns the snapshot number, or -1 (no
// card, write error)
int sdSave(const SdSnapshot &s);

// For the web task
String sdListJson();          // [{"n":"IMG_0001.png","s":21000},...], oldest first
bool sdValidName(const String &name);
// Opens /ircam/<name>; then sdRead() in chunks until it returns 0, then sdClose()
bool sdOpen(const String &name, size_t &size);
size_t sdRead(uint8_t *buf, size_t len);
void sdClose();
bool sdRemove(const String &name);
