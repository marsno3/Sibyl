#include <WiFi.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DRV2605.h>

#define BUTTON_PIN 13
#define PRINTER_TX 17
#define PRINTER_RX 16

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* tdIP = "TD_MACHINE_IP";
const int tdOSCPort = 9000;
const int localPort = 9001;

WiFiServer server(8888);
WiFiUDP udp;
bool lastButton = HIGH;

// Heart rate
MAX30105 particleSensor;
const byte RATE_SIZE = 8;      
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte rateCount = 0;            
long lastBeat = 0;
float beatsPerMinute;
int beatAvg = 0;               
long beatCount = 0;
unsigned long lastBPMSend = 0;

// UI 
enum UiState { UI_IDLE, UI_READING, UI_READY, UI_PRINTING, UI_COLLECT };
UiState uiState = UI_IDLE;
UiState lastDrawn = UI_IDLE;
const unsigned long READ_TIME = 15000;      // 15s timer
const unsigned long READY_TIMEOUT = 7000;  

unsigned long fingerStart = 0;
unsigned long lastFingerSeen = 0;
bool fingerWas = false;
bool waitingForRelease = false;            
unsigned long printingStart = 0;           
unsigned long collectStart = 0;            
const unsigned long COLLECT_TIME = 5000;   
unsigned long beatPulseUntil = 0;

// OLED renew
unsigned long lastOledReinit = 0;
const unsigned long OLED_REINIT_INTERVAL = 30000;

// OLED
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// haptic feedback
Adafruit_DRV2605 drv;
bool drvReady = false;

void sendOSCInt(const char* address, int32_t value) {
  IPAddress remoteIp;
  remoteIp.fromString(tdIP);
  OSCMessage msg(address);
  msg.add(value);
  udp.beginPacket(remoteIp, tdOSCPort);
  msg.send(udp);
  udp.endPacket();
  msg.empty();
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting WiFi");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    udp.begin(localPort);
    server.begin();
  } else {
    Serial.println("\nWiFi failed, retrying...");
  }
}

void updateDisplay() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(2);

  switch (uiState) {
    case UI_IDLE:
      oled.setCursor(58, 2);
      oled.write((uint8_t)0x19);     // ↓
      oled.setCursor(34, 24);
      oled.println("PLACE");
      oled.setCursor(28, 44);
      oled.println("FINGER");
      break;
    case UI_READING:
      oled.setCursor(4, 26);
      oled.println("KEEP STILL");
      break;
    case UI_READY:
      oled.setCursor(58, 2);
      oled.write((uint8_t)0x19);     // ↓
      oled.setCursor(34, 24);
      oled.println("PRESS");
      oled.setCursor(28, 44);
      oled.println("BUTTON");
      break;
    case UI_PRINTING:
      oled.setCursor(28, 24);
      oled.println("PLEASE");
      oled.setCursor(40, 44);
      oled.println("WAIT");
      break;
    case UI_COLLECT:
      oled.setCursor(22, 24);
      oled.println("COLLECT");
      oled.setCursor(34, 44);
      oled.println("PRINT");
      break;
  }
  oled.display();
  lastDrawn = uiState;
}

