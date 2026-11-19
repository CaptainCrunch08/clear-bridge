/*
  GEEN1400 teacher device
  ESP32-2432S024 ("CYD"): ESP32-WROOM-32, ILI9341 240x320.
  Picture: SPI. Touch: I2C address 0x15. Board: ESP32 Dev Module.
  Partition: Default 4MB with spiffs. Library: LovyanGFX.

  The week stays in flash. The website reads it over USB after the
  class PIN and teacher code are accepted. See CLEAR_WebLink.h.
  Drawings stay on the student panel.

  Blank button: GPIO22 to GND. Buzzer: GPIO21 to GND.
  Sections: board, queue screen, week file, radio, touch.
*/

#include <SPIFFS.h>
#include "CLEAR_WebLink.h"
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <LovyanGFX.hpp>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include "CLEAR_Link.h"
#include "CLEAR_Seal.h"
#if __has_include("CLEAR_Teacher_Config.h")
#include "CLEAR_Teacher_Config.h"
#endif

#ifndef CLEAR_WIFI_SSID
#define CLEAR_WIFI_SSID ""
#endif
#ifndef CLEAR_WIFI_PASSWORD
#define CLEAR_WIFI_PASSWORD ""
#endif
#include <Wire.h>

static const int PIN_LCD_MOSI = 13;
static const int PIN_LCD_MISO = 12;
static const int PIN_LCD_SCK = 14;
static const int PIN_LCD_CS = 15;
static const int PIN_LCD_DC = 2;
static const int PIN_LCD_BL = 27;
static const int PIN_TP_SDA = 33;
static const int PIN_TP_SCL = 32;
static const int PIN_TP_RST = 25;
static const uint8_t TP_ADDR = 0x15;
static const int PIN_SD_CS = 5;
static const int PIN_BLANK = 22;
static const int PIN_BUZZ = 21;

static const int BUTTON_Y = 268;
static const int BUTTON_H = 52;
static const int BUTTON_W = 80;
static const int YES_X = 0;
static const int NO_X = 80;
static const int NEXT_X = 160;

static uint16_t COL_BG = 0xF7BD;
static uint16_t COL_FG = 0x2610;
static uint16_t COL_TRY = 0x6F9B;
static uint16_t COL_ASK = 0xFE70;
static uint16_t COL_NO = 0xFBE4;
static uint16_t COL_FOOT = 0xE73C;

// The glass stays covered until the teacher finishes both codes.
// After that, five minutes with no touch covers the student message again.
static const int MODE_MAKE_PIN = 1;
static const int MODE_MAKE_PIN_AGAIN = 2;
static const int MODE_MAKE_CODE = 3;
static const int MODE_MAKE_CODE_AGAIN = 4;
static const int MODE_ENTER_PIN = 5;
static const int MODE_ENTER_CODE = 6;
static const int MODE_QUEUE = 7;
static const int MODE_COVER = 8;
static const int MODE_PENDING = 9;
static const int MODE_TRUSTED = 10;
static const int MODE_LOOK = 11;
static const int MODE_STUDENT = 12;

static const int GEAR_X = 200;
static const int GEAR_Y = 18;
static const int GEAR_W = 36;
static const int GEAR_H = 32;
static const int BRIGHT_MIN = 20;
static const int BRIGHT_MAX = 100;
static const unsigned long PRIVACY_IDLE_MS = 5UL * 60UL * 1000UL;
static const int TRUSTED_MAX = 12;
static const int PENDING_MAX = 8;

static const char *LOG_PATH = "/clear.csv";
static const char *LOG_HEADER =
  "id,time_sent,student,feeling,choice,kind,teacher_response,time_answered\n";

static const int LIST_MAX = 12;
static const int RING_MAX = 8;
static const unsigned long BUTTON_GAP_MS = 200;

// US Mountain time, so the small screen matches a Colorado classroom.
// The file itself stores UTC. The website uses the computer's time zone.
static const char *CLOCK_TZ = "MST7MDT,M3.2.0,M11.1.0";

class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9341 _panel;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 2500000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.pin_sclk = PIN_LCD_SCK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.bus_shared = true;
      _panel.config(cfg);
    }
    {
      // PWM on the backlight pin. A higher number is brighter.
      // Channel 1 stays clear of the buzzer, which uses channel 7.
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.freq = 20000;
      cfg.pwm_channel = 1;
      cfg.invert = false;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

// The glass is the real panel. lcd is a hidden page. The picture is
// finished there, then copied once, so a new screen does not flash.
static LGFX glass;

class QuietCanvas : public lgfx::LGFX_Sprite {
 public:
  explicit QuietCanvas(LGFX *device) : lgfx::LGFX_Sprite(device), hw(device) {}

  bool beginBuffer() {
    setColorDepth(16);
    setPsram(true);
    if (!createSprite(hw->width(), hw->height())) {
      setPsram(false);
      if (!createSprite(hw->width(), hw->height())) {
        direct = true;
        return false;
      }
    }
    direct = false;
    return true;
  }

  void init() { hw->init(); }
  void setBrightness(uint8_t brightness) { hw->setBrightness(brightness); }
  void setRotation(uint_fast8_t rotation) { hw->setRotation(rotation); }
  void setFont(const lgfx::IFont *font) {
    if (direct) hw->setFont(font);
    else lgfx::LGFX_Sprite::setFont(font);
  }
  void setTextDatum(lgfx::textdatum_t datum) {
    if (direct) hw->setTextDatum(datum);
    else lgfx::LGFX_Sprite::setTextDatum(datum);
  }
  template <typename T>
  void setTextColor(const T &ink) {
    if (direct) hw->setTextColor(ink);
    else lgfx::LGFX_Sprite::setTextColor(ink);
  }
  template <typename T, typename U>
  void setTextColor(const T &ink, const U &paper) {
    if (direct) hw->setTextColor(ink, paper);
    else lgfx::LGFX_Sprite::setTextColor(ink, paper);
  }
  int32_t textWidth(const char *text) {
    return direct ? hw->textWidth(text) : lgfx::LGFX_Sprite::textWidth(text);
  }
  int32_t textWidth(const String &text) { return textWidth(text.c_str()); }
  int32_t fontHeight() {
    return direct ? hw->fontHeight() : lgfx::LGFX_Sprite::fontHeight();
  }

  template <typename T>
  void fillScreen(const T &color) {
    int w = direct ? hw->width() : lgfx::LGFX_Sprite::width();
    int h = direct ? hw->height() : lgfx::LGFX_Sprite::height();
    mark(0, 0, w, h);
    if (direct) hw->fillScreen(color);
    else lgfx::LGFX_Sprite::fillScreen(color);
  }
  template <typename T>
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, const T &color) {
    mark(x, y, w, h);
    if (direct) hw->fillRect(x, y, w, h, color);
    else lgfx::LGFX_Sprite::fillRect(x, y, w, h, color);
  }
  template <typename T>
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const T &color) {
    mark(x, y, w, h);
    if (direct) hw->fillRoundRect(x, y, w, h, r, color);
    else lgfx::LGFX_Sprite::fillRoundRect(x, y, w, h, r, color);
  }
  template <typename T>
  void fillCircle(int32_t x, int32_t y, int32_t r, const T &color) {
    mark(x - r, y - r, r * 2 + 1, r * 2 + 1);
    if (direct) hw->fillCircle(x, y, r, color);
    else lgfx::LGFX_Sprite::fillCircle(x, y, r, color);
  }
  template <typename T>
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, const T &color) {
    mark(x, y, w, h);
    if (direct) hw->drawRect(x, y, w, h, color);
    else lgfx::LGFX_Sprite::drawRect(x, y, w, h, color);
  }
  size_t drawString(const char *text, int32_t x, int32_t y) {
    int32_t w = textWidth(text);
    int32_t h = fontHeight();
    mark(x - w, y - h, w * 2 + 8, h * 3);
    if (direct) return hw->drawString(text, x, y);
    return lgfx::LGFX_Sprite::drawString(text, x, y);
  }
  size_t drawString(const String &text, int32_t x, int32_t y) {
    return drawString(text.c_str(), x, y);
  }

  void present() {
    if (direct || !dirty || getBuffer() == nullptr) {
      dirty = false;
      return;
    }
    int viewW = lgfx::LGFX_Sprite::width();
    int viewH = lgfx::LGFX_Sprite::height();
    int left = x0 < 0 ? 0 : x0;
    int top = y0 < 0 ? 0 : y0;
    int right = x1 > viewW ? viewW : x1;
    int bottom = y1 > viewH ? viewH : y1;
    dirty = false;
    x0 = viewW;
    y0 = viewH;
    x1 = 0;
    y1 = 0;
    if (right <= left || bottom <= top) {
      return;
    }
    hw->setClipRect(left, top, right - left, bottom - top);
    pushSprite(hw, 0, 0);
    hw->waitDMA();
    hw->clearClipRect();
  }

 private:
  LGFX *hw;
  bool direct = true;
  bool dirty = false;
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  void mark(int x, int y, int w, int h) {
    if (direct) {
      return;
    }
    if (w < 0) {
      x += w;
      w = -w;
    }
    if (h < 0) {
      y += h;
      h = -h;
    }
    int right = x + w + 2;
    int bottom = y + h + 2;
    if (!dirty) {
      x0 = x - 1;
      y0 = y - 1;
      x1 = right;
      y1 = bottom;
      dirty = true;
      return;
    }
    if (x - 1 < x0) x0 = x - 1;
    if (y - 1 < y0) y0 = y - 1;
    if (right > x1) x1 = right;
    if (bottom > y1) y1 = bottom;
  }
};

