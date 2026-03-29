/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║         BioSignal Monitor — ESP32 WebSocket Firmware         ║
 * ║  Streams ECG, BPM, Temperature, Motion over WebSocket        ║
 * ║  Dashboard connects to ws://<ESP32_IP>:81                    ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * Libraries needed (install via Arduino Library Manager):
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - Adafruit MPU6050
 *   - Adafruit Unified Sensor
 *   - OneWire
 *   - DallasTemperature
 *   - WebSockets  (by Markus Sattler — search "WebSockets")
 *   - ArduinoJson (by Benoit Blanchon — search "ArduinoJson")
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ──────────────────────────────────────────────────────────────────────────────
//  ★  CHANGE THESE TO YOUR WIFI CREDENTIALS  ★
// ──────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "NSTL";
const char* WIFI_PASSWORD = "18931618";
// ──────────────────────────────────────────────────────────────────────────────

// ── OLED ──────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oled_ok = false;

// ── I2C ───────────────────────────────────────────────────────────────────────
#define SDA_PIN 21
#define SCL_PIN 22

// ── MPU6050 ───────────────────────────────────────────────────────────────────
Adafruit_MPU6050 mpu;
bool mpu_ok = false;

// ── DS18B20 TEMP ──────────────────────────────────────────────────────────────
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
bool temp_ok = false;

// ── ECG (AD8232) ──────────────────────────────────────────────────────────────
#define ECG_PIN   34
#define LO_PLUS   32
#define LO_MINUS  33

// ── OUTPUTS ───────────────────────────────────────────────────────────────────
#define LED_GREEN  25
#define LED_YELLOW 26
#define LED_RED    27
#define BUZZER     14

// ── WebSocket Server (port 81) ────────────────────────────────────────────────
WebSocketsServer wsServer(81);

// ── Vitals state ──────────────────────────────────────────────────────────────
float temperatureC = 0;
float motion       = 0;
float bpm          = 0;
float smoothBPM    = 0;
int   ecgValue     = 0;
bool  leadOff      = false;

unsigned long lastPeak    = 0;
bool          peakDetected = false;
unsigned long lastTempRead = 0;

// ── Timing ───────────────────────────────────────────────────────────────────
unsigned long lastECGSend    = 0;   // ECG raw: every 5 ms  (~200 sps)
unsigned long lastVitalsSend = 0;   // Vitals : every 200 ms
unsigned long lastOLEDUpdate = 0;   // OLED   : every 500 ms

// ── WebSocket connected clients ───────────────────────────────────────────────
uint8_t connectedClient = 255;      // 255 = nobody

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);

  pinMode(LO_PLUS,  INPUT);
  pinMode(LO_MINUS, INPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  pinMode(BUZZER,     OUTPUT);

  // ── OLED init ────────────────────────────────────────────────────────────
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oled_ok = true;
    oledBoot("OLED  OK");
  } else {
    Serial.println("[OLED] FAIL");
  }

  // ── MPU6050 init ─────────────────────────────────────────────────────────
  if (mpu.begin()) {
    mpu_ok = true;
    oledBoot("MPU   OK");
    Serial.println("[MPU] OK");
  } else {
    Serial.println("[MPU] FAIL — motion will read 0");
  }

  // ── DS18B20 init ─────────────────────────────────────────────────────────
  tempSensor.begin();
  temp_ok = true;
  oledBoot("TEMP  OK");
  Serial.println("[TEMP] OK");

  // ── WiFi connect ─────────────────────────────────────────────────────────
  oledMsg("Connecting WiFi...");
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    oledMsg("WiFi OK\n" + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] FAILED — running without network");
    oledMsg("WiFi FAILED\nStandalone mode");
    delay(2000);
  }

  // ── WebSocket server ──────────────────────────────────────────────────────
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  Serial.printf("[WS] Server started on port 81\n");
  Serial.printf("[WS] Dashboard URL: http://%s  (open in browser)\n",
                WiFi.localIP().toString().c_str());

  // ── Ready screen ─────────────────────────────────────────────────────────
  if (oled_ok) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("=== BIOSIGNAL ===");
    display.println("WiFi: " + (WiFi.status() == WL_CONNECTED ?
                    WiFi.localIP().toString() : "OFFLINE"));
    display.println("WS Port: 81");
    display.println("Waiting client...");
    display.display();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  wsServer.loop();                    // MUST call every loop

  unsigned long now = millis();

  readECG();
  readMPU();
  readTemp();
  classifyAndAlert();

  // ── Send raw ECG sample at ~200 sps ──────────────────────────────────────
  if (now - lastECGSend >= 5) {
    lastECGSend = now;
    if (connectedClient != 255) {
      // Compact single-line JSON for speed: {"t":"ecg","v":2048,"lo":0}
      char buf[48];
      snprintf(buf, sizeof(buf), "{\"t\":\"ecg\",\"v\":%d,\"lo\":%d}",
               ecgValue, leadOff ? 1 : 0);
      wsServer.sendTXT(connectedClient, buf);
    }
  }

  // ── Send vitals snapshot every 200 ms ────────────────────────────────────
  if (now - lastVitalsSend >= 200) {
    lastVitalsSend = now;
    if (connectedClient != 255) {
      StaticJsonDocument<192> doc;
      doc["t"]      = "vitals";
      doc["bpm"]    = (int)round(smoothBPM);
      doc["temp"]   = round(temperatureC * 10.0) / 10.0;
      doc["motion"] = round(motion * 100.0) / 100.0;
      doc["lo"]     = leadOff;

      char buf[192];
      serializeJson(doc, buf);
      wsServer.sendTXT(connectedClient, buf);
    }
    printSerial();
  }

  // ── OLED update every 500 ms ─────────────────────────────────────────────
  if (now - lastOLEDUpdate >= 500) {
    lastOLEDUpdate = now;
    updateOLED();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocket event handler
// ─────────────────────────────────────────────────────────────────────────────
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      connectedClient = num;
      Serial.printf("[WS] Client #%u connected from %s\n", num,
                    wsServer.remoteIP(num).toString().c_str());
      // Send a hello/handshake so the dashboard knows device info
      {
        StaticJsonDocument<128> hello;
        hello["t"]       = "hello";
        hello["device"]  = "ESP32-BioSignal";
        hello["version"] = "1.0";
        hello["sps"]     = 200;
        char buf[128];
        serializeJson(hello, buf);
        wsServer.sendTXT(num, buf);
      }
      if (oled_ok) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(WHITE);
        display.setCursor(0,0);
        display.println("=== BIOSIGNAL ===");
        display.println("Dashboard LIVE");
        display.display();
      }
      break;

    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client #%u disconnected\n", num);
      connectedClient = 255;
      break;

    case WStype_TEXT:
      // Accept optional ping from dashboard: {"cmd":"ping"}
      Serial.printf("[WS] Msg from #%u: %s\n", num, payload);
      break;

    default:
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor reads (same logic as original, unchanged)
// ─────────────────────────────────────────────────────────────────────────────
void readECG() {
  leadOff = (digitalRead(LO_PLUS) || digitalRead(LO_MINUS));

  if (leadOff) {
    bpm = 0;
    return;
  }

  ecgValue = (analogRead(ECG_PIN) + analogRead(ECG_PIN)) / 2;

  if (ecgValue > 1800 && !peakDetected) {
    unsigned long now = millis();
    if (lastPeak != 0 && (now - lastPeak > 300)) {
      bpm = 60000.0 / (now - lastPeak);
      smoothBPM = (0.7 * smoothBPM) + (0.3 * bpm);
    }
    lastPeak    = now;
    peakDetected = true;
  }
  if (ecgValue < 1800) peakDetected = false;
}

