/*
    ESP32 IR/RF GATEWAY + COMMAND STORAGE v1.1.2
    ============================================
    RF Command Storage:
      POST /api/rf/save          Save last received RF with name
      GET  /api/rf/saved         List all saved RF commands
      POST /api/rf/send/name     Send saved RF by name
      DELETE /api/rf/delete      Delete saved RF command
      
    IR Command Storage:
      POST /api/ir/save          Save last received IR with name (combined frame)
      GET  /api/ir/saved         List all saved IR commands
      POST /api/ir/send/name     Send saved IR by name
      DELETE /api/ir/delete      Delete saved IR command
*/
/*
    IR:
      TX (LEDC) : GPIO2
      RX (RMT0) : GPIO23  (TSOP active-low)
    
    RF 433MHz:
      RX : GPIO13
      TX : GPIO22

    Web API:
      GET  /                      -> "OK"
      GET  /api/status
      GET  /api/hostname
      POST /api/hostname          {"hostname":"haptique-extender","instance":"Haptique Extender"}
      GET  /api/wifi/status
      POST /api/wifi/save         {"ssid":"...","pass":"..."}
      POST /api/wifi/forget
      GET  /api/wifi/scan 
      GET  /api/ir/test           ?ms=800
      POST /api/ir/send           {"freq_khz":38,"duty":33,"repeat":1,"raw":[...]}
      GET  /api/ir/last
      
      GET  /api/rf/last           -> Get last RF received
      POST /api/rf/send           {"code":12345,"bits":24,"protocol":1,"repeat":10}
      GET  /api/rf/status         -> RF module status

    OTA:
      GET  /api/ota/status
      GET  /api/ota/config
      POST /api/ota/config
      GET  /api/ota/manifest
      POST /api/ota/check
      POST /api/ota/url

    Challenge-Response Auth:
      POST /api/auth/challenge/setup   -> Setup PIN
      GET  /api/auth/challenge/get     -> Get challenge + MAC
      POST /api/auth/challenge/verify  -> Verify response & get token
      POST /api/auth/challenge/reset   -> Reset PIN
      GET  /api/auth/challenge/status  -> Get auth status
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include <driver/ledc.h>
#include <driver/rmt.h>
#include <RCSwitch.h>
#include "esp_log.h"
#include "esp_wifi_types.h"

// OTA
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

// Challenge-Response
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

// ======= VERSION =======
#define FIRMWARE_VERSION "1.1.2"
#define MANUFACTURE "KINCONY"
#define MODEL "KC868-AG"

// ======= Pins =======
#define IR_TX_PIN             2
#define IR_RX_PIN             23
#define IR_RX_ACTIVE_LOW      1

// ======= RF 433MHz Pins =======
#define RF_RX_PIN 13
#define RF_TX_PIN 22

// ======= Factory reset =======
#define FACTORY_BTN_PIN      0
#define FACTORY_ACTIVE_LOW   1
#define FACTORY_HOLD_MS      10000

#define OTA_API_URL "https://app.cantatacs.com/remote/ir-extender/software/update/get"

// ======= Storage Limits =======
#define MAX_RF_COMMANDS  100
#define MAX_IR_COMMANDS  50
#define MAX_NAME_LENGTH  32
#define MAX_IR_RAW_STORE 512  // Limit stored IR raw data
#define MAX_INDEX_SIZE   2048 // Max size for name index string

struct Manifest;
struct OtaInfo {
  String version;
  String url;
  size_t size;
  String md5;
  bool force;
  String notes;
};

// ======= Stored Command Structures =======
struct RfCommand {
  char name[MAX_NAME_LENGTH];
  uint32_t code;
  uint8_t bits;
  uint8_t protocol;
  uint16_t pulseLen;
};

/*
 * The IR command structure persists captured IR timings to flash via the
 * Preferences (NVS) API.  Previous versions of this sketch stored the
 * pulse durations as an array of 32‑bit values.  Unfortunately NVS has
 * a practical limit on the size of a single value (roughly <2 kB).  A
 * struct holding 512 × 32‑bit values (~2088 bytes) would exceed that limit
 * when combined with the other fields, causing `prefs.putBytes()` to
 * silently fail.  To avoid this problem while still supporting long
 * sequences, the durations are now stored in compressed form.  Each
 * microsecond value is divided by IR_STORE_DIV (currently 10) and the
 * result rounded to the nearest integer.  The result fits into a 16‑bit
 * integer, reducing the overall size of the struct to ~1064 bytes and
 * keeping it well within the NVS per‑value limit.  When reading the
 * command back the values are multiplied by the same divisor to restore
 * the original timing in microseconds.  This introduces a maximum
 * rounding error of ±(IR_STORE_DIV/2) microseconds, which is well within
 * the tolerances of typical IR receivers.
 */
#define IR_STORE_DIV 10
struct IrCommand {
  char name[MAX_NAME_LENGTH];     // sanitized command name
  uint32_t freqHz;                // carrier frequency (Hz)
  uint8_t duty;                   // duty cycle percent
  uint16_t count;                 // number of timing values
  uint16_t raw[MAX_IR_RAW_STORE]; // compressed durations (µs / IR_STORE_DIV)
};

// ======= AP =======
static const char* AP_SSID = "HAP_IRHUB";
static const char* AP_PASS = "12345678";

// ======= Hostname =======
static const char* DEFAULT_HOSTNAME = "haptique-extender";
static const char* DEFAULT_INSTANCE = "Haptique Extender";

// ======= IR TX =======
#define IR_MODE         LEDC_LOW_SPEED_MODE
#define IR_TIMER        LEDC_TIMER_0
#define IR_CHANNEL      LEDC_CHANNEL_0
#define IR_DUTY_RES     LEDC_TIMER_10_BIT
#define IR_DUTY_MAX     ((1U << IR_DUTY_RES) - 1U)

// ======= IR RX =======
#define IR_RMT_CHANNEL      RMT_CHANNEL_0
#define RMT_CLK_DIV         80
#define IR_RX_FILTER_US     100
#define IR_RX_IDLE_US       18000
#define DEFAULT_RX_FREQ_KHZ 38
#define WAIT_FOR_B_MS       350
#define WINDOW_QUIET_MS     220
#define WINDOW_TOTAL_MS     800
#define DEFAULT_AB_GAP_US   30000
#define IR_RAW_MAX          2048

// AP auto-recover
static bool     apEnabled = false;
static uint32_t apReenableAtMs = 0;
static const uint32_t AP_REENABLE_DELAY_MS = 20000;

// Globals
Preferences prefs;
WebServer   server(80);

String gHostname = DEFAULT_HOSTNAME;
String gInstance = DEFAULT_INSTANCE;
bool   mdnsStarted = false;

String staSsid, staPass;
bool   staHaveCreds = false;
volatile bool staConnected = false;
volatile bool staConnecting = false;
volatile uint32_t irFreqHz = 38000;  // update at capture
volatile uint8_t  irDuty   = 33;     // update at capture


// ================== AUTH: globals ==================
String gAuthToken = "";
bool   gAuthTokenSet = false;

// Challenge-Response globals
String userPIN = "";
bool   pinConfigured = false;
String currentChallenge = "";
uint32_t challengeExpiry = 0;
uint32_t lastChallengeCheck = 0;

#define CHALLENGE_DIGITS 6
#define CHALLENGE_EXPIRY_MS 60000
#define CHALLENGE_RATE_LIMIT_MS 3000

// Wi-Fi state
enum WifiState : uint8_t {
  WIFI_IDLE = 0,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED,
  WIFI_AP_FALLBACK
};
volatile WifiState wifiState   = WIFI_IDLE;
volatile uint8_t   wifiRetries = 0;
volatile uint16_t  lastStaReason = 0;
volatile uint32_t  wifiDeadlineMs = 0;

// IR RX
RingbufHandle_t irRb = NULL;
String lastIrJson;

// IR A/B state
uint32_t *irA = nullptr, *irB = nullptr, *irC = nullptr, *scratch = nullptr;
size_t irAc = 0, irBc = 0, irCc = 0;
bool haveIrA = false, haveIrB = false;
uint32_t tIrA = 0, tIrB = 0, irWin = 0, irLast = 0;

// ✅ PERSISTENT IR STORAGE (for saving commands)
// ✅ PERSISTENT IR STORAGE (for saving commands)
uint32_t lastIrFreqHz = 38000;
uint8_t lastIrDuty = 33;

// Frame A storage
uint16_t lastIrCountA = 0;
uint32_t lastIrDataA[MAX_IR_RAW_STORE];
bool hasLastIrDataA = false;

// Frame B storage
uint16_t lastIrCountB = 0;
uint32_t lastIrDataB[MAX_IR_RAW_STORE];
bool hasLastIrDataB = false;

// Combined frame storage
uint16_t lastIrCountC = 0;
uint32_t lastIrDataC[MAX_IR_RAW_STORE];
bool hasLastIrDataC = false;

// Legacy support (points to combined by default)
uint16_t lastIrCount = 0;
uint32_t lastIrData[MAX_IR_RAW_STORE];
bool hasLastIrData = false;

// RF 433
RCSwitch rfRx = RCSwitch();
RCSwitch rfTx = RCSwitch();
String lastRfJson;
uint32_t lastRfCode = 0;
uint8_t lastRfBits = 0;
uint8_t lastRfProtocol = 0;
uint16_t lastRfPulseLen = 0;
uint32_t rfRxCount = 0;

// RX-mute during TX
volatile bool irRxPaused = false;
volatile uint32_t irRxMuteUntil = 0;

// Factory button
static bool g_btnPrev = false;
static uint32_t g_btnDownSince = 0;

// OTA State
static bool otaInProgress = false;
static uint32_t otaRebootAtMs = 0;
static uint32_t otaLastBytes = 0;
static bool otaLastOk = false;
static String otaLastErr = "";

struct OtaCfg {
  String manifestUrl;
  String authType;
  String bearer;
  String basicUser;
  String basicPass;
  bool autoCheck = false;
  bool autoInstall = false;
  uint32_t intervalMin = 360;
  bool allowInsecureTLS = true;
};
OtaCfg otaCfg;
static uint32_t nextOtaCheckAtMs = 0;
static bool otaUpdateAvailable = false;
static String lastManifestVersion = "";

// Forward declarations
static void addCORS();

// ========== STORAGE HELPER FUNCTIONS ==========

String sanitizeName(const String& input) {
  String name = input;
  name.trim();
  name.toLowerCase();
  
  // Replace spaces with underscores
  name.replace(" ", "_");
  
  // Remove invalid characters
  String clean = "";
  for (size_t i = 0; i < name.length() && clean.length() < MAX_NAME_LENGTH - 1; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
      clean += c;
    }
  }
  
  return clean.length() > 0 ? clean : "unnamed";
}

// ========== RF NAME INDEX MANAGEMENT ==========

void addRfNameToIndex(const String& name) {
  prefs.begin("rf_cmd", false);
  String index = prefs.getString("index", "");
  
  // Check if name already exists
  if (index.indexOf(name + ",") >= 0 || index.indexOf("," + name) >= 0 || index == name) {
    prefs.end();
    return;
  }
  
  // Add name to index
  if (index.length() > 0) {
    index += ",";
  }
  index += name;
  
  // Truncate if too long
  if (index.length() > MAX_INDEX_SIZE) {
    Serial.println("[RF] Warning: Index truncated");
    index = index.substring(0, MAX_INDEX_SIZE);
  }
  
  prefs.putString("index", index);
  prefs.end();
}

void removeRfNameFromIndex(const String& name) {
  prefs.begin("rf_cmd", false);
  String index = prefs.getString("index", "");
  
  // Remove name from index
  index.replace(name + ",", "");
  index.replace("," + name, "");
  if (index == name) index = "";
  
  prefs.putString("index", index);
  prefs.end();
}

String getRfNameIndex() {
  prefs.begin("rf_cmd", true);
  String index = prefs.getString("index", "");
  prefs.end();
  return index;
}

// ========== IR NAME INDEX MANAGEMENT ==========

