#include <sha256.h>
#include <SPI.h>
#include <Ethernet.h>

#define RELAY_ON LOW
#define RELAY_OFF HIGH

const int PIN_BUTTON = A0;
const int PIN_LED_RED = 4;
const int PIN_LED_YELLOW = 5;
const int PIN_LED_GREEN = 6;
const int PIN_SIREN = A1;
const int PIN_ROTATOR = A3;

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }; 
IPAddress ip(192, 168, 0, 177);
IPAddress myDns(1, 1, 1, 1);
IPAddress gateway(192, 168, 0, 1); 
IPAddress subnet(255, 255, 255, 0); 

char serverName[] = "sakura.proxy.rlwy.net";
const int serverPort = 27373;
EthernetClient client;

const char* DEVICE_ID = "ARDPB0011";

const uint8_t SECRET_KEY[32] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 
  0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 
  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 
  0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 5000;
bool isPanicActive = false;
bool lastButtonState = HIGH; 
bool isSilentMode = false;

void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP); 
  
  digitalWrite(PIN_LED_GREEN, RELAY_ON);
  digitalWrite(PIN_LED_RED, RELAY_OFF);
  digitalWrite(PIN_LED_YELLOW, RELAY_OFF);
  digitalWrite(PIN_SIREN, RELAY_OFF);
  digitalWrite(PIN_ROTATOR, RELAY_OFF);

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_SIREN, OUTPUT);
  pinMode(PIN_ROTATOR, OUTPUT);
  
  Ethernet.init(10); 
  Serial.begin(9600);
  while (!Serial) {
    ; 
  }
  
  Serial.println(F("Initialize Ethernet"));
  if (Ethernet.begin(mac) == 0) {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      digitalWrite(PIN_LED_RED, RELAY_ON);
      digitalWrite(PIN_LED_GREEN, RELAY_OFF);
      Serial.println(F("Ethernet shield was not found."));
    }
    if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println(F("Ethernet cable is not connected."));
    }
    
    Serial.println(F("Initialize Ethernet with Static IP..."));
    Ethernet.begin(mac, ip, myDns, gateway, subnet);
  }

  Serial.print(F("My IP address: "));
  Serial.println(Ethernet.localIP());

  delay(1000);
  randomSeed(analogRead(A2)); 
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastHeartbeat >= heartbeatInterval || lastHeartbeat == 0) {
    lastHeartbeat = currentMillis;
    sendApiRequest("/api/heartbeat", "heartbeat");
  }

  bool currentButtonState = digitalRead(PIN_BUTTON);
  delay(50); 

  if (currentButtonState != lastButtonState) {
    if (digitalRead(PIN_BUTTON) == currentButtonState) {
      if (currentButtonState == LOW) {
        Serial.println(F("Physical Button Pressed - Latching Panic ON!"));
        triggerPanicON();
      } else {
        Serial.println(F("Physical Button Released - Ignoring."));
      }
    }
  }
  lastButtonState = currentButtonState;
  
  Ethernet.maintain(); 
}

void triggerPanicON() {
  if (isPanicActive) return;
  
  isPanicActive = true;
  Serial.println(F("\n*** PANIC BUTTON TRIGGERED - ALARM ON! ***"));

  digitalWrite(PIN_LED_GREEN, RELAY_OFF);
  digitalWrite(PIN_LED_YELLOW, RELAY_ON);
  digitalWrite(PIN_ROTATOR, RELAY_ON);
  
  if (!isSilentMode) {
    digitalWrite(PIN_SIREN, RELAY_ON);
  } else {
    Serial.println(F("Silent Mode Active - Siren Disabled"));
  }

  sendApiRequest("/api/panic", "panic");
}

