#include <sha256.h>
#include "bmp280_ltsm.hpp"
#include <SPI.h>
#include <Ethernet.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

#define RELAY_ON  LOW
#define RELAY_OFF HIGH
#define PIN_BTN   A0
#define PIN_BTN2  A1
#define PIN_RED   4
#define PIN_YEL   5
#define PIN_GRN   6
#define PIN_ROT   7
#define PIN_SIR   8

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 0, 177);
IPAddress myDns(1, 1, 1, 1);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);

const char serverName[] PROGMEM = "sakura.proxy.rlwy.net";
char serverNameBuf[26];  // RAM copy for Ethernet
const int serverPort = 27373;
EthernetClient client;

const char DEVICE_ID[] PROGMEM = "ARDPB0011";
char devIdBuf[10];  // RAM copy

const uint8_t SECRET_KEY[32] PROGMEM = {
  0x5f, 0x10, 0xdd, 0xa8, 0xfe, 0x51, 0x4d, 0x04,
  0xfb, 0x0b, 0x27, 0xb2, 0x79, 0xd3, 0xac, 0xe2,
  0xa2, 0xb9, 0x5c, 0x3c, 0x7e, 0x35, 0x4d, 0x6b,
  0x06, 0xdc, 0x81, 0xdc, 0xe7, 0x80, 0xa1, 0x24
};

uint8_t I2C_ADDRESS = 0x76;
uint32_t I2C_BUS_SPEED = 100000;
BMP280_Sensor bmp280(I2C_ADDRESS, &Wire, I2C_BUS_SPEED);

unsigned long lastHB = 0;
bool isPanic = false;
bool muteSirene = false;
bool muteRotator = false;
bool resetLocked = false;
uint8_t failCount = 0;
bool srvDown = false;

// --- LCD display state ---
char lcdMsg[64];            // Current message for row 1
char lcdSrvMsg[64];         // Server-sent message (low priority)
uint8_t lcdMsgLen = 0;      // Cached strlen of lcdMsg
int8_t  lcdScrollPos = 0;   // Current scroll offset
unsigned long lastScrollT = 0;
char lcdPrevRow0[17];       // Previous row 0 content (flicker prevention)
char lcdPrevRow1[17];       // Previous row 1 content (flicker prevention)
double lastTemp = 0.0;      // Cached temperature for LCD
bool btnOpen = false;       // Debounced PIN_BTN state (true = circuit open)
bool jackDisconnected = false; // Debounced PIN_BTN2 state (true = 3.5mm jack disconnected)

// --- Helpers to save flash ---

// Tiny hex byte converter — avoids pulling in full sprintf/printf formatter
static void hexByte(char* out, uint8_t b) {
  static const char h[] PROGMEM = "0123456789abcdef";
  out[0] = pgm_read_byte(&h[b >> 4]);
  out[1] = pgm_read_byte(&h[b & 0x0F]);
}

// Append a PROGMEM string to dst (returns pointer to new end)
static char* appendP(char* dst, const char* pstr) {
  char c;
  while ((c = pgm_read_byte(pstr++))) *dst++ = c;
  *dst = '\0';
  return dst;
}

// Append a RAM string to dst
static char* appendR(char* dst, const char* s) {
  while (*s) *dst++ = *s++;
  *dst = '\0';
  return dst;
}

// Append "true" or "false" based on condition
static char* appendBool(char* dst, bool val) {
  return val ? appendP(dst, PSTR("true")) : appendP(dst, PSTR("false"));
}

// Set all alarm outputs at once
static void setAlarmOutputs(uint8_t red, uint8_t yel, uint8_t grn, uint8_t sir, uint8_t rot) {
  digitalWrite(PIN_RED, red);
  digitalWrite(PIN_YEL, yel);
  digitalWrite(PIN_GRN, grn);
  digitalWrite(PIN_SIR, sir);
  digitalWrite(PIN_ROT, rot);
}

// --- LCD functions ---