void addIrNameToIndex(const String& name) {
  prefs.begin("ir_cmd", false);
  String index = prefs.getString("index", "");
  
  // Check if name already exists
  if (index.indexOf(name + ",") >= 0 || index.indexOf("," + name) >= 0 || index == name) {
    prefs.end();
    return;
  }
  
  // Add name to index
  if (index.length() > 0) {
    index += ",";
  }
  index += name;
  
  // Truncate if too long
  if (index.length() > MAX_INDEX_SIZE) {
    Serial.println("[IR] Warning: Index truncated");
    index = index.substring(0, MAX_INDEX_SIZE);
  }
  
  prefs.putString("index", index);
  prefs.end();
}

void removeIrNameFromIndex(const String& name) {
  prefs.begin("ir_cmd", false);
  String index = prefs.getString("index", "");
  
  // Remove name from index
  index.replace(name + ",", "");
  index.replace("," + name, "");
  if (index == name) index = "";
  
  prefs.putString("index", index);
  prefs.end();
}

String getIrNameIndex() {
  prefs.begin("ir_cmd", true);
  String index = prefs.getString("index", "");
  prefs.end();
  return index;
}

// ========== RF COMMAND STORAGE ==========
bool saveRfCommand(const String& name, uint32_t code, uint8_t bits, uint8_t protocol, uint16_t pulseLen) {
  String cleanName = sanitizeName(name);
  
  prefs.begin("rf_cmd", false);
  
  // Check if we've reached max commands
  int count = prefs.getInt("count", 0);
  bool isUpdate = prefs.isKey(cleanName.c_str());
  
  if (count >= MAX_RF_COMMANDS && !isUpdate) {
    prefs.end();
    Serial.println("[RF] Storage full!");
    return false;
  }
  
  // Save command data
  RfCommand cmd;
  strncpy(cmd.name, cleanName.c_str(), MAX_NAME_LENGTH - 1);
  cmd.name[MAX_NAME_LENGTH - 1] = '\0';
  cmd.code = code;
  cmd.bits = bits;
  cmd.protocol = protocol;
  cmd.pulseLen = pulseLen;
  
  size_t written = prefs.putBytes(cleanName.c_str(), &cmd, sizeof(RfCommand));
  
  if (written > 0 && !isUpdate) {
    prefs.putInt("count", count + 1);
    
    // Store name in numbered key for listing
    String keyName = "n" + String(count);
    prefs.putString(keyName.c_str(), cleanName);
  }
  
  prefs.end();
  
  Serial.printf("[RF] Saved '%s': code=%u bits=%u proto=%u\n", 
                cleanName.c_str(), code, bits, protocol);
  return written > 0;
}

bool loadRfCommand(const String& name, RfCommand& cmd) {
  String cleanName = sanitizeName(name);
  
  prefs.begin("rf_cmd", true);
  size_t len = prefs.getBytesLength(cleanName.c_str());
  
  if (len != sizeof(RfCommand)) {
    prefs.end();
    return false;
  }
  
  prefs.getBytes(cleanName.c_str(), &cmd, sizeof(RfCommand));
  prefs.end();
  
  return true;
}

bool deleteRfCommand(const String& name) {
  String cleanName = sanitizeName(name);
  
  prefs.begin("rf_cmd", false);
  bool existed = prefs.isKey(cleanName.c_str());
  
  if (existed) {
    prefs.remove(cleanName.c_str());
    int count = prefs.getInt("count", 0);
    if (count > 0) {
      prefs.putInt("count", count - 1);
    }
  }
  
  prefs.end();
  
  // Remove from name index
  if (existed) {
    removeRfNameFromIndex(cleanName);
  }
  
  Serial.printf("[RF] Deleted '%s': %s\n", cleanName.c_str(), existed ? "OK" : "NOT_FOUND");
  return existed;
}

String listRfCommands() {
  String index = getRfNameIndex();
  
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("commands");
  
  prefs.begin("rf_cmd", true);
  int count = prefs.getInt("count", 0);
  prefs.end();
  
  doc["count"] = count;
  
  // Parse comma-separated index
  if (index.length() > 0) {
    int startIdx = 0;
    int commaIdx = 0;
    
    while ((commaIdx = index.indexOf(',', startIdx)) >= 0) {
      String cmdName = index.substring(startIdx, commaIdx);
      cmdName.trim();
      
      if (cmdName.length() > 0) {
        RfCommand cmd;
        if (loadRfCommand(cmdName, cmd)) {
          JsonObject obj = arr.createNestedObject();
          obj["name"] = String(cmd.name);
          obj["code"] = cmd.code;
          obj["bits"] = cmd.bits;
          obj["protocol"] = cmd.protocol;
          obj["pulseLen"] = cmd.pulseLen;
        }
      }
      
      startIdx = commaIdx + 1;
    }
    
    // Handle last name (or only name if no commas)
    String cmdName = index.substring(startIdx);
    cmdName.trim();
    
    if (cmdName.length() > 0) {
      RfCommand cmd;
      if (loadRfCommand(cmdName, cmd)) {
        JsonObject obj = arr.createNestedObject();
        obj["name"] = String(cmd.name);
        obj["code"] = cmd.code;
        obj["bits"] = cmd.bits;
        obj["protocol"] = cmd.protocol;
        obj["pulseLen"] = cmd.pulseLen;
      }
    }
  }
  
  String output;
  serializeJson(doc, output);
  return output;
}

// ========== IR COMMAND STORAGE ==========

bool saveIrCommand(const String& name, uint32_t freqHz, uint8_t duty, const uint32_t* raw, uint16_t count) {
  String cleanName = sanitizeName(name);
  
  if (count > MAX_IR_RAW_STORE) {
    Serial.printf("[IR] Too many timings: %u (max %u)\n", count, MAX_IR_RAW_STORE);
    return false;
  }
  
  prefs.begin("ir_cmd", false);
  
  // Check if we've reached max commands
  int cmdCount = prefs.getInt("count", 0);
  bool isUpdate = prefs.isKey(cleanName.c_str());
  
  if (cmdCount >= MAX_IR_COMMANDS && !isUpdate) {
    prefs.end();
    Serial.println("[IR] Storage full!");
    return false;
  }
  
  IrCommand cmd;
  // Copy and sanitize the name
  strncpy(cmd.name, cleanName.c_str(), MAX_NAME_LENGTH - 1);
  cmd.name[MAX_NAME_LENGTH - 1] = '\0';
  // Store carrier and duty directly
  cmd.freqHz = freqHz;
  cmd.duty = duty;
  // Truncate count if necessary
  uint16_t safeCount = count;
  if (safeCount > MAX_IR_RAW_STORE) safeCount = MAX_IR_RAW_STORE;
  cmd.count = safeCount;
  // Compress the raw pulse durations into 16‑bit values.  Each duration
  // measured in microseconds is divided by IR_STORE_DIV and rounded.  Large
  // values are saturated at 0xFFFF.  See IR_STORE_DIV definition for
  // details.
  for (uint16_t i = 0; i < safeCount; i++) {
    uint32_t v = raw[i];
    // Perform rounding to minimise error; add half of divisor before
    // integer division.  Example: (589 + 5) / 10 = 59 → 590 µs after
    // decompression.
    uint32_t scaled = (v + (IR_STORE_DIV / 2)) / IR_STORE_DIV;
    if (scaled > 0xFFFF) scaled = 0xFFFF;
    cmd.raw[i] = (uint16_t)scaled;
  }
  // Zero any unused entries to avoid reading garbage when iterating
  for (uint16_t i = safeCount; i < MAX_IR_RAW_STORE; i++) cmd.raw[i] = 0;

  // Persist only the used portion of the struct to NVS.  The
  // Preferences/NVS library stores values as variable‑length blobs and
  // space is scarce.  Storing the entire raw buffer (all
  // MAX_IR_RAW_STORE entries) would waste space when the captured
  // sequence is shorter.  Compute the size of the struct header (all
  // fields before the raw array) then add only `safeCount` entries of
  // the 16‑bit raw array.  This dramatically reduces the size of
  // each stored command and avoids NVS write failures due to large
  // values.
  const size_t headerSize = sizeof(IrCommand) - (MAX_IR_RAW_STORE * sizeof(uint16_t));
  size_t dataSize = headerSize + (size_t)safeCount * sizeof(uint16_t);
  size_t written = prefs.putBytes(cleanName.c_str(), &cmd, dataSize);
  
  if (written > 0 && !isUpdate) {
    prefs.putInt("count", cmdCount + 1);
    
    // Store name in numbered key for listing
    String keyName = "n" + String(cmdCount);
    prefs.putString(keyName.c_str(), cleanName);
  }
  
  prefs.end();
  
  Serial.printf("[IR] Saved '%s': freq=%uHz duty=%u%% count=%u\n", 
                cleanName.c_str(), freqHz, duty, count);
  return written > 0;
}

bool loadIrCommand(const String& name, IrCommand& cmd) {
  String cleanName = sanitizeName(name);
  
  prefs.begin("ir_cmd", true);
  size_t len = prefs.getBytesLength(cleanName.c_str());
  
  // The stored data length must at least cover the struct header.
  const size_t headerSize = sizeof(IrCommand) - (MAX_IR_RAW_STORE * sizeof(uint16_t));
  if (len < headerSize) {
    prefs.end();
    return false;
  }

  // Initialise the structure so that any unused raw entries are zeroed.
  memset(&cmd, 0, sizeof(IrCommand));
  // Read only the stored number of bytes.  Extra bytes in the struct
  // remain zeroed, which is safe because cmd.count indicates how
  // many raw entries are valid.
  prefs.getBytes(cleanName.c_str(), &cmd, len);
  prefs.end();
  
  return true;
}

bool deleteIrCommand(const String& name) {
  String cleanName = sanitizeName(name);
  
  prefs.begin("ir_cmd", false);
  bool existed = prefs.isKey(cleanName.c_str());
  
  if (existed) {
    prefs.remove(cleanName.c_str());
    int count = prefs.getInt("count", 0);
    if (count > 0) {
      prefs.putInt("count", count - 1);
    }
    
    // Remove from numbered keys (rebuild list)
    int newIdx = 0;
    for (int i = 0; i < count; i++) {
      String keyName = "n" + String(i);
      String storedName = prefs.getString(keyName.c_str(), "");
      
      if (storedName != cleanName && storedName.length() > 0) {
        if (newIdx != i) {
          String newKeyName = "n" + String(newIdx);
          prefs.putString(newKeyName.c_str(), storedName);
        }
        newIdx++;
      }
    }
    
    // Clear any leftover keys
    for (int i = newIdx; i < count; i++) {
      String keyName = "n" + String(i);
      prefs.remove(keyName.c_str());
    }
  }
  
  prefs.end();
  
  Serial.printf("[IR] Deleted '%s': %s\n", cleanName.c_str(), existed ? "OK" : "NOT_FOUND");
  return existed;
}

String listIrCommands() {
  DynamicJsonDocument doc(12288);
  JsonArray arr = doc.createNestedArray("commands");
  
  prefs.begin("ir_cmd", true);
  int count = prefs.getInt("count", 0);
  
  doc["count"] = count;
  doc["max"] = MAX_IR_COMMANDS;
  doc["available"] = MAX_IR_COMMANDS - count;
  
  // Iterate through numbered keys
  for (int i = 0; i < count; i++) {
    String keyName = "n" + String(i);
    String cmdName = prefs.getString(keyName.c_str(), "");
    
    if (cmdName.length() > 0) {
      IrCommand cmd;
      size_t len = prefs.getBytesLength(cmdName.c_str());
      
      // Load the stored command if it is at least large enough to
      // contain the header.  The size of the header is the total
      // struct size minus the maximum raw array length.  Commands are
      // stored as variable‑length blobs, so len may be less than
      // sizeof(IrCommand) if only part of the raw array was saved.
      const size_t headerSize = sizeof(IrCommand) - (MAX_IR_RAW_STORE * sizeof(uint16_t));
      if (len >= headerSize) {
        memset(&cmd, 0, sizeof(IrCommand));
        size_t toRead = (len > sizeof(IrCommand)) ? sizeof(IrCommand) : len;
        prefs.getBytes(cmdName.c_str(), &cmd, toRead);

        JsonObject obj = arr.createNestedObject();
        obj["name"] = String(cmd.name);
        obj["freq_hz"] = cmd.freqHz;
        obj["duty"] = cmd.duty;
        obj["count"] = cmd.count;

        // Add preview of first 10 timings.  Stored values are
        // compressed (divided by IR_STORE_DIV), so multiply back to
        // approximate the original microsecond durations.
        JsonArray preview = obj.createNestedArray("preview");
        for (size_t j = 0; j < min(cmd.count, (uint16_t)10); j++) {
          uint32_t us = (uint32_t)cmd.raw[j] * IR_STORE_DIV;
          preview.add(us);
        }
      }
    }
  }
  
  prefs.end();
  
  String output;
  serializeJson(doc, output);
  return output;
}

