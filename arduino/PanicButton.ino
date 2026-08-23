/*
 * Proyek: Panic Button v2
 * Deskripsi: Sistem tombol panik terhubung ke jaringan Ethernet dengan sensor suhu (BMP280),
 *            layar LCD, indikator LED, sirene, dan rotator. Koneksi dengan sever menggunakan Ethernet dan terenkripsi HMAC-SHA256.
 *
 * Pustaka yang digunakan:
 * - sha256: Diambil dan dimodifikasi dari https://github.com/Cathedrow/Cryptosuite.git
 * - bmp280_ltsm: Untuk pembacaan sensor dari https://github.com/gavinlyonsrepo/BMP280_LTSM
 * - SPI: Untuk komunikasi SPI dari https://docs.arduino.cc/language-reference/en/functions/communication/SPI/
 * - Ethernet: Untuk komunikasi jaringan dari https://github.com/arduino-libraries/ethernet
 * - LiquidCrystal_I2C: Untuk tampilan LCD dari https://github.com/johnrickman/LiquidCrystal_I2C
 */
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
// Salinan RAM untuk Ethernet
char serverNameBuf[26];
const int serverPort = 27373;
EthernetClient client;

const char DEVICE_ID[] PROGMEM = "ARDPB0011";
// Salinan RAM untuk ID perangkat
char devIdBuf[10];

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

// --- Status tampilan LCD ---
// Pesan saat ini untuk baris 1
char lcdMsg[64];
// Pesan dari server (prioritas rendah)
char lcdSrvMsg[64];
// Panjang string pesan LCD yang disimpan
uint8_t lcdMsgLen = 0;
// Offset gulir saat ini
int8_t  lcdScrollPos = 0;
unsigned long lastScrollT = 0;
// Konten baris 0 sebelumnya (mencegah kedipan)
char lcdPrevRow0[17];
// Konten baris 1 sebelumnya (mencegah kedipan)
char lcdPrevRow1[17];
// Suhu yang disimpan untuk LCD
double lastTemp = 0.0;
// Status PIN_BTN yang telah didestabilisasi (true = sirkuit terbuka)
bool btnOpen = false;
// Status PIN_BTN2 yang telah didestabilisasi (true = jack 3.5mm terlepas)
bool jackDisconnected = false;

// --- Fungsi pembantu untuk menghemat flash ---

// Konverter byte hex kecil — menghindari penggunaan formatter sprintf/printf penuh
static void hexByte(char* out, uint8_t b) {
  static const char h[] PROGMEM = "0123456789abcdef";
  out[0] = pgm_read_byte(&h[b >> 4]);
  out[1] = pgm_read_byte(&h[b & 0x0F]);
}

// Tambahkan string PROGMEM ke tujuan (mengembalikan pointer ke akhir yang baru)
static char* appendP(char* dst, const char* pstr) {
  char c;
  while ((c = pgm_read_byte(pstr++))) *dst++ = c;
  *dst = '\0';
  return dst;
}

// Tambahkan string RAM ke tujuan
static char* appendR(char* dst, const char* s) {
  while (*s) *dst++ = *s++;
  *dst = '\0';
  return dst;
}

// Tambahkan "true" atau "false" berdasarkan kondisi
static char* appendBool(char* dst, bool val) {
  return val ? appendP(dst, PSTR("true")) : appendP(dst, PSTR("false"));
}

// Atur semua output alarm sekaligus
static void setAlarmOutputs(uint8_t red, uint8_t yel, uint8_t grn, uint8_t sir, uint8_t rot) {
  digitalWrite(PIN_RED, red);
  digitalWrite(PIN_YEL, yel);
  digitalWrite(PIN_GRN, grn);
  digitalWrite(PIN_SIR, sir);
  digitalWrite(PIN_ROT, rot);
}

// --- Fungsi LCD ---

// Atur pesan LCD baru dan atur ulang posisi gulir
void setLcdMessage(const char* msg) {
  // Tidak ada perubahan
  if (strcmp(lcdMsg, msg) == 0) return;
  strncpy(lcdMsg, msg, sizeof(lcdMsg) - 1);
  lcdMsg[sizeof(lcdMsg) - 1] = '\0';
  lcdMsgLen = strlen(lcdMsg);
  lcdScrollPos = 0;
  lastScrollT = millis();
}

