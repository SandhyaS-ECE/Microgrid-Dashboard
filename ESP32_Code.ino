/***********************************************************************
   ESP32-S3 EDGE GATEWAY  —  FreeRTOS Dual-Core Rewrite
   Project : LoRa-Enabled Metering and Predictive Usage Notifications
             for Rural Microgrids
   Target  : ESP32-S3 (dual Xtensa LX7 cores)

   ═══════════════════════════════════════════════════════════════════
   ARCHITECTURE OVERVIEW
   ═══════════════════════════════════════════════════════════════════

   ┌─────────────────────────────┐   ┌─────────────────────────────┐
   │  CORE 1  –  Radio & Math   │   │  CORE 0  –  Cloud & HTTP    │
   │  (Arduino loop())          │   │  (cloudTask FreeRTOS task)  │
   ├─────────────────────────────┤   ├─────────────────────────────┤
   │ • LoRa.parsePacket()        │   │ • WiFi reconnect watchdog   │
   │ • Checksum validation       │   │ • Firebase RTDB PUT         │
   │ • Digital-twin rate calc    │   │ • Telegram POST             │
   │ • EMA smoothing             │   │ • Node-offline alerts       │
   │ • Billing forecast          │   │ • vTaskDelay() yields CPU   │
   │ • Preferences (flash) write │   │                             │
   │ • Node watchdog logic       │   │                             │
   └──────────┬──────────────────┘   └──────────────┬──────────────┘
              │                                      │
              │        ┌──────────────────┐          │
              └───────►│  Shared Memory   │◄─────────┘
                       │  houses[]        │
                       │  pendingAlerts[] │
                       │  Protected by    │
                       │  dataMutex       │
                       └──────────────────┘

   THREAD SAFETY MODEL
   ───────────────────
   A single FreeRTOS mutex (dataMutex) guards all reads and writes to
   the shared houses[] array and pendingAlerts[] queue.

   Core 1 locks the mutex to:
     • Write parsed telemetry into HouseData
     • Compute the new prediction and update status

   Core 0 locks the mutex to:
     • Take a snapshot copy of HouseData before each HTTP call
     • Drain pending alert strings from pendingAlerts[]

   HTTP calls (which can block for 1–2 s during TLS handshake) are
   ALWAYS made OUTSIDE the mutex lock so Core 1 is never stalled.

   DIGITAL TWIN TIME MODEL
   ────────────────────────
   The simulator drives 1 real second ≡ 1 virtual hour.
   Rather than using millis() for Δt, the firmware counts the number
   of packets received per node (virtual_hour). Each new packet
   represents exactly 1 elapsed virtual hour. Consumption rate is:

       rate_kwh_hr = current_kwh − previous_kwh   (Δ over 1 virtual hr)

   The rate is EMA-smoothed, then projected:

       remaining_hrs  = 720 − virtual_hour
       predicted_kwh  = cumulative_kwh + smoothed_rate × remaining_hrs

************************************************************************/

// ─── Arduino + ESP32 SDK ────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // required for HTTPS on ESP32
#include <Preferences.h>
#include <SPI.h>
#include <LoRa.h>

// ─── FreeRTOS (included by ESP32 Arduino core) ──────────────────────
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/***********************************************************************
   ===================== USER CONFIGURATION ===========================
   Change these values before flashing.
************************************************************************/

// Wi-Fi credentials
const char* WIFI_SSID        = "IOT";
const char* WIFI_PASS        = "IOT123456" ;

// Firebase Realtime Database
const char* FIREBASE_HOST    = "https://solaris-4ca9a-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char* FIREBASE_AUTH    = "Secret";

// Telegram bot
const char* TELEGRAM_TOKEN   = "Secret";
const char* TELEGRAM_CHAT_ID = "6886162366" ;

// LoRa hardware pins (adjust to your PCB)
#define LORA_SS    10
#define LORA_RST    9
#define LORA_DIO0   2

/***********************************************************************
   ===================== SYSTEM CONSTANTS ==============================
************************************************************************/