static QuietCanvas lcd(&glass);
static bool storageOk = false;
static int viewIndex = 0;
static int waitingCount = 0;
static ClearNote waiting[LIST_MAX];
static ClearNote noticeRing[RING_MAX];
static volatile uint8_t ringWrite = 0;
static uint8_t ringRead = 0;
static unsigned long nextWifiRetryMs = 0;
static unsigned long buttonMs = 0;
static bool touchWasDown = false;
static BLECharacteristic *replyChar = nullptr;
static Preferences privacyPrefs;
static int screenMode = MODE_ENTER_PIN;
static int lookReturn = MODE_ENTER_PIN;
static int teacherBright = 40;
static bool teacherDark = false;
static bool drawingEnabled = true;
static bool studentCalm = false;
static bool studentBridger = true;
static uint8_t studentTools = 31;
static bool studentSettingsPending = true;
static unsigned long lastSettingsBroadcastMs = 0;
static uint8_t connectedPanels = 0;
static BLEServer *teacherServer = nullptr;
static char classPin[5] = "";
static char teacherCode[5] = "";
static char pinEntry[5] = "";
static char pinFirst[5] = "";
static char privacyNote[48] = "";
static bool secretsReady = false;
static bool servicesStarted = false;
static unsigned long activityMs = 0;
static char trustedNames[TRUSTED_MAX][17];
static int trustedCount = 0;
static int trustedView = 0;
static ClearNote pendingNotes[PENDING_MAX];
static int pendingCount = 0;
static int pendingView = 0;
static String weekPlain;

static void copyField(char *dest, int size, const char *src) {
  int i = 0;
  if (src == nullptr) {
    src = "";
  }
  while (src[i] != 0 && i < size - 1) {
    dest[i] = src[i];
    i++;
  }
  dest[i] = 0;
}

static void drawWrapped(const char *text, int x, int y, int maxWidth, int bottom) {
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(COL_FG, COL_BG);
  int cursorX = x;
  int cursorY = y;
  const char *word = text;
  while (word != nullptr && *word != 0 && cursorY < bottom) {
    while (*word == ' ') {
      word++;
    }
    if (*word == 0) {
      break;
    }
    const char *end = word;
    while (*end != 0 && *end != ' ') {
      end++;
    }
    char buf[48];
    int len = end - word;
    if (len > 47) {
      len = 47;
    }
    for (int i = 0; i < len; i++) {
      buf[i] = word[i];
    }
    buf[len] = 0;
    int width = lcd.textWidth(buf);
    if (cursorX > x && cursorX + width > x + maxWidth) {
      cursorX = x;
      cursorY += 22;
    }
    if (cursorY >= bottom) {
      break;
    }
    lcd.drawString(buf, cursorX, cursorY);
    cursorX += width + lcd.textWidth(" ");
    word = end;
  }
}

static void copyPin(char *dest, const char *src) {
  for (int i = 0; i < 4; i++) {
    dest[i] = src[i];
  }
  dest[4] = 0;
}

static bool fourDigits(const char *text) {
  if (text == nullptr) {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
  }
  return text[4] == 0;
}

// The website sends the class PIN and teacher code over USB. They have
// to match the codes saved on this device. 1234 is only a screen bypass.
static bool webCodesMatch(const char *pin, const char *code) {
  return fourDigits(pin) && fourDigits(code) &&
         strcmp(pin, classPin) == 0 && strcmp(code, teacherCode) == 0;
}

static uint32_t classPasskey() {
  return (uint32_t)atoi(classPin);
}

static void clearEntry() {
  pinEntry[0] = 0;
}