// Set a new LCD message and reset scroll position
void setLcdMessage(const char* msg) {
  if (strcmp(lcdMsg, msg) == 0) return;  // No change
  strncpy(lcdMsg, msg, sizeof(lcdMsg) - 1);
  lcdMsg[sizeof(lcdMsg) - 1] = '\0';
  lcdMsgLen = strlen(lcdMsg);
  lcdScrollPos = 0;
  lastScrollT = millis();
}

// Set LCD message from PROGMEM
void setLcdMessage_P(const char* pmsg) {
  char tmp[64];
  strncpy_P(tmp, pmsg, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  setLcdMessage(tmp);
}

// Determine the current message based on system state priorities
void refreshLcdMessage() {
  if (jackDisconnected) {
    setLcdMessage_P(PSTR("Jack Disconnected!"));
  } else if (isPanic) {
    setLcdMessage_P(PSTR("!! EMERGENCY ACTIVE !!"));
  } else if (srvDown) {
    setLcdMessage_P(PSTR("Server disconnected!"));
  } else if (btnOpen) {
    setLcdMessage_P(PSTR("BTN circuit OPEN!"));
  } else if (lcdSrvMsg[0] != '\0') {
    setLcdMessage(lcdSrvMsg);
  } else if (muteSirene && muteRotator) {
    setLcdMessage_P(PSTR("Mute: SIR + ROT"));
  } else if (muteSirene) {
    setLcdMessage_P(PSTR("Mute: Sirene"));
  } else if (muteRotator) {
    setLcdMessage_P(PSTR("Mute: Rotator"));
  } else {
    setLcdMessage_P(PSTR("System OK - Online"));
  }
}

// Update the LCD display — called every loop iteration
void updateLCD(double temp) {
  char row0[17];
  char row1[17];

  // --- Build Row 0: "XX.X\xDF C  STATUS" ---
  // Temperature left side (cols 0-6): "XX.X\xDFC" (degree symbol + C)
  char tbuf[7];
  dtostrf(temp, 4, 1, tbuf);  // e.g. "28.5"
  // Build row0 with spaces
  memset(row0, ' ', 16);
  row0[16] = '\0';
  // Copy temp digits
  uint8_t tlen = strlen(tbuf);
  memcpy(row0, tbuf, tlen);
  row0[tlen] = '\xDF';      // degree symbol
  row0[tlen + 1] = 'C';

  // Status right side (cols 8-15)
  const char* stat;
  if (isPanic)        stat = "!!PANIC";
  else if (srvDown)   stat = "NO SRVR";
  else if (muteSirene || muteRotator)  stat = " SILENT";
  else                stat = " NORMAL";
  // Right-align status into cols 9-15 (7 chars)
  uint8_t slen = strlen(stat);
  memcpy(row0 + 16 - slen, stat, slen);

  // --- Build Row 1: message (with scrolling if needed) ---
  memset(row1, ' ', 16);
  row1[16] = '\0';

  if (lcdMsgLen <= 16) {
    // Static — center or left-align
    memcpy(row1, lcdMsg, lcdMsgLen);
  } else {
    // Scrolling: show 16-char window from lcdScrollPos
    // We pad the message with 4 trailing spaces for visual gap
    uint8_t totalLen = lcdMsgLen + 4;  // message + gap
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t idx = (lcdScrollPos + i) % totalLen;
      if (idx < lcdMsgLen)
        row1[i] = lcdMsg[idx];
      else
        row1[i] = ' ';
    }

    // Advance scroll every 300ms (0.3 second)
    unsigned long now = millis();
    if (now - lastScrollT >= 300UL) {
      lastScrollT = now;
      lcdScrollPos++;
      if (lcdScrollPos >= (int8_t)totalLen) lcdScrollPos = 0;
    }
  }

  // --- Write to LCD only if content changed (prevents flicker) ---
  if (memcmp(lcdPrevRow0, row0, 16) != 0) {
    lcd.setCursor(0, 0);
    lcd.print(row0);
    memcpy(lcdPrevRow0, row0, 16);
  }
  if (memcmp(lcdPrevRow1, row1, 16) != 0) {
    lcd.setCursor(0, 1);
    lcd.print(row1);
    memcpy(lcdPrevRow1, row1, 16);
  }
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);

  // Set relay states before setting as OUTPUT
  setAlarmOutputs(RELAY_OFF, RELAY_OFF, RELAY_ON, RELAY_OFF, RELAY_OFF);

  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YEL, OUTPUT);
  pinMode(PIN_GRN, OUTPUT);
  pinMode(PIN_SIR, OUTPUT);
  pinMode(PIN_ROT, OUTPUT);

  lcd.init();
  lcd.backlight();

  // Initialize LCD state
  memset(lcdMsg, 0, sizeof(lcdMsg));
  memset(lcdSrvMsg, 0, sizeof(lcdSrvMsg));
  memset(lcdPrevRow0, 0, sizeof(lcdPrevRow0));
  memset(lcdPrevRow1, 0, sizeof(lcdPrevRow1));
  setLcdMessage_P(PSTR("Initializing..."));

  // Show boot message
  lcd.setCursor(0, 0);
  lcd.print(F("PanicButton v2"));
  lcd.setCursor(0, 1);
  lcd.print(F("Booting..."));

  // Copy PROGMEM strings to RAM buffers (needed by Ethernet/SHA libs)
  strcpy_P(serverNameBuf, serverName);
  strcpy_P(devIdBuf, DEVICE_ID);

  // Ethernet CS must be set early to prevent W5500 SPI conflicts
  Ethernet.init(10);
  Serial.begin(9600);

  while (!bmp280.InitSensor()) {
    delay(3000);
    Serial.println(F("BMP280 not found"));
    lcd.setCursor(0, 1);
    lcd.print(F("BMP280 ERROR!   "));
  }
  Serial.print(F("ChipID:0x"));
  Serial.println(bmp280.readForChipID(), HEX);
  delay(2000);

  Serial.println(F("Init Eth"));
  lcd.setCursor(0, 1);
  lcd.print(F("Init Ethernet..."));

  if (Ethernet.begin(mac) == 0) {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      digitalWrite(PIN_RED, RELAY_ON);
      digitalWrite(PIN_GRN, RELAY_OFF);
      lcd.setCursor(0, 1);
      lcd.print(F("No ETH Hardware!"));
    }
    Ethernet.begin(mac, ip, myDns, gateway, subnet);
  }

  Serial.print(F("IP:"));
  Serial.println(Ethernet.localIP());
  Serial.print(F("Srv:"));
  Serial.println(serverNameBuf);

  delay(1000);
  randomSeed(analogRead(A2));

  // Clear boot screen and set initial message
  lcd.clear();
  setLcdMessage_P(PSTR("System OK - Online"));
}

