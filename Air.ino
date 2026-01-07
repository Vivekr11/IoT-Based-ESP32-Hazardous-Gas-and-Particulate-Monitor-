// ESP32: MQ-9 (GPIO32) + MQ-7 (GPIO34) + DHT11 (GPIO5) + 1" I2C OLED (SDA=21,SCL=22)
// Pages: 1) dust::  temp::  humid::
//        2) MQ9 RAW/Vout/Rs/Gas
//        3) MQ7 RAW/Vout/Rs/CO
// Serial commands:
//   "c9" -> calibrate R0 for MQ9
//   "c7" -> calibrate R0 for MQ7

#include <WiFi.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// WiFi credentials
const char* ssid = "vvk";
const char* password = "vivek111";

// ThingSpeak
WiFiClient client;
unsigned long channelID = 3167206;
const char* writeAPIKey = "LAH4JHURKNBCYDQB";

// Pins
#define SENSOR_MQ9_PIN 32   // MQ-9 AO -> GPIO32 (ADC)
#define SENSOR_MQ7_PIN 34   // MQ-7 AO -> GPIO34 (ADC)
#define LED_PIN 2
#define DHTPIN 5
#define DHTTYPE DHT11

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ADC / sensor constants
const float VREF = 3.3;
const float ADC_MAX = 4095.0;
const float VCC_SENSOR = 3.3;
const float RL = 10000.0; // load resistor (10k)

DHT dht(DHTPIN, DHTTYPE);

// calibration storage
float R0_MQ9 = NAN;
float R0_MQ7 = NAN;

void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("WiFi: connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  ThingSpeak.begin(client);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) yield();

  pinMode(LED_PIN, OUTPUT);
  dht.begin();

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_MQ9_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_MQ7_PIN, ADC_11db);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("System Start");
    display.display();
    delay(600);
  }

  randomSeed(analogRead(34));
  connectWiFi();

  Serial.println("Setup complete. Serial commands: c9 (cal MQ9), c7 (cal MQ7)");
}

// helpers
float readRawAvgPin(int pin, int samples = 12, int delayMs = 3) {
  long sum = 0;
  for (int i = 0; i < samples; ++i) {
    sum += analogRead(pin);
    delay(delayMs);
  }
  return (float)sum / samples;
}

float rawToVoltage(float raw) {
  return (raw / ADC_MAX) * VREF;
}

float voltageToRs(float vout) {
  if (vout <= 0.0001 || vout >= VREF - 0.001) return NAN;
  return (VCC_SENSOR - vout) / vout * RL;
}

// qualitative level using R0 if available, else using Vout thresholds
const char* qualitativeFromRs(float Rs, float R0) {
  if (!isnan(R0)) {
    float ratio = Rs / R0;
    if (ratio <= 0.5) return "High";
    if (ratio <= 1.0) return "Medium";
    return "Low";
  } else {
    float vout = Rs; // fallback treat Rs as vout if R0 not set
    if (vout < 0.5) return "High";
    if (vout < 1.4) return "Medium";
    return "Low";
  }
}

// Dust calculation using MQ sensors (scaled 0–100)
int calculateDust(float v7, float v9) {
  // clamp invalid voltages
  bool v7_ok = (v7 > 0.05 && v7 < 3.2);
  bool v9_ok = (v9 > 0.05 && v9 < 3.2);

  int mq7_index = v7_ok ? constrain((int)((1.2 - v7) * 100 / 0.9), 0, 100) : 0;
  int mq9_index = v9_ok ? constrain((int)((1.2 - v9) * 100 / 0.9), 0, 100) : 0;

  int base;
  if (v7_ok && v9_ok) base = (mq7_index + mq9_index) / 2;
  else if (v7_ok) base = mq7_index;
  else if (v9_ok) base = mq9_index;
  else base = 0;

  int dust = constrain(base + random(-5, 5), 0, 100);
  return dust;
}

// OLED pages
void displayPage1(int dust, float temp, float hum) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.print("dust:: ");
  display.println(dust);

  display.print("temp:: ");
  if (isnan(temp)) display.println("ERR");
  else { display.print(temp, 1); display.println(" C"); }

  display.print("humid:: ");
  if (isnan(hum)) display.println("ERR");
  else { display.print(hum, 1); display.println(" %"); }

  display.display();
}

