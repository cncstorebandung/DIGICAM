#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ===== Camera pins =====
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      15
#define SIOD_GPIO_NUM       4
#define SIOC_GPIO_NUM       5
#define Y9_GPIO_NUM         16
#define Y8_GPIO_NUM         17
#define Y7_GPIO_NUM         18
#define Y6_GPIO_NUM         12
#define Y5_GPIO_NUM         10
#define Y4_GPIO_NUM          8
#define Y3_GPIO_NUM          9
#define Y2_GPIO_NUM         11
#define VSYNC_GPIO_NUM       6
#define HREF_GPIO_NUM        7
#define PCLK_GPIO_NUM       13

// ===== Display pins =====
#define TFT_SCK   21
#define TFT_MOSI  47
#define TFT_RST   45
#define TFT_DC    48

// ===== Buttons =====
#define CAPTURE_BUTTON_PIN  0
#define NEXT_BUTTON_PIN     2

// ===== Colors (RGB565) =====
#define BLACK   0x0000
#define WHITE   0xFFFF
#define GRAY    0x8410
#define DKGRAY  0x10CA
#define TEAL    0x021B
#define ORANGE  0x64BF
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, GFX_NOT_DEFINED, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RST, 0, true, 240, 240);

// ===== State machine =====
enum Mode { MENU, CAMERA, ABOUT };
Mode mode = MENU;

const char *menuItems[] = {"Kamera", "Tentang"};
const int menuCount = 2;
int menuSelect = 0;

#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0 40
int photoCount = 0;
int nextPhotoNumber = 0;
bool storageOK = false;

// ----- Proper debounce state (fixes intermittent button response) -----
bool stableCapState = HIGH, rawCapState = HIGH;
unsigned long capDebounceTimer = 0;
bool stableNextState = HIGH, rawNextState = HIGH;
unsigned long nextDebounceTimer = 0;
const unsigned long DEBOUNCE_STABLE_MS = 40;

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
  return true;
}

// ===================== Icons =====================

void drawCameraIcon(int x, int y) {
  gfx->fillRoundRect(x, y + 4, 28, 18, 3, WHITE);
  gfx->fillRoundRect(x + 8, y, 12, 6, 2, WHITE);
  gfx->fillCircle(x + 14, y + 13, 6, TEAL);
  gfx->fillCircle(x + 14, y + 13, 3, WHITE);
}

void drawAboutIcon(int x, int y) {
  gfx->drawCircle(x + 12, y + 10, 11, WHITE);
  gfx->fillCircle(x + 12, y + 5, 1, WHITE);
  gfx->fillRect(x + 11, y + 9, 2, 8, WHITE);
}

void drawHeader(const char *title) {
  gfx->fillRect(0, 0, 240, 40, TEAL);
  gfx->drawFastHLine(0, 40, 240, ORANGE);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  int textW = strlen(title) * 12;
  gfx->setCursor((240 - textW) / 2, 12);
  gfx->print(title);
}

// ===================== Screens =====================

void drawMenu() {
  gfx->fillScreen(BLACK);
  drawHeader("KAMERA DIGITAL");

  int y = 82;
  for (int i = 0; i < menuCount; i++) {
    bool sel = (i == menuSelect);
    uint16_t fg = sel ? BLACK : WHITE;

    if (sel) gfx->fillRoundRect(16, y, 208, 40, 10, ORANGE);
    else     gfx->drawRoundRect(16, y, 208, 40, 10, DKGRAY);

    if (i == 0) drawCameraIcon(28, y + 8);
    else drawAboutIcon(28, y + 8);

    gfx->setTextColor(fg);
    gfx->setTextSize(2);
    gfx->setCursor(68, y + 12);
    gfx->print(menuItems[i]);

    y += 50;
  }

  gfx->fillRect(0, 210, 240, 30, DKGRAY);
  gfx->fillTriangle(20, 220, 20, 230, 28, 225, CYAN);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(36, 220);
  gfx->print("GESER");
  gfx->fillCircle(150, 225, 5, GREEN);
  gfx->setCursor(162, 220);
  gfx->print("PILIH");
}