void readMPU() {
  if (!mpu_ok) { motion = 0; return; }
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  motion = sqrt(
    a.acceleration.x * a.acceleration.x +
    a.acceleration.y * a.acceleration.y +
    a.acceleration.z * a.acceleration.z
  );
}

void readTemp() {
  if (!temp_ok) return;
  if (millis() - lastTempRead > 1000) {
    tempSensor.requestTemperatures();
    float raw = tempSensor.getTempCByIndex(0);
    if (raw != DEVICE_DISCONNECTED_C) temperatureC = raw;
    lastTempRead = millis();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void classifyAndAlert() {
  String state = "NORMAL";
  if (smoothBPM > 0 && (smoothBPM > 120 || smoothBPM < 50 || temperatureC > 38)) {
    state = "CRITICAL";
  } else if (motion > 10.5) {
    state = "FALSE_ALARM";
  }

  digitalWrite(LED_GREEN,  state == "NORMAL");
  digitalWrite(LED_YELLOW, state == "FALSE_ALARM");
  digitalWrite(LED_RED,    state == "CRITICAL");

  if (state == "CRITICAL") {
    digitalWrite(BUZZER, millis() % 500 < 250);
  } else {
    digitalWrite(BUZZER, LOW);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void printSerial() {
  Serial.printf("ECG:%d  BPM:%.1f  Temp:%.1f  Motion:%.2f  LO:%d\n",
                ecgValue, smoothBPM, temperatureC, motion, leadOff ? 1 : 0);
}

// ─────────────────────────────────────────────────────────────────────────────
void updateOLED() {
  if (!oled_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("=== BIOSIGNAL ===");
  display.print("BPM:  "); display.println((int)round(smoothBPM));
  display.print("Temp: "); display.print(temperatureC, 1); display.println(" C");
  display.print("Mot:  "); display.print(motion, 2);       display.println(" m/s2");
  display.print("ECG:  "); display.println(ecgValue);
  if (connectedClient != 255) {
    display.println("WS: LIVE");
  } else {
    display.print("IP: ");
    display.println(WiFi.localIP().toString());
  }
  display.display();
}

// ─────────────────────────────────────────────────────────────────────────────
//  OLED helpers
// ─────────────────────────────────────────────────────────────────────────────
void oledBoot(const String& msg) {
  if (!oled_ok) return;
  static int line = 0;
  display.print(msg); display.println(" OK");
  display.display();
  line++;
}

void oledMsg(const String& msg) {
  if (!oled_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(msg);
  display.display();
  delay(800);
}
