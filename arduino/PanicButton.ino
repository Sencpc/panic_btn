#include <sha256.h>
#include "bmp280_ltsm.hpp"
#include <SPI.h>
#include <Ethernet.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 20, 4);

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

const char serverName[] PROGMEM = "panama-api.smartbid.co.id";
char serverNameBuf[26];  // RAM copy for Ethernet
const int serverPort = 80;
EthernetClient client;

const char DEVICE_ID[] PROGMEM = "ARDPB0011";
char devIdBuf[10];  // RAM copy

const uint8_t SECRET_KEY[32] PROGMEM = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
  0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
  0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

uint8_t I2C_ADDRESS = 0x76;
uint32_t I2C_BUS_SPEED = 100000;
BMP280_Sensor bmp280(I2C_ADDRESS, &Wire, I2C_BUS_SPEED);

unsigned long lastHB = 0;
bool isPanic = false;
bool isSilent = false;
bool resetLocked = false;
uint8_t failCount = 0;
bool srvDown = false;

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

  // Copy PROGMEM strings to RAM buffers (needed by Ethernet/SHA libs)
  strcpy_P(serverNameBuf, serverName);
  strcpy_P(devIdBuf, DEVICE_ID);

  Serial.begin(9600);

  while (!bmp280.InitSensor()) {
    delay(3000);
    Serial.println(F("BMP280 not found"));
  }
  Serial.print(F("ChipID:0x"));
  Serial.println(bmp280.readForChipID(), HEX);
  delay(2000);

  Ethernet.init(10);

  Serial.println(F("Init Eth"));
  if (Ethernet.begin(mac) == 0) {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      digitalWrite(PIN_RED, RELAY_ON);
      digitalWrite(PIN_GRN, RELAY_OFF);
    }
    Ethernet.begin(mac, ip, myDns, gateway, subnet);
  }

  Serial.print(F("IP:"));
  Serial.println(Ethernet.localIP());

  delay(1000);
  randomSeed(analogRead(A2));
}

void loop() {
  bool btnState = digitalRead(PIN_BTN);
  double temp = bmp280.readTemperature();
  delay(50);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Temp:"));
  lcd.setCursor(0, 1);
  // Use dtostrf instead of String() — saves ~1.6KB flash
  char tbuf[8];
  dtostrf(temp, 4, 2, tbuf);
  lcd.print(tbuf);

  if (btnState == LOW && resetLocked) {
    resetLocked = false;
  }

  if (btnState == HIGH && !isPanic && !resetLocked) {
    Serial.println(F("CircuitOPEN-PanicON"));
    triggerPanicON();
  }

  if (digitalRead(PIN_BTN2) == LOW && !isPanic && !resetLocked) {
    delay(50);
    if (digitalRead(PIN_BTN2) == LOW) {
      Serial.println(F("A1-PanicON"));
      digitalWrite(PIN_RED, RELAY_ON);
      triggerPanicON();
    }
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
  digitalWrite(PIN_ROT, RELAY_ON);
  if (!isSilent) digitalWrite(PIN_SIR, RELAY_ON);

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
  char buf[320];
  char* p = buf;

  p = appendP(p, PSTR("{\"device_id\":\""));
  p = appendR(p, devIdBuf);

  if (isHeartbeat) {
    p = appendP(p, PSTR("\",\"status\":\"heartbeat\",\"timestamp\":"));
    p = appendR(p, tsBuf);
    p = appendP(p, PSTR(",\"led_red\":"));
    p = appendBool(p, digitalRead(PIN_RED) == RELAY_ON);
    p = appendP(p, PSTR(",\"led_yellow\":"));
    p = appendBool(p, digitalRead(PIN_YEL) == RELAY_ON);
    p = appendP(p, PSTR(",\"led_green\":"));
    p = appendBool(p, digitalRead(PIN_GRN) == RELAY_ON);
    p = appendP(p, PSTR(",\"panic_button\":"));
    p = appendBool(p, digitalRead(PIN_BTN) == HIGH);
    p = appendP(p, PSTR(",\"sirene\":"));
    p = appendBool(p, digitalRead(PIN_SIR) == RELAY_ON);
    p = appendP(p, PSTR(",\"rotator\":"));
    p = appendBool(p, digitalRead(PIN_ROT) == RELAY_ON);
    p = appendP(p, PSTR(",\"panic_state\":"));
    p = appendBool(p, isPanic);
    *p++ = '}'; *p = '\0';
  } else {
    p = appendP(p, PSTR("\",\"status\":\"panic\",\"timestamp\":"));
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

  // Copy endpoint from PROGMEM
  char ep[20];
  strcpy_P(ep, endpoint_P);

  if (client.connect(serverNameBuf, serverPort)) {
    failCount = 0;
    if (srvDown && !isPanic) {
      digitalWrite(PIN_RED, RELAY_OFF);
      digitalWrite(PIN_GRN, RELAY_ON);
      srvDown = false;
    }

    client.print(F("POST "));
    client.print(ep);
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
      if (strstr_P(buf, PSTR("\"command\":\"reset\""))) {
        isPanic = false;
        resetLocked = true;
        setAlarmOutputs(RELAY_OFF, RELAY_OFF, RELAY_ON, RELAY_OFF, RELAY_OFF);
      }
      else if (strstr_P(buf, PSTR("\"command\":\"panic\""))) {
        isPanic = true;
        digitalWrite(PIN_GRN, RELAY_OFF);
        digitalWrite(PIN_YEL, RELAY_ON);
        digitalWrite(PIN_ROT, RELAY_ON);
        if (!isSilent) digitalWrite(PIN_SIR, RELAY_ON);
      }

      // Parse silent mode
      if (strstr_P(buf, PSTR("\"is_active\":true"))) {
        isSilent = true;
        if (isPanic) digitalWrite(PIN_SIR, RELAY_OFF);
      }
      else if (strstr_P(buf, PSTR("\"is_active\":false"))) {
        isSilent = false;
        if (isPanic) digitalWrite(PIN_SIR, RELAY_ON);
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
}