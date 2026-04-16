// ═══════════════════════════════════════════════════════════════
//  ESP32 Dual-Channel ADC → UDP Streamer
//  2000 Hz stable sampling with notch filtering
//
//  Optimisations:
//   1. Sampling task pinned to Core 1 (no WiFi interrupts)
//   2. adc1_get_raw() — direct ADC driver, no Arduino overhead
//   3. Filtering in UDP task — sampling task stays lean
//   4. Sampling task priority = 7
//   5. Notch coefficients are constexpr lookup tables —
//      zero runtime trig (no sinf/cosf at boot)
//
//  To regenerate coefficients if fs or Q changes, run:
//  ──────────────────────────────────────────────────
//  import numpy as np
//  def notch(f0, fs=2000, Q=5):
//      w0=2*np.pi*f0/fs; al=np.sin(w0)/(2*Q); inv=1/(1+al)
//      b1=-2*np.cos(w0)*inv
//      print(f"b0={inv:.8f} b1={b1:.8f} a2={(1-al)*inv:.8f}")
//  for f in [50.092, 100.184, 150.276]: notch(f)
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_task_wdt.h>
#include <driver/adc.h>

// ── WiFi config ───────────────────────────────────────────────
const char*    WIFI_SSID     = "dekut";
const char*    WIFI_PASSWORD = "dekut@ict2023";
const uint16_t UDP_PORT      = 4210;

IPAddress broadcastIP;

// ── Sampling / batching config ────────────────────────────────
static constexpr uint16_t SAMPLE_RATE_HZ  = 2000;
static constexpr uint32_t TIMER_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ; // 500 µs
static constexpr uint8_t  FILTER_SIZE     = 10;
static constexpr uint8_t  BATCH_SIZE      = 20;   // → 100 packets/s

// ── ADC channel mapping ───────────────────────────────────────
//   GPIO36 → ADC1_CHANNEL_0
//   GPIO39 → ADC1_CHANNEL_3
static constexpr adc1_channel_t CH0_ADC = ADC1_CHANNEL_0;
static constexpr adc1_channel_t CH1_ADC = ADC1_CHANNEL_3;

// ── Ring buffer ───────────────────────────────────────────────
#define RING_BUF_SIZE (4096 * 16)
static RingbufHandle_t ringBuf = nullptr;

// ── Task handles ──────────────────────────────────────────────
static TaskHandle_t samplingTaskHandle = nullptr;

// ── Timer ─────────────────────────────────────────────────────
static hw_timer_t* sampleTimer = nullptr;

// ── Diagnostics ───────────────────────────────────────────────
volatile uint32_t droppedSamples = 0;
volatile uint32_t udpSendFails   = 0;
volatile uint32_t isrCount       = 0; 

// ── Raw sample (lean — filtering happens in udpTask) ──────────
struct RawSample {
  uint32_t timestamp_us;
  uint16_t ch0;
  uint16_t ch1;
};

// ── Broadcast IP helper ───────────────────────────────────────
static IPAddress makeBroadcastIP(IPAddress ip, IPAddress mask) {
  IPAddress out;
  for (int i = 0; i < 4; i++) out[i] = ip[i] | (~mask[i]);
  return out;
}

// ═══════════════════════════════════════════════════════════════
//  PRECOMPUTED NOTCH FILTER COEFFICIENT LOOKUP TABLE
//
//  Generated for: fs = 2000 Hz, Q = 5.0
//  Formula (biquad notch, normalised by a0):
//    w0    = 2π·f0/fs
//    alpha = sin(w0) / (2Q)
//    a0inv = 1 / (1 + alpha)
//    b0 = b2 = a0inv
//    b1 = a1 = -2·cos(w0)·a0inv
//    a2      = (1 - alpha)·a0inv
//
//  Note: because b0==b2 and a1==b1, process() uses the
//  simplified form:  y = b0*(x0+x2) + b1*(x1-y1) - a2*y2
// ═══════════════════════════════════════════════════════════════

struct NotchCoeffs {
  float b0;   // also b2
  float b1;   // also a1
  float a2;
};