static bool nameIsTrusted(const char *name) {
  for (int i = 0; i < trustedCount; i++) {
    if (strcmp(trustedNames[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static void applyLook();

static void saveTrusted() {
  String packed;
  for (int i = 0; i < trustedCount; i++) {
    if (i > 0) {
      packed += "\n";
    }
    packed += trustedNames[i];
  }
  privacyPrefs.begin("clear-teacher", false);
  privacyPrefs.putString("trusted", packed);
  privacyPrefs.end();
}

static void loadPrivacy() {
  privacyPrefs.begin("clear-teacher", true);
  String pin = privacyPrefs.getString("cpin", "");
  String code = privacyPrefs.getString("tcode", "");
  String packed = privacyPrefs.getString("trusted", "");
  teacherBright = privacyPrefs.getInt("bright", 40);
  teacherDark = privacyPrefs.getInt("dark", 0) != 0;
  drawingEnabled = privacyPrefs.getInt("draw", 1) != 0;
  studentCalm = privacyPrefs.getInt("scalm", 0) != 0;
  studentBridger = privacyPrefs.getInt("sbridger", 1) != 0;
  studentTools = (uint8_t)privacyPrefs.getInt("stools", 31);
  privacyPrefs.end();
  studentTools &= 31;
  if (teacherBright < BRIGHT_MIN) {
    teacherBright = BRIGHT_MIN;
  }
  if (teacherBright > BRIGHT_MAX) {
    teacherBright = BRIGHT_MAX;
  }
  teacherBright = (teacherBright / 10) * 10;
  applyLook();
  secretsReady = fourDigits(pin.c_str()) && fourDigits(code.c_str());
  if (secretsReady) {
    copyPin(classPin, pin.c_str());
    copyPin(teacherCode, code.c_str());
    screenMode = MODE_ENTER_PIN;
    copyField(privacyNote, sizeof(privacyNote), "Enter the class PIN");
  } else {
    classPin[0] = 0;
    teacherCode[0] = 0;
    screenMode = MODE_MAKE_PIN;
    copyField(privacyNote, sizeof(privacyNote), "Choose a class PIN");
  }
  trustedCount = 0;
  int start = 0;
  while (trustedCount < TRUSTED_MAX && start < packed.length()) {
    int end = packed.indexOf('\n', start);
    if (end < 0) {
      end = packed.length();
    }
    String name = packed.substring(start, end);
    name.trim();
    if (name.length() > 0) {
      copyField(trustedNames[trustedCount], sizeof(trustedNames[0]), name.c_str());
      trustedCount++;
    }
    start = end + 1;
  }
  clearEntry();
}

static void saveSecrets() {
  privacyPrefs.begin("clear-teacher", false);
  privacyPrefs.putString("cpin", classPin);
  privacyPrefs.putString("tcode", teacherCode);
  privacyPrefs.end();
  secretsReady = true;
}

static int findPending(const char *name) {
  for (int i = 0; i < pendingCount; i++) {
    if (strcmp(pendingNotes[i].student, name) == 0) {
      return i;
    }
  }
  return -1;
}

static void holdPending(const ClearNote *note) {
  int slot = findPending(note->student);
  if (slot < 0) {
    if (pendingCount >= PENDING_MAX) {
      slot = 0;
    } else {
      slot = pendingCount;
      pendingCount++;
    }
  }
  pendingNotes[slot] = *note;
  if (screenMode == MODE_QUEUE || screenMode == MODE_PENDING) {
    screenMode = MODE_PENDING;
    pendingView = slot;
  }
}

class ClearSecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    return classPasskey();
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    (void)pass_key;
  }
  bool onSecurityRequest() override {
    return secretsReady;
  }
  bool onConfirmPIN(uint32_t pin) override {
    return pin == classPasskey();
  }
};

static void paintTapButton(int x, const char *label, uint16_t fill);
static void finishSignIn();
static void paintGear();
static void paintStudentSettings();

static void paintPrivacyKeys() {
  static const char *labels[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "<"
  };
  const int padX = 2;
  const int padY = 96;
  const int padW = 76;
  const int padH = 50;
  const int gap = 4;
  for (int i = 0; i < 12; i++) {
    int x = padX + (i % 3) * (padW + gap);
    int y = padY + (i / 3) * (padH + gap);
    lcd.fillRoundRect(x, y, padW, padH, 10, COL_FOOT);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COL_FG, COL_FOOT);
    lcd.drawString(labels[i], x + padW / 2, y + padH / 2);
  }
}

// --- Queue screen ---
static void paintPrivacy() {
  lcd.fillScreen(COL_BG);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextColor(COL_FG, COL_BG);
  if (screenMode == MODE_PENDING) {
    lcd.setFont(&fonts::Font2);
    lcd.drawString("Allow this panel?", 12, 16);
    if (pendingCount > 0) {
      if (pendingView < 0 || pendingView >= pendingCount) {
        pendingView = 0;
      }
      lcd.setFont(&fonts::Font4);
      lcd.drawString(pendingNotes[pendingView].student, 12, 52);
      lcd.setFont(&fonts::Font2);
      char countBuf[24];
      snprintf(countBuf, sizeof(countBuf), "%d waiting", pendingCount);
      lcd.drawString(countBuf, 12, 96);
      drawWrapped("Messages stay hidden until you allow the panel.", 12, 124, 216, 240);
    }
    paintTapButton(YES_X, "Allow", COL_TRY);
    paintTapButton(NO_X, "Not now", COL_NO);
    paintTapButton(NEXT_X, "Next", COL_FOOT);
    paintGear();
    return;
  }
  if (screenMode == MODE_TRUSTED) {
    lcd.setFont(&fonts::Font2);
    lcd.drawString("Allowed panels", 12, 16);
    if (trustedCount < 1) {
      lcd.drawString("None yet", 12, 52);
    } else {
      if (trustedView < 0 || trustedView >= trustedCount) {
        trustedView = 0;
      }
      lcd.setFont(&fonts::Font4);
      lcd.drawString(trustedNames[trustedView], 12, 52);
      lcd.setFont(&fonts::Font2);
      lcd.drawString("Remove forgets this panel.", 12, 96);
    }
    paintTapButton(YES_X, "Remove", COL_NO);
    paintTapButton(NO_X, "Back", COL_FOOT);
    paintTapButton(NEXT_X, "Next", COL_TRY);
    paintGear();
    return;
  }
  const char *title = "Class PIN";
  if (screenMode == MODE_MAKE_CODE || screenMode == MODE_MAKE_CODE_AGAIN ||
      screenMode == MODE_ENTER_CODE) {
    title = "Teacher code";
  }
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(title, 16, 20);
  lcd.setFont(&fonts::Font2);
  lcd.drawString(privacyNote, 16, 52);
  lcd.setFont(&fonts::Font4);
  char dots[8] = "_ _ _ _";
  int filled = (int)strlen(pinEntry);
  if (filled > 4) {
    filled = 4;
  }
  for (int i = 0; i < filled; i++) {
    dots[i * 2] = '*';
  }
  lcd.drawString(dots, 16, 78);
  paintPrivacyKeys();
  paintGear();
}

static void acceptDigits() {
  if (!fourDigits(pinEntry)) {
    return;
  }
  if (screenMode == MODE_MAKE_PIN) {
    copyPin(pinFirst, pinEntry);
    clearEntry();
    screenMode = MODE_MAKE_PIN_AGAIN;
    copyField(privacyNote, sizeof(privacyNote), "Enter the class PIN again");
  } else if (screenMode == MODE_MAKE_PIN_AGAIN) {
    if (strcmp(pinEntry, pinFirst) != 0) {
      clearEntry();
      pinFirst[0] = 0;
      screenMode = MODE_MAKE_PIN;
      copyField(privacyNote, sizeof(privacyNote), "Those did not match");
    } else {
      copyPin(classPin, pinEntry);
      clearEntry();
      screenMode = MODE_MAKE_CODE;
      copyField(privacyNote, sizeof(privacyNote), "Choose the teacher code");
    }
  } else if (screenMode == MODE_MAKE_CODE) {
    copyPin(pinFirst, pinEntry);
    clearEntry();
    screenMode = MODE_MAKE_CODE_AGAIN;
    copyField(privacyNote, sizeof(privacyNote), "Enter the teacher code again");
  } else if (screenMode == MODE_MAKE_CODE_AGAIN) {
    if (strcmp(pinEntry, pinFirst) != 0) {
      clearEntry();
      screenMode = MODE_MAKE_CODE;
      copyField(privacyNote, sizeof(privacyNote), "Those did not match");
    } else {
      copyPin(teacherCode, pinEntry);
      clearEntry();
      saveSecrets();
      finishSignIn();
    }
  } else if (screenMode == MODE_ENTER_PIN || screenMode == MODE_COVER) {
    if (strcmp(pinEntry, classPin) != 0 && !clearTestPin(pinEntry)) {
      clearEntry();
      copyField(privacyNote, sizeof(privacyNote), "Try the class PIN again");
    } else if (screenMode == MODE_COVER) {
      clearEntry();
      screenMode = MODE_QUEUE;
      activityMs = millis();
    } else {
      clearEntry();
      screenMode = MODE_ENTER_CODE;
      copyField(privacyNote, sizeof(privacyNote), "Enter the teacher code");
    }
  } else if (screenMode == MODE_ENTER_CODE) {
    if (strcmp(pinEntry, teacherCode) != 0) {
      clearEntry();
      copyField(privacyNote, sizeof(privacyNote), "Try the teacher code again");
    } else {
      clearEntry();
      finishSignIn();
    }
  }
}

static bool privacyKeyAt(int x, int y, char *label) {
  const int padX = 2;
  const int padY = 96;
  const int padW = 76;
  const int padH = 50;
  const int gap = 4;
  static const char *labels[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "<"
  };
  for (int i = 0; i < 12; i++) {
    int keyX = padX + (i % 3) * (padW + gap);
    int keyY = padY + (i / 3) * (padH + gap);
    if (x >= keyX && x < keyX + padW && y >= keyY && y < keyY + padH) {
      *label = labels[i][0];
      return true;
    }
  }
  return false;
}

static void typePrivacyKey(char key) {
  int n = (int)strlen(pinEntry);
  if (key == 'C') {
    clearEntry();
  } else if (key == '<') {
    if (n > 0) {
      pinEntry[n - 1] = 0;
    }
  } else if (key >= '0' && key <= '9' && n < 4) {
    pinEntry[n] = key;
    pinEntry[n + 1] = 0;
  }
  if (fourDigits(pinEntry)) {
    acceptDigits();
  }
}

static void paintTapButton(int x, const char *label, uint16_t fill) {
  lcd.fillRect(x, BUTTON_Y, BUTTON_W, BUTTON_H, fill);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::Font2);
  uint16_t ink = teacherDark ? COL_FG : (uint16_t)0x0000;
  lcd.setTextColor(ink, fill);
  lcd.drawString(label, x + BUTTON_W / 2, BUTTON_Y + BUTTON_H / 2);
}

static void paintMinuteButton() {
  lcd.fillRect(NEXT_X, BUTTON_Y, BUTTON_W, BUTTON_H, COL_FOOT);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(teacherDark ? COL_FG : (uint16_t)0x0000, COL_FOOT);
  lcd.drawString("In a", NEXT_X + BUTTON_W / 2, BUTTON_Y + 18);
  lcd.drawString("minute", NEXT_X + BUTTON_W / 2, BUTTON_Y + 36);
}

static bool viewingAid() {
  return waitingCount > 0 && viewIndex >= 0 && viewIndex < waitingCount &&
         waiting[viewIndex].asksTeacher == CLEAR_ASK_AID;
}

static bool viewingZone() {
  return waitingCount > 0 && viewIndex >= 0 && viewIndex < waitingCount &&
         strcmp(waiting[viewIndex].choice, "My energy is in this zone") == 0;
}

static void paintSoonButton() {
  lcd.fillRect(0, BUTTON_Y, 240, BUTTON_H, COL_TRY);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(teacherDark ? COL_FG : (uint16_t)0x0000, COL_TRY);
  lcd.drawString("I'll be there soon", 120, BUTTON_Y + BUTTON_H / 2);
}

static bool screenBlank = false;
static bool blankWasDown = false;
static unsigned long blankMs = 0;
static int buzzStep = -1;
static unsigned long buzzUntil = 0;
static const unsigned *buzzPlan = nullptr;
static const unsigned BUZZ_ACT[] = {1000, 0};
static const unsigned BUZZ_AID[] = {140, 90, 140, 0};
static const unsigned BUZZ_HELP[] = {140, 90, 140, 90, 520, 0};

static void buzzWrite(bool on) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BUZZ, on ? 128 : 0);
#else
  ledcWrite(7, on ? 128 : 0);