// ========== CHALLENGE-RESPONSE HELPER FUNCTIONS ==========

String generateChallenge() {
  uint64_t mac = ESP.getEfuseMac();
  randomSeed((uint32_t)(millis() ^ (mac & 0xFFFFFFFF)));
  
  String challenge = "";
  for (int i = 0; i < CHALLENGE_DIGITS; i++) {
    challenge += String(random(0, 10));
  }
  return challenge;
}

String getMacShort() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac.substring(mac.length() - 6);
}

String calculateExpectedResponse(const String& challenge, const String& pin, const String& mac) {
  String combined = challenge + pin + mac;
  
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)combined.c_str(), combined.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  
  uint32_t value = (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
  uint32_t response = value % 1000000;
  
  char buffer[7];
  snprintf(buffer, sizeof(buffer), "%06u", response);
  return String(buffer);
}

void loadPinConfig() {
  prefs.begin("auth", true);
  userPIN = prefs.getString("pin", "");
  prefs.end();
  
  if (userPIN.length() >= 4 && userPIN.length() <= 8) {
    pinConfigured = true;
    Serial.printf("[AUTH] PIN loaded (length: %d)\n", userPIN.length());
  } else {
    pinConfigured = false;
    Serial.println("[AUTH] No PIN configured");
  }
}

void savePinConfig(const String& pin) {
  prefs.begin("auth", false);
  prefs.putString("pin", pin);
  prefs.end();
  
  userPIN = pin;
  pinConfigured = true;
  Serial.printf("[AUTH] PIN saved (length: %d)\n", pin.length());
}

bool verifyChallengeResponse(const String& challenge, const String& response) {
  if (!pinConfigured) {
    Serial.println("[AUTH] ERROR: PIN not configured!");
    return false;
  }
  
  uint32_t now = millis();
  if ((now - lastChallengeCheck) < CHALLENGE_RATE_LIMIT_MS) {
    Serial.println("[AUTH] Rate limit exceeded");
    return false;
  }
  lastChallengeCheck = now;
  
  if (challenge != currentChallenge) {
    Serial.println("[AUTH] Challenge mismatch");
    return false;
  }
  
  if (now > challengeExpiry) {
    Serial.println("[AUTH] Challenge expired");
    return false;
  }
  
  String mac = getMacShort();
  String expected = calculateExpectedResponse(challenge, userPIN, mac);
  bool valid = (response == expected);
  
  Serial.printf("[AUTH] Valid: %s\n", valid ? "YES" : "NO");
  
  if (valid) {
    currentChallenge = "";
    challengeExpiry = 0;
  }
  
  return valid;
}

void printTokenInfo() {
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║     DEVICE CHALLENGE-RESPONSE AUTHENTICATION          ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  
  if (gAuthTokenSet && gAuthToken.length() > 0) {
    Serial.printf("║  Token: %-47s║\n", gAuthToken.c_str());
    Serial.println("║  Status: ✓ STORED                                      ║");
  } else {
    Serial.println("║  Token: NOT CREATED YET                                ║");
  }
  
  Serial.println("╠════════════════════════════════════════════════════════╣");
  
  if (pinConfigured) {
    Serial.println("║  PIN: ✓ CONFIGURED                                     ║");
  } else {
    Serial.println("║  PIN: ✗ NOT CONFIGURED                                 ║");
  }
  
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.printf("║  MAC: %-49s║\n", WiFi.macAddress().c_str());
  Serial.printf("║  Firmware: v%-44s║\n", FIRMWARE_VERSION);
  Serial.println("╚════════════════════════════════════════════════════════╝\n");
}

static String generateToken(size_t len = 32) {
  static const char ALPH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String t;
  t.reserve(len);
  uint64_t mac = ESP.getEfuseMac();
  randomSeed((uint32_t)(millis() ^ (mac & 0xFFFFFFFF)));
  for (size_t i = 0; i < len; i++) t += ALPH[random(0, (int)sizeof(ALPH) - 1)];
  return t;
}

static void authCreateNewToken(const char* reason) {
  gAuthToken = generateToken(40);
  gAuthTokenSet = true;
  
  prefs.begin("wifi", false);
  prefs.putString("token", gAuthToken);
  prefs.end();
  
  Serial.printf("[AUTH] Token created (%s): %s\n", reason, gAuthToken.c_str());
}

static String readTokenFromRequest() {
  if (server.hasHeader("X-Auth-Token")) return server.header("X-Auth-Token");
  if (server.hasHeader("Authorization")) {
    String a = server.header("Authorization");
    a.trim();
    if (a.startsWith("Bearer ")) return a.substring(7);
    return a;
  }
  return "";
}

static inline bool isAPOn() { return apEnabled; }

static bool requireAuth() {
  if (!gAuthTokenSet) {
    addCORS();
    server.send(503, "application/json", "{\"error\":\"token_unavailable\"}");
    return false;
  }
  String tok = readTokenFromRequest();
  if (tok == gAuthToken) return true;
  addCORS();
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

// Utils
static inline bool isAlnumHyphen(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

static bool validHostname(const String& s) {
  if (s.length() < 1 || s.length() > 63) return false;
  if (s[0] == '-' || s[s.length() - 1] == '-') return false;
  for (size_t i = 0; i < s.length(); i++)
    if (!isAlnumHyphen(s[i])) return false;
  return true;
}

static const char* wifiReasonToString(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_TIMEOUT";
    default: return "UNKNOWN";
  }
}

static String trimWS(const String& s) {
  String t = s;
  t.trim();
  return t;
}

// Identity
void loadIdentity() {
  prefs.begin("wifi", true);
  String h = prefs.getString("host", DEFAULT_HOSTNAME);
  String i = prefs.getString("inst", DEFAULT_INSTANCE);
  gAuthToken = prefs.getString("token", "");
  prefs.end();
  
  h.toLowerCase();
  gHostname = validHostname(h) ? h : DEFAULT_HOSTNAME;
  gInstance = i.length() ? i : DEFAULT_INSTANCE;
  
  if (gAuthToken.length() > 0) {
    gAuthTokenSet = true;
    Serial.printf("[AUTH] Token loaded: %s\n", gAuthToken.c_str());
  } else {
    gAuthTokenSet = false;
    Serial.println("[AUTH] No token in flash");
  }
}

void saveIdentity(const String& host, const String& inst) {
  prefs.begin("wifi", false);
  prefs.putString("host", host);
  prefs.putString("inst", inst);
  prefs.end();
  gHostname = host;
  gInstance = inst;
}

// STA creds
void loadStaCreds() {
  prefs.begin("net", true);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  prefs.end();
  staHaveCreds = (staSsid.length() > 0);
}

void saveStaCreds(const String& ssid, const String& pass) {
  prefs.begin("net", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  staSsid = ssid;
  staPass = pass;
  staHaveCreds = (ssid.length() > 0);
}

void forgetStaCreds() {
  prefs.begin("net", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  staSsid = "";
  staPass = "";
  staHaveCreds = false;
}

// CORS
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Auth-Token");
  server.sendHeader("Access-Control-Max-Age", "86400");
}

void startAPIfNeeded(const char* reason) {
  if (apEnabled) return;
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  apEnabled = ok;
  
  if (ok && !gAuthTokenSet) {
    authCreateNewToken("AP_start");
  } else if (ok && gAuthTokenSet) {
    Serial.printf("[AUTH] Using existing token\n");
  }
  
  Serial.printf("[AP] %s -> %s  IP:%s\n", reason, ok ? "Started" : "FAILED",
                WiFi.softAPIP().toString().c_str());
}

void stopAPIfRunning(const char* reason) {
  if (!apEnabled) return;
  WiFi.softAPdisconnect(true);
  apEnabled = false;
  Serial.printf("[AP] %s -> Disabled\n", reason);
}

// mDNS
void startMDNSOnce() {
  if (mdnsStarted) return;
  if (!MDNS.begin(gHostname.c_str())) {
    Serial.println("[mDNS] Failed");
    return;
  }
  mdnsStarted = true;
  MDNS.setInstanceName(gInstance.c_str());
  MDNS.addService("http", "tcp", 80);
  String macAddress = WiFi.macAddress();
  MDNS.addServiceTxt("http", "tcp", "dev", gHostname.c_str());
  MDNS.addServiceTxt("http", "tcp", "fw", FIRMWARE_VERSION);
  MDNS.addServiceTxt("http", "tcp", "mf", MANUFACTURE);
  MDNS.addServiceTxt("http", "tcp", "mac", macAddress.c_str());
  MDNS.addServiceTxt("http", "tcp", "model", MODEL);
  Serial.printf("[mDNS] http://%s.local/\n", gHostname.c_str());
}

// Wi-Fi events
void onWiFiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] STA connected");
      staConnecting = true;
      staConnected = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] IP: %s (RSSI %d)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      staConnected = true;
      staConnecting = false;
      lastStaReason = 0;
      apReenableAtMs = 0;
      stopAPIfRunning("STA connected");
      startMDNSOnce();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      staConnected = false;
      staConnecting = false;
      uint8_t reason = info.wifi_sta_disconnected.reason;
      lastStaReason = reason;
      Serial.printf("[WiFi] Disconnected (reason=%u %s)\n", reason, wifiReasonToString(reason));
      apReenableAtMs = millis() + AP_REENABLE_DELAY_MS;
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[WiFi] Client joined AP");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("[WiFi] Client left AP");
      break;
    default:
      break;
  }
}

void bringUpWifi() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname(gHostname.c_str());

  startAPIfNeeded("Boot");

  if (staHaveCreds) {
    staConnecting = true;
    Serial.printf("[STA] Connecting to \"%s\"\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  startMDNSOnce();
}

// Factory reset
void factoryResetNow(const char* reason) {
  Serial.printf("[FACTORY] %s -> Clearing config...\n", reason);
  forgetStaCreds();
  
  prefs.begin("wifi", false);
  prefs.remove("token");
  prefs.end();
  
  prefs.begin("auth", false);
  prefs.remove("pin");
  prefs.end();
  
  // Clear stored commands
  prefs.begin("rf_cmd", false);
  prefs.clear();
  prefs.end();
  
  prefs.begin("ir_cmd", false);
  prefs.clear();
  prefs.end();
  
  gAuthToken = "";
  gAuthTokenSet = false;
  userPIN = "";
  pinConfigured = false;
  
  WiFi.disconnect(true, true);
  delay(200);
  ESP.restart();
}

void pollFactoryButton() {
  const bool pressed = (digitalRead(FACTORY_BTN_PIN) == (FACTORY_ACTIVE_LOW ? LOW : HIGH));
  const uint32_t now = millis();

  if (pressed) {
    if (!g_btnPrev) {
      g_btnPrev = true;
      g_btnDownSince = now;
      Serial.println("[BTN] BOOT pressed (hold 10s for reset)");
    } else {
      uint32_t held = now - g_btnDownSince;
      if ((held % 1000) < 25) Serial.printf("[BTN] %us\n", held / 1000);
      if (held >= FACTORY_HOLD_MS) factoryResetNow("Button held");
    }
  } else if (g_btnPrev) {
    g_btnPrev = false;
    if ((now - g_btnDownSince) < FACTORY_HOLD_MS) {
      Serial.println("[BTN] Short press (need 10s)");
    }
  }
}

// IR TX
void irInitTimer() {
  ledc_timer_config_t t{};
  t.speed_mode = IR_MODE;
  t.timer_num = IR_TIMER;
  t.duty_resolution = IR_DUTY_RES;
  t.freq_hz = 38000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));
}

