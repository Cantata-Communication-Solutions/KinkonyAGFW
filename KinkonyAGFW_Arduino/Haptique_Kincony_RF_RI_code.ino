/*

    IR:
      TX (LEDC) : GPIO2
      RX (RMT0) : GPIO23  (TSOP active-low)

    Web API (existing):
      GET  /                      -> "OK"
      GET  /ir.html               -> IR viewer
      GET  /api/status
      GET  /api/hostname
      POST /api/hostname          {"hostname":"haptique-extender","instance":"Haptique Extender"}
      GET  /api/wifi/status
      POST /api/wifi/save         {"ssid":"...","pass":"..."}
      POST /api/wifi/forget
      GET  /api/wifi/scan
      GET  /api/ir/test           ?ms=800
      POST /api/ir/send           {"freq_khz":38,"duty":33,"repeat":1,"raw":[...]}

    OTA (new/updated):
      GET  /api/ota/status        -> current fw + last OTA result
      GET  /api/ota/config        -> current OTA config
      POST /api/ota/config        -> set manifest URL, auth, auto settings
      GET  /api/ota/manifest      -> fetch manifest + report update_available
      POST /api/ota/check         -> install if manifest shows newer
      POST /api/ota/url           -> install from direct URL {"url":"https://.../firmware.bin"}

    WebSocket (port 81):
      {"type":"ota_progress","bytes":N,"total":M}
      {"type":"ota_done","ok":true,"written":M}
      {"type":"ota_done","ok":false,"error":"..."}
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

#include <esp_err.h>
#include <driver/ledc.h>    // IR TX
#include <driver/rmt.h>     // IR RX
#include <WebSocketsServer.h>
#include <RCSwitch.h>
#include "esp_log.h"
#include "esp_wifi_types.h"

// ---------- OTA ----------
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

// ======= VERSION: bump each release =======
#define FIRMWARE_VERSION "2.0.1"
#define MANUFACTURE "KINCONY"

// ======= Pins =======
#define IR_TX_PIN             2
#define IR_RX_PIN             23
#define IR_RX_ACTIVE_LOW      1   // TSOP is active-low

// ======= Factory reset (BOOT / IO0) =======
#define FACTORY_BTN_PIN      0
#define FACTORY_ACTIVE_LOW   1
#define FACTORY_HOLD_MS      10000

// --- Fix for Arduino auto-prototype: forward declare the struct
struct Manifest;

// ======= AP =======
static const char* AP_SSID = "HAP_IRHUB";
static const char* AP_PASS = "12345678";

// ======= Hostname/mDNS =======
static const char* DEFAULT_HOSTNAME = "haptique-extender";
static const char* DEFAULT_INSTANCE = "Haptique Extender";

// ======= IR TX (LEDC) =======
#define IR_MODE         LEDC_LOW_SPEED_MODE
#define IR_TIMER        LEDC_TIMER_0
#define IR_CHANNEL      LEDC_CHANNEL_0
#define IR_DUTY_RES     LEDC_TIMER_10_BIT
#define IR_DUTY_MAX     ((1U << IR_DUTY_RES) - 1U)

// ======= IR RX (RMT) =======
#define IR_RMT_CHANNEL      RMT_CHANNEL_0
#define RMT_CLK_DIV         80        // 1 MHz → 1 µs

// IR capture settings (AC-friendly)
#define IR_RX_FILTER_US     100
#define IR_RX_IDLE_US       18000
#define DEFAULT_RX_FREQ_KHZ 38

// A/B assembly timing
#define WAIT_FOR_B_MS       350
#define WINDOW_QUIET_MS     220
#define WINDOW_TOTAL_MS     800
#define DEFAULT_AB_GAP_US   30000

// Buffers
#define IR_RAW_MAX          2048

// AP auto-recover
static bool     apEnabled = false;
static uint32_t apReenableAtMs = 0;
static const uint32_t AP_REENABLE_DELAY_MS = 20000;

// Globals
Preferences prefs;
WebServer   server(80);
WebSocketsServer ws(81);

String gHostname = DEFAULT_HOSTNAME;
String gInstance = DEFAULT_INSTANCE;
bool   mdnsStarted = false;

// Wi-Fi state
String staSsid, staPass;
bool   staHaveCreds = false;
volatile bool staConnected = false;
volatile bool staConnecting = false;

// IR RX
RingbufHandle_t irRb = NULL;
String lastIrJson;

// RF 433 globals
#define RF_RX_PIN 13
#define RF_TX_PIN 22
RCSwitch rfRx = RCSwitch();
RCSwitch rfTx = RCSwitch();
String  lastRfJson;

// RX-mute during TX (IR)
volatile bool     irRxPaused = false;
volatile uint32_t irRxMuteUntil = 0;

// Factory button state
static bool     g_btnPrev = false;
static uint32_t g_btnDownSince = 0;

// ---------- OTA State / Config ----------
static bool     otaInProgress = false;
static uint32_t otaRebootAtMs = 0;
static uint32_t otaLastBytes  = 0;
static bool     otaLastOk     = false;
static String   otaLastErr    = "";

struct OtaCfg {
  String manifestUrl;        // e.g. https://cdn.example.com/irgw/manifest.json
  String authType;           // "none" | "bearer" | "basic"
  String bearer;             // for Bearer
  String basicUser;          // for Basic
  String basicPass;          // for Basic
  bool   autoCheck = false;  // auto fetch manifest on interval
  bool   autoInstall = false;// install if newer automatically
  uint32_t intervalMin = 360;// 6h
  bool   allowInsecureTLS = true; // setInsecure(); set to false + provide cert to verify properly
};
OtaCfg otaCfg;
static uint32_t nextOtaCheckAtMs = 0;
static bool     otaUpdateAvailable = false;
static String   lastManifestVersion = "";

// ---------- Utils ----------
static inline bool isAlnumHyphen(char c){
  return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-';
}
static bool validHostname(const String& s){
  if (s.length()<1 || s.length()>63) return false;
  if (s[0]=='-' || s[s.length()-1]=='-') return false;
  for (size_t i=0;i<s.length();i++) if(!isAlnumHyphen(s[i])) return false;
  return true;
}
static const char* wifiReasonToString(uint8_t r){
  switch(r){
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_TIMEOUT";
    default: return "?";
  }
}
static String trimWS(const String& s){ String t=s; t.trim(); return t; }

// ---------- Identity ----------
void loadIdentity() {
  prefs.begin("wifi", true);
  String h  = prefs.getString("host", DEFAULT_HOSTNAME);
  String i  = prefs.getString("inst", DEFAULT_INSTANCE);
  prefs.end();
  h.toLowerCase();
  gHostname = validHostname(h) ? h : DEFAULT_HOSTNAME;
  gInstance = i.length() ? i : DEFAULT_INSTANCE;
}
void saveIdentity(const String& host, const String& inst) {
  prefs.begin("wifi", false);
  prefs.putString("host", host);
  prefs.putString("inst", inst);
  prefs.end();
  gHostname = host; gInstance = inst;
}

// ---------- STA creds ----------
void loadStaCreds(){
  prefs.begin("net", true);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  prefs.end();
  staHaveCreds = (staSsid.length() > 0);
}
void saveStaCreds(const String& ssid, const String& pass){
  prefs.begin("net", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  staSsid = ssid; staPass = pass; staHaveCreds = (ssid.length()>0);
}
void forgetStaCreds(){
  prefs.begin("net", false);
  prefs.remove("ssid"); prefs.remove("pass");
  prefs.end();
  staSsid=""; staPass=""; staHaveCreds=false;
}

// ---------- AP helpers ----------
void startAPIfNeeded(const char* reason){
  if (apEnabled) return;
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  apEnabled = ok;
  Serial.printf("[AP] %s -> %s  SSID:%s PASS:%s IP:%s\n",
                reason, ok?"Started":"FAILED", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
}
void stopAPIfRunning(const char* reason){
  if (!apEnabled) return;
  WiFi.softAPdisconnect(true);
  apEnabled = false;
  Serial.printf("[AP] %s -> Disabled\n", reason);
}

// ---------- mDNS ----------
void startMDNSOnce() {
  if (mdnsStarted) return;
  if (!MDNS.begin(gHostname.c_str())) { Serial.println("[mDNS] Failed"); return; }
  mdnsStarted = true;
  MDNS.setInstanceName(gInstance.c_str());
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http","tcp","dev", gHostname.c_str());
  MDNS.addServiceTxt("http","tcp","fw",  FIRMWARE_VERSION);
  MDNS.addServiceTxt("http","tcp","Mf",  MANUFACTURE);
  Serial.printf("[mDNS] %s (%s) -> http://%s.local/  [_http._tcp]\n",
                gInstance.c_str(), gHostname.c_str(), gHostname.c_str());
}

// ---------- Wi-Fi ----------
void onWiFiEvent(WiFiEvent_t e, WiFiEventInfo_t info){
  switch(e){
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] STA connected (no IP yet)");
      staConnecting=true; staConnected=false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] STA IP: %s (RSSI %d)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      staConnected=true; staConnecting=false;
      apReenableAtMs = 0;
      stopAPIfRunning("STA got IP");
      startMDNSOnce();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      staConnected=false; staConnecting=false;
      uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.printf("[WiFi] STA disconnected (reason=%u %s)\n", reason, wifiReasonToString(reason));
      apReenableAtMs = millis() + AP_REENABLE_DELAY_MS;
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:    Serial.println("[WiFi] Client joined AP"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: Serial.println("[WiFi] Client left AP");  break;
    default: break;
  }
}
void bringUpWifi(){
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(gHostname.c_str());

  startAPIfNeeded("Boot");

  if (staHaveCreds){
    staConnecting = true;
    Serial.printf("[STA] begin(\"%s\")\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  startMDNSOnce();
}

// ---------- Factory reset ----------
void factoryResetNow(const char* reason) {
  Serial.printf("[FACTORY] %s -> clearing STA creds & rebooting...\n", reason);
  forgetStaCreds();
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
      Serial.println("[BTN] BOOT pressed (hold 10s to factory reset)");
    } else {
      uint32_t held = now - g_btnDownSince;
      if ((held % 1000) < 25) Serial.printf("[BTN] holding %lus\r\n", held/1000);
      if (held >= FACTORY_HOLD_MS) factoryResetNow("Long press on IO0");
    }
  } else if (g_btnPrev) {
    g_btnPrev = false;
    if ((now - g_btnDownSince) < FACTORY_HOLD_MS) {
      Serial.println("[BTN] Short press ignored (need 10s)");
    }
  }
}

// ---------- IR TX ----------
void irInitTimer(){
  ledc_timer_config_t t{};
  t.speed_mode      = IR_MODE;
  t.timer_num       = IR_TIMER;
  t.duty_resolution = IR_DUTY_RES;
  t.freq_hz         = 38000;
  t.clk_cfg         = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));
}
void irInitChannel(){
  ledc_channel_config_t c{};
  c.speed_mode = IR_MODE;
  c.channel    = IR_CHANNEL;
  c.timer_sel  = IR_TIMER;
  c.gpio_num   = (gpio_num_t)IR_TX_PIN;
  c.intr_type  = LEDC_INTR_DISABLE;
  c.duty       = 0;
  c.hpoint     = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}
inline void carrierOn(uint32_t freqHz, uint8_t dutyPercent){
  ledc_set_freq(IR_MODE, IR_TIMER, freqHz);
  uint32_t duty = (IR_DUTY_MAX * dutyPercent)/100U;
  if (duty==0) duty = IR_DUTY_MAX/3;
  ledc_set_duty(IR_MODE, IR_CHANNEL, duty);
  ledc_update_duty(IR_MODE, IR_CHANNEL);
}
inline void carrierOff(){ ledc_set_duty(IR_MODE, IR_CHANNEL, 0); ledc_update_duty(IR_MODE, IR_CHANNEL); }

static void irRxStop()  { rmt_rx_stop(IR_RMT_CHANNEL); }
static void irRxStart() { rmt_rx_start(IR_RMT_CHANNEL, true); }
static void irRxFlush(){
  if (!irRb) return;
  size_t sz=0; void* it=nullptr;
  while ((it = xRingbufferReceive(irRb, &sz, 0)) != nullptr){
    vRingbufferReturnItem(irRb, it);
  }
}
static uint32_t sum_us(const uint32_t* raw, size_t len){ uint64_t s=0; for(size_t i=0;i<len;i++) s+=raw[i]; return (uint32_t)(s>60000000ULL?60000000ULL:s); }

static bool irSendRawCore(const uint32_t* raw, size_t len, uint32_t freqHz, uint8_t dutyPercent, uint16_t repeat){
  if (!raw || !len) return false;
  if (freqHz<10000 || freqHz>60000) return false;
  if (dutyPercent==0 || dutyPercent>90) dutyPercent=33;
  for (uint16_t r=0; r<(repeat?repeat:1); r++){
    for (size_t i=0;i<len;i++){
      if ((i & 1)==0) carrierOn(freqHz, dutyPercent); else carrierOff();
      uint32_t us = raw[i];
      while (us>50000){ delayMicroseconds(50000); yield(); us-=50000; }
      if (us>0) delayMicroseconds(us);
      yield();
    }
    carrierOff(); delay(5);
  }
  return true;
}
bool irSendRaw(const uint32_t* raw, size_t len, uint32_t freqHz, uint8_t dutyPercent, uint16_t repeat){
  irRxPaused = true; irRxStop(); irRxFlush();
  bool ok = irSendRawCore(raw,len,freqHz,dutyPercent,repeat);
  uint32_t guard_ms = 80 + (sum_us(raw,len)/1000U)/5; if (guard_ms>300) guard_ms=300;
  irRxMuteUntil = millis() + guard_ms;
  irRxStart(); irRxPaused=false;
  return ok;
}

// ---------- RMT helpers ----------
static size_t rmtItemsToRaw_activeLow(const rmt_item32_t* it, size_t nitems, uint32_t* out, size_t outMax, bool active_low){
  size_t outCount=0; auto push=[&](uint32_t v){ if(outCount<outMax) out[outCount++]=v; };
  for (size_t i=0; i<nitems && outCount<outMax; i++){
    if (it[i].duration0){
      bool isMark = active_low ? (it[i].level0==0) : (it[i].level0==1);
      uint32_t us = it[i].duration0;
      if (outCount==0 && !isMark){} else push(us);
    }
    if (it[i].duration1 && outCount<outMax){
      bool isMark = active_low ? (it[i].level1==0) : (it[i].level1==1);
      uint32_t us = it[i].duration1;
      if (outCount==0 && !isMark){} else push(us);
    }
  }
  if (outCount & 1){ if (outCount<outMax) out[outCount++]=300; }
  return outCount;
}

// ---------- IR RX init ----------
void irRxInit(){
  rmt_config_t rx{};
  rx.rmt_mode                       = RMT_MODE_RX;
  rx.channel                        = IR_RMT_CHANNEL;
  rx.gpio_num                       = (gpio_num_t)IR_RX_PIN;
  rx.clk_div                        = RMT_CLK_DIV;
  rx.mem_block_num                  = 4;
  rx.rx_config.filter_en            = true;
  rx.rx_config.filter_ticks_thresh  = IR_RX_FILTER_US;
  rx.rx_config.idle_threshold       = IR_RX_IDLE_US;
  ESP_ERROR_CHECK(rmt_config(&rx));
  ESP_ERROR_CHECK(rmt_driver_install(rx.channel, 8192, 0));
  ESP_ERROR_CHECK(rmt_get_ringbuf_handle(rx.channel, &irRb));
  ESP_ERROR_CHECK(rmt_rx_start(rx.channel, true));
}

// ---------- IR A/B state ----------
uint32_t *irA = nullptr, *irB = nullptr, *irC = nullptr, *scratch = nullptr;
size_t irAc=0, irBc=0, irCc=0; bool haveIrA=false, haveIrB=false; uint32_t tIrA=0, tIrB=0, irWin=0, irLast=0;
String irA_csv, irB_csv;

// ---------- A/B helpers ----------
static void resetIr(){ haveIrA=false; haveIrB=false; irAc=irBc=irCc=0; tIrA=tIrB=irWin=irLast=0; }

static void makeCsv(const uint32_t* arr, size_t n, String& out){
  out=""; out.reserve(n*6);
  for (size_t i=0;i<n;i++){ if (i) out+=','; out+=String((unsigned)arr[i]); }
}
static void buildCombined(const uint32_t* A,size_t Ac,const uint32_t* B,size_t Bc,uint32_t gap_us,uint32_t* C,size_t& Cc,size_t Cmax){
  Cc=0;
  for (size_t i=0;i<Ac && i<Cmax;i++) C[Cc++]=A[i];
  if (Bc){
    if (Cc>0){
      if ((Cc & 1)==0) C[Cc-1]+=gap_us; else C[Cc++]=gap_us;
    } else C[Cc++]=gap_us;
    for (size_t i=0;i<Bc && Cc<Cmax;i++) C[Cc++]=B[i];
  }
}

// ---------- Broadcast makers ----------
static void broadcastIR(uint32_t gap_ms){
  if (!haveIrA) return;
  uint32_t gap_us=gap_ms*1000UL;
  if (haveIrB) buildCombined(irA,irAc,irB,irBc,gap_us,irC,irCc,IR_RAW_MAX);
  else { irCc=irAc; for(size_t i=0;i<irAc;i++) irC[i]=irA[i]; }
  makeCsv(irA,irAc,irA_csv); if (haveIrB) makeCsv(irB,irBc,irB_csv); else irB_csv="";

  String s; s.reserve(256 + (irAc+irBc+irCc)*6);
  s+="{\"type\":\"ir_rx\",\"freq_khz\":";
  s+=String((int)DEFAULT_RX_FREQ_KHZ);
  s+=",\"frames\":"; s+=(haveIrB?"2":"1");
  if (haveIrB){ s+=",\"gap_ms\":"; s+=String((unsigned)gap_ms); s+=",\"gap_us\":"; s+=String((unsigned)gap_us); }
  s+=",\"countA\":"; s+=String((unsigned)irAc); s+=",\"a\":[";
  for(size_t i=0;i<irAc;i++){ if(i) s+=','; s+=String((unsigned)irA[i]); } s+="]";
  if (haveIrB){ s+=",\"countB\":"; s+=String((unsigned)irBc); s+=",\"b\":["; for(size_t i=0;i<irBc;i++){ if(i) s+=','; s+=String((unsigned)irB[i]); } s+="]"; }
  s+=",\"combined_count\":"; s+=String((unsigned)irCc); s+=",\"combined\":[";
  for(size_t i=0;i<irCc;i++){ if(i) s+=','; s+=String((unsigned)irC[i]); } s+="]";
  s+=",\"frameA\":\""; s+=irA_csv; s+="\"";
  if (haveIrB){ s+=",\"frameB\":\""; s+=irB_csv; s+="\""; }
  s+='}';
  lastIrJson=s; ws.broadcastTXT(lastIrJson);
}

// ---------- IR poll ----------
void pollIr(){
  if (irRxPaused || (irRxMuteUntil && (int32_t)(millis()-irRxMuteUntil)<0)){ irRxFlush(); return; }
  while (true){
    size_t rx_size=0;
    rmt_item32_t* items=(rmt_item32_t*)xRingbufferReceive(irRb,&rx_size,0);
    if(!items) break;
    size_t nitems=rx_size/sizeof(rmt_item32_t);
    size_t n = rmtItemsToRaw_activeLow(items,nitems,scratch,IR_RAW_MAX, IR_RX_ACTIVE_LOW);
    vRingbufferReturnItem(irRb,(void*)items);
    if (n<2) continue;

    uint32_t now=millis();
    if(!haveIrA){
      irAc=n; for(size_t i=0;i<n;i++) irA[i]=scratch[i];
      haveIrA=true; tIrA=now; irWin=now; irLast=now; continue;
    }
    if(!haveIrB){
      uint32_t dt=now - tIrA;
      if (dt<=WAIT_FOR_B_MS){
        irBc=n; for(size_t i=0;i<n;i++) irB[i]=scratch[i];
        haveIrB=true; tIrB=now; irLast=now;
        uint32_t gap_ms = (tIrB - tIrA);
        broadcastIR(gap_ms); resetIr(); continue;
      }else{
        broadcastIR(0); resetIr();
        irAc=n; for(size_t i=0;i<n;i++) irA[i]=scratch[i];
        haveIrA=true; tIrA=now; irLast=now; irWin=now; continue;
      }
    }
    uint32_t gap_ms = (tIrB - tIrA);
    broadcastIR(gap_ms); resetIr();
    irAc=n; for(size_t i=0;i<n;i++) irA[i]=scratch[i];
    haveIrA=true; tIrA=now; irLast=now; irWin=now;
  }
  uint32_t now=millis();
  if (haveIrA && !haveIrB){
    if ((now - tIrA) > WAIT_FOR_B_MS && (now - irLast) > WINDOW_QUIET_MS){ broadcastIR(0); resetIr(); }
    else if ((now - irWin) > WINDOW_TOTAL_MS){ broadcastIR(0); resetIr(); }
  }
}

// ---------- HTML (IR only) ----------
const char IR_VIEW_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset="utf-8"/>
<title>IR Gateway</title>
<style>
 body{font-family:system-ui;margin:16px;line-height:1.35}
 textarea,input,button,select{font-size:14px}
 textarea{width:100%;height:120px;margin:8px 0}
 pre{border:1px solid #ccc;padding:10px;height:120px;overflow:auto}
 .row{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:6px 0}
 .box{border:1px solid #ddd;border-radius:10px;padding:12px;margin:12px 0}
 .grid{display:grid;grid-template-columns:1fr;gap:12px}
 @media(min-width:900px){ .grid{grid-template-columns:1fr 1fr} }
 .muted{opacity:.7}
 button,a{padding:6px 10px;border:1px solid #999;border-radius:6px;text-decoration:none;background:#f7f7f7;color:inherit}
 .w80{width:80px}
 .w100{width:100px}
 h3{margin:0 0 10px 0}
 h4{margin:6px 0}
</style>

<h3>Status: <span id="st">connecting…</span></h3>

<div class="box">
  <h3>IR Receiver (A / B / Combined)</h3>
  <div class="row">
    <span id="metaIR" class="muted">–</span>
    <button id="copyA">Copy A</button><button id="copyB">Copy B</button><button id="copyC">Copy Combined</button>
    <a id="dlJsonIR" download="ir_capture.json">Download JSON</a>
  </div>
  <div class="grid">
    <div>
      <h4>Frame A</h4>
      <textarea id="rawA" readonly></textarea>
      <div class="row">
        <button id="sendA">Send A</button>
        <span id="countA" class="muted"></span>
      </div>
    </div>
    <div>
      <h4>Frame B</h4>
      <textarea id="rawB" readonly></textarea>
      <div class="row">
        <button id="sendB">Send B</button>
        <span id="countB" class="muted"></span>
      </div>
    </div>
  </div>
  <div class="box">
    <h4>Combined (A + gap + B)</h4>
    <textarea id="rawC" readonly></textarea>
    <div class="row">
      <label>freq_khz <input id="freq" class="w80" type="number" value="38"></label>
      <label>duty% <input id="duty" class="w80" type="number" value="33"></label>
      <label>repeat <input id="repeat" class="w80" type="number" value="1"></label>
      <label>gap_us <input id="gapus" class="w100" type="number" value="30000"></label>
      <button id="sendAB">Send A+B</button>
      <button id="testIR">Carrier 800ms</button>
    </div>
  </div>
</div>

<h4>Events</h4>
<pre id="log"></pre>

<script>
let ws;
const st = document.getElementById('st');
const logEl=document.getElementById('log'); function log(t){ logEl.textContent=t+"\\n"+logEl.textContent; }
function join(a){ return (a||[]).join(','); }
function copy(el){ el.select(); document.execCommand('copy'); }

const rawA=document.getElementById('rawA'), rawB=document.getElementById('rawB'), rawC=document.getElementById('rawC');
const countA=document.getElementById('countA'), countB=document.getElementById('countB'), metaIR=document.getElementById('metaIR');
const dlIR=document.getElementById('dlJsonIR');
document.getElementById('copyA').onclick=()=>copy(rawA);
document.getElementById('copyB').onclick=()=>copy(rawB);
document.getElementById('copyC').onclick=()=>copy(rawC);

function send(o){ if(!ws||ws.readyState!==1){ log('WS not connected'); return;} ws.send(JSON.stringify(o)); }

function connect(){
  ws = new WebSocket('ws://'+location.hostname+':81/');
  ws.onopen  = ()=>{ st.textContent='connected'; log('[WS] connected'); };
  ws.onclose = ()=>{ st.textContent='disconnected (retry in 2s)'; log('[WS] closed'); setTimeout(connect,2000); };
  ws.onmessage=(ev)=>{
    try{
      const m = JSON.parse(ev.data);
      if (m.type==='ir_rx'){
        rawA.value = m.frameA || join(m.a||[]);
        rawB.value = m.frameB || join(m.b||[]);
        rawC.value = join(m.combined||[]);
        countA.textContent = (m.countA!==undefined?`countA=${m.countA}`:'');
        countB.textContent = (m.countB!==undefined?`countB=${m.countB}`:'');
        metaIR.textContent = `frames=${m.frames} freq=${m.freq_khz}kHz` + (m.gap_ms!==undefined?` gap=${m.gap_ms}ms`:'');
        const blob = new Blob([JSON.stringify(m)], {type:'application/json'}); dlIR.href = URL.createObjectURL(blob);
        log(`IR RX frames=${m.frames} A=${m.countA||0} B=${m.countB||0}`);
      } else if (m.type==='ir_tx_ack' || m.type==='ir_tx_err'){
        log(`TX ${m.type==='ir_tx_ack'?'OK':'ERR'} count=${m.count||''} freq=${m.freq||''} duty=${m.duty||''} ${m.error||''}`);
      } else if (m.type==='ota_progress'){
        log(`OTA ${m.bytes}/${m.total} bytes`);
      } else if (m.type==='ota_done'){
        log(`OTA ${m.ok?'OK':'ERR'} ${m.ok?('written='+m.written):(m.error||'')}`);
      }
    }catch(e){ log(ev.data); }
  };
}
connect();

document.getElementById('sendA').onclick=()=>{
  const f=parseInt(document.getElementById('freq').value||'38');
  const d=parseInt(document.getElementById('duty').value||'33');
  const r=parseInt(document.getElementById('repeat').value||'1');
  const arr=rawA.value.split(',').map(s=>parseInt(s.trim())).filter(n=>!isNaN(n));
  if(!arr.length) return log('No IR A');
  send({type:'ir_sendA',freq_khz:f,duty:d,repeat:r,a:arr});
};
document.getElementById('sendB').onclick=()=>{
  const f=parseInt(document.getElementById('freq').value||'38');
  const d=parseInt(document.getElementById('duty').value||'33');
  const r=parseInt(document.getElementById('repeat').value||'1');
  const arr=rawB.value.split(',').map(s=>parseInt(s.trim())).filter(n=>!isNaN(n));
  if(!arr.length) return log('No IR B');
  send({type:'ir_sendB',freq_khz:f,duty:d,repeat:r,b:arr});
};
document.getElementById('sendAB').onclick=()=>{
  const f=parseInt(document.getElementById('freq').value||'38');
  const d=parseInt(document.getElementById('duty').value||'33');
  const r=parseInt(document.getElementById('repeat').value||'1');
  const gap=parseInt(document.getElementById('gapus').value||'30000');
  const a=rawA.value.split(',').map(s=>parseInt(s.trim())).filter(n=>!isNaN(n));
  const b=rawB.value.split(',').map(s=>parseInt(s.trim())).filter(n=>!isNaN(n));
  if(!a.length && !b.length) return log('No IR A/B');
  send({type:'ir_sendAB',freq_khz:f,duty:d,repeat:r,gap_us:gap,a:a,b:b});
};
document.getElementById('testIR').onclick=()=>send({type:'ir_test',ms:800});
</script>
)HTML";

// ---------- OTA helpers ----------
static void wsSendObjAll(const JsonDocument& d){ String out; serializeJson(d,out); ws.broadcastTXT(out); }

static void otaProgressCb(size_t prgs, size_t total){
  DynamicJsonDocument j(96);
  j["type"]="ota_progress";
  j["bytes"]=(uint32_t)prgs;
  j["total"]=(uint32_t)total;
  wsSendObjAll(j);
}

static int cmpInt(int a,int b){ return (a>b)-(a<b); }
static int cmpSemver(String A, String B){
  // compare "x.y.z" numerically; missing parts = 0
  auto toParts=[](String s){ int p[3]={0,0,0}; int idx=0; int val=0; bool in=false;
    for (size_t i=0;i<s.length()&&idx<3;i++){ char c=s[i];
      if(c>='0'&&c<='9'){ in=true; val=val*10+(c-'0'); }
      else { if(in){ p[idx++]=val; val=0; in=false; } }
    } if(in&&idx<3) p[idx++]=val; return std::array<int,3>{p[0],p[1],p[2]}; };
  auto a = toParts(A), b = toParts(B);
  if(int d=cmpInt(a[0],b[0])) return d;
  if(int d=cmpInt(a[1],b[1])) return d;
  return cmpInt(a[2],b[2]);
}

static void loadOtaCfg(){
  prefs.begin("ota", true);
  otaCfg.manifestUrl      = prefs.getString("manifest", "");
  otaCfg.authType         = prefs.getString("authType", "none");
  otaCfg.bearer           = prefs.getString("bearer", "");
  otaCfg.basicUser        = prefs.getString("bUser", "");
  otaCfg.basicPass        = prefs.getString("bPass", "");
  otaCfg.autoCheck        = prefs.getBool("autoCheck", false);
  otaCfg.autoInstall      = prefs.getBool("autoInst", false);
  otaCfg.intervalMin      = prefs.getUInt("interval", 360);
  otaCfg.allowInsecureTLS = prefs.getBool("insecure", true);
  prefs.end();
}
static void saveOtaCfg(){
  prefs.begin("ota", false);
  prefs.putString("manifest", otaCfg.manifestUrl);
  prefs.putString("authType", otaCfg.authType);
  prefs.putString("bearer",   otaCfg.bearer);
  prefs.putString("bUser",    otaCfg.basicUser);
  prefs.putString("bPass",    otaCfg.basicPass);
  prefs.putBool("autoCheck",  otaCfg.autoCheck);
  prefs.putBool("autoInst",   otaCfg.autoInstall);
  prefs.putUInt("interval",   otaCfg.intervalMin);
  prefs.putBool("insecure",   otaCfg.allowInsecureTLS);
  prefs.end();
}

// return base + rel; supports absolute, root-relative, and dir-relative
static String resolveRelative(const String& baseUrl, const String& rel){
  if (rel.startsWith("http://") || rel.startsWith("https://")) return rel;
  if (rel.length()==0) return baseUrl;
  if (rel[0]=='/'){
    int p = baseUrl.indexOf("://");
    if (p<0) return rel;
    p += 3;
    int s = baseUrl.indexOf('/', p);
    if (s < 0) return baseUrl.substring(0); // odd case
    return baseUrl.substring(0, s) + rel;
  }
  int lastSlash = baseUrl.lastIndexOf('/');
  if (lastSlash<0) return baseUrl + "/" + rel;
  return baseUrl.substring(0, lastSlash+1) + rel;
}

// set up HTTPClient + client with auth and TLS
static bool httpBeginAuth(HTTPClient& http, WiFiClientSecure& wcs, const String& url){
  if (otaCfg.allowInsecureTLS) wcs.setInsecure();
  else {
    // TODO: setCACert(root_ca_pem);  // supply your PEM root here for strict TLS
    wcs.setInsecure();               // fallback unless you add a CA
  }
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-OTA (IR-GW)");
  if (!http.begin(wcs, url)) return false;
  http.addHeader("Cache-Control","no-cache");
  if (otaCfg.authType=="bearer" && otaCfg.bearer.length())
    http.addHeader("Authorization", "Bearer " + otaCfg.bearer);
  else if (otaCfg.authType=="basic" && otaCfg.basicUser.length()){
    http.setAuthorization(otaCfg.basicUser.c_str(), otaCfg.basicPass.c_str());
  }
  return true;
}

static void setUpdateMD5IfAny(const String& md5hex){
  String m=md5hex; m.trim();
  if (m.length()==32){ Update.setMD5(m.c_str()); }
}

static bool doHttpOtaUrl(const String& url, const String& md5hex, String& errOut, uint32_t& writtenOut){
  writtenOut=0; errOut="";
  if (WiFi.status()!=WL_CONNECTED){ errOut="wifi_not_connected"; return false; }

  otaInProgress = true;
  Update.onProgress(otaProgressCb);

  WiFiClientSecure wcs;
  HTTPClient http;
  if (!httpBeginAuth(http, wcs, url)){ errOut="http_begin_failed"; otaInProgress=false; return false; }

  Serial.printf("[OTA] GET %s\n", url.c_str());
  int code = http.GET();
  if (code != HTTP_CODE_OK){ errOut = String("http_err_")+code; http.end(); otaInProgress=false; return false; }

  int64_t contentLen = http.getSize(); // may be -1
  Serial.printf("[OTA] HTTP %d, len=%lld\n", code, (long long)contentLen);

  if (!Update.begin(contentLen>0 ? contentLen : UPDATE_SIZE_UNKNOWN)){
    errOut = String("update_begin_failed_") + Update.getError();
    http.end(); otaInProgress=false; return false;
  }
  setUpdateMD5IfAny(md5hex);

  WiFiClient& stream = http.getStream();
  size_t written = Update.writeStream(stream);
  Serial.printf("[OTA] Written %u bytes\n", (unsigned)written);

  if (contentLen>0 && written != (size_t)contentLen){
    errOut = "short_write"; Update.abort(); http.end(); otaInProgress=false; return false;
  }
  if (!Update.end()){ errOut = String("update_end_failed_")+Update.getError(); http.end(); otaInProgress=false; return false; }
  if (!Update.isFinished()){ errOut = "update_not_finished"; http.end(); otaInProgress=false; return false; }

  http.end();

  // success
  writtenOut = (uint32_t)written;
  DynamicJsonDocument j(96); j["type"]="ota_done"; j["ok"]=true; j["written"]=writtenOut; wsSendObjAll(j);
  Serial.println("[OTA] SUCCESS. Will reboot.");
  otaInProgress=false;
  return true;
}

// Manifest model
struct Manifest {
  String version;
  String url;      // full URL (preferred)
  String file;     // relative file (if url absent)
  String md5;
  uint32_t size=0;
  String notes;
  String min;
  bool   force=false;
};
static bool parseManifest(const String& body, Manifest& m){
  DynamicJsonDocument doc(4096);
  DeserializationError e = deserializeJson(doc, body);
  if (e) return false;
  m.version = String(doc["version"]|"");
  m.url     = String(doc["url"]|"");
  m.file    = String(doc["file"]|"");
  m.md5     = String(doc["md5"]|"");
  m.size    = (uint32_t)(doc["size"]|0);
  m.notes   = String(doc["notes"]|"");
  m.min     = String(doc["min"]|"");
  m.force   = (bool)(doc["force"]|false);
  return m.version.length()>0 && (m.url.length()>0 || m.file.length()>0);
}
static bool fetchManifest(Manifest& out, String& err){
  err=""; out = Manifest{};
  String murl = trimWS(otaCfg.manifestUrl);
  if (murl==""){ err="no_manifest_url"; return false; }
  if (WiFi.status()!=WL_CONNECTED){ err="wifi_not_connected"; return false; }

  WiFiClientSecure wcs;
  HTTPClient http;
  if (!httpBeginAuth(http, wcs, murl)){ err="http_begin_failed"; return false; }

  Serial.printf("[OTA] GET manifest %s\n", murl.c_str());
  int code = http.GET();
  if (code != HTTP_CODE_OK){ err = String("http_err_")+code; http.end(); return false; }

  String body = http.getString();
  http.end();

  if (!parseManifest(body, out)){ err="manifest_parse_failed"; return false; }
  return true;
}

// ---------- HTTP helpers ----------
void wsSendObj(uint8_t num, const JsonDocument& d){ String out; serializeJson(d,out); ws.sendTXT(num,out); }

bool parseHostnameBody(String& hostname,String& instance){
  hostname=""; instance="";
  if (server.hasArg("plain")){
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))){
      hostname = String(doc["hostname"]|"");
      instance = String(doc["instance"]|"");
    }
  } else {
    if (server.hasArg("hostname")) hostname = server.arg("hostname");
    if (server.hasArg("instance")) instance = server.arg("instance");
  }
  return hostname.length()>0;
}

// ---------- HTTP: status + identity ----------
void handleRoot(){ server.send(200, "text/plain", "OK"); }
void handleIrView(){ server.send_P(200, "text/html; charset=utf-8", IR_VIEW_HTML); }

void handleStatus(){
  DynamicJsonDocument d(768);
  d["mode"]     = "WIFI_AP_STA";
  d["ap_on"]    = apEnabled;
  d["hostname"] = gHostname;
  d["instance"] = gInstance;
  d["ap_ip"]    = WiFi.softAPIP().toString();
  d["sta_ip"]   = WiFi.localIP().toString();
  d["sta_ok"]   = staConnected;
  d["sta_ssid"] = staHaveCreds ? staSsid : "";
  d["mac"]      = WiFi.macAddress();
  d["ir_tx"]    = IR_TX_PIN; d["ir_rx"] = IR_RX_PIN;
  d["fw_ver"]   = FIRMWARE_VERSION;

  JsonObject ota = d.createNestedObject("ota");
  ota["in_progress"] = otaInProgress;
  ota["last_ok"]     = otaLastOk;
  ota["last_bytes"]  = otaLastBytes;
  ota["last_err"]    = otaLastErr;
  ota["auto_check"]  = otaCfg.autoCheck;
  ota["auto_install"]= otaCfg.autoInstall;
  ota["interval_min"]= otaCfg.intervalMin;
  ota["update_avail"]= otaUpdateAvailable;
  ota["manifest_ver"]= lastManifestVersion;

  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}
void handleGetHostname(){
  DynamicJsonDocument d(256);
  d["hostname"]=gHostname; d["instance"]=gInstance;
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}
void handleSetHostname(){
  String newHost,newInst;
  if (!parseHostnameBody(newHost,newInst)){ server.send(400,"application/json","{\"error\":\"Missing hostname\"}"); return; }
  newHost.toLowerCase();
  if (!validHostname(newHost)){ server.send(400,"application/json","{\"error\":\"Invalid hostname\"}"); return; }
  if (newInst.isEmpty()) newInst=gInstance;
  saveIdentity(newHost, newInst);
  DynamicJsonDocument d(256);
  d["status"]="saved"; d["hostname"]=newHost; d["instance"]=newInst;
  String out; serializeJson(d,out); server.send(200,"application/json",out);
  Serial.println("[HOST] Hostname saved. Rebooting..."); delay(300); ESP.restart();
}

// ---------- HTTP: Wi-Fi ----------
void handleWifiStatus(){
  DynamicJsonDocument d(512);
  d["ap"] = WiFi.softAPIP().toString();
  JsonObject sta = d.createNestedObject("sta");
  sta["saved"]   = staHaveCreds;
  sta["ssid"]    = staHaveCreds ? staSsid : "";
  sta["ip"]      = WiFi.localIP().toString();
  sta["ok"]      = staConnected;
  sta["connecting"] = staConnecting && !staConnected;
  if (staConnected) sta["rssi"] = WiFi.RSSI();
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}
void handleWifiSave(){
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if (ssid=="" && server.hasArg("plain")){
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, server.arg("plain"))){ ssid = String(doc["ssid"]|""); pass = String(doc["pass"]|""); }
  }
  ssid.trim();
  if (ssid==""){ server.send(400,"application/json","{\"error\":\"missing ssid\"}"); return; }

  int n = WiFi.scanNetworks(false, true);
  int bestIdx=-1, bestRssi=-999, bestChan=-1;
  for (int i=0;i<n;i++){
    if (WiFi.SSID(i) == ssid){
      int ch = WiFi.channel(i), rssi=WiFi.RSSI(i);
      if (ch>=1 && ch<=14 && rssi>bestRssi){ bestRssi=rssi; bestIdx=i; bestChan=ch; }
    }
  }
  WiFi.scanDelete();
  if (bestIdx < 0){
    server.send(400,"application/json",
      "{\"error\":\"ssid_not_2g\",\"msg\":\"ESP32 supports only 2.4GHz. Pick 2.4G SSID or enable mixed mode.\"}");
    return;
  }

  if (apEnabled){
    WiFi.softAPdisconnect(true); apEnabled=false;
    bool ok = WiFi.softAP(AP_SSID, AP_PASS, bestChan);
    apEnabled = ok;
    Serial.printf("[AP] Retuned to channel %d -> %s\n", bestChan, ok?"OK":"FAIL");
  }

  saveStaCreds(ssid, pass);
  WiFi.disconnect(); delay(100);
  staConnecting=true; staConnected=false;
  Serial.printf("[STA] begin(\"%s\") ch=%d\n", ssid.c_str(), bestChan);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t t0=millis(); bool ok=false;
  while (millis()-t0 < 10000){ if (WiFi.status()==WL_CONNECTED){ ok=true; break; } delay(150); yield(); }
  if (ok) stopAPIfRunning("Provision OK"); else startAPIfNeeded("Provision failed");

  DynamicJsonDocument d(256);
  d["saved"]=true; d["ssid"]=ssid; d["connected"]=ok; d["ip"]=WiFi.localIP().toString();
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}
void handleWifiForget(){
  forgetStaCreds(); WiFi.disconnect(); staConnected=false; staConnecting=false;
  startAPIfNeeded("Forget Wi-Fi");
  server.send(200,"application/json","{\"status\":\"forgot\"}");
}
void handleWifiScan(){
  int n = WiFi.scanNetworks(false, true);
  DynamicJsonDocument d(4096);
  JsonArray arr = d.createNestedArray("nets");
  for (int i=0;i<n;i++){
    String ssid = WiFi.SSID(i);
    JsonObject o = arr.createNestedObject();
    o["ssid"]   = ssid;
    o["bssid"]  = WiFi.BSSIDstr(i);
    o["rssi"]   = WiFi.RSSI(i);
    o["enc"]    = (int)WiFi.encryptionType(i);
    o["chan"]   = WiFi.channel(i);
    o["hidden"] = (ssid.length()==0);
  }
  WiFi.scanDelete();
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}

// ---------- HTTP: IR ----------
void handleIrTest(){
  int ms = server.hasArg("ms") ? server.arg("ms").toInt() : 800;
  if (ms<50) ms=50; if (ms>3000) ms=3000;
  carrierOn(38000,33); delay(ms); carrierOff();
  server.send(200,"application/json", String("{\"status\":\"ok\",\"pin\":")+IR_TX_PIN+",\"ms\":"+ms+"}");
}
void handleIRSend(){
  if (!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"Missing JSON body\"}"); return; }
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, server.arg("plain"))){ server.send(400,"application/json","{\"error\":\"Invalid JSON\"}"); return; }
  uint32_t freqHz = doc.containsKey("freq_khz") ? (uint32_t)doc["freq_khz"].as<uint32_t>()*1000UL : (uint32_t)38000;
  uint8_t  duty   = doc["duty"]   | 33;
  uint16_t repeat = doc["repeat"] | 1;
  JsonArray raw = doc["raw"].as<JsonArray>();
  if (raw.isNull() || raw.size()==0){ server.send(400,"application/json","{\"error\":\"Missing raw array\"}"); return; }
  size_t n=min((size_t)raw.size(), (size_t)IR_RAW_MAX);
  for (size_t i=0;i<n;i++){ int v=raw[i].as<int>(); if (v<0) v=0; scratch[i]=(uint32_t)v; }
  bool ok = irSendRaw(scratch,n,freqHz,duty,repeat);
  if (ok){
    DynamicJsonDocument resp(256);
    resp["status"]="sent"; resp["pin"]=IR_TX_PIN; resp["freq"]=freqHz; resp["duty"]=duty; resp["count"]=(uint32_t)n;
    String out; serializeJson(resp,out); server.send(200,"application/json",out);
  } else server.send(500,"application/json","{\"error\":\"IR send failed\"}");
}
void handleIrLast(){ if (lastIrJson.length()==0) server.send(404,"application/json","{\"error\":\"no_capture_yet\"}"); else server.send(200,"application/json", lastIrJson); }
void handleRxInfo(){
  DynamicJsonDocument d(256);
  d["ir_rb_ok"] = (bool)irRb;
  d["ir_pin"]   = IR_RX_PIN;
  d["ir_idle"]  = IR_RX_IDLE_US;
  d["ir_filter"]= IR_RX_FILTER_US;
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}

// ---------- HTTP: OTA status/config/manifest/check/url ----------
void handleOtaStatus(){
  DynamicJsonDocument d(320);
  d["in_progress"]=otaInProgress;
  d["last_ok"]=otaLastOk;
  d["last_bytes"]=otaLastBytes;
  d["last_err"]=otaLastErr;
  d["fw_ver"]=FIRMWARE_VERSION;
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}
void handleOtaGetCfg(){
  DynamicJsonDocument d(640);
  d["manifest_url"]=otaCfg.manifestUrl;
  JsonObject auth = d.createNestedObject("auth");
  auth["type"]=otaCfg.authType;
  if (otaCfg.authType=="bearer") auth["bearer"]=(otaCfg.bearer.length()?"***":"");
  if (otaCfg.authType=="basic") { auth["user"]=otaCfg.basicUser; auth["pass"]=(otaCfg.basicPass.length()?"***":""); }
  d["auto_check"]=otaCfg.autoCheck;
  d["auto_install"]=otaCfg.autoInstall;
  d["interval_min"]=otaCfg.intervalMin;
  d["allow_insecure_tls"]=otaCfg.allowInsecureTLS;
  d["update_available"]=otaUpdateAvailable;
  d["manifest_version"]=lastManifestVersion;
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}
void handleOtaSetCfg(){
  if (!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"Missing JSON\"}"); return; }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))){ server.send(400,"application/json","{\"error\":\"Invalid JSON\"}"); return; }
  if (doc.containsKey("manifest_url")) otaCfg.manifestUrl = String(doc["manifest_url"].as<const char*>()); 
  if (doc.containsKey("auto_check"))   otaCfg.autoCheck   = (bool)doc["auto_check"];
  if (doc.containsKey("auto_install")) otaCfg.autoInstall = (bool)doc["auto_install"];
  if (doc.containsKey("interval_min")) otaCfg.intervalMin = (uint32_t)doc["interval_min"];
  if (doc.containsKey("allow_insecure_tls")) otaCfg.allowInsecureTLS = (bool)doc["allow_insecure_tls"];

  if (doc.containsKey("auth")){
    JsonObject a = doc["auth"].as<JsonObject>();
    String t = String(a["type"]|"none");
    t.toLowerCase();
    if (t!="none" && t!="bearer" && t!="basic") t="none";
    otaCfg.authType = t;
    if (t=="bearer"){ otaCfg.bearer = String(a["bearer"]|""); otaCfg.basicUser=""; otaCfg.basicPass=""; }
    else if (t=="basic"){ otaCfg.basicUser = String(a["user"]|""); otaCfg.basicPass = String(a["pass"]|""); otaCfg.bearer=""; }
    else { otaCfg.bearer=""; otaCfg.basicUser=""; otaCfg.basicPass=""; }
  }
  saveOtaCfg();
  server.send(200,"application/json","{\"ok\":true}");
}

void handleOtaManifest(){
  Manifest m; String err;
  bool ok = fetchManifest(m, err);
  DynamicJsonDocument d(1024);
  d["ok"]=ok; if(!ok) d["error"]=err;
  if (ok){
    d["version"]=m.version; d["url"]=m.url; d["file"]=m.file; d["size"]=m.size; d["md5"]=m.md5; d["min"]=m.min; d["force"]=m.force; d["notes"]=m.notes;
    int cmp = cmpSemver(m.version, FIRMWARE_VERSION);
    bool meetsMin = (m.min=="") || (cmpSemver(String(FIRMWARE_VERSION), m.min) >= 0);
    d["update_available"] = (cmp>0) && meetsMin;
  }
  String out; serializeJson(d,out);
  server.send(ok?200:500,"application/json",out);
}

void handleOtaCheck(){
  if (otaInProgress){ server.send(409,"application/json","{\"error\":\"ota_in_progress\"}"); return; }
  Manifest m; String err;
  if (!fetchManifest(m, err)){ server.send(500,"application/json", String("{\"ok\":false,\"error\":\"")+err+"\"}"); return; }

  String fwUrl = m.url.length()? m.url : resolveRelative(otaCfg.manifestUrl, m.file);
  int cmp = cmpSemver(m.version, FIRMWARE_VERSION);
  bool meetsMin = (m.min=="") || (cmpSemver(String(FIRMWARE_VERSION), m.min) >= 0);
  bool should = (cmp>0) && meetsMin;

  if (!should && !m.force){
    DynamicJsonDocument d(256); d["ok"]=false; d["reason"]="up_to_date"; d["current"]=FIRMWARE_VERSION; d["target"]=m.version;
    String out; serializeJson(d,out); server.send(200,"application/json",out); return;
  }

  String err2; uint32_t written=0;
  bool ok = doHttpOtaUrl(fwUrl, m.md5, err2, written);
  otaLastOk = ok; otaLastBytes = written; otaLastErr = ok?"":err2;

  DynamicJsonDocument resp(320);
  resp["ok"]=ok; resp["written"]=written; resp["target"]=m.version; resp["rebooting"]=ok;
  if(!ok) resp["error"]=err2;
  String out; serializeJson(resp,out);
  server.send(ok?200:500,"application/json",out);
  if (ok) otaRebootAtMs = millis() + 1200;
}

// direct URL (no manifest), kept for manual installs
void handleOtaUrl(){
  String url="";
  if (server.hasArg("plain")){
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) url = String(doc["url"]|"");
  }
  if (url=="") url = server.arg("url");
  url.trim();
  if (url==""){ server.send(400,"application/json","{\"error\":\"missing url\"}"); return; }
  if (otaInProgress){ server.send(409,"application/json","{\"error\":\"ota_in_progress\"}"); return; }

  String err; uint32_t written=0;
  bool ok = doHttpOtaUrl(url, "", err, written);
  otaLastOk = ok; otaLastBytes = written; otaLastErr = ok?"":err;

  DynamicJsonDocument resp(256);
  resp["ok"]=ok; resp["written"]=written; resp["rebooting"]=ok;
  if(!ok) resp["error"]=err;
  String out; serializeJson(resp,out);
  server.send(ok?200:500, "application/json", out);
  if (ok) otaRebootAtMs = millis() + 1200;
}

// ---------- WS ----------
static size_t parseCsvDurations(const char* csv, uint32_t* out, size_t max){
  if(!csv) return 0;
  size_t n=0; uint32_t val=0; bool in=false, neg=false;
  for(const char* p=csv; *p && n<max; ++p){
    char c=*p;
    if(c=='-'){ neg=true; continue; }
    if(c>='0'&&c<='9'){ in=true; val=val*10u+(uint32_t)(c-'0'); }
    else { if(in){ out[n++]=neg?0u:val; val=0; in=false; neg=false; } }
  }
  if(in && n<max) out[n++]=neg?0u:val;
  return n;
}
void wsSendObj(uint8_t num, const JsonDocument& d);
void wsHandleText(uint8_t num, const char* txt, size_t len){
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, txt, len)){
    DynamicJsonDocument e(96); e["type"]="ir_tx_err"; e["error"]="Invalid JSON"; wsSendObj(num,e); return;
  }
  String type = String(doc["type"]|"");

  auto jFreqHz = [&](){
    uint32_t f=0; if (doc.containsKey("freq_khz")) f = (uint32_t)doc["freq_khz"].as<uint32_t>()*1000UL;
    return f?f:38000U;
  };
  uint8_t  duty   = doc["duty"]   | 33;
  uint16_t repeat = doc["repeat"] | 1;

  if (type=="ir_send_csv"||type=="ir_sendA_csv"||type=="ir_sendB_csv"||type=="ir_sendAB_csv"){
    const char* a_csv = doc["a_csv"] | "";
    const char* b_csv = doc["b_csv"] | "";
    const char* raw_csv=doc["raw_csv"]| "";
    uint32_t gap_us = doc["gap_us"] | DEFAULT_AB_GAP_US;
    size_t n=0;
    if (type=="ir_send_csv") n=parseCsvDurations(raw_csv,scratch,IR_RAW_MAX);
    else if (type=="ir_sendA_csv") n=parseCsvDurations(a_csv,scratch,IR_RAW_MAX);
    else if (type=="ir_sendB_csv") n=parseCsvDurations(b_csv,scratch,IR_RAW_MAX);
    else { if (a_csv&&*a_csv){ n=parseCsvDurations(a_csv,scratch,IR_RAW_MAX); if(n>0){ if((n&1)==0) scratch[n-1]+=gap_us; else scratch[n++]=gap_us; } else scratch[n++]=gap_us; } else scratch[n++]=gap_us;
           if (b_csv&&*b_csv) n+=parseCsvDurations(b_csv,scratch+n,IR_RAW_MAX-n); }
    bool ok = (n>0) && irSendRaw(scratch,n,jFreqHz(),duty,repeat);
    DynamicJsonDocument r(160); r["type"]= ok?"ir_tx_ack":"ir_tx_err"; r["count"]=(uint32_t)n; r["freq"]=jFreqHz(); r["duty"]=duty; if(!ok) r["error"]="send_csv failed"; wsSendObj(num,r); return;
  }

  if (type=="ir_send"||type=="ir_sendA"||type=="ir_sendB"||type=="ir_sendAB"){
    size_t n=0;
    if (type=="ir_send"){
      JsonArray raw=doc["raw"].as<JsonArray>(); if(raw.isNull()||raw.size()==0){DynamicJsonDocument e(96);e["type"]="ir_tx_err";e["error"]="Missing raw[]";wsSendObj(num,e);return;}
      n=min((size_t)raw.size(),(size_t)IR_RAW_MAX); for(size_t i=0;i<n;i++) scratch[i]=(uint32_t)max(0,raw[i].as<int>());
    } else if (type=="ir_sendA"||type=="ir_sendB"){
      JsonArray arr=(type=="ir_sendA")?doc["a"].as<JsonArray>():doc["b"].as<JsonArray>();
      if(arr.isNull()||arr.size()==0){DynamicJsonDocument e(96);e["type"]="ir_tx_err";e["error"]="Missing A/B";wsSendObj(num,e);return;}
      n=min((size_t)arr.size(),(size_t)IR_RAW_MAX); for(size_t i=0;i<n;i++) scratch[i]=(uint32_t)max(0,arr[i].as<int>());
    } else {
      JsonArray a=doc["a"].as<JsonArray>(), b=doc["b"].as<JsonArray>(); uint32_t gap_us = doc["gap_us"] | DEFAULT_AB_GAP_US;
      if (!a.isNull()){ size_t na=min((size_t)a.size(),(size_t)IR_RAW_MAX); for(size_t i=0;i<na && n<IR_RAW_MAX;i++) scratch[n++]=(uint32_t)max(0,a[i].as<int>()); if(n>0){ if((n&1)==0) scratch[n-1]+=gap_us; else scratch[n++]=gap_us; } else scratch[n++]=gap_us; } else scratch[n++]=gap_us;
      if (!b.isNull()){ size_t nb=min((size_t)b.size(),(size_t)IR_RAW_MAX-n); for(size_t i=0;i<nb;i++) scratch[n++]=(uint32_t)max(0,b[i].as<int>()); }
    }
    bool ok= irSendRaw(scratch,n,jFreqHz(),duty,repeat);
    DynamicJsonDocument r(160); r["type"]= ok?"ir_tx_ack":"ir_tx_err"; r["count"]=(uint32_t)n; r["freq"]=jFreqHz(); r["duty"]=duty; if(!ok) r["error"]="send failed"; wsSendObj(num,r); return;
  }

  if (type=="ir_test"){
    int ms=doc["ms"]|800; if(ms<50)ms=50; if(ms>3000)ms=3000; carrierOn(38000,33); delay(ms); carrierOff();
    DynamicJsonDocument r(120); r["type"]="ir_tx_ack"; r["ok"]=true; r["test_ms"]=ms; r["freq"]=38000; r["duty"]=33; wsSendObj(num,r); return;
  }

  
  if (type=="rf_send"){
    uint32_t code = doc["code"] | 0;
    uint8_t  bits = doc["bits"] | 24;
    uint8_t  proto= doc["protocol"] | 1;
    uint16_t pulselen = doc["pulselen"] | 0;
    uint8_t  repeat = doc["repeat"] | 8;
    if (!code){ DynamicJsonDocument e(96); e["type"]="rf_tx_err"; e["error"]="missing code"; wsSendObj(num,e); return; }
    rfTx.setRepeatTransmit(repeat);
    if (pulselen) rfTx.setPulseLength(pulselen);
    if (proto) rfTx.setProtocol(proto);
    rfTx.send(code, bits);
    DynamicJsonDocument a(160); a["type"]="rf_tx_ack"; a["code"]=code; a["bits"]=bits; a["protocol"]=proto; a["pulselen"]=pulselen; a["repeat"]=repeat; wsSendObj(num,a);
    return;
  }
  DynamicJsonDocument r(64); r["type"]="hello"; wsSendObj(num,r);
}
void wsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length){
  switch(type){
    case WStype_CONNECTED:
      if (lastIrJson.length()) ws.sendTXT(num, lastIrJson);
      if (lastRfJson.length()) ws.sendTXT(num, lastRfJson);
      break;
    case WStype_TEXT:
      wsHandleText(num, (const char*)payload, length);
      break;
    default: break;
  }
}


// ---------- RF 433 helpers ----------
static void rfBroadcast(uint32_t code, uint8_t bits, uint8_t proto, uint16_t pulselen){
  DynamicJsonDocument d(192);
  d["type"]="rf_rx";
  d["code"]=code;
  d["bits"]=bits;
  d["protocol"]=proto;
  d["pulselen"]=pulselen;
  String out; serializeJson(d,out);
  lastRfJson = out;
  ws.broadcastTXT(out);
}
static void pollRf(){
  if (rfRx.available()){
    uint32_t code = rfRx.getReceivedValue();
    uint8_t  bits = rfRx.getReceivedBitlength();
    uint8_t  proto= rfRx.getReceivedProtocol();
    uint16_t pulselen = rfRx.getReceivedDelay();
    rfBroadcast(code,bits,proto,pulselen);
    rfRx.resetAvailable();
  }
}
static void handleRfSend(){
  uint32_t code = 0; 
  uint8_t  bits = 24;
  uint8_t  proto = 1;
  uint16_t pulselen = 0;
  uint8_t  repeat = 8;

  if (server.hasArg("plain")){
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))){
      code = doc["code"] | 0;
      bits = doc["bits"] | bits;
      proto = doc["protocol"] | proto;
      pulselen = doc["pulselen"] | pulselen;
      repeat = doc["repeat"] | repeat;
    }
  } else {
    code = (uint32_t) (server.arg("code").toInt());
    if (server.hasArg("bits")) bits = (uint8_t) server.arg("bits").toInt();
    if (server.hasArg("protocol")) proto = (uint8_t) server.arg("protocol").toInt();
    if (server.hasArg("pulselen")) pulselen = (uint16_t) server.arg("pulselen").toInt();
    if (server.hasArg("repeat")) repeat = (uint8_t) server.arg("repeat").toInt();
  }
  if (!code){ server.send(400,"application/json","{\"error\":\"missing code\"}"); return; }

  rfTx.setRepeatTransmit(repeat);
  if (pulselen) rfTx.setPulseLength(pulselen);
  if (proto) rfTx.setProtocol(proto);
  rfTx.send(code, bits);

  DynamicJsonDocument d(160);
  d["status"]="ok";
  d["type"]="rf_tx_ack";
  d["code"]=code; d["bits"]=bits; d["protocol"]=proto; d["pulselen"]=pulselen; d["repeat"]=repeat;
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
  ws.broadcastTXT(out);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(250);
  esp_log_level_set("*", ESP_LOG_WARN);

  Serial.println("\n[Boot] ESP32 IR Gateway (IR only) + OTA via manifest/url");

  // RF 433 init
  rfRx.enableReceive(digitalPinToInterrupt(RF_RX_PIN)); // Receive on GPIO25
  rfTx.enableTransmit(RF_TX_PIN);                       // Transmit on GPIO22
  rfTx.setProtocol(1); rfTx.setRepeatTransmit(8);

  // heap buffers
  irA=(uint32_t*)malloc(IR_RAW_MAX*sizeof(uint32_t));
  irB=(uint32_t*)malloc(IR_RAW_MAX*sizeof(uint32_t));
  irC=(uint32_t*)malloc(IR_RAW_MAX*sizeof(uint32_t));
  scratch=(uint32_t*)malloc(IR_RAW_MAX*sizeof(uint32_t));
  if(!irA||!irB||!irC||!scratch){
    Serial.println("[MEM] FATAL: not enough heap for buffers");
    while(true) delay(1000);
  }

  // Factory reset button
  pinMode(FACTORY_BTN_PIN, INPUT_PULLUP);

  loadIdentity(); loadStaCreds(); loadOtaCfg();
  WiFi.onEvent(onWiFiEvent);
  bringUpWifi();

  // HTTP routes
  server.on("/",                  HTTP_GET,  handleRoot);
  server.on("/ir.html",           HTTP_GET,  handleIrView);
  server.on("/api/status",        HTTP_GET,  handleStatus);
  server.on("/api/hostname",      HTTP_GET,  handleGetHostname);
  server.on("/api/hostname",      HTTP_POST, handleSetHostname);

  server.on("/api/wifi/status",   HTTP_GET,  handleWifiStatus);
  server.on("/api/wifi/save",     HTTP_POST, handleWifiSave);
  server.on("/api/wifi/forget",   HTTP_POST, handleWifiForget);
  server.on("/api/wifi/scan",     HTTP_GET,  handleWifiScan);

  server.on("/api/ir/test",       HTTP_GET,  handleIrTest);
  server.on("/api/ir/send",       HTTP_POST, handleIRSend);
  server.on("/api/ir/last",       HTTP_GET,  handleIrLast);
  server.on("/api/ir/rxinfo",     HTTP_GET,  handleRxInfo);

  // OTA
  server.on("/api/ota/status",    HTTP_GET,  handleOtaStatus);
  server.on("/api/ota/config",    HTTP_GET,  handleOtaGetCfg);
  server.on("/api/ota/config",    HTTP_POST, handleOtaSetCfg);
  server.on("/api/ota/manifest",  HTTP_GET,  handleOtaManifest);
  server.on("/api/ota/check",     HTTP_POST, handleOtaCheck);
  server.on("/api/ota/url",       HTTP_POST, handleOtaUrl);
  server.on("/api/rf/send",      HTTP_POST, handleRfSend);

  server.begin();
  startMDNSOnce();

  // IR init
  irInitTimer(); irInitChannel(); irRxInit();

  // WS
  ws.begin();
  ws.onEvent(wsEvent);

  // schedule first auto-check soon after boot
  nextOtaCheckAtMs = millis() + 15000;

  Serial.println("[API] Ready");
}

void loop() {
  server.handleClient();
  ws.loop();

  pollIr();
  pollRf();
  pollFactoryButton();

  if (!staConnected && !apEnabled && apReenableAtMs && millis() >= apReenableAtMs){
    startAPIfNeeded("Re-enable (STA down)");
  }

  // Auto-check OTA
  if (otaCfg.autoCheck && !otaInProgress && WiFi.status()==WL_CONNECTED){
    if (nextOtaCheckAtMs && (int32_t)(millis() - nextOtaCheckAtMs) >= 0){
      Manifest m; String err;
      if (fetchManifest(m, err)){
        lastManifestVersion = m.version;
        bool meetsMin = (m.min=="") || (cmpSemver(String(FIRMWARE_VERSION), m.min) >= 0);
        otaUpdateAvailable = (cmpSemver(m.version, FIRMWARE_VERSION) > 0) && meetsMin;
        if (otaUpdateAvailable && otaCfg.autoInstall){
          String fwUrl = m.url.length()? m.url : resolveRelative(otaCfg.manifestUrl, m.file);
          String err2; uint32_t written=0;
          bool ok = doHttpOtaUrl(fwUrl, m.md5, err2, written);
          otaLastOk = ok; otaLastBytes = written; otaLastErr = ok?"":err2;
          if (ok) otaRebootAtMs = millis() + 1200;
        }
      }
      // schedule next check
      uint32_t gap = (otaCfg.intervalMin>1? otaCfg.intervalMin:1)*60*1000UL;
      nextOtaCheckAtMs = millis() + gap;
    }
  }

  // Deferred reboot after OTA
  if (otaRebootAtMs && (int32_t)(millis() - otaRebootAtMs) >= 0){
    otaRebootAtMs = 0;
    Serial.println("[OTA] Rebooting now...");
    delay(100);
    ESP.restart();
  }

  delay(1);
}