#endif
}

static void startBuzz(int kind) {
  if (kind == 2) buzzPlan = BUZZ_HELP;
  else if (kind == 1) buzzPlan = BUZZ_AID;
  else buzzPlan = BUZZ_ACT;
  buzzStep = 0;
  buzzUntil = millis() + buzzPlan[0];
  buzzWrite(true);
}

static void buzzFor(const ClearNote *note) {
  if (strcmp(note->choice, "I need help") == 0 || strcmp(note->choice, "I need my teacher") == 0) {
    startBuzz(2);
  } else if (note->asksTeacher == CLEAR_ASK_AID) {
    startBuzz(1);
  } else {
    startBuzz(0);
  }
}

static void serviceBuzz() {
  if (buzzStep < 0 || buzzPlan == nullptr) {
    return;
  }
  if ((long)(millis() - buzzUntil) < 0) {
    return;
  }
  buzzStep++;
  unsigned dur = buzzPlan[buzzStep];
  if (dur == 0) {
    buzzWrite(false);
    buzzStep = -1;
    return;
  }
  buzzWrite((buzzStep % 2) == 0);
  buzzUntil = millis() + dur;
}

static void serviceBlank() {
  bool down = digitalRead(PIN_BLANK) == LOW;
  if (down && !blankWasDown && (millis() - blankMs) > 300) {
    blankMs = millis();
    screenBlank = !screenBlank;
    if (screenBlank) {
      lcd.setBrightness(0);
      lcd.fillScreen(0x0000);
    } else {
      applyBrightness();
      paintScreen();
    }
  }
  blankWasDown = down;
}

// Tell the student, and add a week-file line, without growing the note.
// Pending means the device has the request. Seen means it is on screen.
static void notifyReply(ClearNote note, uint8_t answer);

static void reportStatus(ClearNote note, uint8_t answer) {
  note.kind = CLEAR_KIND_REPLY;
  note.answer = answer;
  notifyReply(note, answer);
  appendRow(&note, clockNow());
}

// The note on the glass has been acknowledged. Ones still in the list
// stay pending until Next brings them up, or until the screen is on.
static void markCurrentSeen() {
  if (screenBlank || screenMode != MODE_QUEUE || waitingCount < 1) {
    return;
  }
  if (viewIndex < 0 || viewIndex >= waitingCount) {
    viewIndex = 0;
  }
  uint8_t state = waiting[viewIndex].answer;
  if (state == CLEAR_ANSWER_SEEN) {
    return;
  }
  if (state != CLEAR_ANSWER_NONE && state != CLEAR_ANSWER_GOT) {
    return;
  }
  waiting[viewIndex].answer = CLEAR_ANSWER_SEEN;
  reportStatus(waiting[viewIndex], CLEAR_ANSWER_SEEN);
}

// 100% on the slider is 150 of 255. Full power on this panel hurts to look at.
static int dutyForBrightness(int pct) {
  if (pct < BRIGHT_MIN) {
    pct = BRIGHT_MIN;
  }
  if (pct > BRIGHT_MAX) {
    pct = BRIGHT_MAX;
  }
  return 36 + (114 * (pct - BRIGHT_MIN)) / (BRIGHT_MAX - BRIGHT_MIN);
}

static void applyBrightness() {
  if (screenBlank) {
    lcd.setBrightness(0);
    return;
  }
  lcd.setBrightness(dutyForBrightness(teacherBright));
}

static void applyLook() {
  if (teacherDark) {
    COL_BG = 0x1082;
    COL_FG = 0xE71C;
    COL_TRY = 0x1B46;
    COL_ASK = 0xA280;
    COL_NO = 0x600C;
    COL_FOOT = 0x3186;
  } else {
    COL_BG = 0xF7BD;
    COL_FG = 0x2610;
    COL_TRY = 0x6F9B;
    COL_ASK = 0xFE70;
    COL_NO = 0xFBE4;
    COL_FOOT = 0xE73C;
  }
}

static void saveLook() {
  privacyPrefs.begin("clear-teacher", false);
  privacyPrefs.putInt("bright", teacherBright);
  privacyPrefs.putInt("dark", teacherDark ? 1 : 0);
  privacyPrefs.putInt("draw", drawingEnabled ? 1 : 0);
  privacyPrefs.putInt("scalm", studentCalm ? 1 : 0);
  privacyPrefs.putInt("sbridger", studentBridger ? 1 : 0);
  privacyPrefs.putInt("stools", studentTools);
  privacyPrefs.end();
}