#define NUM_HOUSES            10
#define BASE_HOUSE_ID        101

// FIX 3 – Demo-optimised watchdog: 15 s instead of 3 min
// At 1 real-second = 1 virtual-hour this represents ~4 missed packets.
#define OFFLINE_TIMEOUT_MS  30000UL

// Cloud task polling period; long enough to batch-drain alerts but
// short enough to keep the Firebase dashboard responsive.
#define CLOUD_POLL_MS        2000UL

// Wi-Fi reconnect attempt interval on Core 0
#define WIFI_RECONNECT_MS    8000UL

// EMA weight for consumption-rate smoothing (0 < α ≤ 1)
// Higher → more reactive; lower → smoother.
#define ALPHA                0.35f

// Billing cycle length in virtual hours (30 days × 24 h)
#define BILLING_HOURS        720

// Maximum pending Telegram alert strings queued between cores
#define MAX_PENDING_ALERTS    32
#define ALERT_MAX_LEN        160

/***********************************************************************
   ===================== DATA STRUCTURES ===============================
************************************************************************/

// FIX 3 – NODE_PENDING prevents false offline alerts at boot.
enum NodeStatus : uint8_t
{
  NODE_PENDING,    // initial state — watchdog is silent
  NODE_OK,         // receiving packets normally
  NODE_OFFLINE,    // missed OFFLINE_TIMEOUT_MS — alert fired
  NODE_DATA_FAULT  // sensor reported ERR field
};

// Complete per-node state.  Written by Core 1, read by Core 0.
struct HouseData
{
  int           id;

  // Live telemetry
  float         power_w;            // instantaneous power (W)
  float         cumulative_kwh;     // running total from node
  float         last_flash_kwh;     // <-- ADD THIS LINE

  // FIX 2 – Digital-twin prediction fields
  float         prev_kwh;           // kWh at the previous virtual hour
  uint32_t      virtual_hour;       // packet counter = elapsed virtual hours
  float         smoothed_rate;      // EMA of hourly consumption (kWh/hr)
  float         predicted_kwh;      // projected end-of-month total

  NodeStatus    status;
  unsigned long last_seen_ms;       // real millis() of last packet

  // Alert de-duplication flags
  bool          warn200;
  bool          warn400;
  bool          warn500;

  // Flag set by Core 1; cleared by Core 0 after successful Firebase PUT
  volatile bool dirty;
};

/***********************************************************************
   ===================== SHARED STATE ==================================
   All fields below are protected by dataMutex.
   Core 1 writes; Core 0 reads snapshot copies.
************************************************************************/

HouseData       houses[NUM_HOUSES];

// Lightweight alert queue: Core 1 enqueues strings; Core 0 sends them.
// Using a fixed array rather than std::queue avoids heap fragmentation.
char            pendingAlerts[MAX_PENDING_ALERTS][ALERT_MAX_LEN];
volatile int    alertHead = 0;   // next slot to write (Core 1)
volatile int    alertTail = 0;   // next slot to read  (Core 0)

// The single mutex that guards houses[] and pendingAlerts[]
SemaphoreHandle_t dataMutex = nullptr;

// Preferences instance (only accessed from Core 1 in loop())
Preferences     prefs;

/***********************************************************************
   ===================== UTILITY — CHECKSUM ============================
************************************************************************/

// Simple modular checksum: sum of all character bytes, mod 100.
// Must match the transmitter implementation exactly.
static int calculateChecksumC(const char* data)
{
  int sum = 0;
  while (*data) {
    // Ignore colons to perfectly match the Python script's "".join() logic
    if (*data != ':') {
      sum += (unsigned char)(*data);
    }
    data++;
  }
  return sum % 100;
}


/***********************************************************************
   ===================== UTILITY — ALERT QUEUE (Core 1 side) ==========
   Called with mutex HELD.  Silently drops if the queue is full to
   avoid blocking Core 1.
************************************************************************/