void irInitChannel() {
  ledc_channel_config_t c{};
  c.speed_mode = IR_MODE;
  c.channel = IR_CHANNEL;
  c.timer_sel = IR_TIMER;
  c.gpio_num = (gpio_num_t)IR_TX_PIN;
  c.intr_type = LEDC_INTR_DISABLE;
  c.duty = 0;
  c.hpoint = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

inline void carrierOn(uint32_t freqHz, uint8_t dutyPercent) {
  ledc_set_freq(IR_MODE, IR_TIMER, freqHz);
  uint32_t duty = (IR_DUTY_MAX * dutyPercent) / 100U;
  if (duty == 0) duty = IR_DUTY_MAX / 3;
  ledc_set_duty(IR_MODE, IR_CHANNEL, duty);
  ledc_update_duty(IR_MODE, IR_CHANNEL);
}

inline void carrierOff() {
  ledc_set_duty(IR_MODE, IR_CHANNEL, 0);
  ledc_update_duty(IR_MODE, IR_CHANNEL);
}

static void irRxStop() { rmt_rx_stop(IR_RMT_CHANNEL); }
static void irRxStart() { rmt_rx_start(IR_RMT_CHANNEL, true); }

static void irRxFlush() {
  if (!irRb) return;
  size_t sz = 0;
  void* it = nullptr;
  while ((it = xRingbufferReceive(irRb, &sz, 0)) != nullptr) {
    vRingbufferReturnItem(irRb, it);
  }
}

static uint32_t sum_us(const uint32_t* raw, size_t len) {
  uint64_t s = 0;
  for (size_t i = 0; i < len; i++) s += raw[i];
  return (uint32_t)(s > 60000000ULL ? 60000000ULL : s);
}

static bool irSendRawCore(const uint32_t* raw, size_t len, uint32_t freqHz, uint8_t dutyPercent, uint16_t repeat) {
  if (!raw || !len) return false;
  if (freqHz < 10000 || freqHz > 60000) return false;
  if (dutyPercent == 0 || dutyPercent > 90) dutyPercent = 33;

  for (uint16_t r = 0; r < (repeat ? repeat : 1); r++) {
    for (size_t i = 0; i < len; i++) {
      if ((i & 1) == 0)
        carrierOn(freqHz, dutyPercent);
      else
        carrierOff();
      uint32_t us = raw[i];
      while (us > 50000) {
        delayMicroseconds(50000);
        yield();
        us -= 50000;
      }
      if (us > 0) delayMicroseconds(us);
      yield();
    }
    carrierOff();
    delay(5);
  }
  return true;
}

bool irSendRaw(const uint32_t* raw, size_t len, uint32_t freqHz, uint8_t dutyPercent, uint16_t repeat) {
  irRxPaused = true;
  irRxStop();
  irRxFlush();
  bool ok = irSendRawCore(raw, len, freqHz, dutyPercent, repeat);
  uint32_t guard_ms = 80 + (sum_us(raw, len) / 1000U) / 5;
  if (guard_ms > 300) guard_ms = 300;
  irRxMuteUntil = millis() + guard_ms;
  irRxStart();
  irRxPaused = false;
  return ok;
}

// RMT helpers
static size_t rmtItemsToRaw_activeLow(const rmt_item32_t* it, size_t nitems, uint32_t* out, size_t outMax, bool active_low) {
  size_t outCount = 0;
  auto push = [&](uint32_t v) {
    if (outCount < outMax) out[outCount++] = v;
  };

  for (size_t i = 0; i < nitems && outCount < outMax; i++) {
    if (it[i].duration0) {
      bool isMark = active_low ? (it[i].level0 == 0) : (it[i].level0 == 1);
      uint32_t us = it[i].duration0;
      if (outCount == 0 && !isMark) {
      } else
        push(us);
    }
    if (it[i].duration1 && outCount < outMax) {
      bool isMark = active_low ? (it[i].level1 == 0) : (it[i].level1 == 1);
      uint32_t us = it[i].duration1;
      if (outCount == 0 && !isMark) {
      } else
        push(us);
    }
  }
  if (outCount & 1) {
    if (outCount < outMax) out[outCount++] = 300;
  }
  return outCount;
}

// IR RX init
void irRxInit() {
  rmt_config_t rx{};
  rx.rmt_mode = RMT_MODE_RX;
  rx.channel = IR_RMT_CHANNEL;
  rx.gpio_num = (gpio_num_t)IR_RX_PIN;
  rx.clk_div = RMT_CLK_DIV;
  rx.mem_block_num = 4;
  rx.rx_config.filter_en = true;
  rx.rx_config.filter_ticks_thresh = IR_RX_FILTER_US;
  rx.rx_config.idle_threshold = IR_RX_IDLE_US;
  ESP_ERROR_CHECK(rmt_config(&rx));
  ESP_ERROR_CHECK(rmt_driver_install(rx.channel, 8192, 0));
  ESP_ERROR_CHECK(rmt_get_ringbuf_handle(rx.channel, &irRb));
  ESP_ERROR_CHECK(rmt_rx_start(rx.channel, true));
}

static void resetIr() {
  haveIrA = false;
  haveIrB = false;
  tIrA = tIrB = irWin = irLast = 0;
}

static void makeCsv(const uint32_t* arr, size_t n, String& out) {
  out = "";
  out.reserve(n * 6);
  for (size_t i = 0; i < n; i++) {
    if (i) out += ',';
    out += String((unsigned)arr[i]);
  }
}

static void buildCombined(const uint32_t* A, size_t Ac, const uint32_t* B, size_t Bc, uint32_t gap_us, uint32_t* C, size_t& Cc, size_t Cmax) {
  Cc = 0;
  for (size_t i = 0; i < Ac && i < Cmax; i++) C[Cc++] = A[i];
  if (Bc) {
    if (Cc > 0) {
      if ((Cc & 1) == 0)
        C[Cc - 1] += gap_us;
      else
        C[Cc++] = gap_us;
    } else
      C[Cc++] = gap_us;
    for (size_t i = 0; i < Bc && Cc < Cmax; i++) C[Cc++] = B[i];
  }
}

static void broadcastIR(uint32_t gap_ms) {
  if (!haveIrA) return;
  uint32_t gap_us = gap_ms * 1000UL;

  // Build combined data
  if (haveIrB)
    buildCombined(irA, irAc, irB, irBc, gap_us, irC, irCc, IR_RAW_MAX);
  else {
    irCc = irAc;
    for (size_t i = 0; i < irAc; i++) irC[i] = irA[i];
  }
  
  // ✅ CRITICAL: Store ALL frames in persistent variables
  lastIrFreqHz = irFreqHz;
  lastIrDuty = irDuty;
  
  // Save Frame A
  lastIrCountA = min(irAc, (size_t)MAX_IR_RAW_STORE);
  for (size_t i = 0; i < lastIrCountA; i++) {
    lastIrDataA[i] = irA[i];
  }
  hasLastIrDataA = true;
  
  // Save Frame B (if exists)
  if (haveIrB) {
    lastIrCountB = min(irBc, (size_t)MAX_IR_RAW_STORE);
    for (size_t i = 0; i < lastIrCountB; i++) {
      lastIrDataB[i] = irB[i];
    }
    hasLastIrDataB = true;
  } else {
    lastIrCountB = 0;
    hasLastIrDataB = false;
  }
  
  // Save Combined Frame
  lastIrCountC = min(irCc, (size_t)MAX_IR_RAW_STORE);
  for (size_t i = 0; i < lastIrCountC; i++) {
    lastIrDataC[i] = irC[i];
  }
  hasLastIrDataC = true;
  
  // Legacy support - default to combined
  lastIrCount = lastIrCountC;
  for (size_t i = 0; i < lastIrCountC; i++) {
    lastIrData[i] = lastIrDataC[i];
  }
  hasLastIrData = true;
  
  Serial.printf("[IR] ✓ Captured & Stored: A=%u, B=%u, Combined=%u @ %u Hz (duty=%u%%)\n", 
                lastIrCountA, lastIrCountB, lastIrCountC, lastIrFreqHz, lastIrDuty);

  String irA_csv, irB_csv;
  makeCsv(irA, irAc, irA_csv);
  if (haveIrB) makeCsv(irB, irBc, irB_csv);
  else irB_csv = "";

  DynamicJsonDocument doc(8192);
  doc["type"] = "ir_rx";
  doc["freq_khz"] = lastIrFreqHz / 1000;
  doc["frames"] = haveIrB ? 2 : 1;

  if (haveIrB) {
    doc["gap_ms"] = gap_ms;
    doc["gap_us"] = gap_us;
  }

  doc["countA"] = irAc;
  JsonArray a = doc.createNestedArray("a");
  for (size_t i = 0; i < irAc; i++) a.add(irA[i]);

  if (haveIrB) {
    doc["countB"] = irBc;
    JsonArray b = doc.createNestedArray("b");
    for (size_t i = 0; i < irBc; i++) b.add(irB[i]);
  }

  doc["combined_count"] = irCc;
  JsonArray c = doc.createNestedArray("combined");
  for (size_t i = 0; i < irCc; i++) c.add(irC[i]);

  doc["frameA"] = irA_csv;
  if (haveIrB) doc["frameB"] = irB_csv;

  serializeJson(doc, lastIrJson);
  Serial.printf("[IR] JSON: %d frames, %d combined timings\n", haveIrB ? 2 : 1, irCc);
}

void pollIr() {
  if (irRxPaused || (irRxMuteUntil && (int32_t)(millis() - irRxMuteUntil) < 0)) {
    irRxFlush();
    return;
  }

  while (true) {
    size_t rx_size = 0;
    rmt_item32_t* items = (rmt_item32_t*)xRingbufferReceive(irRb, &rx_size, 0);
    if (!items) break;
    size_t nitems = rx_size / sizeof(rmt_item32_t);
    size_t n = rmtItemsToRaw_activeLow(items, nitems, scratch, IR_RAW_MAX, IR_RX_ACTIVE_LOW);
    vRingbufferReturnItem(irRb, (void*)items);
    if (n < 2) continue;

    uint32_t now = millis();
    if (!haveIrA) {
      irAc = n;
      for (size_t i = 0; i < n; i++) irA[i] = scratch[i];
      haveIrA = true;
      tIrA = now;
      irWin = now;
      irLast = now;
      continue;
    }
    if (!haveIrB) {
      uint32_t dt = now - tIrA;
      if (dt <= WAIT_FOR_B_MS) {
        irBc = n;
        for (size_t i = 0; i < n; i++) irB[i] = scratch[i];
        haveIrB = true;
        tIrB = now;
        irLast = now;
        uint32_t gap_ms = (tIrB - tIrA);
        broadcastIR(gap_ms);
        resetIr();
        continue;
      } else {
        broadcastIR(0);
        resetIr();
        irAc = n;
        for (size_t i = 0; i < n; i++) irA[i] = scratch[i];
        haveIrA = true;
        tIrA = now;
        irLast = now;
        irWin = now;
        continue;
      }
    }
    uint32_t gap_ms = (tIrB - tIrA);
    broadcastIR(gap_ms);
    resetIr();
    irAc = n;
    for (size_t i = 0; i < n; i++) irA[i] = scratch[i];
    haveIrA = true;
    tIrA = now;
    irLast = now;
    irWin = now;
  }

  uint32_t now = millis();
  if (haveIrA && !haveIrB) {
    if ((now - tIrA) > WAIT_FOR_B_MS && (now - irLast) > WINDOW_QUIET_MS) {
      broadcastIR(0);
      resetIr();
    } else if ((now - irWin) > WINDOW_TOTAL_MS) {
      broadcastIR(0);
      resetIr();
    }
  }
}

// OTA helpers
static int cmpInt(int a, int b) { return (a > b) - (a < b); }

static int cmpSemver(String A, String B) {
  auto toParts = [](String s) {
    int p[3] = {0, 0, 0};
    int idx = 0;
    int val = 0;
    bool in = false;
    for (size_t i = 0; i < s.length() && idx < 3; i++) {
      char c = s[i];
      if (c >= '0' && c <= '9') {
        in = true;
        val = val * 10 + (c - '0');
      } else {
        if (in) {
          p[idx++] = val;
          val = 0;
          in = false;
        }
      }
    }
    if (in && idx < 3) p[idx++] = val;
    return std::array<int, 3>{p[0], p[1], p[2]};
  };
  auto a = toParts(A), b = toParts(B);
  if (int d = cmpInt(a[0], b[0])) return d;
  if (int d = cmpInt(a[1], b[1])) return d;
  return cmpInt(a[2], b[2]);
}

static void loadOtaCfg() {
  prefs.begin("ota", true);
  otaCfg.manifestUrl = prefs.getString("manifest", "");
  otaCfg.authType = prefs.getString("authType", "none");
  otaCfg.bearer = prefs.getString("bearer", "");
  otaCfg.basicUser = prefs.getString("bUser", "");
  otaCfg.basicPass = prefs.getString("bPass", "");
  otaCfg.autoCheck = prefs.getBool("autoCheck", false);
  otaCfg.autoInstall = prefs.getBool("autoInst", false);
  otaCfg.intervalMin = prefs.getUInt("interval", 360);
  otaCfg.allowInsecureTLS = prefs.getBool("insecure", true);
  prefs.end();
}

static void saveOtaCfg() {
  prefs.begin("ota", false);
  prefs.putString("manifest", otaCfg.manifestUrl);
  prefs.putString("authType", otaCfg.authType);
  prefs.putString("bearer", otaCfg.bearer);
  prefs.putString("bUser", otaCfg.basicUser);
  prefs.putString("bPass", otaCfg.basicPass);
  prefs.putBool("autoCheck", otaCfg.autoCheck);
  prefs.putBool("autoInst", otaCfg.autoInstall);
  prefs.putUInt("interval", otaCfg.intervalMin);
  prefs.putBool("insecure", otaCfg.allowInsecureTLS);
  prefs.end();
}

static String resolveRelative(const String& baseUrl, const String& rel) {
  if (rel.startsWith("http://") || rel.startsWith("https://")) return rel;
  if (rel.length() == 0) return baseUrl;
  if (rel[0] == '/') {
    int p = baseUrl.indexOf("://");
    if (p < 0) return rel;
    p += 3;
    int s = baseUrl.indexOf('/', p);
    if (s < 0) return baseUrl.substring(0);
    return baseUrl.substring(0, s) + rel;
  }
  int lastSlash = baseUrl.lastIndexOf('/');
  if (lastSlash < 0) return baseUrl + "/" + rel;
  return baseUrl.substring(0, lastSlash + 1) + rel;
}

static bool httpBeginAuth(HTTPClient& http, WiFiClientSecure& wcs, const String& url) {
  if (otaCfg.allowInsecureTLS)
    wcs.setInsecure();
  else
    wcs.setInsecure();
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OTA (IR-GW)");
  if (!http.begin(wcs, url)) return false;
  http.addHeader("Cache-Control", "no-cache");
  if (otaCfg.authType == "bearer" && otaCfg.bearer.length())
    http.addHeader("Authorization", "Bearer " + otaCfg.bearer);
  else if (otaCfg.authType == "basic" && otaCfg.basicUser.length()) {
    http.setAuthorization(otaCfg.basicUser.c_str(), otaCfg.basicPass.c_str());
  }
  return true;
}

static void setUpdateMD5IfAny(const String& md5hex) {
  String m = md5hex;
  m.trim();
  if (m.length() == 32) {
    Update.setMD5(m.c_str());
  }
}

static void otaProgressCb(size_t prgs, size_t total) {
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    Serial.printf("[OTA] Progress: %u / %u (%d%%)\n", prgs, total, (prgs * 100) / total);
    lastPrint = millis();
  }
}