static void setBright(int pct) {
  if (pct < BRIGHT_MIN) {
    pct = BRIGHT_MIN;
  }
  if (pct > BRIGHT_MAX) {
    pct = BRIGHT_MAX;
  }
  pct = (pct / 10) * 10;
  if (pct == teacherBright) {
    return;
  }
  teacherBright = pct;
  applyBrightness();
  saveLook();
}

static void paintBattery(int x, int y, uint16_t paper) {
  const int w = 18;
  const int h = 10;
  uint16_t ink = COL_FG;
  lcd.drawRect(x, y, w, h, ink);
  lcd.fillRect(x + w, y + 3, 2, 4, ink);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(ink, paper);
  lcd.drawString("USB", x + w + 4, y + h / 2);
  lcd.setTextDatum(textdatum_t::top_left);
}

static void paintGear() {
  uint16_t btn = teacherDark ? (uint16_t)0x39E7 : (uint16_t)0xDEDB;
  uint16_t metal = teacherDark ? (uint16_t)0xBDF7 : (uint16_t)0x4A69;
  lcd.fillRoundRect(GEAR_X, GEAR_Y, GEAR_W, GEAR_H, 8, btn);
  int cx = GEAR_X + GEAR_W / 2;
  int cy = GEAR_Y + GEAR_H / 2;
  for (int i = 0; i < 8; i++) {
    float angle = i * 3.14159f / 4.0f;
    int tx = cx + (int)(cosf(angle) * 6.0f);
    int ty = cy + (int)(sinf(angle) * 6.0f);
    lcd.fillCircle(tx, ty, 2, metal);
  }
  lcd.fillCircle(cx, cy, 5, metal);
  lcd.fillCircle(cx, cy, 2, btn);
  uint16_t paper = COL_BG;
  if (screenMode == MODE_QUEUE && waitingCount > 0 &&
      viewIndex >= 0 && viewIndex < waitingCount) {
    paper = waiting[viewIndex].asksTeacher ? COL_ASK : COL_TRY;
  }
  paintBattery(176, 2, paper);
}

static bool gearHit(int x, int y) {
  return x >= GEAR_X && x < GEAR_X + GEAR_W && y >= GEAR_Y && y < GEAR_Y + GEAR_H;
}

static void paintLook() {
  lcd.fillScreen(COL_BG);
  lcd.fillRoundRect(148, 8, 80, 36, 8, COL_TRY);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_FG, COL_TRY);
  lcd.drawString("Done", 188, 26);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.drawString("Look", 12, 26);
  paintBattery(88, 4, COL_BG);

  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.drawString("Brightness", 12, 68);
  lcd.fillRoundRect(8, 86, 36, 36, 8, COL_FOOT);
  lcd.fillRoundRect(196, 86, 36, 36, 8, COL_FOOT);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(COL_FG, COL_FOOT);
  lcd.drawString("-", 26, 104);
  lcd.drawString("+", 214, 104);
  lcd.fillRoundRect(52, 96, 136, 16, 6, COL_FOOT);
  int fillW = 136 * (teacherBright - BRIGHT_MIN) / (BRIGHT_MAX - BRIGHT_MIN);
  if (fillW < 0) {
    fillW = 0;
  }
  if (fillW > 136) {
    fillW = 136;
  }
  if (fillW > 0) {
    lcd.fillRoundRect(52, 96, fillW, 16, 6, COL_TRY);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", teacherBright);
  lcd.setTextDatum(textdatum_t::top_center);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.drawString(pct, 120, 136);

  uint16_t lightFill = teacherDark ? COL_FOOT : COL_TRY;
  uint16_t darkFill = teacherDark ? COL_TRY : COL_FOOT;
  lcd.fillRoundRect(12, 176, 104, 44, 8, lightFill);
  lcd.fillRoundRect(124, 176, 104, 44, 8, darkFill);
  lcd.setTextColor(COL_FG, lightFill);
  lcd.drawString("Light", 64, 198);
  lcd.setTextColor(COL_FG, darkFill);
  lcd.drawString("Dark", 176, 198);
  lcd.fillRoundRect(12, 232, 216, 44, 8, COL_FOOT);
  lcd.setTextColor(COL_FG, COL_FOOT);
  lcd.drawString("Student settings", 120, 254);
  lcd.setTextDatum(textdatum_t::top_left);
}

static bool studentToggleOn(int row) {
  if (row == 0) return drawingEnabled;
  if (row == 1) return studentCalm;
  if (row == 2) return studentBridger;
  static const uint8_t bits[] = {1, 2, 16, 4, 8};
  if (row >= 3 && row <= 7) {
    return (studentTools & bits[row - 3]) != 0;
  }
  return false;
}

static void flipStudentToggle(int row) {
  if (row == 0) drawingEnabled = !drawingEnabled;
  else if (row == 1) studentCalm = !studentCalm;
  else if (row == 2) studentBridger = !studentBridger;
  else if (row >= 3 && row <= 7) {
    static const uint8_t bits[] = {1, 2, 16, 4, 8};
    studentTools ^= bits[row - 3];
  } else {
    return;
  }
  studentSettingsPending = true;
  saveLook();
}

static void paintStudentSettings() {
  static const char *labels[] = {
    "Drawing", "Calm screen", "Bridger", "Thermometer",
    "See, hear, feel", "Breathing", "First, then", "Schedule"
  };
  lcd.fillScreen(COL_BG);
  lcd.fillRoundRect(148, 8, 80, 36, 8, COL_TRY);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_FG, COL_TRY);
  lcd.drawString("Back", 188, 26);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.drawString("Students", 12, 26);
  paintBattery(88, 4, COL_BG);
  for (int i = 0; i < 8; i++) {
    int top = 44 + i * 34;
    bool on = studentToggleOn(i);
    lcd.fillRoundRect(8, top, 224, 32, 6, COL_FOOT);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setTextColor(COL_FG, COL_FOOT);
    lcd.drawString(labels[i], 16, top + 16);
    uint16_t fill = on ? COL_TRY : COL_BG;
    lcd.fillRoundRect(156, top + 4, 70, 24, 6, fill);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(teacherDark || !on ? COL_FG : (uint16_t)0x0000, fill);
    lcd.drawString(on ? "On" : "Off", 191, top + 16);
  }
  lcd.setTextDatum(textdatum_t::top_left);
}

