#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_task_wdt.h>

// ── WiFi config ───────────────────────────────────────────
const char* WIFI_SSID     = "Trojan2";
const char* WIFI_PASSWORD = "Brian254";
const uint16_t UDP_PORT   = 4210;

IPAddress broadcastIP;

// ── Sampling / batching config ────────────────────────────
static constexpr uint16_t SAMPLE_RATE_HZ = 2000;
static constexpr uint16_t TIMER_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ; // 500us
static constexpr uint8_t  FILTER_SIZE = 10;
static constexpr uint8_t  BATCH_SIZE  = 20;   // 20 samples/packet => 100 packets/sec

// ── ADC pins ──────────────────────────────────────────────
const int CH0_PIN = 36;   // GPIO36 / ADC1
const int CH1_PIN = 39;   // GPIO39 / ADC1

// ── Ring buffer ───────────────────────────────────────────
#define RING_BUF_SIZE (4096 * 16)
RingbufHandle_t ringBuf = nullptr;

// ── Task handles ──────────────────────────────────────────
TaskHandle_t samplingTaskHandle = nullptr;

// ── Timer ─────────────────────────────────────────────────
hw_timer_t* sampleTimer = nullptr;

// ── Diagnostics ───────────────────────────────────────────
volatile uint32_t droppedSamples = 0;
volatile uint32_t udpSendFails   = 0;

// ── Sample packet ─────────────────────────────────────────
struct Sample {
  uint32_t timestamp_us;
  uint16_t ch0;
  uint16_t ch1;
};

// ── Broadcast IP helper ───────────────────────────────────
IPAddress makeBroadcastIP(IPAddress ip, IPAddress mask) {
  IPAddress out;
  for (int i = 0; i < 4; i++) {
    out[i] = ip[i] | (~mask[i]);
  }
  return out;
}

// ── Notch filter ──────────────────────────────────────────
struct Notch {
  float b0, b1, b2, a1, a2;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  Notch() : b0(1), b1(0), b2(0), a1(0), a2(0) {}

  Notch(float f0, float fs, float Q) {
    float w0    = 2.0f * PI * f0 / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    float a0    = 1.0f + alpha;

    b0 =  1.0f / a0;
    b1 = -2.0f * cosf(w0) / a0;
    b2 =  1.0f / a0;
    a1 = -2.0f * cosf(w0) / a0;
    a2 = (1.0f - alpha) / a0;
  }

  float process(float x0) {
    float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x0;
    y2 = y1; y1 = y0;
    return y0;
  }
};

// ── Per-channel filter state ──────────────────────────────
struct ChannelFilters {
  Notch n50a, n50b, n100, n150;

  ChannelFilters() {
    n50a = Notch( 50.092f, 2000.0f, 5.0f);
    n50b = Notch( 50.092f, 2000.0f, 5.0f);
    n100 = Notch(100.184f, 2000.0f, 5.0f);
    n150 = Notch(150.276f, 2000.0f, 5.0f);
  }

  float process(float x) {
    x = n50a.process(x);
    x = n50b.process(x);
    x = n100.process(x);
    x = n150.process(x);
    return fminf(fmaxf(x, 0.0f), 4095.0f);
  }
};

ChannelFilters ch0Filters;
ChannelFilters ch1Filters;

// ── Moving average ────────────────────────────────────────
struct MovingAvg {
  uint16_t buf[FILTER_SIZE] = {0};
  uint8_t  idx = 0;
  uint32_t total = 0;

  float update(uint16_t raw) {
    total -= buf[idx];
    buf[idx] = raw;
    total += raw;
    idx = (idx + 1) % FILTER_SIZE;
    return (float)total / FILTER_SIZE;
  }
};

MovingAvg avg0, avg1;

// ── Timer ISR ─────────────────────────────────────────────
void IRAM_ATTR onTimer() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (samplingTaskHandle != nullptr) {
    vTaskNotifyGiveFromISR(samplingTaskHandle, &xHigherPriorityTaskWoken);
  }

  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// ── Sampling task — Core 0 ───────────────────────────────