void readHeartRate() {
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute > 45 && beatsPerMinute < 180) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      if (rateCount < RATE_SIZE) rateCount++;

      byte tmp[RATE_SIZE];
      memcpy(tmp, rates, rateCount);
      for (byte i = 1; i < rateCount; i++) {   
        byte k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
      }
      beatAvg = tmp[rateCount / 2];
    }

    beatCount++;
    sendOSCInt("/beat", beatCount);

    
    if (drvReady && fingerWas) {
      beatPulseUntil = millis() + 150;
    }

    Serial.print("Beat! BPM median: ");
    Serial.println(beatAvg);
  }

  
  static bool fingerHold = false;
  static unsigned long fingerLostAt = 0;

  if (irValue > 40000) {
    fingerHold = true;
    fingerLostAt = 0;
  } else if (irValue < 36000) {
    if (fingerHold && fingerLostAt == 0) fingerLostAt = millis();
    if (fingerLostAt && millis() - fingerLostAt > 500) fingerHold = false;
  }
  bool fingerPresent = fingerHold;

  if (fingerPresent) lastFingerSeen = millis();

  // UI 
  switch (uiState) {
    case UI_IDLE:
      if (waitingForRelease) {
        if (!fingerPresent) waitingForRelease = false;
        break;   
      }
      if (fingerPresent && !fingerWas) {
        fingerStart = millis();
        rateCount = 0;                          
        rateSpot = 0;
        beatAvg = 0;
        uiState = UI_READING;
      }
      break;
    case UI_READING:
      if (!fingerPresent) {
        uiState = UI_IDLE;                       
      } else if (millis() - fingerStart >= READ_TIME) {
        uiState = UI_READY;                      
      }
      break;
    case UI_READY:
      if (!fingerPresent && millis() - lastFingerSeen > READY_TIMEOUT) {
        uiState = UI_IDLE;                       
        sendOSCInt("/reset", 1);                 
      }
      break;
    case UI_PRINTING:
      if (millis() - printingStart > 15000) {    
        uiState = UI_IDLE;
        waitingForRelease = true;
      }
      break;
    case UI_COLLECT:
      if (millis() - collectStart > COLLECT_TIME) {
        uiState = UI_IDLE;
      }
      break;
  }

  fingerWas = fingerPresent;

  
  static uint8_t lastVib = 0;
  uint8_t target;
  if (!fingerPresent || uiState == UI_PRINTING || uiState == UI_COLLECT) target = 0x00;
  else if (millis() < beatPulseUntil) target = 0x7F;   
  else if (uiState == UI_READING && millis() - lastBeat > 1500)
    target = 0x50;                                     
  else target = 0x00;                                  
  if (drvReady && target != lastVib) {
    drv.setRealtimeValue(target);
    lastVib = target;
  }

  if (millis() - lastBPMSend > 200) {
    lastBPMSend = millis();
    sendOSCInt("/bpm", beatAvg);
    sendOSCInt("/finger", fingerPresent ? 1 : 0);
    sendOSCInt("/ir", irValue);
    if (uiState != lastDrawn) {
      updateDisplay();
    }
  }

  if (millis() - lastOledReinit > OLED_REINIT_INTERVAL) {
    lastOledReinit = millis();
    oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false);
    updateDisplay();
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, PRINTER_RX, PRINTER_TX);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setClock(100000);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 not found, check wiring");
  } else {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeGreen(0);
    Serial.println("MAX30102 ready");
  }

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
    Serial.println("SSD1306 not found, check wiring");
  } else {
    updateDisplay();
    Serial.println("SSD1306 ready");
  }

  if (!drv.begin()) {
    Serial.println("DRV2605 not found, check wiring");
  } else {
    drvReady = true;
    drv.selectLibrary(1);
    drv.setMode(DRV2605_MODE_REALTIME);
    drv.setRealtimeValue(0x00);
    Serial.println("DRV2605 ready");
  }

  WiFi.mode(WIFI_STA);
  connectWiFi();

  Serial.println("Ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  readHeartRate();

  // Button: OSC sent + OLED: Printing
  bool currentButton = digitalRead(BUTTON_PIN);
  if (currentButton == LOW && lastButton == HIGH) {
    sendOSCInt("/button", 1);
    Serial.println("Button OSC sent");
    if (uiState == UI_READY) {
      uiState = UI_PRINTING;
      printingStart = millis();
      updateDisplay();
    }
    delay(50);
  }
  lastButton = currentButton;

  // TCP print
  WiFiClient client = server.available();
  if (client) {
    Serial.println("Client connected");

    uiState = UI_PRINTING;
    printingStart = millis();
    updateDisplay();
    if (drvReady) drv.setRealtimeValue(0x00);   

    unsigned long t = millis();
    bool headerOk = true;
    while (client.available() < 4) {
      if (millis() - t > 3000) {
        Serial.println("Timeout waiting for header");
        client.stop();
        headerOk = false;
        break;
      }
    }
    if (!headerOk) {
      uiState = UI_IDLE;
      waitingForRelease = true;
      updateDisplay();
      sendOSCInt("/printdone", 1);
      return;
    }

    int height = 0;
    height |= client.read();
    height |= client.read() << 8;
    height |= client.read() << 16;
    height |= client.read() << 24;
    Serial.print("Height: ");
    Serial.println(height);

    int bytesPerRow = 48;
    int totalBytes = height * bytesPerRow;

    uint8_t* buf = (uint8_t*)malloc(totalBytes);
    if (!buf) {
      Serial.println("malloc failed");
      client.stop();
      uiState = UI_IDLE;
      waitingForRelease = true;
      updateDisplay();
      sendOSCInt("/printdone", 1);
      return;
    }

    int received = 0;
    unsigned long dataTimeout = millis();
    while (received < totalBytes) {
      int avail = client.available();
      if (avail > 0) {
        int n = client.read(buf + received, min(avail, totalBytes - received));
        if (n > 0) {
          received += n;
          dataTimeout = millis();
        }
      } else if (millis() - dataTimeout > 5000) {
        break;
      }
    }
    client.stop();
    Serial.print("Received: ");
    Serial.print(received);
    Serial.print(" / ");
    Serial.println(totalBytes);

    if (received == totalBytes) {
      Serial2.write(0x1B); Serial2.write(0x40);
      delay(200);
      Serial2.write(0x1D); Serial2.write(0x76);
      Serial2.write(0x30); Serial2.write(0x00);
      Serial2.write(bytesPerRow & 0xFF);
      Serial2.write((bytesPerRow >> 8) & 0xFF);
      Serial2.write(height & 0xFF);
      Serial2.write((height >> 8) & 0xFF);
      Serial2.write(buf, totalBytes);   
      Serial2.write(0x1B); Serial2.write(0x64); Serial2.write(12);
      Serial2.flush();
      Serial.println("Print done!");
      uiState = UI_COLLECT;                 
      collectStart = millis();
    } else {
      Serial.println("Incomplete data, not printing");
      uiState = UI_IDLE;                    
    }
    free(buf);

    waitingForRelease = true;               
    updateDisplay();
    sendOSCInt("/printdone", 1);
  }
}