static void paintScreen() {
  if (screenBlank) {
    lcd.fillScreen(0x0000);
    return;
  }
  if (screenMode == MODE_LOOK) {
    paintLook();
    return;
  }
  if (screenMode == MODE_STUDENT) {
    paintStudentSettings();
    return;
  }
  if (screenMode != MODE_QUEUE) {
    paintPrivacy();
    return;
  }
  markCurrentSeen();
  lcd.fillScreen(COL_BG);
  if (viewingZone()) {
    paintSoonButton();
  } else if (viewingAid()) {
    paintTapButton(YES_X, "Accept", COL_TRY);
    paintTapButton(NO_X, "Decline", COL_NO);
    paintMinuteButton();
  } else {
    paintTapButton(YES_X, "Yes", COL_TRY);
    paintTapButton(NO_X, "No", COL_NO);
    paintTapButton(NEXT_X, "Next", COL_FOOT);
  }

  if (waitingCount < 1) {
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COL_FG, COL_BG);
    lcd.drawString("Waiting for a student", 12, 36);
    paintGear();
    return;
  }
  if (viewIndex < 0 || viewIndex >= waitingCount) {
    viewIndex = 0;
  }
  const ClearNote *note = &waiting[viewIndex];
  uint16_t band = note->asksTeacher ? COL_ASK : COL_TRY;
  lcd.fillRect(0, 0, 240, 36, band);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_FG, band);
  lcd.drawString(note->asksTeacher ? "Ask" : "Try", 12, 18);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d of %d", viewIndex + 1, waitingCount);
  lcd.setTextDatum(textdatum_t::middle_right);
  lcd.drawString(countBuf, 168, 18);

  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(note->student, 12, 58);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setFont(&fonts::Font2);
  lcd.drawString("Issue", 12, 100);
  lcd.drawString(note->feeling, 12, 122);
  lcd.drawString(note->asksTeacher ? "Request" : "Solution", 12, 156);
  drawWrapped(note->choice, 12, 178, 216, 248);
  const char *state = "Pending";
  if (note->answer == CLEAR_ANSWER_SEEN) {
    state = "Seen";
  }
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(COL_FG, COL_BG);
  if (pendingCount > 0) {
    char waitBuf[32];
    snprintf(waitBuf, sizeof(waitBuf), "%d panel%s waiting", pendingCount,
             pendingCount == 1 ? "" : "s");
    lcd.drawString(waitBuf, 12, 252);
  }
  lcd.setTextDatum(textdatum_t::top_right);
  lcd.drawString(state, 228, 252);
  paintGear();
}

static bool saveWeekPlain() {
  if (!fourDigits(classPin)) {
    File file = SPIFFS.open(LOG_PATH, FILE_WRITE);
    if (!file) {
      return false;
    }
    file.print(weekPlain);
    file.close();
    return true;
  }
  String sealed;
  if (!clearSealText(classPin, weekPlain, sealed)) {
    return false;
  }
  File file = SPIFFS.open(LOG_PATH, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print(sealed);
  file.close();
  return true;
}

static bool loadWeekPlain() {
  File file = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!file) {
    weekPlain = LOG_HEADER;
    return true;
  }
  String raw = file.readString();
  file.close();
  if (raw.startsWith("CLEARENC1")) {
    return clearOpenText(classPin, raw, weekPlain);
  }
  weekPlain = raw;
  return true;
}

static void normalizeWeekPlain() {
  int split = weekPlain.indexOf('\n');
  String header = split < 0 ? weekPlain : weekPlain.substring(0, split);
  header.trim();
  if (header.indexOf("time_answered") >= 0) {
    return;
  }
  String rest = split < 0 ? "" : weekPlain.substring(split + 1);
  String rebuilt = LOG_HEADER;
  int start = 0;
  while (start < rest.length()) {
    int end = rest.indexOf('\n', start);
    if (end < 0) {
      end = rest.length();
    }
    String line = rest.substring(start, end);
    line.trim();
    if (line.length() > 0) {
      rebuilt += line;
      rebuilt += ",\n";
    }
    if (end >= rest.length()) {
      break;
    }
    start = end + 1;
  }
  weekPlain = rebuilt;
}

static bool writeFreshLog() {
  weekPlain = LOG_HEADER;
  return saveWeekPlain();
}

// --- Week file and clock ---
// A saved time covers a reboot. Wi-Fi, when a network name is set, sets the clock.
static bool ntpStarted = false;
static bool clockSynced = false;
static unsigned long clockSaveMs = 0;

static void saveTeacherClock() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return;
  }
  privacyPrefs.begin("clear-teacher", false);
  privacyPrefs.putUInt("epoch", (uint32_t)now);
  privacyPrefs.end();
}

static void startTeacherClock() {
  setenv("TZ", CLOCK_TZ, 1);
  tzset();
  privacyPrefs.begin("clear-teacher", true);
  uint32_t saved = privacyPrefs.getUInt("epoch", 0);
  privacyPrefs.end();
  if (saved < 1700000000UL) {
    return;
  }
  struct timeval tv;
  tv.tv_sec = saved;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  clockSaveMs = millis();
}

static void serviceNetworkClock() {
  if (CLEAR_WIFI_SSID[0] == 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if ((long)(millis() - nextWifiRetryMs) >= 0) {
      WiFi.begin(CLEAR_WIFI_SSID, CLEAR_WIFI_PASSWORD);
      nextWifiRetryMs = millis() + 30000;
    }
    return;
  }
  if (!ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpStarted = true;
  }
  if (!clockSynced && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    clockSynced = true;
    saveTeacherClock();
    clockSaveMs = millis();
  }
}

// A student message carries the time of the tap. Until the network
// clock is set, this board catches up to that time. After that, the
// network time stays in charge.
static void adoptStudentClock(uint32_t timeSent) {
  if (clockSynced || timeSent < 1700000000UL) {
    return;
  }
  time_t now = time(nullptr);
  if ((uint32_t)now >= timeSent) {
    return;
  }
  struct timeval tv;
  tv.tv_sec = timeSent;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// Zero means the board does not know the time yet.
static uint32_t clockNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  return (uint32_t)now;
}

// timeAnswered is 0 for the first line of a selection. A Yes or No
// writes a second line with the same id and the time of the tap.
static void appendRow(const ClearNote *note, uint32_t timeAnswered) {
  if (!storageOk || !loadWeekPlain()) {
    return;
  }
  const char *answer = "";
  if (note->answer == CLEAR_ANSWER_YES) {
    answer = "yes";
  } else if (note->answer == CLEAR_ANSWER_NO) {
    answer = "no";
  } else if (note->answer == CLEAR_ANSWER_LATER) {
    answer = "minute";
  } else if (note->answer == CLEAR_ANSWER_GOT) {
    answer = "pending";
  } else if (note->answer == CLEAR_ANSWER_SEEN) {
    answer = "seen";
  }
  weekPlain += note->eventId;
  weekPlain += ',';
  if (note->timeSent > 0) {
    weekPlain += note->timeSent;
  }
  weekPlain += ",\"";
  weekPlain += note->student;
  weekPlain += "\",\"";
  weekPlain += note->feeling;
  weekPlain += "\",\"";
  weekPlain += note->choice;
  weekPlain += "\",";
  const char *kindWord = "try";
  if (note->asksTeacher == CLEAR_ASK_HELPED) kindWord = "helped";
  else if (note->asksTeacher == CLEAR_ASK_NOTYET) kindWord = "notyet";
  else if (note->asksTeacher) kindWord = "ask";
  weekPlain += kindWord;
  weekPlain += ',';
  weekPlain += answer;
  weekPlain += ',';
  if (timeAnswered > 0) {
    weekPlain += timeAnswered;
  }
  weekPlain += "\n";
  // Keep the newest lines when the week file gets large, so old student
  // notes do not stay on this device forever.
  if (weekPlain.length() > 48000) {
    int headerEnd = weekPlain.indexOf('\n');
    int cut = headerEnd + 1;
    int drop = (int)weekPlain.length() / 2;
    while (cut < drop) {
      int next = weekPlain.indexOf('\n', cut);
      if (next < 0) {
        break;
      }
      cut = next + 1;
    }
    if (headerEnd >= 0 && cut > headerEnd) {
      weekPlain = weekPlain.substring(0, headerEnd + 1) + weekPlain.substring(cut);
    }
  }
  saveWeekPlain();
}