static void enqueueAlert(const char* msg)
{
  int nextHead = (alertHead + 1) % MAX_PENDING_ALERTS;
  if (nextHead == alertTail)
  {
    // Queue full — drop rather than block radio task
    return;
  }
  strncpy(pendingAlerts[alertHead], msg, ALERT_MAX_LEN - 1);
  pendingAlerts[alertHead][ALERT_MAX_LEN - 1] = '\0';
  alertHead = nextHead;
}

/***********************************************************************
   ===================== FLASH STORAGE (Core 1 only) ===================
   Preferences is not thread-safe; it is only called from loop() on
   Core 1.  The mutex is NOT held during flash writes because they
   only touch the local copy of cumulative_kwh which was already
   committed to the struct before the mutex was released.
************************************************************************/

/***********************************************************************
   ===================== FLASH STORAGE =================================
   Writes the safely captured float directly to the open NVS namespace.
   Does not use the mutex or read the live struct, preventing data races.
************************************************************************/
static void savePersistentData(int index, float safe_kwh)
{
  prefs.begin("energy", false); // Open namespace in read/write mode
  String key = "kwh" + String(index);
  prefs.putFloat(key.c_str(), safe_kwh);
  prefs.end(); // Close namespace to commit and protect flash
}

/***********************************************************************
   ===================== CORE 1 — PREDICTION ENGINE ====================
   FIX 2 – Digital-twin time model.

   Called INSIDE the mutex lock, immediately after updating
   cumulative_kwh.  Uses virtual_hour as the time axis — each packet
   received represents exactly 1 elapsed virtual hour — so the math
   is completely decoupled from real wall-clock time.

   Steps:
     1. Compute instantaneous rate = Δ kWh over the 1-virtual-hour step.
     2. EMA-smooth that rate.
     3. Remaining hours = 720 − virtual_hour.
     4. Forecast = actual so far + smoothed_rate × remaining_hours.
     5. Enqueue Telegram alerts at the 200 / 400 kWh TANGEDCO tiers.
************************************************************************/

static void updatePrediction(int index)
{
  HouseData& h = houses[index];
  const float PROXIMITY_THRESHOLD = 15.0f; // Warn when 15kWh away from next slab

  // --- Step A: Calculate 1-Month Forecast (FOR WEBSITE ONLY) ---
  float instant_rate = h.cumulative_kwh - h.prev_kwh;
  if (instant_rate < 0.0f) instant_rate = 0.0f;

  if (h.virtual_hour == 1) {
    h.smoothed_rate = instant_rate;
  } else {
    h.smoothed_rate = ALPHA * instant_rate + (1.0f - ALPHA) * h.smoothed_rate;
  }

  uint32_t current_cycle_hour = h.virtual_hour % BILLING_HOURS;
  int32_t remaining = BILLING_HOURS - (int32_t)current_cycle_hour;
  if (remaining < 0) remaining = 0;

  // This value is sent to Firebase via the 'dirty' flag later
  h.predicted_kwh = h.cumulative_kwh + h.smoothed_rate * (float)remaining;
  h.prev_kwh = h.cumulative_kwh;

  // --- Step B: Short-Term Proximity Alerts (FOR TELEGRAM) ---
  char buf[ALERT_MAX_LEN];

  // Check Slab 1 (200 kWh)
  float distTo200 = 200.0f - h.cumulative_kwh;
  if (distTo200 > 0 && distTo200 <= PROXIMITY_THRESHOLD && !h.warn200)
  {
    snprintf(buf, sizeof(buf),
      "⚠️ BUDGET ALERT: House %d is only %.1f units away from Slab 2. Current usage: %.1f kWh.",
      h.id, distTo200, h.cumulative_kwh);
    enqueueAlert(buf);
    h.warn200 = true; // Prevents spamming the alert
  }

  // Check Slab 2 (400 kWh)
  float distTo400 = 400.0f - h.cumulative_kwh;
  if (distTo400 > 0 && distTo400 <= PROXIMITY_THRESHOLD && !h.warn400)
  {
    snprintf(buf, sizeof(buf),
      "⚠️ BUDGET ALERT: House %d is only %.1f units away from Slab 3. Current usage: %.1f kWh.",
      h.id, distTo400, h.cumulative_kwh);
    enqueueAlert(buf);
    h.warn400 = true;
  }

  // Check Slab 3 (500 kWh)
float distTo500 = 500.0f - h.cumulative_kwh;
if (distTo500 > 0 && distTo500 <= PROXIMITY_THRESHOLD && !h.warn500)
{
  snprintf(buf, sizeof(buf),
    "🚨 CRITICAL BUDGET: House %d is only %.1f units away from Slab 4 (MAX TIER). Current usage: %.1f kWh.",
    h.id,distTo500, h.cumulative_kwh);
  enqueueAlert(buf);
  h.warn500 = true;
}
}