void drawAbout() {
  gfx->fillScreen(BLACK);
  drawHeader("TENTANG");

  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(20, 70);
  gfx->print("Kamera Digital ESP32-S3");
  gfx->setCursor(20, 90);
  gfx->setTextColor(CYAN);
  gfx->printf("Foto tersimpan: %d", photoCount);
  gfx->setTextColor(GRAY);
  gfx->setCursor(20, 120);
  gfx->print("Arduino_GFX + TJpg_Decoder");
  gfx->setCursor(20, 140);
  gfx->print("Penyimpanan SD Card");
  gfx->setCursor(20, 160);
  if (storageOK) gfx->printf("Sisa: %u KB", (unsigned)((SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1024));
  else           gfx->print("Memori tidak siap");

  gfx->fillRect(0, 210, 240, 30, DKGRAY);
  gfx->fillCircle(30, 225, 5, GREEN);
  gfx->setTextColor(WHITE);
  gfx->setCursor(42, 220);
  gfx->print("PILIH: kembali ke menu");
}

// ===================== Penyimpanan SD Card =====================
void scanPhotos() {
  photoCount = 0;
  nextPhotoNumber = 0;
  if (!storageOK) return;
  File root = SD_MMC.open("/photos");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    int us = name.lastIndexOf("img_");
    int dot = name.lastIndexOf(".jpg");
    if (!f.isDirectory() && us >= 0 && dot > us) {
      int num = name.substring(us + 4, dot).toInt();
      photoCount++;
      if (num >= nextPhotoNumber) nextPhotoNumber = num + 1;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
}

void takePhotoFreeze() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Capture FAILED"); return; }

  if (!storageOK) {
    Serial.println("Storage not mounted - photo NOT saved");
  } else if (SD_MMC.totalBytes() - SD_MMC.usedBytes() < fb->len + 8192) {
    Serial.println("Storage FULL - photo NOT saved");
  } else {
    String path = "/photos/img_" + String(nextPhotoNumber) + ".jpg";
    while (SD_MMC.exists(path.c_str())) {
      path = "/photos/img_" + String(++nextPhotoNumber) + ".jpg";
    }
    File file = SD_MMC.open(path.c_str(), FILE_WRITE);
    if (file) {
      size_t w = file.write(fb->buf, fb->len);
      file.close();
      Serial.printf("Saved %s (%u / %u bytes)\n", path.c_str(), (unsigned)w, (unsigned)fb->len);
      if (w == fb->len) {
        nextPhotoNumber++;
        photoCount++;
      } else {
        SD_MMC.remove(path.c_str());   // adhoori file hata do
        Serial.println("Incomplete write - file removed");
      }
    } else {
      Serial.println("File open FAILED: " + path);
    }
  }

  gfx->fillScreen(BLACK);
  TJpgDec.drawJpg(0, 0, fb->buf, fb->len);
  esp_camera_fb_return(fb);
  delay(2000);
}

// ===================== Mode transitions =====================

void enterMode(Mode m) {
  mode = m;
  gfx->setRotation(mode == CAMERA ? 1 : 0);
  switch (mode) {
    case CAMERA: gfx->fillScreen(BLACK); break;
    case ABOUT: scanPhotos(); drawAbout(); break;
    case MENU: drawMenu(); break;
  }
}

void handleCaptureShortPress() {
  switch (mode) {
    case MENU:
      if (menuSelect == 0) enterMode(CAMERA);
      else enterMode(ABOUT);
      break;
    case CAMERA:
      takePhotoFreeze();
      gfx->fillScreen(BLACK);
      break;
    case ABOUT:
      enterMode(MENU);
      break;
  }
}

// ===================== Setup =====================

void setup() {
  Serial.begin(115200);
  pinMode(CAPTURE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);

  gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 100);
  gfx->print("Memulai...");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tftOutput);

  camera_config_t config = {};   // zero-init (garbage fields se bachne ke liye)
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_240X240;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) Serial.printf("Camera init FAILED 0x%x (%s)\n", err, esp_err_to_name(err));
  else Serial.println("Camera init OK");

  // Slot microSD bawaan ESP32-S3 CAM, mode 1-bit (Freenove).
  storageOK = SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)
              && SD_MMC.begin("/sdcard", true, false);
  if (storageOK && SD_MMC.cardType() != CARD_NONE) {
    if (!SD_MMC.exists("/photos")) storageOK = SD_MMC.mkdir("/photos");
  } else {
    storageOK = false;
  }
  if (storageOK) {
    Serial.println("SD Card OK");
    scanPhotos();
  } else {
    Serial.println("SD Card mount FAILED");
  }

  drawMenu();
}

// ===================== Loop =====================

void loop() {
  unsigned long now = millis();

  // ===== Debounce filter for CAPTURE button =====
  bool rawCap = digitalRead(CAPTURE_BUTTON_PIN);
  if (rawCap != rawCapState) {
    capDebounceTimer = now;
    rawCapState = rawCap;
  }
  bool capChanged = false;
  if ((now - capDebounceTimer) > DEBOUNCE_STABLE_MS && rawCapState != stableCapState) {
    stableCapState = rawCapState;
    capChanged = true;
  }

  // ===== Debounce filter for NEXT button =====
  bool rawNext = digitalRead(NEXT_BUTTON_PIN);
  if (rawNext != rawNextState) {
    nextDebounceTimer = now;
    rawNextState = rawNext;
  }
  bool nextChanged = false;
  if ((now - nextDebounceTimer) > DEBOUNCE_STABLE_MS && rawNextState != stableNextState) {
    stableNextState = rawNextState;
    nextChanged = true;
  }

  // Capture saat tombol dilepas.
  if (capChanged && stableCapState == HIGH) {
    handleCaptureShortPress();
  }

  // ----- NEXT button (using debounced state) -----
  if (stableNextState == LOW && nextChanged) {
    switch (mode) {
      case MENU:
        menuSelect = (menuSelect + 1) % menuCount;
        drawMenu();
        break;
      case CAMERA:
        enterMode(MENU);
        break;
      case ABOUT:
        enterMode(MENU);
        break;
    }
  }

  // ----- Live streaming (CAMERA mode only) -----
  if (mode == CAMERA) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      TJpgDec.drawJpg(0, 0, fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
  }
}