// Atur pesan LCD dari PROGMEM
void setLcdMessage_P(const char* pmsg) {
  char tmp[64];
  strncpy_P(tmp, pmsg, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  setLcdMessage(tmp);
}

// Tentukan pesan saat ini berdasarkan prioritas status sistem
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

// Perbarui tampilan LCD — dipanggil setiap iterasi loop
void updateLCD(double temp) {
  char row0[17];
  char row1[17];

  // --- Buat Baris 0: "XX.X\xDF C  STATUS" ---
  // Suhu di sisi kiri (kolom 0-6): "XX.X\xDFC" (simbol derajat + C)
  char tbuf[7];
  // contoh "28.5"
  dtostrf(temp, 4, 1, tbuf);
  // Buat row0 dengan spasi
  memset(row0, ' ', 16);
  row0[16] = '\0';
  // Salin digit suhu
  uint8_t tlen = strlen(tbuf);
  memcpy(row0, tbuf, tlen);
  // simbol derajat
  row0[tlen] = '\xDF';
  row0[tlen + 1] = 'C';

  // Status di sisi kanan (kolom 8-15)
  const char* stat;
  if (isPanic)        stat = "!!PANIC";
  else if (srvDown)   stat = "NO SRVR";
  else if (muteSirene || muteRotator)  stat = " SILENT";
  else                stat = " NORMAL";
  // Rata kanan status ke dalam kolom 9-15 (7 karakter)
  uint8_t slen = strlen(stat);
  memcpy(row0 + 16 - slen, stat, slen);

  // --- Buat Baris 1: pesan (dengan pengguliran jika diperlukan) ---
  memset(row1, ' ', 16);
  row1[16] = '\0';

  if (lcdMsgLen <= 16) {
    // Statis — rata tengah atau kiri
    memcpy(row1, lcdMsg, lcdMsgLen);
  } else {
    // Menggulir: tampilkan jendela 16 karakter dari lcdScrollPos
    // Kami menambahkan spasi di akhir pesan sebanyak 4 buah untuk celah visual
    // pesan + celah
    uint8_t totalLen = lcdMsgLen + 4;
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t idx = (lcdScrollPos + i) % totalLen;
      if (idx < lcdMsgLen)
        row1[i] = lcdMsg[idx];
      else
        row1[i] = ' ';
    }

    // Majukan gulir setiap 300ms (0.3 detik)
    unsigned long now = millis();
    if (now - lastScrollT >= 300UL) {
      lastScrollT = now;
      lcdScrollPos++;
      if (lcdScrollPos >= (int8_t)totalLen) lcdScrollPos = 0;
    }
  }

  // --- Tulis ke LCD hanya jika konten berubah (mencegah kedipan) ---
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

  // Atur status relai sebelum mengatur sebagai OUTPUT
  setAlarmOutputs(RELAY_OFF, RELAY_OFF, RELAY_ON, RELAY_OFF, RELAY_OFF);

  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YEL, OUTPUT);
  pinMode(PIN_GRN, OUTPUT);
  pinMode(PIN_SIR, OUTPUT);
  pinMode(PIN_ROT, OUTPUT);

  lcd.init();
  lcd.backlight();

  // Inisialisasi status LCD
  memset(lcdMsg, 0, sizeof(lcdMsg));
  memset(lcdSrvMsg, 0, sizeof(lcdSrvMsg));
  memset(lcdPrevRow0, 0, sizeof(lcdPrevRow0));
  memset(lcdPrevRow1, 0, sizeof(lcdPrevRow1));
  setLcdMessage_P(PSTR("Initializing..."));

  // Tampilkan pesan boot
  lcd.setCursor(0, 0);
  lcd.print(F("PanicButton v2"));
  lcd.setCursor(0, 1);
  lcd.print(F("Booting..."));

  // Salin string PROGMEM ke buffer RAM (diperlukan oleh library Ethernet/SHA)
  strcpy_P(serverNameBuf, serverName);
  strcpy_P(devIdBuf, DEVICE_ID);

  // Ethernet CS harus diatur lebih awal untuk mencegah konflik SPI W5500
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

  // Bersihkan layar boot dan atur pesan awal
  lcd.clear();
  setLcdMessage_P(PSTR("System OK - Online"));
}

void loop() {
  // Baca PIN_BTN (A0) dan PIN_BTN2 (A1) secara langsung
  bool btnState = digitalRead(PIN_BTN);
  bool btn2State = digitalRead(PIN_BTN2);
  double temp = bmp280.readTemperature();
  lastTemp = temp;

  // Perbarui status yang disimpan untuk LCD
  btnOpen = (btnState == HIGH);

  // LOW menunjukkan jack ditarik keluar / terputus
  jackDisconnected = (btn2State == LOW);

  // Kontrol LED Merah untuk pemutusan jack
  if (jackDisconnected) {
    digitalWrite(PIN_RED, RELAY_ON);
  } else if (!srvDown && !isPanic) {
    digitalWrite(PIN_RED, RELAY_OFF);
  }

  // Perbarui pesan LCD berbasis prioritas
  refreshLcdMessage();
  // Perbarui tampilan LCD (tanpa kedipan — hanya menulis konten yang berubah)
  updateLCD(temp);

  // Hanya buka kunci reset jika sirkuit tombol ditutup dan jack dicolokkan
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

  // Hasilkan nonce — hex manual menghindari sprintf
  char nonce[9];
  uint16_t r1 = random(65536), r2 = random(65536);
  hexByte(nonce, r1 >> 8); hexByte(nonce + 2, r1 & 0xFF);
  hexByte(nonce + 4, r2 >> 8); hexByte(nonce + 6, r2 & 0xFF);
  nonce[8] = '\0';

  // Bangun payload JSON secara manual — menghindari string format PSTR besar + snprintf
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
    // Tambahkan pembacaan suhu
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

  // Tanda tangan HMAC-SHA256
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

      // Urai perintah
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

      // Urai bendera bisu mode senyap granular
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

      // Urai pesan LCD dari server
      {
        const char* lcdKey = strstr_P(buf, PSTR("\"lcd\":\""));
        if (lcdKey) {
          // lewati "lcd":"
          lcdKey += 7;
          char* endQuote = strchr(lcdKey, '"');
          if (endQuote && (endQuote - lcdKey) < (int)sizeof(lcdSrvMsg)) {
            uint8_t mlen = endQuote - lcdKey;
            memcpy(lcdSrvMsg, lcdKey, mlen);
            lcdSrvMsg[mlen] = '\0';
          }
        } else if (strstr_P(buf, PSTR("\"lcd\":null"))) {
          // Hapus pesan server
          lcdSrvMsg[0] = '\0';
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

  // Selalu perbarui pesan LCD setelah panggilan API (status mungkin telah berubah)
  refreshLcdMessage();
}