// Older week files have no time_answered column. Add an empty one so
// later Yes and No lines can store a time without a mismatched header.
static void ensureLogHeader() {
  File file = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!file) {
    return;
  }
  String header = file.readStringUntil('\n');
  if (header.startsWith("CLEARENC1")) {
    file.close();
    return;
  }
  header.trim();
  if (header.indexOf("time_answered") >= 0) {
    file.close();
    return;
  }
  String rest = file.readString();
  file.close();
  File out = SPIFFS.open(LOG_PATH, FILE_WRITE);
  if (!out) {
    return;
  }
  out.print(LOG_HEADER);
  int start = 0;
  while (start < rest.length()) {
    int end = rest.indexOf('\n', start);
    if (end < 0) {
      end = rest.length();
    }
    String line = rest.substring(start, end);
    line.trim();
    if (line.length() > 0) {
      out.print(line);
      out.println(",");
    }
    if (end >= rest.length()) {
      break;
    }
    start = end + 1;
  }
  out.close();
}

static void startStorage() {
  storageOk = SPIFFS.begin(true);
  if (!storageOk) {
    return;
  }
  if (!SPIFFS.exists(LOG_PATH)) {
    storageOk = writeFreshLog();
  }
  if (storageOk) {
    ensureLogHeader();
  }
}

static void remember(const ClearNote *note) {
  for (int i = 0; i < waitingCount; i++) {
    if (waiting[i].eventId == note->eventId &&
        strcmp(waiting[i].student, note->student) == 0) {
      waiting[i] = *note;
      return;
    }
  }
  if (waitingCount >= LIST_MAX) {
    for (int i = 1; i < LIST_MAX; i++) {
      waiting[i - 1] = waiting[i];
    }
    waitingCount = LIST_MAX - 1;
    if (viewIndex > 0) {
      viewIndex--;
    }
  }
  waiting[waitingCount] = *note;
  waitingCount++;
}

static void takeNotice(const uint8_t *data, int len) {
  if (len != (int)sizeof(ClearNote)) {
    return;
  }
  ClearNote note;
  memcpy(&note, data, sizeof(note));
  note.student[sizeof(note.student) - 1] = 0;
  note.feeling[sizeof(note.feeling) - 1] = 0;
  note.choice[sizeof(note.choice) - 1] = 0;
  if (note.version != CLEAR_LINK_VERSION || note.kind != CLEAR_KIND_NOTICE) {
    return;
  }
  uint8_t next = (ringWrite + 1) % RING_MAX;
  if (next == ringRead) {
    return;
  }
  noticeRing[ringWrite] = note;
  ringWrite = next;
}

class NoticeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    size_t length = characteristic->getValue().length();
    if (length > sizeof(ClearNote)) {
      return;
    }
    takeNotice(characteristic->getData(), length);
  }
};

static NoticeCallbacks noticeCallbacks;

// --- Radio ---
static void startRadio() {
  BLEDevice::init(CLEAR_BLE_NAME);
  BLEDevice::setMTU(CLEAR_BLE_MTU);
  // Secure Connections with a passkey. Both panels use the class PIN,
  // and the bond is remembered for the next class.
  BLEDevice::setSecurityCallbacks(new ClearSecurityCallbacks());
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  BLESecurity::setKeySize(16);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setPassKey(true, classPasskey());
  teacherServer = BLEDevice::createServer();
  BLEService *service = teacherServer->createService(CLEAR_SERVICE_UUID);
  BLECharacteristic *notice = service->createCharacteristic(
    CLEAR_NOTICE_UUID, BLECharacteristic::PROPERTY_WRITE);
  notice->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  notice->setCallbacks(&noticeCallbacks);
  replyChar = service->createCharacteristic(
    CLEAR_REPLY_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  replyChar->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  replyChar->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(CLEAR_SERVICE_UUID);
  // The name and the long service id do not both fit in one advert.
  // The name set above goes in the scan reply so the student can find it.
  advertising->setScanResponse(true);
  advertising->start();
}

// Tell connected panels whether drawing is on. A newly connected
// panel gets the current choice, then the same choice repeats slowly.
static void serviceStudentSettings() {
  uint8_t connected = teacherServer == nullptr
                        ? 0
                        : (uint8_t)teacherServer->getConnectedCount();
  if (connected > 0 && connectedPanels == 0) {
    studentSettingsPending = true;
  }
  connectedPanels = connected;
  if (connected == 0 || replyChar == nullptr) {
    return;
  }
  if (!studentSettingsPending &&
      millis() - lastSettingsBroadcastMs < 5000) {
    return;
  }
  ClearStudentSettings packet = {};
  packet.version = CLEAR_LINK_VERSION;
  packet.kind = CLEAR_KIND_STUDENT_SETTINGS;
  packet.drawingEnabled = drawingEnabled ? 1 : 0;
  packet.expressOn = drawingEnabled ? 1 : 0;
  packet.calmMode = studentCalm ? 1 : 0;
  packet.bridgerOn = studentBridger ? 1 : 0;
  packet.toolMask = studentTools;
  replyChar->setValue((uint8_t *)&packet, sizeof(packet));
  replyChar->notify();
  studentSettingsPending = false;
  lastSettingsBroadcastMs = millis();
}

static void notifyReply(ClearNote note, uint8_t answer) {
  note.kind = CLEAR_KIND_REPLY;
  note.answer = answer;
  uint32_t now = clockNow();
  if (now > note.timeSent) {
    note.timeSent = now;
  }
  if (replyChar != nullptr) {
    replyChar->setValue((uint8_t *)&note, sizeof(note));
    replyChar->notify();
  }
}

static void sendReply(ClearNote note, uint8_t answer) {
  notifyReply(note, answer);
  note.kind = CLEAR_KIND_REPLY;
  note.answer = answer;
  appendRow(&note, clockNow());
}

static void answerCurrent(uint8_t answer) {
  if (waitingCount < 1 || viewIndex < 0 || viewIndex >= waitingCount) {
    return;
  }
  ClearNote note = waiting[viewIndex];
  for (int i = viewIndex + 1; i < waitingCount; i++) {
    waiting[i - 1] = waiting[i];
  }
  waitingCount--;
  viewIndex = 0;
  sendReply(note, answer);
  paintScreen();
}

static void cycleStudent() {
  if (waitingCount < 2) {
    return;
  }
  viewIndex = (viewIndex + 1) % waitingCount;
  paintScreen();
}

// Rotation 2 turns the picture right side up. The chip's raw point
// then matches the pixel under the finger.
// --- Touch ---
static bool readCapTouch(int32_t *x, int32_t *y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(TP_ADDR, (uint8_t)7) < 7) {
    return false;
  }
  uint8_t report[7];
  for (int i = 0; i < 7; i++) {
    report[i] = Wire.read();
  }
  if (report[2] == 0) {
    return false;
  }
  int rawX = ((report[3] & 0x0F) << 8) | report[4];
  int rawY = ((report[5] & 0x0F) << 8) | report[6];
  if (rawX < 0) {
    rawX = 0;
  }
  if (rawY < 0) {
    rawY = 0;
  }
  if (rawX > 239) {
    rawX = 239;
  }
  if (rawY > 319) {
    rawY = 319;
  }
  *x = rawX;
  *y = rawY;
  return true;
}

// One press counts once. A finger resting on Yes must not answer again.
static bool takeTap(int32_t *x, int32_t *y) {
  int32_t tx = 0;
  int32_t ty = 0;
  bool down = readCapTouch(&tx, &ty);
  bool hit = down && !touchWasDown && (millis() - buttonMs) >= BUTTON_GAP_MS;
  if (hit) {
    buttonMs = millis();
    *x = tx;
    *y = ty;
  }
  touchWasDown = down;
  return hit;
}