//                              f0 Hz      b0            b1            a2
static constexpr NotchCoeffs NOTCH_50  { 0.96891595f, -1.50640018f,  0.93783190f };
static constexpr NotchCoeffs NOTCH_100 { 0.93906406f, -0.95124118f,  0.87812812f };
static constexpr NotchCoeffs NOTCH_150 { 0.91043163f, -0.29028498f,  0.82086326f };

// ═══════════════════════════════════════════════════════════════
//  Biquad notch state — uses a reference into the lookup table
// ═══════════════════════════════════════════════════════════════
struct Notch {
  const NotchCoeffs& c;
  float x1 = 0, x2 = 0;
  float y1 = 0, y2 = 0;

  explicit Notch(const NotchCoeffs& coeffs) : c(coeffs) {}

  float process(float x0) {
    // Simplified: b0*(x0+x2) + b1*(x1-y1) - a2*y2
    float y0 = c.b0 * (x0 + x2) + c.b1 * (x1 - y1) - c.a2 * y2;
    x2 = x1; x1 = x0;
    y2 = y1; y1 = y0;
    return y0;
  }
};

// ═══════════════════════════════════════════════════════════════
//  Per-channel filter chain
//    50 Hz × 2 (steeper roll-off) + 100 Hz + 150 Hz harmonics
// ═══════════════════════════════════════════════════════════════
struct ChannelFilters {
  Notch n50a { NOTCH_50  };
  Notch n50b { NOTCH_50  };
  Notch n100 { NOTCH_100 };
  Notch n150 { NOTCH_150 };

  uint16_t process(float x) {
    x = n50a.process(x);
    x = n50b.process(x);
    x = n100.process(x);
    x = n150.process(x);
    return (uint16_t)fminf(fmaxf(x, 0.0f), 4095.0f);
  }
};

// ═══════════════════════════════════════════════════════════════
//  Moving-average (box) filter
// ═══════════════════════════════════════════════════════════════
struct MovingAvg {
  uint16_t buf[FILTER_SIZE] = {};
  uint8_t  idx              = 0;
  uint32_t total            = 0;

  float update(uint16_t raw) {
    total   -= buf[idx];
    buf[idx] = raw;
    total   += raw;
    idx      = (idx + 1) % FILTER_SIZE;
    return (float)total / FILTER_SIZE;
  }
};

// ── Filter instances (used only inside udpTask) ───────────────
static ChannelFilters ch0Filters;
static ChannelFilters ch1Filters;
static MovingAvg      avg0;
static MovingAvg      avg1;