/***********************************************************************
   ===================== CORE 1 — PACKET PARSER ========================
   Packet format (colon-delimited, checksum appended):
     "<houseID>:<power_W|ERR>:<cumulative_kWh>:<checksum>"
   Example: "101:1450.5:87.32:43"

   Full mutex lock wraps all state mutations.
   Flash write happens OUTSIDE the lock (slow operation).
   dirty flag is set inside the lock so Core 0 picks up the change.
************************************************************************/

static void processPacket(char* packetBuffer)
{
  // Find the last colon to isolate the checksum
  char* lastColon = strrchr(packetBuffer, ':');
  if (!lastColon) return;

  // Terminate string at the last colon to separate data from checksum
  *lastColon = '\0'; 
  int rxChecksum = atoi(lastColon + 1);

  if (calculateChecksumC(packetBuffer) != rxChecksum) {
    return; // Checksum failed, silent drop
  }

  // Tokenize the remaining string: "HouseID:Power:kWh"
  char* saveptr;
  char* idStr = strtok_r(packetBuffer, ":", &saveptr);
  char* pwrStr = strtok_r(NULL, ":", &saveptr);
  char* kwhStr = strtok_r(NULL, ":", &saveptr);

  if (!idStr || !pwrStr || !kwhStr) return; // Malformed payload

  int houseID = atoi(idStr);
  int index = houseID - BASE_HOUSE_ID;
  if (index < 0 || index >= NUM_HOUSES) return; // Unknown node

  bool needFlashSave = false;
  float kwhToSave = 0.0f;
  float kwh = atof(kwhStr);

  // ══ MUTEX LOCK ═══════════════════════════════════════════════════
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
  {
    HouseData& h = houses[index];
    h.last_seen_ms = millis();
    char buf[ALERT_MAX_LEN];

    if (strcmp(pwrStr, "ERR") == 0)
    {
      if (h.status != NODE_DATA_FAULT)
      {
        snprintf(buf, sizeof(buf), "⚠️ HARDWARE FAULT: House %d sensor data corrupted.", h.id);
        enqueueAlert(buf);
      }
      h.status = NODE_DATA_FAULT;
      h.dirty = true;
    }
    else
    {
      if (h.status == NODE_OFFLINE || h.status == NODE_DATA_FAULT)
      {
        snprintf(buf, sizeof(buf), "✅ RESTORED: House %d is back online.", h.id);
        enqueueAlert(buf);
      }

      h.status = NODE_OK;
      h.power_w = atof(pwrStr);

      if (kwh > h.cumulative_kwh)
      {
        h.cumulative_kwh = kwh;
        
        // <-- REPLACE THE OLD FLASH LOGIC WITH THIS:
        // Only burn a physical flash cycle every 0.5 virtual kWh
        if (h.cumulative_kwh - h.last_flash_kwh >= 0.5f) {
          needFlashSave = true;
          kwhToSave = kwh;
          h.last_flash_kwh = h.cumulative_kwh;
        }
      }
      else if (kwh < h.cumulative_kwh - 1.0f) 
      {
        h.cumulative_kwh = kwh;
        h.last_flash_kwh = kwh;    
        h.prev_kwh = kwh;          
        h.warn200 = false;  // <--- RESET THIS       
        h.warn400 = false;  // <--- RESET THIS
        h.virtual_hour = 1; // Reset the virtual clock
        needFlashSave = true;
        kwhToSave = kwh;
      }
      // ------------------------------------------------------------

      h.virtual_hour++;
      updatePrediction(index);
      h.dirty = true;
    }
    Serial.printf("RX [Node %d] | Pwr: %.1f W | Total: %.3f kWh | Forecast: %.1f kWh | Status: %d\n", h.id, h.power_w, h.cumulative_kwh, h.predicted_kwh, h.status);
    xSemaphoreGive(dataMutex);
  }
  // ══ MUTEX UNLOCK ═════════════════════════════════════════════════

  if (needFlashSave)
  {
    savePersistentData(index, kwhToSave);
  }
}