static void drainNotices() {
  while (ringRead != ringWrite) {
    ClearNote note = noticeRing[ringRead];
    ringRead = (ringRead + 1) % RING_MAX;
    if (note.asksTeacher == CLEAR_ASK_HELPED || note.asksTeacher == CLEAR_ASK_NOTYET) {
      if (nameIsTrusted(note.student)) {
        adoptStudentClock(note.timeSent);
        note.answer = CLEAR_ANSWER_NONE;
        appendRow(&note, 0);
      }
      continue;
    }
    buzzFor(&note);
    if (!nameIsTrusted(note.student)) {
      holdPending(&note);
      notifyReply(note, CLEAR_ANSWER_PENDING);
      paintScreen();
      continue;
    }
    adoptStudentClock(note.timeSent);
    note.answer = CLEAR_ANSWER_NONE;
    appendRow(&note, 0);
    note.answer = CLEAR_ANSWER_GOT;
    remember(&note);
    reportStatus(note, CLEAR_ANSWER_GOT);
    paintScreen();
  }
}

static void finishSignIn() {
  screenMode = MODE_QUEUE;
  activityMs = millis();
  if (storageOk && loadWeekPlain()) {
    normalizeWeekPlain();
    saveWeekPlain();
  }
  if (!servicesStarted) {
    startRadio();
    servicesStarted = true;
  }
  paintScreen();
}

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  delay(50);
  // The website looks for this line. Say it before the screen starts,
  // because opening the USB port can miss anything sent later.
  Serial.println("CLEAR TEACHER 1");
  setenv("TZ", CLOCK_TZ, 1);
  tzset();
  pinMode(PIN_BLANK, INPUT_PULLUP);
  pinMode(PIN_BUZZ, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZ, 2000, 8);
  ledcWrite(PIN_BUZZ, 0);
#else
  ledcSetup(7, 2000, 8);
  ledcAttachPin(PIN_BUZZ, 7);
  ledcWrite(7, 0);
#endif
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(20);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(200);
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL);
  Wire.setClock(100000);
  loadPrivacy();
  lcd.init();
  lcd.setRotation(2);
  lcd.beginBuffer();
  applyBrightness();
  startStorage();
  ClearWebLink::begin(SPIFFS, LOG_PATH, "/", webCodesMatch);
  ClearWebLink::poll();
  startTeacherClock();
  activityMs = millis();
  paintScreen();
  lcd.present();
  ClearWebLink::poll();
}

void loop() {
  lcd.present();
  ClearWebLink::poll();
  serviceBlank();
  serviceBuzz();
  if (screenBlank) {
    if (servicesStarted) {
      drainNotices();
    }
    return;
  }
  if (servicesStarted) {
      drainNotices();
      serviceStudentSettings();
      serviceNetworkClock();
    if ((long)(millis() - clockSaveMs) >= 60000) {
      saveTeacherClock();
      clockSaveMs = millis();
    }
  }
  int32_t tx = 0;
  int32_t ty = 0;
  if (!takeTap(&tx, &ty)) {
    if (screenMode == MODE_QUEUE && (millis() - activityMs) >= PRIVACY_IDLE_MS) {
      screenMode = MODE_COVER;
      clearEntry();
      copyField(privacyNote, sizeof(privacyNote), "Enter the class PIN");
      paintScreen();
    }
    return;
  }
  activityMs = millis();
  if (screenMode == MODE_LOOK) {
    if (tx >= 148 && ty < 52) {
      screenMode = lookReturn;
    } else if (ty >= 80 && ty < 140) {
      if (tx < 52) {
        setBright(teacherBright - 10);
      } else if (tx >= 188) {
        setBright(teacherBright + 10);
      } else if (tx >= 52 && tx < 188) {
        int rel = tx - 52;
        if (rel < 0) {
          rel = 0;
        }
        if (rel > 136) {
          rel = 136;
        }
        setBright(BRIGHT_MIN + rel * (BRIGHT_MAX - BRIGHT_MIN) / 136);
      }
    } else if (ty >= 232 && ty < 276) {
      screenMode = MODE_STUDENT;
    } else if (ty >= 176 && ty < 220) {
      bool nextDark = tx >= 124;
      if (nextDark != teacherDark) {
        teacherDark = nextDark;
        applyLook();
        saveLook();
      }
    }
    paintScreen();
    return;
  }
  if (screenMode == MODE_STUDENT) {
    if (tx >= 148 && ty < 52) {
      screenMode = MODE_LOOK;
    } else if (ty >= 44) {
      int row = (ty - 44) / 34;
      if (row >= 0 && row < 8 && ty < 44 + row * 34 + 32) {
        flipStudentToggle(row);
      }
    }
    paintScreen();
    return;
  }
  if (gearHit(tx, ty)) {
    lookReturn = screenMode;
    screenMode = MODE_LOOK;
    paintScreen();
    return;
  }
  if (screenMode == MODE_PENDING) {
    if (ty < BUTTON_Y) {
      return;
    }
    if (pendingCount < 1) {
      screenMode = MODE_QUEUE;
      paintScreen();
      return;
    }
    if (tx < NO_X) {
      ClearNote note = pendingNotes[pendingView];
      if (!nameIsTrusted(note.student) && trustedCount < TRUSTED_MAX) {
        copyField(trustedNames[trustedCount], sizeof(trustedNames[0]), note.student);
        trustedCount++;
        saveTrusted();
      }
      for (int i = pendingView + 1; i < pendingCount; i++) {
        pendingNotes[i - 1] = pendingNotes[i];
      }
      pendingCount--;
      if (pendingView >= pendingCount) {
        pendingView = 0;
      }
      note.answer = CLEAR_ANSWER_NONE;
      adoptStudentClock(note.timeSent);
      appendRow(&note, 0);
      remember(&note);
      if (pendingCount == 0) {
        screenMode = MODE_QUEUE;
      }
    } else if (tx < NEXT_X) {
      screenMode = MODE_QUEUE;
    } else if (pendingCount > 1) {
      pendingView = (pendingView + 1) % pendingCount;
    }
    paintScreen();
    return;
  }
  if (screenMode == MODE_TRUSTED) {
    if (ty < BUTTON_Y) {
      return;
    }
    if (tx < NO_X && trustedCount > 0) {
      for (int i = trustedView + 1; i < trustedCount; i++) {
        memcpy(trustedNames[i - 1], trustedNames[i], sizeof(trustedNames[0]));
      }
      trustedCount--;
      if (trustedView >= trustedCount) {
        trustedView = 0;
      }
      saveTrusted();
    } else if (tx < NEXT_X) {
      screenMode = MODE_QUEUE;
    } else if (trustedCount > 1) {
      trustedView = (trustedView + 1) % trustedCount;
    }
    paintScreen();
    return;
  }
  if (screenMode != MODE_QUEUE) {
    char key = 0;
    if (privacyKeyAt(tx, ty, &key)) {
      typePrivacyKey(key);
      paintScreen();
    }
    return;
  }
  if (ty >= 236 && ty < BUTTON_Y && pendingCount > 0) {
    screenMode = MODE_PENDING;
    paintScreen();
    return;
  }
  if (ty < 90 && waitingCount < 1) {
    screenMode = MODE_TRUSTED;
    paintScreen();
    return;
  }
  if (ty >= BUTTON_Y) {
    if (viewingZone()) {
      answerCurrent(CLEAR_ANSWER_LATER);
    } else if (tx < NO_X) {
      answerCurrent(CLEAR_ANSWER_YES);
    } else if (tx < NEXT_X) {
      answerCurrent(CLEAR_ANSWER_NO);
    } else if (viewingAid()) {
      answerCurrent(CLEAR_ANSWER_LATER);
    } else {
      cycleStudent();
    }
  }
}