void loop() {
  // Read PIN_BTN (A0) and PIN_BTN2 (A1) directly
  bool btnState = digitalRead(PIN_BTN);
  bool btn2State = digitalRead(PIN_BTN2);
  double temp = bmp280.readTemperature();
  lastTemp = temp;

  btnOpen = (btnState == HIGH);  // Update cached state for LCD

  jackDisconnected = (btn2State == LOW); // LOW indicates jack pulled out / disconnected

  // Control Red LED for jack disconnection
  if (jackDisconnected) {
    digitalWrite(PIN_RED, RELAY_ON);
  } else if (!srvDown && !isPanic) {
    digitalWrite(PIN_RED, RELAY_OFF);
  }

  // Refresh the priority-based LCD message
  refreshLcdMessage();
  // Update the LCD display (no flicker — only writes changed content)
  updateLCD(temp);

  // Only unlock reset if both button circuit is closed and jack is plugged in
  if (btnState == LOW && !jackDisconnected && resetLocked) {
    resetLocked = false;
  }

  if (btnOpen && !isPanic && !resetLocked) {
    Serial.println(F("CircuitOPEN-PanicON"));
    triggerPanicON();
  }

  if (jackDisconnected && !isPanic && !resetLocked) {
    Serial.println(F("A1-JackPull-PanicON"));
    digitalWrite(PIN_RED, RELAY_ON);
    triggerPanicON();
  }

  unsigned long now = millis();
  if (now - lastHB >= 5000UL || lastHB == 0) {
    lastHB = now;
    sendApiRequest(PSTR("/api/heartbeat"), true);
  }

  Ethernet.maintain();
}