/***********************************************************************
   ===================== CORE 1 — LoRa POLL ===========================
   Called every loop() iteration on Core 1.  Non-blocking: returns
   immediately if no packet is in the FIFO.
************************************************************************/


static void checkLoRa()
{
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  // Use a static-sized buffer to prevent heap fragmentation
  char packetBuffer[256];
  int idx = 0;
  
  while (LoRa.available() && idx < (sizeof(packetBuffer) - 1)) {
    packetBuffer[idx++] = (char)LoRa.read();
  }
  packetBuffer[idx] = '\0'; // Null-terminate the string

  processPacket(packetBuffer);
}

/***********************************************************************
   ===================== CORE 1 — NODE WATCHDOG ========================
   FIX 3 – Only alerts when a previously NODE_OK node exceeds the
   15-second demo timeout.  NODE_PENDING nodes are silently skipped so
   the gateway doesn't spam Telegram on boot.
************************************************************************/

static void checkNodeHealth()
{
  unsigned long now = millis();

  // ══ MUTEX LOCK ═══════════════════════════════════════════════════
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  for (int i = 0; i < NUM_HOUSES; i++)
  {
    HouseData& h = houses[i];

    // Skip nodes that are already offline, faulted, or never seen
    if (h.status != NODE_OK) continue;

    if (now - h.last_seen_ms > OFFLINE_TIMEOUT_MS)
    {
      h.status = NODE_OFFLINE;
      h.dirty  = true;

      char buf[ALERT_MAX_LEN];
      snprintf(buf, sizeof(buf),
        "❌ CRITICAL: House %d is OFFLINE (no packet for >15 s).", h.id);
      enqueueAlert(buf);
    }
  }

  xSemaphoreGive(dataMutex);
  // ══ MUTEX UNLOCK ═════════════════════════════════════════════════
}



/***********************************************************************
   ===================== CORE 0 — CLOUD TASK ===========================
   FIX 1 – All Wi-Fi / HTTP work runs here on Core 0.
   Core 1 (loop()) never touches Wi-Fi, so LoRa polling is never
   interrupted by TLS handshake latency (1–2 s).

   Each iteration of this task:
     1. Reconnects Wi-Fi if dropped.
     2. Drains the pending Telegram alert queue (one alert per turn to
        spread the HTTP calls and avoid watchdog resets).
     3. Iterates houses[] looking for dirty flags; takes a stack-local
        snapshot copy inside a brief mutex lock, then performs the HTTP
        PUT outside the lock.
     4. Yields for CLOUD_POLL_MS via vTaskDelay().
************************************************************************/
/***********************************************************************
   ===================== CORE 0 — HTTP HELPERS =========================
   Run exclusively on Core 0. Wi-Fi must be connected.
************************************************************************/