void sendApiRequest(const char* endpoint, const char* statusType) {
  unsigned long timestamp = millis();
  Serial.println(F("Sending Api Request..."));
  
  char nonce[9];
  sprintf(nonce, "%04x%04x", (unsigned int)random(65536), (unsigned int)random(65536));

  char buffer[300]; 
  
  if (strcmp(statusType, "heartbeat") == 0) {
    snprintf_P(buffer, sizeof(buffer), 
      PSTR("{\"device_id\":\"%s\",\"status\":\"heartbeat\",\"timestamp\":%lu,"
      "\"led_red\":%s,\"led_yellow\":%s,\"led_green\":%s,"
      "\"panic_button\":%s,\"sirene\":%s,\"rotator\":%s,\"panic_state\":%s}"), 
      DEVICE_ID, timestamp, 
      digitalRead(PIN_LED_RED) == RELAY_ON ? "true" : "false",
      digitalRead(PIN_LED_YELLOW) == RELAY_ON ? "true" : "false",
      digitalRead(PIN_LED_GREEN) == RELAY_ON ? "true" : "false",
      !digitalRead(PIN_BUTTON) ? "true" : "false",
      digitalRead(PIN_SIREN) == RELAY_ON ? "true" : "false",
      digitalRead(PIN_ROTATOR) == RELAY_ON ? "true" : "false",
      isPanicActive ? "true" : "false");
  } else {
    snprintf_P(buffer, sizeof(buffer), 
      PSTR("{\"device_id\":\"%s\",\"status\":\"%s\",\"timestamp\":%lu}"), 
      DEVICE_ID, statusType, timestamp);
  }

  char tsStr[12];
  ultoa(timestamp, tsStr, 10);

  Sha256.initHmac(SECRET_KEY, sizeof(SECRET_KEY));
  Sha256.print(DEVICE_ID);
  Sha256.print(tsStr);
  Sha256.print(nonce);
  Sha256.print(buffer);
  
  uint8_t* hash = Sha256.resultHmac();
  char signature[65];
  for (int i = 0; i < 32; i++) {
    sprintf(signature + (i * 2), "%02x", hash[i]);
  }
  signature[64] = '\0';

  Serial.print(F("Connecting to server for "));
  Serial.print(endpoint);
  Serial.print(F("... "));

  if (client.connect(serverName, serverPort)) {
    Serial.print(F("connected to "));
    Serial.println(client.remoteIP());

    client.print(F("POST ")); 
    client.print(endpoint); 
    client.println(F(" HTTP/1.1"));

    client.print(F("Host: ")); 
    client.println(serverName);
    client.println(F("Content-Type: application/json"));
    client.println(F("Connection: close"));
    client.print(F("X-Device-ID: ")); 
    client.println(DEVICE_ID);
    client.print(F("X-Timestamp: ")); 
    client.println(tsStr);
    client.print(F("X-Nonce: ")); 
    client.println(nonce);
    client.print(F("X-Signature: ")); 
    client.println(signature);
    client.print(F("Content-Length: ")); 
    client.println(strlen(buffer));
    client.println();

    client.print(buffer); 
    
    client.setTimeout(3000);
    if (client.find("\r\n\r\n")) {
      
      int bytesRead = client.readBytes(buffer, sizeof(buffer) - 1);
      buffer[bytesRead] = '\0';
      
      if (strstr(buffer, "\"command\":\"reset\"")) {
        Serial.println(F("\n>>> Received RESET command from Dashboard <<<"));
        isPanicActive = false;
        digitalWrite(PIN_LED_YELLOW, RELAY_OFF);
        digitalWrite(PIN_SIREN, RELAY_OFF);
        digitalWrite(PIN_ROTATOR, RELAY_OFF);
        digitalWrite(PIN_LED_GREEN, RELAY_ON);
      } 
      else if (strstr(buffer, "\"command\":\"panic\"")) {
        Serial.println(F("\n>>> Received PANIC command from Dashboard <<<"));
        isPanicActive = true;
        digitalWrite(PIN_LED_GREEN, RELAY_OFF);
        digitalWrite(PIN_LED_YELLOW, RELAY_ON);
        digitalWrite(PIN_ROTATOR, RELAY_ON);
        if (!isSilentMode) digitalWrite(PIN_SIREN, RELAY_ON);
      }

      if (strstr(buffer, "\"is_active\":true")) { 
        if (!isSilentMode) Serial.println(F("\n>>> Silent Mode ACTIVE (Scheduled) <<<"));
        isSilentMode = true;
        if (isPanicActive) digitalWrite(PIN_SIREN, RELAY_OFF); 
      } 
      else if (strstr(buffer, "\"is_active\":false")) { 
        if (isSilentMode) Serial.println(F("\n>>> Silent Mode INACTIVE <<<"));
        isSilentMode = false;
        if (isPanicActive) digitalWrite(PIN_SIREN, RELAY_ON); 
      }
      
    } else {
      Serial.println(F("Response timeout or no body."));
    }

    client.stop();
    Serial.println(F("Request complete."));
  } else {
    Serial.println(F("Server connection failed."));
  }
}