static bool doHttpOtaUrl(const String& url, const String& md5hex, String& errOut, uint32_t& writtenOut) {
  writtenOut = 0;
  errOut = "";
  if (WiFi.status() != WL_CONNECTED) {
    errOut = "wifi_not_connected";
    return false;
  }

  otaInProgress = true;
  Update.onProgress(otaProgressCb);

  WiFiClientSecure wcs;
  HTTPClient http;
  if (!httpBeginAuth(http, wcs, url)) {
    errOut = "http_begin_failed";
    otaInProgress = false;
    return false;
  }

  Serial.printf("[OTA] GET %s\n", url.c_str());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    errOut = String("http_err_") + code;
    http.end();
    otaInProgress = false;
    return false;
  }

  int64_t contentLen = http.getSize();
  Serial.printf("[OTA] HTTP %d, len=%lld\n", code, (long long)contentLen);

  if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
    errOut = String("update_begin_failed_") + Update.getError();
    http.end();
    otaInProgress = false;
    return false;
  }
  setUpdateMD5IfAny(md5hex);

  WiFiClient& stream = http.getStream();
  size_t written = Update.writeStream(stream);
  Serial.printf("[OTA] Written %u bytes\n", (unsigned)written);

  if (contentLen > 0 && written != (size_t)contentLen) {
    errOut = "short_write";
    Update.abort();
    http.end();
    otaInProgress = false;
    return false;
  }
  if (!Update.end()) {
    errOut = String("update_end_failed_") + Update.getError();
    http.end();
    otaInProgress = false;
    return false;
  }
  if (!Update.isFinished()) {
    errOut = "update_not_finished";
    http.end();
    otaInProgress = false;
    return false;
  }

  http.end();

  writtenOut = (uint32_t)written;
  Serial.println("[OTA] SUCCESS. Rebooting...");
  otaInProgress = false;
  return true;
}

struct Manifest {
  String version;
  String url;
  String file;
  String md5;
  uint32_t size = 0;
  String notes;
  String min;
  bool force = false;
};

static bool parseManifest(const String& body, Manifest& m) {
  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    Serial.printf("[OTA] JSON parse error: %s\n", e.c_str());
    return false;
  }

  JsonObject dataObj;
  if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
    dataObj = doc["data"].as<JsonObject>();
  } else {
    dataObj = doc.as<JsonObject>();
  }

  m.version = String(dataObj["version"] | "");
  m.url = String(dataObj["url"] | "");
  m.file = String(dataObj["file"] | "");
  m.md5 = String(dataObj["md5"] | "");
  m.size = (uint32_t)(dataObj["size"] | 0);
  m.notes = String(dataObj["notes"] | "");
  m.min = String(dataObj["min"] | "");
  m.force = (bool)(dataObj["force"] | false);

  bool valid = m.version.length() > 0 && (m.url.length() > 0 || m.file.length() > 0);
  return valid;
}

static bool fetchManifest(Manifest& out, String& err) {
  err = "";
  out = Manifest{};
  String murl = trimWS(otaCfg.manifestUrl);
  if (murl == "") {
    err = "no_manifest_url";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    err = "wifi_not_connected";
    return false;
  }

  WiFiClientSecure wcs;
  HTTPClient http;
  if (!httpBeginAuth(http, wcs, murl)) {
    err = "http_begin_failed";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    err = String("http_err_") + code;
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (!parseManifest(body, out)) {
    err = "manifest_parse_failed";
    return false;
  }
  return true;
}

// ========== RF 433MHz FUNCTIONS ==========

void rfBroadcast(uint32_t code, uint8_t bits, uint8_t proto, uint16_t pulselen) {
  lastRfCode = code;
  lastRfBits = bits;
  lastRfProtocol = proto;
  lastRfPulseLen = pulselen;
  rfRxCount++;

  DynamicJsonDocument d(256);
  d["type"] = "rf_rx";
  d["code"] = code;
  d["bits"] = bits;
  d["protocol"] = proto;
  d["pulselen"] = pulselen;
  d["count"] = rfRxCount;

  serializeJson(d, lastRfJson);

  Serial.printf("[RF] RX #%u: Code=%u, Bits=%u, Proto=%u, Pulse=%u\n",
                rfRxCount, code, bits, proto, pulselen);
}

void pollRf() {
  if (rfRx.available()) {
    uint32_t code = rfRx.getReceivedValue();

    if (code == 0) {
      Serial.println("[RF] Unknown encoding");
    } else {
      uint8_t bits = rfRx.getReceivedBitlength();
      uint8_t proto = rfRx.getReceivedProtocol();
      uint16_t pulselen = rfRx.getReceivedDelay();
      rfBroadcast(code, bits, proto, pulselen);
    }
    rfRx.resetAvailable();
  }
}

void testRfHardware() {
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║               RF 433MHz HARDWARE TEST                  ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.printf("║  RX Pin: GPIO%-2d                                        ║\n", RF_RX_PIN);
  Serial.printf("║  TX Pin: GPIO%-2d                                        ║\n", RF_TX_PIN);
  Serial.printf("║  Interrupt: %-2d                                         ║\n", digitalPinToInterrupt(RF_RX_PIN));
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.println("║  Testing TX by sending code 12345...                   ║");
  
  rfTx.send(12345, 24);
  delay(100);
  
  Serial.println("║  ✓ Test code sent successfully                         ║");
  Serial.println("║  Receiver is now active and listening...               ║");
  Serial.println("╚════════════════════════════════════════════════════════╝\n");
}

// ========== HTTP HANDLERS ==========

// Challenge-Response Auth Handlers
void handleChallengeSetup() {
  addCORS();

  if (pinConfigured) {
    server.send(400, "application/json", "{\"error\":\"pin_already_configured\"}");
    return;
  }

  String pin = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      pin = String(doc["pin"] | "");
    }
  }

  if (pin.length() == 0) {
    pin = server.arg("pin");
  }

  pin.trim();

  if (pin.length() < 4 || pin.length() > 8) {
    server.send(400, "application/json", "{\"error\":\"invalid_pin_length\"}");
    return;
  }

  for (size_t i = 0; i < pin.length(); i++) {
    if (!isdigit(pin[i])) {
      server.send(400, "application/json", "{\"error\":\"invalid_pin_format\"}");
      return;
    }
  }

  savePinConfig(pin);

  DynamicJsonDocument doc(256);
  doc["ok"] = true;
  doc["message"] = "PIN configured";
  doc["pin_length"] = pin.length();
  doc["mac"] = getMacShort();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleChallengeGet() {
  addCORS();

  if (!pinConfigured) {
    server.send(400, "application/json", "{\"error\":\"pin_not_configured\"}");
    return;
  }

  currentChallenge = generateChallenge();
  challengeExpiry = millis() + CHALLENGE_EXPIRY_MS;

  String mac = getMacShort();

  DynamicJsonDocument doc(256);
  doc["ok"] = true;
  doc["challenge"] = currentChallenge;
  doc["mac"] = mac;
  doc["expires_in"] = CHALLENGE_EXPIRY_MS / 1000;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);

  Serial.printf("[AUTH] Challenge: %s, MAC: %s\n", currentChallenge.c_str(), mac.c_str());
}

void handleChallengeVerify() {
  addCORS();

  if (!pinConfigured) {
    server.send(400, "application/json", "{\"error\":\"pin_not_configured\"}");
    return;
  }

  String challenge = "";
  String response = "";

  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, server.arg("plain"))) {
      challenge = String(doc["challenge"] | "");
      response = String(doc["response"] | "");
    }
  }

  if (challenge.length() == 0) challenge = server.arg("challenge");
  if (response.length() == 0) response = server.arg("response");

  challenge.trim();
  response.trim();

  if (challenge.length() != CHALLENGE_DIGITS || response.length() != CHALLENGE_DIGITS) {
    server.send(400, "application/json", "{\"error\":\"invalid_format\"}");
    return;
  }

  bool valid = verifyChallengeResponse(challenge, response);

  if (valid) {
    if (!gAuthTokenSet) {
      authCreateNewToken("Challenge_verified");
    }

    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    doc["valid"] = true;
    doc["token"] = gAuthToken;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    Serial.println("[AUTH] ✓ Challenge verified");
  } else {
    server.send(401, "application/json", "{\"ok\":false,\"valid\":false,\"error\":\"invalid_response\"}");
  }
}