static void doFirebasePut(const HouseData& snap)
{
  WiFiClientSecure sc;
  sc.setInsecure(); // Skip TLS certificate validation for prototype

  HTTPClient http;

  // Use a C-string buffer to avoid String fragmentation
  char url[256];
  snprintf(url, sizeof(url), "%shouses/%d.json?auth=%s", FIREBASE_HOST, snap.id, FIREBASE_AUTH);

  http.begin(sc, url);
  http.addHeader("Content-Type", "application/json");

  // Build JSON payload
  String json = "{";
json += "\"power\":" + String(snap.power_w, 2) + ",";
json += "\"actual_kwh\":" + String(snap.cumulative_kwh, 3) + ",";
json += "\"smoothed_rate\":" + String(snap.smoothed_rate, 4) + ",";
json += "\"predicted_kwh\":" + String(snap.predicted_kwh, 2) + ",";
json += "\"virtual_hour\":" + String((unsigned long)snap.virtual_hour) + ",";
json += "\"status\":" + String((int)snap.status);
json += "}";

http.PUT(json);
  http.end();
}

static void doTelegramPost(const char* message)
{
  WiFiClientSecure sc;
  sc.setInsecure();

  HTTPClient http;

  char url[256];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TELEGRAM_TOKEN);

  http.begin(sc, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // Safely URL encode spaces for the HTTP POST payload
  String msgStr = String(message);
  msgStr.replace(" ", "+");

  String payload = "chat_id=" + String(TELEGRAM_CHAT_ID) + "&text=" + msgStr;

  http.POST(payload);
  http.end();
}

static void doFirebasePut(const HouseData& snap);
static void doTelegramPost(const char* message);

static void cloudTask(void* pvParameters)
{
  unsigned long lastWifiAttempt = 0;

  for (;;)   // FreeRTOS tasks must not return
  {
    // ── 1. Passive Wi-Fi health ──────────────────────────────
    // Let the setAutoReconnect(true) and events handle the work.
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    
    if (!wifiUp) {
       // Just log it once in a while, do NOT call begin() or reconnect()
       static unsigned long lastLog = 0;
       if (millis() - lastLog > 10000) {
           Serial.printf("[WIFI] Waiting for connection. Status: %d\n", WiFi.status());
           lastLog = millis();
       }
    }

    // --- 2. Drain ONE pending Telegram alert per loop ---
    if (wifiUp)
    {
      char alertBuf[ALERT_MAX_LEN] = {0};
      bool hasAlert = false;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (alertTail != alertHead) {
          strncpy(alertBuf, pendingAlerts[alertTail], ALERT_MAX_LEN - 1);
          alertTail = (alertTail + 1) % MAX_PENDING_ALERTS;
          hasAlert = true;
        }
        xSemaphoreGive(dataMutex);
      }

      if (hasAlert) {
        doTelegramPost(alertBuf);
      }
    }
    
    // --- 3. Firebase sync for dirty houses ---
    if (wifiUp)
    {
      for (int i = 0; i < NUM_HOUSES; i++)
      {
        HouseData snap;
        bool shouldPush = false;

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
          if (houses[i].dirty)
          {
            snap = houses[i];   
            houses[i].dirty = false;   
            shouldPush = true;
          }
          xSemaphoreGive(dataMutex);
        }

        if (shouldPush) {
          doFirebasePut(snap);
          vTaskDelay(pdMS_TO_TICKS(50)); // Yield to allow Core 1 radio work
        }
      }
    }

    // --- 4. Final Yield ---
    vTaskDelay(pdMS_TO_TICKS(CLOUD_POLL_MS));
  }
}


/***********************************************************************
   ===================== SETUP ========================================
   Runs on Core 1 before loop() starts.
   Sequence: Serial → Mutex → HouseData init → Flash load →
             LoRa init → Wi-Fi start → cloudTask spawn on Core 0.
************************************************************************/

