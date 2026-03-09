/*
 * Power Monitoring System
 * ESP32-C3 Super Mini
 *
 * Sensors:
 *   - CT1 (SCT-013-060): GPIO2 - Current Line 1
 *   - CT2 (SCT-013-060): GPIO3 - Current Line 2
 *   - ZMPT101B:          GPIO4 - AC Voltage
 *
 * Sends JSON via HTTPS POST every 5 seconds: {"v":220.0,"i1":8.0,"i2":5.0}
 * OTA updates enabled for wireless firmware uploads
 *
 * Copy config.example.h to config.h and fill in your credentials.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include "config.h"

// Pin definitions
const int CT1_PIN = 2;
const int CT2_PIN = 3;
const int VOLT_PIN = 4;

// Calibration constants (adjust these against a reference clamp meter)
const float CT1_RATIO = 101.1;
const float CT2_RATIO = 101.1;
const float VOLT_CALIBRATION = 688.8;

// Noise thresholds (readings below these are zeroed out)
const float CURRENT_NOISE_THRESHOLD = 0.5;  // Amps
const float VOLTAGE_NOISE_THRESHOLD = 5.0;  // Volts

// Sampling: 1000 samples over ~5 full 50Hz cycles (100ms)
const int SAMPLES = 1000;

// Timing
const unsigned long SEND_INTERVAL = 5000;  // 5 seconds
unsigned long lastSendTime = 0;

// Variables
float voltage_rms = 0;
float current1_rms = 0;
float current2_rms = 0;

void setup() {
  Serial.begin(115200);

  unsigned long startWait = millis();
  while (!Serial && millis() - startWait < 3000) {
    delay(10);
  }
  delay(500);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial.println();
  Serial.println("=== Power Monitor v2 (OTA) ===");

  connectWiFi();
  setupOTA();
}

void connectWiFi() {
  Serial.print("WiFi: ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" OK (");
    Serial.print(WiFi.localIP());
    Serial.println(")");
  } else {
    Serial.println(" FAILED");
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("power-monitor");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: Update starting...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: Done! Rebooting...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA: Ready");
}

// Measure RMS using analogReadMilliVolts for linear ADC response
// Uses per-batch DC offset removal (high-pass filter)
float measureRMS(int pin) {
  // First pass: collect samples using calibrated millivolt readings
  float samples[SAMPLES];
  float sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    samples[i] = analogReadMilliVolts(pin) / 1000.0;  // Convert mV to V
    sum += samples[i];
    delayMicroseconds(100);  // ~100us per sample = 100ms total = 5 cycles at 50Hz
  }
  float dcOffset = sum / SAMPLES;

  // Second pass: calculate RMS with DC offset removed
  float sumSquares = 0;
  for (int i = 0; i < SAMPLES; i++) {
    float v = samples[i] - dcOffset;
    sumSquares += v * v;
  }
  return sqrt(sumSquares / SAMPLES);
}

float measureCurrent(int pin, float ratio) {
  float vRMS = measureRMS(pin);
  float current = vRMS * ratio;
  return (current < CURRENT_NOISE_THRESHOLD) ? 0.0 : current;
}

float measureVoltage(int pin) {
  float vRMS = measureRMS(pin);
  float volts = vRMS * VOLT_CALIBRATION;
  return (volts < VOLTAGE_NOISE_THRESHOLD) ? 0.0 : volts;
}

void sendToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" [WiFi lost, reconnecting...]");
    connectWiFi();
    return;
  }

  String json = "{\"v\":" + String(voltage_rms, 2)
              + ",\"i1\":" + String(current1_rms, 2)
              + ",\"i2\":" + String(current2_rms, 2) + "}";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(json);

  Serial.print(" -> ");
  if (httpCode == 200) {
    Serial.println("OK");
  } else if (httpCode > 0) {
    Serial.print("ERR ");
    Serial.println(httpCode);
  } else {
    Serial.print("FAIL: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

void loop() {
  ArduinoOTA.handle();

  voltage_rms = measureVoltage(VOLT_PIN);
  current1_rms = measureCurrent(CT1_PIN, CT1_RATIO);
  current2_rms = measureCurrent(CT2_PIN, CT2_RATIO);

  Serial.print("V:");
  Serial.print(voltage_rms, 1);
  Serial.print(" I1:");
  Serial.print(current1_rms, 2);
  Serial.print(" I2:");
  Serial.print(current2_rms, 2);

  if (millis() - lastSendTime >= SEND_INTERVAL) {
    sendToServer();
    lastSendTime = millis();
  } else {
    Serial.println();
  }

  delay(500);
}