void handleChallengeStatus() {
  addCORS();

  DynamicJsonDocument doc(256);
  doc["configured"] = pinConfigured;
  doc["mac"] = getMacShort();
  doc["mac_full"] = WiFi.macAddress();
  doc["challenge_active"] = (currentChallenge.length() > 0 && millis() < challengeExpiry);

  if (currentChallenge.length() > 0 && millis() < challengeExpiry) {
    doc["challenge_expires_in"] = (challengeExpiry - millis()) / 1000;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleChallengeReset() {
  addCORS();
  if (!requireAuth()) return;

  prefs.begin("auth", false);
  prefs.remove("pin");
  prefs.end();

  userPIN = "";
  pinConfigured = false;
  currentChallenge = "";
  challengeExpiry = 0;

  server.send(200, "application/json", "{\"ok\":true,\"message\":\"PIN reset\"}");
  Serial.println("[AUTH] PIN reset");
}

// ========== RF COMMAND STORAGE HANDLERS ==========

void handleRfSave() {
  addCORS();
  if (!requireAuth()) return;

  if (lastRfCode == 0) {
    Serial.println("[RF] ERROR: No RF data captured yet");
    server.send(404, "application/json", "{\"error\":\"no_rf_received_yet\"}");
    return;
  }

  String name = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      name = String(doc["name"] | "");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }

  name.trim();

  if (name.length() == 0) {
    Serial.println("[RF] ERROR: Missing name");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  Serial.printf("[RF] Saving: name='%s', code=%u\n", name.c_str(), lastRfCode);

  bool ok = saveRfCommand(name, lastRfCode, lastRfBits, lastRfProtocol, lastRfPulseLen);

  if (ok) {
    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    doc["name"] = sanitizeName(name);
    doc["code"] = lastRfCode;
    doc["bits"] = lastRfBits;
    doc["protocol"] = lastRfProtocol;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  } else {
    Serial.println("[RF] ERROR: Save failed");
    server.send(500, "application/json", "{\"error\":\"save_failed\"}");
  }
}

void handleRfListSaved() {
  addCORS();
  if (!requireAuth()) return;

  Serial.println("[RF] Listing saved commands...");
  String json = listRfCommands();
  server.send(200, "application/json", json);
}

void handleRfSendByName() {
  addCORS();
  if (!requireAuth()) return;

  String name = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      name = String(doc["name"] | "");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }

  name.trim();

  if (name.length() == 0) {
    Serial.println("[RF] ERROR: Missing name for send");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  Serial.printf("[RF] Loading command '%s'...\n", name.c_str());

  RfCommand cmd;
  if (!loadRfCommand(name, cmd)) {
    Serial.printf("[RF] ERROR: Command '%s' not found\n", name.c_str());
    server.send(404, "application/json", "{\"error\":\"command_not_found\"}");
    return;
  }

  Serial.printf("[RF] Sending '%s': Code=%u, Bits=%u, Protocol=%u\n",
                cmd.name, cmd.code, cmd.bits, cmd.protocol);

  rfTx.setProtocol(cmd.protocol);
  rfTx.setRepeatTransmit(8);
  rfTx.send(cmd.code, cmd.bits);

  DynamicJsonDocument doc(256);
  doc["ok"] = true;
  doc["name"] = String(cmd.name);
  doc["code"] = cmd.code;
  doc["bits"] = cmd.bits;
  doc["protocol"] = cmd.protocol;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);

  Serial.printf("[RF] ✓ Sent '%s' successfully\n", cmd.name);
}

void handleRfDelete() {
  addCORS();
  if (!requireAuth()) return;

  String name = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      name = String(doc["name"] | "");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }

  name.trim();

  if (name.length() == 0) {
    Serial.println("[RF] ERROR: Missing name for delete");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  Serial.printf("[RF] Deleting '%s'...\n", name.c_str());

  bool ok = deleteRfCommand(name);

  if (ok) {
    DynamicJsonDocument doc(128);
    doc["ok"] = true;
    doc["deleted"] = sanitizeName(name);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    Serial.printf("[RF] ✓ Deleted '%s'\n", sanitizeName(name).c_str());
  } else {
    Serial.printf("[RF] ERROR: Command '%s' not found\n", name.c_str());
    server.send(404, "application/json", "{\"error\":\"command_not_found\"}");
  }
}

// ========== IR COMMAND STORAGE HANDLERS (FIXED) ==========

void handleIrSave() {
  addCORS();
  if (!requireAuth()) return;

  // ✅ Check if we have any captured data
  if (!hasLastIrDataA && !hasLastIrDataB && !hasLastIrDataC) {
    Serial.println("[IR] ERROR: No IR data captured yet");
    server.send(404, "application/json", 
      "{\"error\":\"no_ir_received_yet\",\"message\":\"Please capture an IR signal first using /api/ir/last\"}");
    return;
  }

  String name = "";
  String frameType = "combined"; // Default to combined

  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      name = String(doc["name"] | "");
      frameType = String(doc["frame"] | "combined");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }
  
  if (frameType.length() == 0) {
    frameType = server.arg("frame");
    if (frameType.length() == 0) frameType = "combined";
  }

  name.trim();
  frameType.trim();
  frameType.toLowerCase();

  if (name.length() == 0) {
    Serial.println("[IR] ERROR: Missing name");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  // Determine which frame to save
  uint32_t* dataToSave = nullptr;
  uint16_t countToSave = 0;
  String actualFrame = "";

  if (frameType == "a" || frameType == "framea") {
    if (!hasLastIrDataA) {
      server.send(404, "application/json", 
        "{\"error\":\"frame_a_not_available\",\"message\":\"Frame A was not captured\"}");
      return;
    }
    dataToSave = lastIrDataA;
    countToSave = lastIrCountA;
    actualFrame = "A";
    
  } else if (frameType == "b" || frameType == "frameb") {
    if (!hasLastIrDataB) {
      server.send(404, "application/json", 
        "{\"error\":\"frame_b_not_available\",\"message\":\"Frame B was not captured (single-frame capture)\"}");
      return;
    }
    dataToSave = lastIrDataB;
    countToSave = lastIrCountB;
    actualFrame = "B";
    
  } else if (frameType == "c" || frameType == "combined" || frameType == "framec") {
    if (!hasLastIrDataC) {
      server.send(404, "application/json", 
        "{\"error\":\"combined_frame_not_available\",\"message\":\"Combined frame was not created\"}");
      return;
    }
    dataToSave = lastIrDataC;
    countToSave = lastIrCountC;
    actualFrame = "Combined";
    
  } else {
    server.send(400, "application/json", 
      "{\"error\":\"invalid_frame\",\"message\":\"Frame must be 'A', 'B', or 'combined'\"}");
    return;
  }

  Serial.printf("[IR] Saving Frame %s: name='%s', freq=%u Hz, duty=%u%%, count=%u\n",
                actualFrame.c_str(), name.c_str(), lastIrFreqHz, lastIrDuty, countToSave);

  bool ok = saveIrCommand(name, lastIrFreqHz, lastIrDuty, dataToSave, countToSave);

  if (ok) {
    DynamicJsonDocument doc(512);
    doc["ok"] = true;
    doc["name"] = sanitizeName(name);
    doc["frame"] = actualFrame;
    doc["freq_hz"] = lastIrFreqHz;
    doc["duty"] = lastIrDuty;
    doc["count"] = countToSave;

    JsonArray preview = doc.createNestedArray("preview");
    for (size_t i = 0; i < min(countToSave, (uint16_t)10); i++) {
      preview.add(dataToSave[i]);
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    Serial.printf("[IR] ✓ Saved '%s' (Frame %s): %u timings @ %u Hz\n",
                  sanitizeName(name).c_str(), actualFrame.c_str(), countToSave, lastIrFreqHz);
  } else {
    Serial.println("[IR] ERROR: Save failed - check storage space");
    
    prefs.begin("ir_cmd", true);
    int count = prefs.getInt("count", 0);
    prefs.end();
    
    String errorMsg = "{\"error\":\"save_failed\",\"stored\":";
    errorMsg += String(count);
    errorMsg += ",\"max\":";
    errorMsg += String(MAX_IR_COMMANDS);
    errorMsg += "}";
    
    server.send(500, "application/json", errorMsg);
  }
}
void handleStorageInfo() {
  addCORS();
  if (!requireAuth()) return;
  
  DynamicJsonDocument doc(768);
  
  // RF Storage
  prefs.begin("rf_cmd", true);
  int rfCount = prefs.getInt("count", 0);
  prefs.end();
  
  JsonObject rf = doc.createNestedObject("rf");
  rf["stored"] = rfCount;
  rf["max"] = MAX_RF_COMMANDS;
  rf["available"] = MAX_RF_COMMANDS - rfCount;
  
  // IR Storage
  prefs.begin("ir_cmd", true);
  int irCount = prefs.getInt("count", 0);
  prefs.end();
  
  JsonObject ir = doc.createNestedObject("ir");
  ir["stored"] = irCount;
  ir["max"] = MAX_IR_COMMANDS;
  ir["available"] = MAX_IR_COMMANDS - irCount;
  
  // Capture status for each frame
  JsonObject capture = ir.createNestedObject("captured");
  capture["frame_a"] = hasLastIrDataA;
  capture["frame_b"] = hasLastIrDataB;
  capture["combined"] = hasLastIrDataC;
  capture["count_a"] = lastIrCountA;
  capture["count_b"] = lastIrCountB;
  capture["count_combined"] = lastIrCountC;
  
  // Flash info
  if (ESP.getFlashChipSize() > 0) {
    doc["flash_size"] = ESP.getFlashChipSize();
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}
void handleClearRfStorage() {
  addCORS();
  if (!requireAuth()) return;
  
  prefs.begin("rf_cmd", false);
  prefs.clear();
  prefs.end();
  
  Serial.println("[RF] Storage cleared");
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"RF storage cleared\"}");
}

void handleClearIrStorage() {
  addCORS();
  if (!requireAuth()) return;
  
  prefs.begin("ir_cmd", false);
  prefs.clear();
  prefs.end();
  
  Serial.println("[IR] Storage cleared");
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"IR storage cleared\"}");
}

void handleIrListSaved() {
  addCORS();
  if (!requireAuth()) return;

  Serial.println("[IR] Listing saved commands...");
  String json = listIrCommands();
  server.send(200, "application/json", json);
}

void handleIrSendByName() {
  addCORS();
  if (!requireAuth()) return;

  String name = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      name = String(doc["name"] | "");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }

  name.trim();

  if (name.length() == 0) {
    Serial.println("[IR] ERROR: Missing name for send");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  Serial.printf("[IR] Loading command '%s'...\n", name.c_str());

  IrCommand cmd;
  if (!loadIrCommand(name, cmd)) {
    Serial.printf("[IR] ERROR: Command '%s' not found\n", name.c_str());
    server.send(404, "application/json", "{\"error\":\"command_not_found\"}");
    return;
  }

  Serial.printf("[IR] Sending '%s': freq=%u Hz, duty=%u%%, count=%u\n",
                cmd.name, cmd.freqHz, cmd.duty, cmd.count);

  // Decompress the stored 16‑bit durations into a 32‑bit array before
  // transmission.  Each stored value represents a microsecond count divided
  // by IR_STORE_DIV.  We multiply by IR_STORE_DIV to restore the
  // approximate original durations.
  uint32_t tmpRaw[MAX_IR_RAW_STORE];
  for (uint16_t i = 0; i < cmd.count && i < MAX_IR_RAW_STORE; i++) {
    tmpRaw[i] = (uint32_t)cmd.raw[i] * IR_STORE_DIV;
  }
  bool ok = irSendRaw(tmpRaw, cmd.count, cmd.freqHz, cmd.duty, 1);

  if (ok) {
    DynamicJsonDocument doc(256);
    doc["ok"] = true;
    doc["name"] = String(cmd.name);
    doc["freq_hz"] = cmd.freqHz;
    doc["duty"] = cmd.duty;
    doc["count"] = cmd.count;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    Serial.printf("[IR] ✓ Sent '%s' successfully\n", cmd.name);
  } else {
    Serial.printf("[IR] ERROR: Failed to send '%s'\n", cmd.name);
    server.send(500, "application/json", "{\"error\":\"send_failed\"}");
  }
}

void handleIrDelete() {
  addCORS();
  if (!requireAuth()) return;

  String name = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      name = String(doc["name"] | "");
    }
  }

  if (name.length() == 0) {
    name = server.arg("name");
  }

  name.trim();

  if (name.length() == 0) {
    Serial.println("[IR] ERROR: Missing name for delete");
    server.send(400, "application/json", "{\"error\":\"missing_name\"}");
    return;
  }

  Serial.printf("[IR] Deleting '%s'...\n", name.c_str());

  bool ok = deleteIrCommand(name);

  if (ok) {
    DynamicJsonDocument doc(128);
    doc["ok"] = true;
    doc["deleted"] = sanitizeName(name);

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    Serial.printf("[IR] ✓ Deleted '%s'\n", sanitizeName(name).c_str());
  } else {
    Serial.printf("[IR] ERROR: Command '%s' not found\n", name.c_str());
    server.send(404, "application/json", "{\"error\":\"command_not_found\"}");
  }
}