void samplingTask(void* pvParameters) {
  while (true) {
    // Block until timer ISR wakes us up
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint32_t now = micros();

    float a0 = avg0.update(analogRead(CH0_PIN));
    float a1 = avg1.update(analogRead(CH1_PIN));

    Sample s;
    s.timestamp_us = now;
    s.ch0 = (uint16_t)ch0Filters.process(a0);
    s.ch1 = (uint16_t)ch1Filters.process(a1);

    if (xRingbufferSend(ringBuf, &s, sizeof(s), 0) != pdTRUE) {
      droppedSamples++;
    }
  }
}

// ── UDP transmit task — Core 1 ───────────────────────────
void udpTask(void* pvParameters) {
  WiFiUDP udp;

  // Optional local bind
  udp.begin(UDP_PORT);

  static constexpr int BUF_SIZE = BATCH_SIZE * 24;
  char buf[BUF_SIZE];

  while (true) {
    int batchLen   = 0;
    int batchCount = 0;

    // Collect up to BATCH_SIZE samples
    while (batchCount < BATCH_SIZE) {
      size_t itemSize = 0;
      Sample* s = (Sample*)xRingbufferReceive(ringBuf, &itemSize, pdMS_TO_TICKS(20));

      if (s == nullptr) {
        break; // timeout: send partial batch if we have one
      }

      int written = snprintf(
        buf + batchLen,
        BUF_SIZE - batchLen,
        "%lu,%u,%u\n",
        (unsigned long)s->timestamp_us,
        (unsigned)s->ch0,
        (unsigned)s->ch1
      );

      vRingbufferReturnItem(ringBuf, s);

      if (written <= 0 || written >= (BUF_SIZE - batchLen)) {
        break;
      }

      batchLen += written;
      batchCount++;
    }

    if (batchLen <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!udp.beginPacket(broadcastIP, UDP_PORT)) {
      udpSendFails++;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    udp.write((const uint8_t*)buf, batchLen);

    if (!udp.endPacket()) {
      udpSendFails++;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(921600);
  delay(500);

  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  IPAddress localIP = WiFi.localIP();
  IPAddress subnet  = WiFi.subnetMask();
  broadcastIP = makeBroadcastIP(localIP, subnet);

  Serial.printf("\nConnected — IP: %s\n", localIP.toString().c_str());
  Serial.printf("Subnet mask: %s\n", subnet.toString().c_str());
  Serial.printf("Broadcasting to %s:%u\n", broadcastIP.toString().c_str(), UDP_PORT);

  // ADC setup
  analogReadResolution(12);
  //analogSetPinAttenuation(CH0_PIN, ADC_ATTEN_DB_11);
  //analogSetPinAttenuation(CH1_PIN, ADC_ATTEN_DB_11);

  for (int i = 0; i < 16; i++) {
    analogRead(CH0_PIN);
    analogRead(CH1_PIN);
  }

  // Watchdog
  esp_task_wdt_init(30, false);

  // Ring buffer
  ringBuf = xRingbufferCreate(RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  if (ringBuf == nullptr) {
    Serial.println("ERR: ring buffer failed");
    while (true) {
      delay(1000);
    }
  }

  // Timer: 80MHz / 80 = 1MHz => 1 tick = 1us
  sampleTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(sampleTimer, &onTimer, true);
  timerAlarmWrite(sampleTimer, TIMER_PERIOD_US, true);
  timerAlarmEnable(sampleTimer);

  xTaskCreatePinnedToCore(
    samplingTask,
    "SamplingTask",
    4096,
    nullptr,
    5,
    &samplingTaskHandle,
    0
  );

  xTaskCreatePinnedToCore(
    udpTask,
    "UDPTask",
    8192,
    nullptr,
    3,
    nullptr,
    1
  );
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  static uint32_t lastPrint = 0;

  if (millis() - lastPrint > 2000) {
    lastPrint = millis();
    Serial.printf(
      "WiFi=%d droppedSamples=%lu udpSendFails=%lu freeHeap=%u\n",
      WiFi.status(),
      (unsigned long)droppedSamples,
      (unsigned long)udpSendFails,
      ESP.getFreeHeap()
    );
  }

  vTaskDelay(pdMS_TO_TICKS(100));
}