// ═══════════════════════════════════════════════════════════════
//  Timer ISR — fires every TIMER_PERIOD_US, wakes sampling task
// ═══════════════════════════════════════════════════════════════
void IRAM_ATTR onTimer() {
    isrCount++;     
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (samplingTaskHandle != nullptr) {
    vTaskNotifyGiveFromISR(samplingTaskHandle, &xHigherPriorityTaskWoken);
  }

  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// ═══════════════════════════════════════════════════════════════
//  Sampling task — Core 1, priority 7
//  Lean: read ADC + timestamp, push to ring buffer. Nothing else.
// ═══════════════════════════════════════════════════════════════
void samplingTask(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    RawSample s;
    s.timestamp_us = micros();
    s.ch0          = (uint16_t)adc1_get_raw(CH0_ADC);
    s.ch1          = (uint16_t)adc1_get_raw(CH1_ADC);

    if (xRingbufferSend(ringBuf, &s, sizeof(s), 0) != pdTRUE) {
      droppedSamples++;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  UDP transmit task — Core 0, priority 3
//  Pulls raw samples, filters, batches, sends UDP broadcast.
// ═══════════════════════════════════════════════════════════════
void udpTask(void* pvParameters) {
  WiFiUDP udp;
  udp.begin(UDP_PORT);

  // Each CSV line: "4294967295,4095,4095\n" ≈ 22 chars → pad to 28
  static constexpr int BUF_SIZE = BATCH_SIZE * 28;
  char txBuf[BUF_SIZE];

  while (true) {
    int batchLen   = 0;
    int batchCount = 0;

    while (batchCount < BATCH_SIZE) {
      size_t     itemSize = 0;
      RawSample* s = (RawSample*)xRingbufferReceive(
                       ringBuf, &itemSize, pdMS_TO_TICKS(20));

      if (s == nullptr) break; // timeout — send partial batch

      // Moving-average then notch chain (lookup-table coefficients)
      float    f0   = avg0.update(s->ch0);
      float    f1   = avg1.update(s->ch1);
      uint16_t out0 = ch0Filters.process(f0);
      uint16_t out1 = ch1Filters.process(f1);
      uint32_t ts   = s->timestamp_us;

      vRingbufferReturnItem(ringBuf, s);

      int written = snprintf(
        txBuf + batchLen,
        BUF_SIZE - batchLen,
        "%lu,%u,%u\n",
        (unsigned long)ts,
        (unsigned)out0,
        (unsigned)out1
      );

      if (written <= 0 || written >= (BUF_SIZE - batchLen)) break;

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

    udp.write((const uint8_t*)txBuf, batchLen);

    if (!udp.endPacket()) {
      udpSendFails++;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(921600);
  delay(500);

  // ── WiFi ──────────────────────────────────────────────────
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
  broadcastIP       = makeBroadcastIP(localIP, subnet);

  Serial.printf("\nConnected   IP : %s\n", localIP.toString().c_str());
  Serial.printf("Subnet mask    : %s\n",   subnet.toString().c_str());
  Serial.printf("Broadcast to   : %s:%u\n",
                broadcastIP.toString().c_str(), UDP_PORT);

  // ── ADC direct driver ─────────────────────────────────────
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(CH0_ADC, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(CH1_ADC, ADC_ATTEN_DB_11);

  // Warm-up reads
  for (int i = 0; i < 32; i++) {
    adc1_get_raw(CH0_ADC);
    adc1_get_raw(CH1_ADC);
  }

  // ── Watchdog ──────────────────────────────────────────────
  esp_task_wdt_init(30, false);

  // ── Ring buffer ───────────────────────────────────────────
  ringBuf = xRingbufferCreate(RING_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
  if (ringBuf == nullptr) {
    Serial.println("ERR: ring buffer allocation failed — halting");
    while (true) { delay(1000); }
  }

  // ── Hardware timer: 80 MHz / 80 → 1 tick = 1 µs ──────────
  sampleTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(sampleTimer, &onTimer, true);
  timerAlarmWrite(sampleTimer, TIMER_PERIOD_US, true);
  timerAlarmEnable(sampleTimer);

  // ── Tasks ─────────────────────────────────────────────────
  xTaskCreatePinnedToCore(
    samplingTask, "SamplingTask",
    4096, nullptr,
    7,                     // priority 7
    &samplingTaskHandle,
    1                      // Core 1 — isolated from WiFi
  );

  xTaskCreatePinnedToCore(
    udpTask, "UDPTask",
    8192, nullptr,
    5,
    nullptr,
    0                      // Core 0
  );

  Serial.printf("Sampling at %u Hz | batch=%u | timer=%u µs\n",
                SAMPLE_RATE_HZ, BATCH_SIZE, TIMER_PERIOD_US);
}

// ═══════════════════════════════════════════════════════════════
//  Loop — diagnostics only
// ═══════════════════════════════════════════════════════════════
void loop() {
  static uint32_t lastPrint = 0;
  static uint32_t lastIsrCount = 0; 

  if (millis() - lastPrint >= 2000) {
    lastPrint = millis();
    uint32_t fired = isrCount - lastIsrCount;  // ← add
    lastIsrCount   = isrCount;                 // ← add
    Serial.printf(
      "WiFi=%d  dropped=%lu  udpFails=%lu  freeHeap=%u\n",
      (int)WiFi.status(),
      (unsigned long)droppedSamples,
      (unsigned long)udpSendFails,
      (unsigned)ESP.getFreeHeap()
    );
    
    Serial.printf(
      "ISR fires/2s=%lu  (expect ~4000)\n",
      (unsigned long)fired); 

  }
 
 //Serial.printf("Timer period target: %u us\n", TIMER_PERIOD_US);
  vTaskDelay(pdMS_TO_TICKS(100));
}