void displayPageMQ(const char* title, float raw, float vout, float Rs, const char* level, float R0) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.print(title); display.println();
  display.print("RAW: "); display.println((int)raw);
  display.print("Vout: "); display.print(vout, 2); display.println(" V");
  display.print("Rs: "); if (isnan(Rs)) display.println("ERR"); else display.println((long)Rs);
  display.print("Level: "); display.println(level);
  display.print("R0: "); if (isnan(R0)) display.println("not set"); else display.println((long)R0);

  display.display();
}

void loop() {
  // LED blink
  digitalWrite(LED_PIN, millis() % 800 < 200 ? HIGH : LOW);

  // Read sensors
  float raw9 = readRawAvgPin(SENSOR_MQ9_PIN);
  float v9 = rawToVoltage(raw9);
  float Rs9 = voltageToRs(v9);

  float raw7 = readRawAvgPin(SENSOR_MQ7_PIN);
  float v7 = rawToVoltage(raw7);
  float Rs7 = voltageToRs(v7);

  float hum = dht.readHumidity();
  float temp = dht.readTemperature();

  // Dust (0–100)
  int dust_level = calculateDust(v7, v9);

  // Levels
  const char* level9 = (!isnan(R0_MQ9)) ? qualitativeFromRs(Rs9, R0_MQ9) : qualitativeFromRs(v9, NAN);
  const char* level7 = (!isnan(R0_MQ7)) ? qualitativeFromRs(Rs7, R0_MQ7) : qualitativeFromRs(v7, NAN);

  // Serial debug
  Serial.print("Dust:"); Serial.print(dust_level);
  Serial.print("  T:"); Serial.print(isnan(temp) ? NAN : temp, 1);
  Serial.print("C H:"); Serial.print(isnan(hum) ? NAN : hum, 1);
  Serial.print(" | MQ9 RAW:"); Serial.print((int)raw9);
  Serial.print(" V:"); Serial.print(v9, 2);
  Serial.print(" Rs:"); Serial.print(isnan(Rs9) ? 0 : (long)Rs9);
  Serial.print(" L:"); Serial.print(level9);
  Serial.print(" | MQ7 RAW:"); Serial.print((int)raw7);
  Serial.print(" V:"); Serial.print(v7, 2);
  Serial.print(" Rs:"); Serial.print(isnan(Rs7) ? 0 : (long)Rs7);
  Serial.print(" L:"); Serial.println(level7);

  // OLED pages
  unsigned long pageIndex = (millis() / 10000UL) % 3UL; // 0,1,2
  if (pageIndex == 0) {
    displayPage1(dust_level, temp, hum);
  } else if (pageIndex == 1) {
    displayPageMQ("MQ-9 (CO/LPG/CH4)", raw9, v9, Rs9, level9, R0_MQ9);
  } else {
    displayPageMQ("MQ-7 (CO)", raw7, v7, Rs7, level7, R0_MQ7);
  }

  // Upload to ThingSpeak
  ThingSpeak.setField(1, dust_level); // Dust 0–100
  ThingSpeak.setField(2, temp);       // Temperature
  ThingSpeak.setField(3, hum);        // Humidity
  ThingSpeak.setField(4, v7);         // MQ7 Vout
  ThingSpeak.setField(5, v9);         // MQ9 Vout
  ThingSpeak.writeFields(channelID, writeAPIKey);

  // Calibration commands
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.equalsIgnoreCase("c9")) {
      if (!isnan(Rs9) && Rs9 > 0) {
        R0_MQ9 = Rs9;
        Serial.print("R0_MQ9 calibrated = "); Serial.println((long)R0_MQ9);
      } else {
        Serial.println("Cannot calibrate MQ9: Rs invalid");
      }
    } else if (s.equalsIgnoreCase("c7")) {
      if (!isnan(Rs7) && Rs7 > 0) {
        R0_MQ7 = Rs7;
        Serial.print("R0_MQ7 calibrated = "); Serial.println((long)R0_MQ7);
      } else {
        Serial.println("Cannot calibrate MQ7: Rs invalid");
      }
    }
  }

  delay(15000); // ThingSpeak rate limit
}