void triggerPanicON() {
  if (isPanic) return;
  isPanic = true;
  Serial.println(F("PANIC ON"));

  digitalWrite(PIN_GRN, RELAY_OFF);
  digitalWrite(PIN_YEL, RELAY_ON);
  if (!muteRotator) digitalWrite(PIN_ROT, RELAY_ON);
  if (!muteSirene) digitalWrite(PIN_SIR, RELAY_ON);

  sendApiRequest(PSTR("/api/panic"), false);
}

void sendApiRequest(const char* endpoint_P, bool isHeartbeat) {
  unsigned long ts = millis();
  char tsBuf[12];
  ultoa(ts, tsBuf, 10);

  // Generate nonce — manual hex avoids sprintf
  char nonce[9];
  uint16_t r1 = random(65536), r2 = random(65536);
  hexByte(nonce, r1 >> 8); hexByte(nonce + 2, r1 & 0xFF);
  hexByte(nonce + 4, r2 >> 8); hexByte(nonce + 6, r2 & 0xFF);
  nonce[8] = '\0';

  // Build JSON payload manually — avoids large PSTR format string + snprintf
  char buf[160];
  char* p = buf;

  p = appendP(p, PSTR("{\"id\":\""));
  p = appendR(p, devIdBuf);

  if (isHeartbeat) {
    p = appendP(p, PSTR("\",\"st\":\"hb\",\"ts\":"));
    p = appendR(p, tsBuf);
    p = appendP(p, PSTR(",\"r\":"));
    p = appendBool(p, digitalRead(PIN_RED) == RELAY_ON);
    p = appendP(p, PSTR(",\"y\":"));
    p = appendBool(p, digitalRead(PIN_YEL) == RELAY_ON);
    p = appendP(p, PSTR(",\"g\":"));
    p = appendBool(p, digitalRead(PIN_GRN) == RELAY_ON);
    p = appendP(p, PSTR(",\"pb\":"));
    p = appendBool(p, digitalRead(PIN_BTN) == HIGH);
    p = appendP(p, PSTR(",\"sir\":"));
    p = appendBool(p, digitalRead(PIN_SIR) == RELAY_ON);
    p = appendP(p, PSTR(",\"rot\":"));
    p = appendBool(p, digitalRead(PIN_ROT) == RELAY_ON);
    p = appendP(p, PSTR(",\"ps\":"));
    p = appendBool(p, isPanic);
    // Append temperature reading
    p = appendP(p, PSTR(",\"t\":"));
    char tempBuf[8];
    dtostrf(lastTemp, 4, 2, tempBuf);
    p = appendR(p, tempBuf);
    *p++ = '}'; *p = '\0';
  } else {
    p = appendP(p, PSTR("\",\"st\":\"p\",\"ts\":"));
    p = appendR(p, tsBuf);
    *p++ = '}'; *p = '\0';
  }

  // HMAC-SHA256 signature
  uint8_t keyBuf[32];
  memcpy_P(keyBuf, SECRET_KEY, 32);
  Sha256.initHmac(keyBuf, 32);
  Sha256.print(devIdBuf);
  Sha256.print(tsBuf);
  Sha256.print(nonce);
  Sha256.print(buf);

  uint8_t* hash = Sha256.resultHmac();
  char sig[65];
  for (uint8_t i = 0; i < 32; i++) {
    hexByte(sig + (i << 1), hash[i]);
  }
  sig[64] = '\0';

  if (client.connect(serverNameBuf, serverPort)) {
    failCount = 0;
    if (srvDown && !isPanic) {
      digitalWrite(PIN_RED, RELAY_OFF);
      digitalWrite(PIN_GRN, RELAY_ON);
      srvDown = false;
    }

    client.print(F("POST "));
    client.print((const __FlashStringHelper*)endpoint_P);
    client.println(F(" HTTP/1.1"));
    client.print(F("Host: "));
    client.println(serverNameBuf);
    client.println(F("Content-Type: application/json\r\nConnection: close"));
    client.print(F("X-Device-ID: "));
    client.println(devIdBuf);
    client.print(F("X-Timestamp: "));
    client.println(tsBuf);
    client.print(F("X-Nonce: "));
    client.println(nonce);
    client.print(F("X-Signature: "));
    client.println(sig);
    client.print(F("Content-Length: "));
    client.println(strlen(buf));
    client.println();
    client.print(buf);

    client.setTimeout(5000);
    if (client.find("\r\n\r\n")) {
      int n = 0;
      unsigned long t0 = millis();
      while (n < (int)sizeof(buf) - 1 && millis() - t0 < 3000) {
        if (client.available()) {
          buf[n++] = client.read();
          t0 = millis();
        } else {
          delay(10);
        }
      }
      buf[n] = '\0';

      // Parse commands
      if (strstr_P(buf, PSTR("\"cmd\":\"rst\""))) {
        isPanic = false;
        resetLocked = true;
        setAlarmOutputs(RELAY_OFF, RELAY_OFF, RELAY_ON, RELAY_OFF, RELAY_OFF);
      }
      else if (strstr_P(buf, PSTR("\"cmd\":\"pnc\""))) {
        isPanic = true;
        digitalWrite(PIN_GRN, RELAY_OFF);
        digitalWrite(PIN_YEL, RELAY_ON);
        if (!muteRotator) digitalWrite(PIN_ROT, RELAY_ON);
        if (!muteSirene) digitalWrite(PIN_SIR, RELAY_ON);
      }

      // Parse granular silent mode mute flags
      if (strstr_P(buf, PSTR("\"ms\":true"))) {
        muteSirene = true;
        if (isPanic) digitalWrite(PIN_SIR, RELAY_OFF);
      } else if (strstr_P(buf, PSTR("\"ms\":false"))) {
        muteSirene = false;
        if (isPanic) digitalWrite(PIN_SIR, RELAY_ON);
      }

      if (strstr_P(buf, PSTR("\"mr\":true"))) {
        muteRotator = true;
        if (isPanic) digitalWrite(PIN_ROT, RELAY_OFF);
      } else if (strstr_P(buf, PSTR("\"mr\":false"))) {
        muteRotator = false;
        if (isPanic) digitalWrite(PIN_ROT, RELAY_ON);
      }

      // Parse LCD message from server
      {
        const char* lcdKey = strstr_P(buf, PSTR("\"lcd\":\""));
        if (lcdKey) {
          lcdKey += 7;  // skip past "lcd":"
          char* endQuote = strchr(lcdKey, '"');
          if (endQuote && (endQuote - lcdKey) < (int)sizeof(lcdSrvMsg)) {
            uint8_t mlen = endQuote - lcdKey;
            memcpy(lcdSrvMsg, lcdKey, mlen);
            lcdSrvMsg[mlen] = '\0';
          }
        } else if (strstr_P(buf, PSTR("\"lcd\":null"))) {
          lcdSrvMsg[0] = '\0';  // Clear server message
        }
      }
    }

    client.stop();
  } else {
    failCount++;
    if (failCount >= 3 && !srvDown) {
      srvDown = true;
      if (!isPanic) {
        digitalWrite(PIN_GRN, RELAY_OFF);
        digitalWrite(PIN_RED, RELAY_ON);
      }
    }
  }

  // Always refresh LCD message after API call (state may have changed)
  refreshLcdMessage();
}