// ========== OTHER HANDLERS ==========

void handleRoot() {
  server.send(200, "text/plain", "OK");
}

void handleApDisable() {
  addCORS();
  if (!requireAuth()) return;

  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "application/json", "{\"error\":\"sta_not_connected\"}");
    return;
  }

  String staIP = WiFi.localIP().toString();

  DynamicJsonDocument doc(256);
  doc["ok"] = true;
  doc["message"] = "AP will be disabled";
  doc["sta_ip"] = staIP;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);

  delay(100);

  apEnabled = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

void handleStatus() {
  addCORS();
  DynamicJsonDocument d(1024);
  d["mode"] = "WIFI_AP_STA";
  d["ap_on"] = apEnabled;
  d["hostname"] = gHostname;
  d["instance"] = gInstance;
  d["ap_ip"] = WiFi.softAPIP().toString();
  d["sta_ip"] = WiFi.localIP().toString();
  d["sta_ok"] = staConnected;
  d["sta_ssid"] = staHaveCreds ? staSsid : "";
  d["mac"] = WiFi.macAddress();
  d["ir_tx"] = IR_TX_PIN;
  d["ir_rx"] = IR_RX_PIN;
  d["rf_rx"] = RF_RX_PIN;
  d["rf_tx"] = RF_TX_PIN;
  d["fw_ver"] = FIRMWARE_VERSION;
  d["has_ir_data"] = hasLastIrData;
  d["last_ir_count"] = lastIrCount;

  if (apEnabled && gAuthTokenSet) {
    d["token"] = gAuthToken;
  }

  JsonObject ota = d.createNestedObject("ota");
  ota["in_progress"] = otaInProgress;
  ota["last_ok"] = otaLastOk;
  ota["last_bytes"] = otaLastBytes;
  ota["last_err"] = otaLastErr;

  JsonObject rf = d.createNestedObject("rf");
  rf["rx_count"] = rfRxCount;
  rf["last_code"] = lastRfCode;
  rf["last_bits"] = lastRfBits;

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleGetToken() {
  addCORS();
  if (!isAPOn()) {
    server.send(404, "application/json", "{\"error\":\"not_in_ap_mode\"}");
    return;
  }
  if (!gAuthTokenSet) authCreateNewToken("lazy_init");
  DynamicJsonDocument d(160);
  d["token"] = gAuthToken;
  d["ap"] = WiFi.softAPIP().toString();
  d["sta_ok"] = staConnected;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

bool parseHostnameBody(String& hostname, String& instance) {
  hostname = "";
  instance = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      hostname = String(doc["hostname"] | "");
      instance = String(doc["instance"] | "");
    }
  } else {
    if (server.hasArg("hostname")) hostname = server.arg("hostname");
    if (server.hasArg("instance")) instance = server.arg("instance");
  }
  return hostname.length() > 0;
}

void handleGetHostname() {
  addCORS();
  DynamicJsonDocument d(256);
  d["hostname"] = gHostname;
  d["instance"] = gInstance;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleSetHostname() {
  addCORS();
  String newHost, newInst;
  if (!parseHostnameBody(newHost, newInst)) {
    server.send(400, "application/json", "{\"error\":\"Missing hostname\"}");
    return;
  }
  newHost.toLowerCase();
  if (!validHostname(newHost)) {
    server.send(400, "application/json", "{\"error\":\"Invalid hostname\"}");
    return;
  }
  if (newInst.isEmpty()) newInst = gInstance;
  saveIdentity(newHost, newInst);
  DynamicJsonDocument d(256);
  d["status"] = "saved";
  d["hostname"] = newHost;
  d["instance"] = newInst;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
  delay(300);
  ESP.restart();
}

void handleWifiStatus() {
  addCORS();
  DynamicJsonDocument d(512);
  d["ap"] = WiFi.softAPIP().toString();
  JsonObject sta = d.createNestedObject("sta");
  sta["last_err"] = (int)lastStaReason;
  sta["last_err_str"] = wifiReasonToString(lastStaReason);
  sta["saved"] = staHaveCreds;
  sta["ssid"] = staHaveCreds ? staSsid : "";
  sta["ip"] = WiFi.localIP().toString();
  sta["ok"] = staConnected;
  sta["connecting"] = staConnecting && !staConnected;
  if (staConnected) sta["rssi"] = WiFi.RSSI();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleWifiSave() {
  addCORS();

  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";

  if (ssid == "" && server.hasArg("plain")) {
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, server.arg("plain"))) {
      ssid = String(doc["ssid"] | "");
      pass = String(doc["pass"] | "");
    }
  }

  ssid.trim();
  if (ssid == "") {
    server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
    return;
  }

  saveStaCreds(ssid, pass);
  startAPIfNeeded("Provision");

  WiFi.disconnect(true, true);
  delay(50);
  WiFi.begin(ssid.c_str(), pass.c_str());
  staConnecting = true;
  staConnected = false;
  lastStaReason = 0;

  DynamicJsonDocument d(256);
  d["saved"] = true;
  d["ssid"] = ssid;
  d["connecting"] = true;
  d["sta_ip"] = WiFi.localIP().toString();
  d["token"] = gAuthToken;

  String out;
  serializeJson(d, out);
  server.send(202, "application/json", out);
}

void handleWifiWait() {
  int timeoutSec = 20;
  if (server.hasArg("timeout")) {
    int t = server.arg("timeout").toInt();
    if (t > 0 && t <= 60) timeoutSec = t;
  }
  uint32_t deadline = millis() + (uint32_t)timeoutSec * 1000UL;
  while ((int32_t)(millis() - deadline) < 0) {
    if (staConnected) {
      DynamicJsonDocument d(96);
      d["result"] = "ok";
      d["ip"] = WiFi.localIP().toString();
      String out;
      serializeJson(d, out);
      server.send(200, "application/json", out);
      return;
    }
    if (!staConnecting && !staConnected && lastStaReason != 0) {
      DynamicJsonDocument d(96);
      d["result"] = "fail";
      d["err"] = wifiReasonToString(lastStaReason);
      d["code"] = (int)lastStaReason;
      String out;
      serializeJson(d, out);
      server.send(200, "application/json", out);
      return;
    }
    delay(25);
    yield();
  }
  server.send(200, "application/json", "{\"result\":\"pending\"}");
}

void handleWifiForget() {
  addCORS();
  if (!requireAuth()) return;

  StaticJsonDocument<128> doc;
  doc["status"] = "ok";
  doc["msg"] = "Wi-Fi credentials cleared";
  String json;
  serializeJson(doc, json);

  server.send(200, "application/json", json);
  delay(250);

  forgetStaCreds();
  WiFi.disconnect(true, true);
  delay(500);

  prefs.begin("wifi", false);
  prefs.remove("token");
  prefs.end();

  startAPIfNeeded("Wi-Fi forget");
  delay(3000);
  ESP.restart();
}

void handleWifiScan() {
  addCORS();
  int n = WiFi.scanNetworks(false, true);
  DynamicJsonDocument d(4096);
  JsonArray arr = d.createNestedArray("networks");
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ssid;
    o["bssid"] = WiFi.BSSIDstr(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = (int)WiFi.encryptionType(i);
    o["chan"] = WiFi.channel(i);
    o["hidden"] = (ssid.length() == 0);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleIrTest() {
  int ms = server.hasArg("ms") ? server.arg("ms").toInt() : 800;
  if (ms < 50) ms = 50;
  if (ms > 3000) ms = 3000;
  carrierOn(38000, 33);
  delay(ms);
  carrierOff();
  server.send(200, "application/json", String("{\"status\":\"ok\",\"pin\":") + IR_TX_PIN + ",\"ms\":" + ms + "}");
}

void handleIRSend() {
  addCORS();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing JSON body\"}");
    return;
  }
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  uint32_t freqHz = doc.containsKey("freq_khz") ? (uint32_t)doc["freq_khz"].as<uint32_t>() * 1000UL : (uint32_t)38000;
  uint8_t duty = doc["duty"] | 33;
  uint16_t repeat = doc["repeat"] | 1;
  JsonArray raw = doc["raw"].as<JsonArray>();
  if (raw.isNull() || raw.size() == 0) {
    server.send(400, "application/json", "{\"error\":\"Missing raw array\"}");
    return;
  }
  size_t n = min((size_t)raw.size(), (size_t)IR_RAW_MAX);
  for (size_t i = 0; i < n; i++) {
    int v = raw[i].as<int>();
    if (v < 0) v = 0;
    scratch[i] = (uint32_t)v;
  }
  bool ok = irSendRaw(scratch, n, freqHz, duty, repeat);
  if (ok) {
    DynamicJsonDocument resp(256);
    resp["status"] = "sent";
    resp["pin"] = IR_TX_PIN;
    resp["freq"] = freqHz;
    resp["duty"] = duty;
    resp["count"] = (uint32_t)n;
    String out;
    serializeJson(resp, out);
    server.send(200, "application/json", out);
  } else
    server.send(500, "application/json", "{\"error\":\"IR send failed\"}");
}

void handleIrLast() {
  addCORS();
  if (lastIrJson.length() == 0)
    server.send(404, "application/json", "{\"error\":\"no_capture_yet\"}");
  else
    server.send(200, "application/json", lastIrJson);
}

void handleRxInfo() {
  addCORS();
  DynamicJsonDocument d(256);
  d["ir_rb_ok"] = (bool)irRb;
  d["ir_pin"] = IR_RX_PIN;
  d["ir_idle"] = IR_RX_IDLE_US;
  d["ir_filter"] = IR_RX_FILTER_US;
  d["has_ir_data"] = hasLastIrData;
  d["last_ir_count"] = lastIrCount;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleOtaStatus() {
  addCORS();
  DynamicJsonDocument d(320);
  d["in_progress"] = otaInProgress;
  d["last_ok"] = otaLastOk;
  d["last_bytes"] = otaLastBytes;
  d["last_err"] = otaLastErr;
  d["fw_ver"] = FIRMWARE_VERSION;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleOtaGetCfg() {
  addCORS();
  DynamicJsonDocument d(640);
  d["manifest_url"] = otaCfg.manifestUrl;
  JsonObject auth = d.createNestedObject("auth");
  auth["type"] = otaCfg.authType;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleOtaSetCfg() {
  addCORS();
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing JSON\"}");
    return;
  }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  if (doc.containsKey("manifest_url")) otaCfg.manifestUrl = String(doc["manifest_url"].as<const char*>());
  if (doc.containsKey("auto_check")) otaCfg.autoCheck = (bool)doc["auto_check"];
  if (doc.containsKey("auto_install")) otaCfg.autoInstall = (bool)doc["auto_install"];
  saveOtaCfg();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleOtaManifest() {
  addCORS();
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"wifi_not_connected\"}");
    return;
  }

  Manifest m;
  String err;
  if (!fetchManifest(m, err)) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"manifest_fetch_failed\"}");
    return;
  }

  bool needsUpdate = (cmpSemver(m.version, FIRMWARE_VERSION) > 0) || m.force;

  String jsonResp = "{\"ok\":true,\"version\":\"" + m.version + "\",\"update_available\":" + String(needsUpdate ? "true" : "false") + "}";
  server.send(200, "application/json", jsonResp);
}

