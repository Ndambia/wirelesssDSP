#include <Arduino.h>
#include <HardwareSerial.h>
#include <driver/adc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_task_wdt.h>

// ── Pin & ADC ────────────────────────────────────────────
const int EOG_PIN     = 36;   // GPIO36 (VP) — input-only, ADC1_CH0, safe on WROOM
const int FILTER_SIZE = 10;

#define RING_BUF_SIZE (4096 * 6)
RingbufHandle_t ringBuf;
SemaphoreHandle_t serialMutex;

// ── Sample packet ─────────────────────────────────────────
struct Sample {
  uint32_t timestamp_us;
  uint16_t value;
};

// ── Generic parametric notch filter ──────────────────────
struct Notch {
  float b0, b1, b2, a1, a2;
  float x1=0, x2=0, y1=0, y2=0;

  Notch(float f0, float fs, float Q) {
    float w0    = 2.0f * M_PI * f0 / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    float a0    = 1.0f + alpha;
    b0 =  1.0f / a0;
    b1 = -2.0f * cosf(w0) / a0;
    b2 =  1.0f / a0;
    a1 = -2.0f * cosf(w0) / a0;
    a2 = (1.0f - alpha) / a0;
  }

  float process(float x0) {
    float y0 = b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2;
    x2=x1; x1=x0; y2=y1; y1=y0;
    return y0;
  }
};

// ── Filter chain ─────────────────────────────────────────
Notch notch50a  = Notch( 50.092f, 2000.0f, 5.0f);
Notch notch50b  = Notch( 50.092f, 2000.0f, 5.0f);
Notch notch100  = Notch(100.184f, 2000.0f, 5.0f);
Notch notch150  = Notch(150.276f, 2000.0f, 5.0f);

// ── Sampling task — Core 0 ───────────────────────────────
void samplingTask(void* pvParameters) {
  uint16_t readings[FILTER_SIZE] = {0};
  uint8_t  readIndex  = 0;
  uint32_t total      = 0;
  uint32_t lastSample = 0;

  while (true) {
    uint32_t now = micros();
    if (now - lastSample < 500) continue;
    lastSample = now;

    uint16_t raw = analogRead(EOG_PIN);

    // Moving average
    total -= readings[readIndex];
    readings[readIndex] = raw;
    total += raw;
    readIndex = (readIndex + 1) % FILTER_SIZE;
    float averaged = (float)(total / FILTER_SIZE);

    // Notch filter chain
    float filtered = notch50a.process(averaged);
    filtered        = notch50b.process(filtered);
    filtered        = notch100.process(filtered);
    filtered        = notch150.process(filtered);
    filtered        = fminf(fmaxf(filtered, 0.0f), 4095.0f);

    Sample s;
    s.timestamp_us = now;
    s.value        = (uint16_t)filtered;
    xRingbufferSend(ringBuf, &s, sizeof(s), 0);
  }
}

// ── Output task — Core 1 ─────────────────────────────────
void outputTask(void* pvParameters) {
  char buf[32];

  while (true) {
    size_t itemSize;
    Sample* s = (Sample*)xRingbufferReceive(
      ringBuf, &itemSize, pdMS_TO_TICKS(10));

    if (s != NULL) {
      uint32_t ts  = s->timestamp_us;
      uint16_t val = s->value;
      vRingbufferReturnItem(ringBuf, s);

      int len = snprintf(buf, sizeof(buf), "%lu,%u\n", ts, val);

      if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        Serial.write((uint8_t*)buf, len);
        xSemaphoreGive(serialMutex);
      }
    }
  }
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(921600);

  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(500);

  // WDT: just configure timeout, leave idle tasks alone
  esp_task_wdt_init(30, false);

  analogReadResolution(12);
  analogSetPinAttenuation(EOG_PIN, ADC_ATTEN_DB_11);  // 0–3.9V range
  for (int i = 0; i < 16; i++) analogRead(EOG_PIN);   // warm up ADC

  serialMutex = xSemaphoreCreateMutex();

  ringBuf = xRingbufferCreate(RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  if (ringBuf == NULL) {
    Serial.println("ERR: ring buffer failed");
    while (true);
  }

  xTaskCreatePinnedToCore(samplingTask, "SamplingTask", 4096, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(outputTask,   "OutputTask",   4096, NULL, 3, NULL, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}