void setup()
{
  Serial.begin(115200);
  delay(500);   // allow USB-CDC to enumerate on ESP32-S3

  Serial.println("\n[BOOT] ESP32-S3 FreeRTOS Edge Gateway starting...");

  // ── Create mutex before any task can use shared state ─────────────
  dataMutex = xSemaphoreCreateMutex();
  configASSERT(dataMutex != nullptr);   // halt if allocation failed

  // ── Initialise house state ────────────────────────────────────────
  unsigned long now = millis();
  for (int i = 0; i < NUM_HOUSES; i++)
  {
    houses[i].id             = BASE_HOUSE_ID + i;
    houses[i].status         = NODE_PENDING;   // FIX 3: no boot alerts
    houses[i].power_w        = 0.0f;
    houses[i].cumulative_kwh = 0.0f;
    houses[i].prev_kwh       = 0.0f;
    houses[i].virtual_hour   = 0;
    houses[i].smoothed_rate  = 0.0f;
    houses[i].predicted_kwh  = 0.0f;
    houses[i].last_seen_ms   = now;
    houses[i].warn200        = false;
    houses[i].warn400        = false;
    houses[i].dirty          = false;
  }

  // ── Restore cumulative kWh from flash (survives power cycle) ──────
  // Open the namespace in Strictly Read-Only mode (true) to pull initial data
  prefs.begin("energy", true); 
  
  for (int i = 0; i < NUM_HOUSES; i++) {
    String key = "kwh" + String(i);
    houses[i].cumulative_kwh = prefs.getFloat(key.c_str(), 0.0f);
    houses[i].last_flash_kwh = houses[i].cumulative_kwh; // <-- ADD THIS LINE
    houses[i].prev_kwh       = houses[i].cumulative_kwh;
    houses[i].dirty          = true; // Force cloud sync on boot
  }
  prefs.end(); // Close immediately after reading
  Serial.println("[BOOT] Flash data restored.");

  // ── LoRa radio init ───────────────────────────────────────────────
  SPI.begin();
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6))
  {
    Serial.println("[FATAL] LoRa init failed. Halting.");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
  }

  // Radio parameters — must match all transmitter nodes
  LoRa.setSpreadingFactor(7);       // SF7: fastest data rate for SF range
  LoRa.setSignalBandwidth(125E3);   // 125 kHz standard BW
  LoRa.setCodingRate4(5);           // 4/5 FEC
  LoRa.setPreambleLength(8);        // 8 symbols
  LoRa.setSyncWord(0xA5);           // private network sync word

  Serial.println("[BOOT] LoRa radio initialised at 433 MHz.");

 // ── Corrected Wi-Fi Setup ──────────────────────────────────────────
  WiFi.disconnect(true); 
  delay(1000);
  WiFi.mode(WIFI_STA);

  // Use event handlers to see the REAL reason for failure
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      Serial.printf("[WIFI EVENT] Disconnected. Reason: %d\n", info.wifi_sta_disconnected.reason);
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      Serial.println("[WIFI EVENT] Connected to AP!");
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      Serial.print("[WIFI EVENT] Got IP: ");
      Serial.println(WiFi.localIP());
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

  WiFi.setAutoReconnect(true); 
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // ── Spawn cloudTask on Core 0 with 16 KB stack ─────────────────────
  // Stack increased to 16 KB to safely handle WiFiClientSecure TLS buffers
  // without triggering a FreeRTOS StackSmash panic.
  xTaskCreatePinnedToCore(
    cloudTask,          // task function
    "cloudTask",        // debug name
    16384,              // stack bytes (16 KB)
    nullptr,            // parameter
    1,                  // priority
    nullptr,            // task handle (not needed)
    0                   // ← Core 0
  );

  Serial.println("[BOOT] cloudTask pinned to Core 0. loop() on Core 1.");
}

/***********************************************************************
   ===================== MAIN LOOP (Core 1) ============================
   Core 1 is entirely devoted to real-time radio work.
   No Wi-Fi calls, no HTTP, no long blocking operations.

   Typical iteration time: <1 ms when idle, ~5 ms when parsing a packet.
   This keeps the LoRa FIFO from overflowing even during cloud surges.
************************************************************************/

void loop()
{
  // Poll radio — returns immediately if no packet in FIFO
  checkLoRa();

  // Check for nodes that have gone silent (NODE_OK → NODE_OFFLINE)
  checkNodeHealth();

  // Yield a minimal slice so the FreeRTOS scheduler can service any
  // high-priority system tasks (Wi-Fi callbacks, watchdog resets, etc.)
  // This does NOT block long enough to cause missed packets.
  vTaskDelay(pdMS_TO_TICKS(1));
}