void handleOtaCheck() {
  addCORS();
  HTTPClient http;
  http.begin(OTA_API_URL);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    server.send(200, "application/json", payload);
  } else {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"http_failed\"}");
  }
  http.end();
}

void handleOtaUrl() {
  addCORS();
  String url = "";
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) url = String(doc["url"] | "");
  }
  if (url == "") url = server.arg("url");
  url.trim();
  if (url == "") {
    server.send(400, "application/json", "{\"error\":\"missing url\"}");
    return;
  }

  String err;
  uint32_t written = 0;
  bool ok = doHttpOtaUrl(url, "", err, written);
  otaLastOk = ok;
  otaLastBytes = written;
  otaLastErr = ok ? "" : err;

  DynamicJsonDocument resp(256);
  resp["ok"] = ok;
  resp["written"] = written;
  String out;
  serializeJson(resp, out);
  server.send(ok ? 200 : 500, "application/json", out);
  if (ok) otaRebootAtMs = millis() + 1200;
}

void handleRfSend() {
  addCORS();

  uint32_t code = 0;
  uint8_t bits = 24;
  uint8_t protocol = 1;
  uint16_t repeat = 8;

  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      code = doc["code"] | 0;
      bits = doc["bits"] | 24;
      protocol = doc["protocol"] | 1;
      repeat = doc["repeat"] | 8;
    }
  } else {
    if (server.hasArg("code")) code = (uint32_t)server.arg("code").toInt();
    if (server.hasArg("bits")) bits = (uint8_t)server.arg("bits").toInt();
    if (server.hasArg("protocol")) protocol = (uint8_t)server.arg("protocol").toInt();
    if (server.hasArg("repeat")) repeat = (uint16_t)server.arg("repeat").toInt();
  }

  if (!code) {
    server.send(400, "application/json", "{\"error\":\"missing code\"}");
    return;
  }

  if (bits < 1 || bits > 64) bits = 24;
  if (protocol < 1 || protocol > 7) protocol = 1;
  if (repeat < 1 || repeat > 20) repeat = 8;

  rfTx.setProtocol(protocol);
  rfTx.setRepeatTransmit(repeat);
  rfTx.send(code, bits);

  Serial.printf("[RF] TX: Code=%u, Bits=%u, Protocol=%u, Repeat=%u\n",
                code, bits, protocol, repeat);

  DynamicJsonDocument d(200);
  d["status"] = "ok";
  d["code"] = code;
  d["bits"] = bits;
  d["protocol"] = protocol;
  d["repeat"] = repeat;
  d["pin"] = RF_TX_PIN;

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleRfLast() {
  addCORS();
  if (lastRfJson.length() == 0) {
    server.send(404, "application/json", "{\"error\":\"no_rf_capture_yet\"}");
  } else {
    server.send(200, "application/json", lastRfJson);
  }
}

void handleRfStatus() {
  addCORS();
  DynamicJsonDocument d(512);
  d["rf_rx_pin"] = RF_RX_PIN;
  d["rf_tx_pin"] = RF_TX_PIN;
  d["rf_enabled"] = true;
  d["rx_count"] = rfRxCount;
  d["last_code"] = lastRfCode;
  d["last_bits"] = lastRfBits;
  d["last_protocol"] = lastRfProtocol;
  d["last_pulselen"] = lastRfPulseLen;

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// ========== SETUP ==========

void setup() {
  Serial.begin(115200);
  delay(250);
  esp_log_level_set("*", ESP_LOG_WARN);

  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║       ESP32 IR/RF GATEWAY + COMMAND STORAGE v1.1.2    ║");
  Serial.println("╚════════════════════════════════════════════════════════╝");

  // Initialize RF 433MHz
  Serial.println("\n[RF] Initializing...");
  rfRx.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  rfTx.enableTransmit(RF_TX_PIN);
  rfTx.setProtocol(1);
  rfTx.setRepeatTransmit(8);
  testRfHardware();

  // Allocate IR buffers
  irA = (uint32_t*)malloc(IR_RAW_MAX * sizeof(uint32_t));
  irB = (uint32_t*)malloc(IR_RAW_MAX * sizeof(uint32_t));
  irC = (uint32_t*)malloc(IR_RAW_MAX * sizeof(uint32_t));
  scratch = (uint32_t*)malloc(IR_RAW_MAX * sizeof(uint32_t));
  if (!irA || !irB || !irC || !scratch) {
    Serial.println("[MEM] FATAL - Cannot allocate buffers");
    while (true) delay(1000);
  }

  pinMode(FACTORY_BTN_PIN, INPUT_PULLUP);

  loadIdentity();
  loadStaCreds();
  loadOtaCfg();
  loadPinConfig();

  printTokenInfo();

  WiFi.onEvent(onWiFiEvent);
  bringUpWifi();

  // Challenge-Response routes (NO AUTH)
  server.on("/api/auth/challenge/setup", HTTP_POST, handleChallengeSetup);
  server.on("/api/auth/challenge/get", HTTP_GET, handleChallengeGet);
  server.on("/api/auth/challenge/verify", HTTP_POST, handleChallengeVerify);
  server.on("/api/auth/challenge/status", HTTP_GET, handleChallengeStatus);
  server.on("/api/auth/challenge/reset", HTTP_POST, handleChallengeReset);

  server.on("/api/storage/info", HTTP_GET, []() { 
    if (!requireAuth()) return; 
    handleStorageInfo(); 
  });
  
  server.on("/api/rf/clear", HTTP_POST, []() { 
    if (!requireAuth()) return; 
    handleClearRfStorage(); 
  });
  
  server.on("/api/ir/clear", HTTP_POST, []() { 
    if (!requireAuth()) return; 
    handleClearIrStorage(); 
  });

  // Public routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/token", HTTP_GET, handleGetToken);

  // Routes requiring auth or AP mode
  server.on("/api/status", HTTP_GET, []() {
    if (!isAPOn() && !requireAuth()) return;
    handleStatus();
  });

  server.on("/api/wifi/scan", HTTP_GET, []() {
    if (!isAPOn() && !requireAuth()) return;
    handleWifiScan();
  });

  // Protected routes
  server.on("/api/hostname", HTTP_GET, []() { if (!requireAuth()) return; handleGetHostname(); });
  server.on("/api/wifi/status", HTTP_GET, []() { if (!requireAuth()) return; handleWifiStatus(); });
  server.on("/api/wifi/wait", HTTP_GET, []() { if (!requireAuth()) return; handleWifiWait(); });
  server.on("/api/ir/test", HTTP_GET, []() { if (!requireAuth()) return; handleIrTest(); });
  server.on("/api/ir/last", HTTP_GET, []() { if (!requireAuth()) return; handleIrLast(); });
  server.on("/api/ir/rxinfo", HTTP_GET, []() { if (!requireAuth()) return; handleRxInfo(); });
  server.on("/api/ap/disable", HTTP_POST, []() { if (!requireAuth()) return; handleApDisable(); });
  server.on("/api/hostname", HTTP_POST, []() { if (!requireAuth()) return; handleSetHostname(); });
  server.on("/api/ir/send", HTTP_POST, []() { if (!requireAuth()) return; handleIRSend(); });
  server.on("/api/ota/config", HTTP_POST, []() { if (!requireAuth()) return; handleOtaSetCfg(); });
  server.on("/api/ota/check", HTTP_POST, []() { if (!requireAuth()) return; handleOtaCheck(); });
  server.on("/api/ota/url", HTTP_POST, []() { if (!requireAuth()) return; handleOtaUrl(); });
  
  // RF 433MHz routes
  server.on("/api/rf/send", HTTP_POST, []() { if (!requireAuth()) return; handleRfSend(); });
  server.on("/api/rf/last", HTTP_GET, []() { if (!requireAuth()) return; handleRfLast(); });
  server.on("/api/rf/status", HTTP_GET, []() { if (!requireAuth()) return; handleRfStatus(); });
  
  // RF Command Storage routes
  server.on("/api/rf/save", HTTP_POST, []() { if (!requireAuth()) return; handleRfSave(); });
  server.on("/api/rf/saved", HTTP_GET, []() { if (!requireAuth()) return; handleRfListSaved(); });
  server.on("/api/rf/send/name", HTTP_POST, []() { if (!requireAuth()) return; handleRfSendByName(); });
  server.on("/api/rf/delete", HTTP_DELETE, []() { if (!requireAuth()) return; handleRfDelete(); });
  
  // IR Command Storage routes (FIXED)
  server.on("/api/ir/save", HTTP_POST, []() { if (!requireAuth()) return; handleIrSave(); });
  server.on("/api/ir/saved", HTTP_GET, []() { if (!requireAuth()) return; handleIrListSaved(); });
  server.on("/api/ir/send/name", HTTP_POST, []() { if (!requireAuth()) return; handleIrSendByName(); });
  server.on("/api/ir/delete", HTTP_DELETE, []() { if (!requireAuth()) return; handleIrDelete(); });
  
  server.on("/api/ota/status", HTTP_GET, []() { if (!requireAuth()) return; handleOtaStatus(); });
  server.on("/api/ota/config", HTTP_GET, []() { if (!requireAuth()) return; handleOtaGetCfg(); });
  server.on("/api/ota/manifest", HTTP_GET, []() { if (!requireAuth()) return; handleOtaManifest(); });
  server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/forget", HTTP_POST, []() { if (!requireAuth()) return; handleWifiForget(); });

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCORS();
      server.send(204);
      return;
    }
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  startMDNSOnce();

  irInitTimer();
  irInitChannel();
  irRxInit();

  nextOtaCheckAtMs = millis() + 15000;

  Serial.println("\n[API] ✓ Server ready with IR/RF command storage!");
  Serial.println("[STORAGE] Name index system active");
  Serial.println("[IR] Persistent capture storage enabled");
  Serial.println("[RF] Listening for 433MHz signals...\n");
}

// ========== LOOP ==========

void loop() {
  server.handleClient();

  pollIr();
  pollRf();
  pollFactoryButton();

  // AP re-enable logic
  if (!staConnected && !apEnabled && apReenableAtMs && millis() >= apReenableAtMs) {
    startAPIfNeeded("Re-enable (STA down)");
  }

  // Auto OTA check
  if (otaCfg.autoCheck && !otaInProgress && WiFi.status() == WL_CONNECTED) {
    if (nextOtaCheckAtMs && (int32_t)(millis() - nextOtaCheckAtMs) >= 0) {
      Manifest m;
      String err;
      if (fetchManifest(m, err)) {
        lastManifestVersion = m.version;
        bool needsUpdate = (cmpSemver(m.version, FIRMWARE_VERSION) > 0) || m.force;
        if (needsUpdate && otaCfg.autoInstall) {
          String fwUrl = m.url.length() ? m.url : resolveRelative(otaCfg.manifestUrl, m.file);
          String err2;
          uint32_t written = 0;
          bool ok = doHttpOtaUrl(fwUrl, m.md5, err2, written);
          otaLastOk = ok;
          otaLastBytes = written;
          if (ok) otaRebootAtMs = millis() + 1200;
        }
      }
      uint32_t gap = (otaCfg.intervalMin > 1 ? otaCfg.intervalMin : 1) * 60 * 1000UL;
      nextOtaCheckAtMs = millis() + gap;
    }
  }

  // OTA reboot
  if (otaRebootAtMs && (int32_t)(millis() - otaRebootAtMs) >= 0) {
    otaRebootAtMs = 0;
    delay(100);
    ESP.restart();
  }

  delay(1);
}
