#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- I2C ----------------
#define SDA_PIN 21
#define SCL_PIN 22

// ---------------- MPU ----------------
Adafruit_MPU6050 mpu;
bool mpu_ok = false;

// ---------------- TEMP ----------------
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
bool temp_ok = false;

// ---------------- ECG ----------------
#define ECG_PIN 34
#define LO_PLUS 32
#define LO_MINUS 33

// ---------------- OUTPUTS ----------------
#define LED_GREEN 25
#define LED_YELLOW 26
#define LED_RED 27
#define BUZZER 14

// ---------------- FLAGS ----------------
bool oled_ok = false;

// ---------------- VARIABLES ----------------
float temperatureC = 0;
float motion = 0;
float bpm = 0;
float smoothBPM = 0;

int ecgValue = 0;

unsigned long lastPeak = 0;
bool peakDetected = false;

unsigned long lastTempRead = 0;

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);

  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  Serial.println("SYSTEM STARTING...");

  // OLED INIT (SAFE I2C)
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oled_ok = true;
    Serial.println("OLED OK");
  } else {
    Serial.println("OLED FAIL");
  }

  // MPU INIT
  if (mpu.begin()) {
    mpu_ok = true;
    Serial.println("MPU OK");
  } else {
    Serial.println("MPU FAIL");
  }

  // TEMP INIT
  tempSensor.begin();
  temp_ok = true;

  // OLED START SCREEN
  if (oled_ok) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(WHITE);

    display.println("SYSTEM READY");
    display.println(oled_ok ? "OLED OK" : "OLED FAIL");
    display.println(mpu_ok ? "MPU OK" : "MPU FAIL");
    display.println("TEMP OK");

    display.display();
    delay(1500);
  }
}

// ---------------- LOOP ----------------
void loop() {

  readECG();
  readMPU();
  readTemp();
  classifyAndAlert();

  printSerial();
  updateOLED();

  delay(200);
}

// ---------------- ECG ----------------
void readECG() {

  if (digitalRead(LO_PLUS) || digitalRead(LO_MINUS)) {
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

    lastPeak = now;
    peakDetected = true;
  }

  if (ecgValue < 1800) {
    peakDetected = false;
  }
}

// ---------------- MPU ----------------
void readMPU() {

  if (!mpu_ok) {
    motion = 0;
    return;
  }

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  motion = sqrt(
    a.acceleration.x * a.acceleration.x +
    a.acceleration.y * a.acceleration.y +
    a.acceleration.z * a.acceleration.z
  );
}

// ---------------- TEMP ----------------
void readTemp() {

  if (!temp_ok) return;

  if (millis() - lastTempRead > 1000) {
    tempSensor.requestTemperatures();
    temperatureC = tempSensor.getTempCByIndex(0);
    lastTempRead = millis();
  }
}

// ---------------- CLASSIFY + ALERT ----------------
void classifyAndAlert() {

  String state = "NORMAL";

  if (smoothBPM > 0 && (smoothBPM > 120 || smoothBPM < 50 || temperatureC > 38)) {
    state = "CRITICAL";
  }
  else if (motion > 12) {
    state = "FALSE_ALARM";
  }

  digitalWrite(LED_GREEN, state == "NORMAL");
  digitalWrite(LED_YELLOW, state == "FALSE_ALARM");
  digitalWrite(LED_RED, state == "CRITICAL");

  // Beep pattern
  if (state == "CRITICAL") {
    digitalWrite(BUZZER, millis() % 500 < 250);
  } else {
    digitalWrite(BUZZER, LOW);
  }
}

// ---------------- SERIAL ----------------
void printSerial() {
  Serial.print("ECG: "); Serial.print(ecgValue);
  Serial.print(" BPM: "); Serial.print(smoothBPM);
  Serial.print(" Temp: "); Serial.print(temperatureC);
  Serial.print(" Motion: "); Serial.println(motion);
}

// ---------------- OLED ----------------
void updateOLED() {

  if (!oled_ok) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("HEALTH MONITOR");

  display.print("BPM: ");
  display.println(smoothBPM);

  display.print("Temp: ");
  display.println(temperatureC);

  display.print("Motion: ");
  display.println(motion);

  display.display();
}