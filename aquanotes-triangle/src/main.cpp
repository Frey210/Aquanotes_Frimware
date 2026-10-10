/*
 * Aerasea RTOS UI – ESP32-S3
 * Versi: 4 tombol (UP, DOWN, OK, BACK), tanpa encoder
 * Display: TFT ILI9341 320x240 (SPI)
 * PlatformIO Version
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <MQTTPubSubClient.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "ui_types.h"
#include "aerasea_logo_128x64_inverted.h"
#include "mqtt_ca.h"
#include "ota_update.h"
#include "device_config.h"

// =======================
//  KONFIG PERANGKAT & NET
// =======================

// TFT ILI9341 (SPI) - landscape
static const uint8_t TFT_ROTATION = 3;
static const uint32_t TFT_SPI_FREQ = 16000000;

// TFT pin mapping (ESP32-S3)
#define TFT_CS   10   // Chip Select
#define TFT_DC   9    // Data/Command
#define TFT_RST  8    // Reset (connect to 3.3V if LCD has reset pin)
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCK  12
#define TFT_BL   21   // Backlight control via 2N2222 transistor
#define TFT_BACKLIGHT_ON HIGH

// 16-bit color aliases (RGB565)
#define TFT_BLACK     0x0000
#define TFT_NAVY      0x000F
#define TFT_DARKGREY  0x7BEF
#define TFT_LIGHTGREY 0xC618
#define TFT_WHITE     0xFFFF
#define TFT_ORANGE    0xFD20
#define TFT_YELLOW    0xFFE0
#define TFT_CYAN      0x07FF
#define TFT_GREEN     0x07E0
#define TFT_BLUE      0x001F
#define TFT_MAGENTA   0xF81F

// RGB LED (common cathode)
#define LED_R_PIN 2
#define LED_G_PIN 3
#define LED_B_PIN 4

// LEDC (PWM) channels
#define LEDC_CH_R 0
#define LEDC_CH_G 1
#define LEDC_CH_B 2
#define LEDC_FREQ 5000
#define LEDC_RES  8

// RS485 (shared bus)
#define RS485_RX_PIN    16
#define RS485_TX_PIN    15
#define RS485_DE_RE_PIN 14

// Slave IDs modular (old sensor)
// #define PH_SLAVE_ID   12   // pH
// #define EC_SLAVE_ID   30   // EC/TDS/Sal
// #define NH4_SLAVE_ID  1   // NH4
// #define DO_SLAVE_ID   55  // DO (default)

// Slave IDs modular (new sensor)
#define PH_SLAVE_ID   3   // pH
#define EC_SLAVE_ID   4   // EC/TDS/Sal
#define NH4_SLAVE_ID  1   // NH4
#define DO_SLAVE_ID   10  // DO (default)

// Sesuaikan dengan sensor yang benar-benar terpasang pada unit ini.
static const bool PH_SENSOR_ENABLED  = true;
static const bool EC_SENSOR_ENABLED  = true;
static const bool NH4_SENSOR_ENABLED = false;
static const bool DO_SENSOR_ENABLED  = true;

// Serial format
#define PH_BAUD   9600
#define EC_BAUD   9600
#define NH4_BAUD  9600
#define DO_BAUD   9600

// Broker & identitas
const char* MQTT_HOST  = "mqtt.farlabs.my.id";
const uint16_t MQTT_PORT = 443;
const char* MQTT_PATH  = "/mqtt";
#ifndef AQUANOTES_FW_VERSION
#define AQUANOTES_FW_VERSION "2.2.0"
#endif
const char* FW_VERSION = AQUANOTES_FW_VERSION;
const uint32_t PUBLISH_INTERVAL_MS = 10000; // 10 detik
const uint32_t MQTT_TIMEOUT_MS = 4000;
const uint32_t BACKLIGHT_TIMEOUT_MS = 15000; // mati setelah 15 detik tanpa interaksi

// =======================
//  INPUT: 4 BUTTON
// =======================
// Button aktif LOW (INPUT_PULLUP)
#define BTN_UP     5
#define BTN_DOWN   6
#define BTN_OK     7
#define BTN_BACK   17

// =======================
//  OBJEK GLOBAL
// =======================
class TFTWrapper : public Adafruit_ILI9341 {
public:
  using Adafruit_ILI9341::Adafruit_ILI9341;

  void setTextFont(uint8_t size) {
    setTextSize(size);
    setFont(NULL);
  }

  int16_t textWidth(const char* text, uint8_t size) {
    setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return static_cast<int16_t>(w);
  }

  void drawString(const char* text, int16_t x, int16_t y) {
    setCursor(x, y);
    print(text);
  }

  void drawCentreString(const char* text, int16_t x, int16_t y, uint8_t size) {
    setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    setCursor(x - static_cast<int16_t>(w / 2), y);
    print(text);
  }
};

TFTWrapper tft(TFT_CS, TFT_DC, TFT_RST);
HardwareSerial RS485(2);

// (opsional) legacy objects untuk menu kalibrasi lama
ModbusMaster mb_ph, mb_ec, mb_nh4, mb_do;

struct RuntimeStatus {
  char sendStatus[16] = "--";
};

struct TaskInputArgs {
  QueueHandle_t qInput;
};

struct TaskUIArgs {
  QueueHandle_t qInput;
  QueueHandle_t qDisplay;
  QueueHandle_t qCalib;
  QueueHandle_t qCalibResult;
  EventGroupHandle_t flags;
};

struct TaskSensorArgs {
  QueueHandle_t qDisplay;
  QueueHandle_t qTelemetry;
  QueueHandle_t qCalib;
  QueueHandle_t qCalibResult;
  SemaphoreHandle_t rs485;
  EventGroupHandle_t flags;
  RuntimeStatus* status;
};

struct TaskMqttArgs {
  QueueHandle_t qTelemetry;
  QueueHandle_t qOtaCommand;
  QueueHandle_t qOtaStatus;
  EventGroupHandle_t flags;
  RuntimeStatus* status;
};

struct TaskOtaArgs {
  QueueHandle_t qOtaCommand;
  QueueHandle_t qOtaStatus;
  EventGroupHandle_t flags;
};

// Helper RS485
static inline void rs485Receive()  { digitalWrite(RS485_DE_RE_PIN, LOW); }
static inline void rs485Transmit() { digitalWrite(RS485_DE_RE_PIN, HIGH); }
static void modbusIdle() { vTaskDelay(pdMS_TO_TICKS(1)); }

// =======================
//  RTOS PRIMITIVES
// =======================
QueueHandle_t qInput;      // Input events -> UI
QueueHandle_t qDisplay;    // Snapshot sensor -> UI (len=1, overwrite)
QueueHandle_t qTelemetry;  // Snapshot sensor -> MQTT (len=1, overwrite)
QueueHandle_t qCalib;      // Perintah kalibrasi -> TaskSensors
QueueHandle_t qCalibResult;// Hasil kalibrasi -> TaskUI
QueueHandle_t qOtaCommand; // Desired OTA dari MQTT -> TaskOTA
QueueHandle_t qOtaStatus;  // Status OTA dari TaskOTA -> TaskMQTT
SemaphoreHandle_t mRS485;  // Mutex RS485
EventGroupHandle_t egFlags;

// Event flags
#define EG_WIFI_OK   (1<<0)
#define EG_PORTAL_ON (1<<1)
#define EG_WIFI_BUSY (1<<2)
#define EG_OTA_ACTIVE (1<<3)

// Input event
enum InputType: uint8_t { ENC_DELTA, BTN_PRESS };
struct InputEvent {
  InputType type;
  int16_t   value;   // delta (+1/-1) atau pin id
};

// Kalibrasi command (diekskusi di TaskSensors)
enum class CalibCmd: uint8_t {
  NONE,
  EC_1413, EC_12880,
  DO_ZERO, DO_AIR,
  PH_400, PH_700, PH_1000
};
struct CalibMsg {
  CalibCmd cmd;
};

struct CalibResult {
  CalibCmd cmd;
  bool ok;
};

struct OtaStatusMessage {
  uint32_t assignmentId = 0;
  int16_t progress = -1;
  bool clearPending = false;
  char targetVersion[64] = {0};
  char state[24] = {0};
  char errorCode[40] = {0};
};

// Register map (from vendor datasheets)
static const uint16_t REG_EC_CAL_FLOAT  = 0x0050;
static const uint16_t REG_PH_CAL_FIXED  = 0x0055;
static const uint16_t REG_DO_ZERO       = 0x001C; // write 1: zero oxygen calibration
static const uint16_t REG_DO_AIR        = 0x001A; // write 1: air calibration

// =======================
//  GLOBAL STATUS
// =======================
RuntimeStatus gStatus;
volatile uint32_t gLastInteractionMs = 0;
volatile bool gBacklightOn = true;
WebSocketsClient gMqttSocket;
MQTTPubSub::PubSubClient<1024> gMqtt;
uint32_t gBootNonce = 0;
uint32_t gMessageSequence = 0;
uint32_t gOtaMessageSequence = 0;
volatile float gBatteryPercent = 0.0f;
volatile bool gBatteryValid = false;
volatile bool gCalibrationActive = false;

// Battery sensor (SEN-0052)
#define BAT_ADC_PIN 1
static const float BAT_DIV_R1   = 100000.0f; // ohm, top resistor to battery+
static const float BAT_DIV_R2   = 22000.0f;  // ohm, bottom resistor to GND
static const float BAT_DIV_RATIO = (BAT_DIV_R1 + BAT_DIV_R2) / BAT_DIV_R2;
static const float BAT_CAL_FACTOR = 1.0338f;
static const uint8_t BAT_SAMPLES = 8;
static const float BAT_FILTER_ALPHA = 0.15f;
static const float BAT_PCT_DEADBAND = 0.5f;

struct BatterySocPoint {
  float volts;
  float pct;
};

// Kurva aproksimasi 4S Li-ion (tegangan pack) untuk kondisi idle / beban ringan.
static const BatterySocPoint BAT_4S_SOC_TABLE[] = {
  {16.80f, 100.0f},
  {16.40f,  95.0f},
  {16.20f,  90.0f},
  {16.00f,  80.0f},
  {15.80f,  70.0f},
  {15.60f,  60.0f},
  {15.40f,  50.0f},
  {15.20f,  40.0f},
  {15.00f,  30.0f},
  {14.80f,  20.0f},
  {14.40f,  10.0f},
  {14.00f,   5.0f},
  {13.20f,   0.0f}
};

static float batteryVoltageToPercent4S(float vbat){
  if (!isfinite(vbat)) return 0.0f;

  const size_t n = sizeof(BAT_4S_SOC_TABLE) / sizeof(BAT_4S_SOC_TABLE[0]);
  if (vbat >= BAT_4S_SOC_TABLE[0].volts) return 100.0f;
  if (vbat <= BAT_4S_SOC_TABLE[n - 1].volts) return 0.0f;

  for (size_t i = 0; i < n - 1; ++i){
    const BatterySocPoint& hi = BAT_4S_SOC_TABLE[i];
    const BatterySocPoint& lo = BAT_4S_SOC_TABLE[i + 1];
    if (vbat <= hi.volts && vbat >= lo.volts){
      const float spanV = hi.volts - lo.volts;
      if (spanV <= 0.0001f) return lo.pct;
      const float t = (vbat - lo.volts) / spanV;
      return lo.pct + t * (hi.pct - lo.pct);
    }
  }

  return 0.0f;
}

TaskInputArgs   gInputArgs{};
TaskUIArgs      gUIArgs{};
TaskSensorArgs  gSensorArgs{};
TaskMqttArgs    gMqttArgs{};
TaskOtaArgs     gOtaArgs{};

// Menu cursors
int menuCursor=0, calCursor=0, ecCalCursor=0, doCalCursor=0, phCalCursor=0;

// Toast (non-blocking)
static uint32_t toastUntil = 0;
static char toastMsg[12] = {0};

// Toast helper
void showToast(const char* msg, uint16_t ms=1200){
  strncpy(toastMsg, msg, sizeof(toastMsg)-1);
  toastMsg[sizeof(toastMsg)-1] = '\0';
  toastUntil = millis() + ms;
}
bool toastActive(){ return toastMsg[0] && millis() < toastUntil; }
void clearToast(){ toastMsg[0] = '\0'; }

// =======================
//  MQTT OVER WSS
// =======================
static String jsonNumber(float value, unsigned int digits) {
  return isfinite(value) ? String(value, digits) : "null";
}

static void setSendStatus(RuntimeStatus& status, const char* value) {
  strncpy(status.sendStatus, value, sizeof(status.sendStatus) - 1);
  status.sendStatus[sizeof(status.sendStatus) - 1] = '\0';
}

static void seedClockFromBuild() {
  if (time(nullptr) >= 1700000000) return;

  struct tm buildTime{};
  if (strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &buildTime)) {
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&buildTime);
    struct timeval now{epoch, 0};
    settimeofday(&now, nullptr);
  }
}

static String buildTelemetryPayload(const DisplayData& s) {
  char messageId[96];
  snprintf(messageId, sizeof(messageId), "%s-%08lX-%lu", deviceUid(),
           static_cast<unsigned long>(gBootNonce),
           static_cast<unsigned long>(gMessageSequence));

  const float salinityPpt = isfinite(s.sal) ? (s.sal / 1000.0f) : NAN;

  String payload = "{";
  payload.reserve(320);
  payload += "\"message_id\":\"" + String(messageId)     + "\",";
  payload += "\"uid\":\""      + String(deviceUid())    + "\",";
  payload += "\"suhu\":"       + jsonNumber(s.t_ds, 2) + ",";
  payload += "\"ph\":"         + jsonNumber(s.ph_ok ? s.ph : NAN, 2) + ",";
  payload += "\"do\":"         + jsonNumber(s.do_ok ? s.do_mgL : NAN, 2) + ",";
  payload += "\"tds\":"        + jsonNumber(s.ec_ok ? s.tds : NAN, 2) + ",";
  payload += "\"ammonia\":"    + jsonNumber(s.nh4_ok ? s.nh4 : NAN, 3) + ",";
  payload += "\"salinitas\":"  + jsonNumber(s.ec_ok ? salinityPpt : NAN, 3);
  payload += "}";
  return payload;
}

static void queueOtaStatus(uint32_t assignmentId, const char* targetVersion,
                           const char* state, int16_t progress = -1,
                           const char* errorCode = nullptr, bool clearPending = false) {
  if (!qOtaStatus || assignmentId == 0) return;
  OtaStatusMessage message{};
  message.assignmentId = assignmentId;
  message.progress = progress;
  message.clearPending = clearPending;
  strncpy(message.targetVersion, targetVersion ? targetVersion : "", sizeof(message.targetVersion) - 1);
  strncpy(message.state, state ? state : "failed", sizeof(message.state) - 1);
  if (errorCode) strncpy(message.errorCode, errorCode, sizeof(message.errorCode) - 1);
  if (xQueueSend(qOtaStatus, &message, 0) != pdTRUE) {
    Serial.println("[OTA] status queue penuh");
  }
}

static String buildOtaStatusPayload(const OtaStatusMessage& status) {
  char messageId[96];
  snprintf(messageId, sizeof(messageId), "%s-ota-%08lX-%lu", deviceUid(),
           static_cast<unsigned long>(gBootNonce),
           static_cast<unsigned long>(gOtaMessageSequence));
  String payload = "{";
  payload.reserve(320);
  payload += "\"schema\":1,";
  payload += "\"message_id\":\"" + String(messageId) + "\",";
  payload += "\"assignment_id\":" + String(status.assignmentId) + ",";
  payload += "\"running_version\":\"" + String(FW_VERSION) + "\",";
  payload += "\"target_version\":\"" + String(status.targetVersion) + "\",";
  payload += "\"state\":\"" + String(status.state) + "\",";
  payload += "\"progress\":" + (status.progress >= 0 ? String(status.progress) : String("null")) + ",";
  payload += "\"error_code\":";
  payload += status.errorCode[0] ? "\"" + String(status.errorCode) + "\"" : "null";
  payload += "}";
  return payload;
}

static bool subscribeOtaDesired() {
  const String topic = String("devices/") + deviceUid() + "/ota/desired";
  return gMqtt.subscribe(topic, 1, [](const char* payload, const size_t length) {
    if (!payload || length == 0) return;  // retained command dibersihkan backend
    OtaCommand command{};
    char errorCode[40] = {0};
    if (!otaParseCommand(payload, length, command, errorCode, sizeof(errorCode))) {
      Serial.printf("[OTA] manifest ditolak: %s\n", errorCode);
      return;
    }
    if (!command.rollback && otaCompareVersions(command.version, FW_VERSION) < 0) {
      queueOtaStatus(command.assignmentId, command.version, "failed", -1, "VERSION_REJECTED");
      return;
    }
    if (xQueueOverwrite(qOtaCommand, &command) != pdTRUE) {
      queueOtaStatus(command.assignmentId, command.version, "failed", -1, "QUEUE_FAILED");
      return;
    }
    queueOtaStatus(command.assignmentId, command.version, "assigned");
    Serial.printf("[OTA] assignment %lu diterima untuk %s\n",
                  static_cast<unsigned long>(command.assignmentId), command.version);
  });
}

static bool connectMqtt(RuntimeStatus& status) {
  if (gMqtt.isConnected()) return true;
  if (!deviceIsProvisioned()) {
    setSendStatus(status, "ECFG");
    return false;
  }

  gMqttSocket.disconnect();
  gMqttSocket.beginSslWithCA(MQTT_HOST, MQTT_PORT, MQTT_PATH, MQTT_ROOT_CA, "mqtt");
  gMqttSocket.setReconnectInterval(5000);
  gMqttSocket.enableHeartbeat(15000, 3000, 2);

  if (!gMqtt.connect(deviceUid(), deviceUid(), deviceMqttPassword())) {
    setSendStatus(status, "ECON");
    Serial.printf("[MQTT] connect gagal: err=%d rc=%d\n",
                  static_cast<int>(gMqtt.getLastError()),
                  static_cast<int>(gMqtt.getReturnCode()));
    return false;
  }

  if (!subscribeOtaDesired()) {
    setSendStatus(status, "ESUB");
    Serial.println("[OTA] subscribe desired gagal");
    gMqtt.disconnect();
    return false;
  }

  Serial.println("[MQTT] connected");
  return true;
}

static bool publishData(const DisplayData& s, RuntimeStatus& status, String& pendingPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    setSendStatus(status, "--");
    return false;
  }

  if (!connectMqtt(status)) return false;
  if (pendingPayload.isEmpty()) pendingPayload = buildTelemetryPayload(s);

  const String topic = String("devices/") + deviceUid() + "/telemetry";
  if (gMqtt.publish(topic, pendingPayload, false, 1)) {
    setSendStatus(status, "OK");
    pendingPayload.clear();
    ++gMessageSequence;
    Serial.println("[MQTT] publish QoS1 OK");
    return true;
  }

  setSendStatus(status, "EPUB");
  Serial.printf("[MQTT] publish gagal: err=%d rc=%d\n",
                static_cast<int>(gMqtt.getLastError()),
                static_cast<int>(gMqtt.getReturnCode()));
  return false;
}

// =======================
//  PEMBACAAN SENSOR
// =======================
static bool readBattery(float& vbat, float& pct){
  static bool batFilterInit = false;
  static float vbatFiltered = 0.0f;
  static float pctFiltered = 0.0f;

  uint32_t acc = 0;
  for (uint8_t i = 0; i < BAT_SAMPLES; ++i){
    acc += analogReadMilliVolts(BAT_ADC_PIN);
  }
  float v_adc = (acc / static_cast<float>(BAT_SAMPLES)) / 1000.0f;
  float vbatRaw = v_adc * BAT_DIV_RATIO * BAT_CAL_FACTOR;
  if (vbatRaw < 0.1f || isnan(vbatRaw)){
    pct = 0.0f;
    return false;
  }

  if (!batFilterInit){
    vbatFiltered = vbatRaw;
    pctFiltered = batteryVoltageToPercent4S(vbatRaw);
    batFilterInit = true;
  } else {
    vbatFiltered += BAT_FILTER_ALPHA * (vbatRaw - vbatFiltered);

    const float pctNow = batteryVoltageToPercent4S(vbatFiltered);
    if (fabsf(pctNow - pctFiltered) >= BAT_PCT_DEADBAND){
      pctFiltered = pctNow;
    }
  }

  vbat = vbatFiltered;
  pct = pctFiltered;

#if DEBUG_MODBUS
  Serial.print("[BAT] v_adc=");
  Serial.print(v_adc, 3);
  Serial.print(" V, raw=");
  Serial.print(vbatRaw, 2);
  Serial.print(" V, filt=");
  Serial.print(vbat, 2);
  Serial.print(", pct=");
  Serial.println(pct, 1);
#endif
  return true;
}

float regsToFloatBE(uint16_t r0, uint16_t r1){
  uint32_t raw = (static_cast<uint32_t>(r0) << 16) | static_cast<uint32_t>(r1);
  float f;
  memcpy(&f, &raw, sizeof(f));
  return f;
}

bool readPH(float& t, float& ph){
  // Sensor pH Rika memakai function 0x03, start 0x0000, length 0x0006.
  // Format respons: [pH][internal][temperature], semuanya float32 big-endian.
  uint8_t res = mb_ph.readHoldingRegisters(0x0000, 6);
  if (res == mb_ph.ku8MBSuccess){
    ph = regsToFloatBE(mb_ph.getResponseBuffer(0), mb_ph.getResponseBuffer(1));
    t  = regsToFloatBE(mb_ph.getResponseBuffer(4), mb_ph.getResponseBuffer(5));

    if (!isfinite(ph) || !isfinite(t) || ph < 0.0f || ph > 14.5f || t < -20.0f || t > 80.0f){
#if DEBUG_MODBUS
      Serial.println("[PH] Invalid float payload");
#endif
      return false;
    }

#if DEBUG_MODBUS
    Serial.print("[PH] pH="); Serial.print(ph);
    Serial.print(", T=");     Serial.println(t);
#endif
    return true;
  }

#if DEBUG_MODBUS
  Serial.print("[PH] Modbus error: 0x");
  Serial.println(res, HEX);
#endif
  return false;
}

bool readEC(float& t, float& ec, float& tds, float& sal){
  // Sensor EC Rika memakai function 0x03, start 0x0000, length 0x000A.
  // Format respons: [EC][internal][temperature][TDS][salinity], float32 big-endian.
  uint8_t res = mb_ec.readHoldingRegisters(0x0000, 10);
  if (res == mb_ec.ku8MBSuccess){
    ec  = regsToFloatBE(mb_ec.getResponseBuffer(0), mb_ec.getResponseBuffer(1));
    t   = regsToFloatBE(mb_ec.getResponseBuffer(4), mb_ec.getResponseBuffer(5));
    tds = regsToFloatBE(mb_ec.getResponseBuffer(6), mb_ec.getResponseBuffer(7));
    sal = regsToFloatBE(mb_ec.getResponseBuffer(8), mb_ec.getResponseBuffer(9));

    if (!isfinite(ec) || !isfinite(t) || !isfinite(tds) || !isfinite(sal) ||
        ec < 0.0f || tds < 0.0f || sal < 0.0f || t < -20.0f || t > 80.0f){
#if DEBUG_MODBUS
      Serial.println("[EC] Invalid float payload");
#endif
      return false;
    }

#if DEBUG_MODBUS
    Serial.print("[EC] EC=");        Serial.print(ec);
    Serial.print(" mS/cm, T=");      Serial.print(t);
    Serial.print(" C, TDS=");        Serial.print(tds);
    Serial.print(" mg/L, Sal=");     Serial.println(sal);
#endif
    return true;
  }

#if DEBUG_MODBUS
  Serial.print("[EC] Modbus error: 0x");
  Serial.println(res, HEX);
#endif
  return false;
}

// Helper pow10u untuk NH4 dan DO
static float pow10u(uint16_t n) {
  float s = 1.0f;
  for (uint16_t i = 0; i < n && i < 6; ++i) s *= 10.0f;
  return s;
}

bool readNH4(float& nh4, float& t){
  uint8_t res = mb_nh4.readHoldingRegisters(0x0000, 4);
  if (res == mb_nh4.ku8MBSuccess){
    uint16_t rawVal    = mb_nh4.getResponseBuffer(0);
    uint16_t rawDecVal = mb_nh4.getResponseBuffer(1);
    uint16_t rawTemp   = mb_nh4.getResponseBuffer(2);
    uint16_t rawDecT   = mb_nh4.getResponseBuffer(3);

    float scaleVal = pow10u(rawDecVal);
    float scaleT   = pow10u(rawDecT);

    nh4 = rawVal  / (scaleVal > 0 ? scaleVal : 1.0f);
    t   = rawTemp / (scaleT   > 0 ? scaleT   : 1.0f);

#if DEBUG_MODBUS
    Serial.print("[NH4] rawVal=");    Serial.print(rawVal);
    Serial.print(" decVal=");         Serial.print(rawDecVal);
    Serial.print(" rawTemp=");        Serial.print(rawTemp);
    Serial.print(" decTemp=");        Serial.println(rawDecT);
    Serial.print("[NH4] NH4=");       Serial.print(nh4);
    Serial.print(" mg/L, T=");        Serial.println(t);
#endif
    return true;
  } else {
#if DEBUG_MODBUS
    Serial.print("[NH4] Modbus error: 0x");
    Serial.println(res, HEX);
#endif
    return false;
  }
}

bool readDO(float& do_mg, float& tC){
  // Sensor DO Rika memakai function 0x03, start 0x0000, length 0x0006.
  // Format respons: 3 float 32-bit big-endian
  // [DO mg/L][Saturation %][Temperature C]
  uint8_t res = mb_do.readHoldingRegisters(0x0000, 6);
  if (res == mb_do.ku8MBSuccess){
    do_mg = regsToFloatBE(mb_do.getResponseBuffer(0), mb_do.getResponseBuffer(1));
    float sat = regsToFloatBE(mb_do.getResponseBuffer(2), mb_do.getResponseBuffer(3));
    tC    = regsToFloatBE(mb_do.getResponseBuffer(4), mb_do.getResponseBuffer(5));

    if (!isfinite(do_mg) || !isfinite(tC) || do_mg < 0.0f || tC < -20.0f || tC > 80.0f){
#if DEBUG_MODBUS
      Serial.println("[DO] Invalid float payload");
#endif
      return false;
    }

#if DEBUG_MODBUS
    Serial.print("[DO] DO=");        Serial.print(do_mg);
    Serial.print(" mg/L, Sat=");     Serial.print(sat);
    Serial.print(" %, T=");
    Serial.println(tC);
#endif
    return true;
  } else {
#if DEBUG_MODBUS
    Serial.print("[DO] Modbus error: 0x");
    Serial.println(res, HEX);
#endif
    return false;
  }
}

static void resolveWaterTemperature(DisplayData& cur){
  if (cur.ph_ok && !isnan(cur.phT)) {
    cur.t_ds = cur.phT;
  } else if (cur.ec_ok && !isnan(cur.ecT)) {
    cur.t_ds = cur.ecT;
  } else if (cur.nh4_ok && !isnan(cur.nh4T)) {
    cur.t_ds = cur.nh4T;
  } else if (cur.do_ok && !isnan(cur.do_tC)) {
    cur.t_ds = cur.do_tC;
  } else {
    cur.t_ds = NAN;
  }
}

static void readOneModularSensor(DisplayData& cur, uint8_t& phase){
  float temp = NAN;
  float value1 = NAN;
  float value2 = NAN;
  float value3 = NAN;

  switch (phase){
    case 0:
      cur.ph_ok = false;
      cur.ph = cur.phT = NAN;
      if (PH_SENSOR_ENABLED && readPH(temp, value1)){
        cur.ph = value1;
        cur.phT = temp;
        cur.ph_ok = true;
      }
      break;
    case 1:
      cur.ec_ok = false;
      cur.ec = cur.tds = cur.sal = cur.ecT = NAN;
      if (EC_SENSOR_ENABLED && readEC(temp, value1, value2, value3)){
        cur.ec = value1;
        cur.tds = value2;
        cur.sal = value3;
        cur.ecT = temp;
        cur.ec_ok = true;
      }
      break;
    case 2:
      cur.nh4_ok = false;
      cur.nh4 = cur.nh4T = NAN;
      if (NH4_SENSOR_ENABLED && readNH4(value1, temp)){
        cur.nh4 = value1;
        cur.nh4T = temp;
        cur.nh4_ok = true;
      }
      break;
    case 3:
    default:
      cur.do_ok = false;
      cur.do_mgL = cur.do_tC = NAN;
      if (DO_SENSOR_ENABLED && readDO(value1, temp)){
        cur.do_mgL = value1;
        cur.do_tC = temp;
        cur.do_ok = true;
      }
      break;
  }

  phase = static_cast<uint8_t>((phase + 1) % 4);
}


// =======================
//  MODBUS WRITE HELPER (Calib)
// =======================
static bool writeReg(ModbusMaster& mb, uint16_t reg, uint16_t value){
  uint8_t res = mb.writeSingleRegister(reg, value);
  return res == mb.ku8MBSuccess;
}

static bool writeFloatRegisters(ModbusMaster& mb, uint16_t reg, float value){
  uint32_t raw = 0;
  memcpy(&raw, &value, sizeof(raw));
  mb.clearTransmitBuffer();
  mb.setTransmitBuffer(0, static_cast<uint16_t>(raw >> 16));
  mb.setTransmitBuffer(1, static_cast<uint16_t>(raw & 0xFFFF));
  return mb.writeMultipleRegisters(reg, 2) == mb.ku8MBSuccess;
}

static bool handleCalibration(CalibCmd cmd){
  bool ok = false;
  switch (cmd){
    case CalibCmd::EC_1413:
      ok = writeFloatRegisters(mb_ec, REG_EC_CAL_FLOAT, 1.413f);
      break;
    case CalibCmd::EC_12880:
      ok = writeFloatRegisters(mb_ec, REG_EC_CAL_FLOAT, 12.88f);
      break;
    case CalibCmd::DO_ZERO:
      ok = writeReg(mb_do, REG_DO_ZERO, 1);
      break;
    case CalibCmd::DO_AIR:
      ok = writeReg(mb_do, REG_DO_AIR, 1);
      break;
    case CalibCmd::PH_400:
      ok = writeReg(mb_ph, REG_PH_CAL_FIXED, 4);
      break;
    case CalibCmd::PH_700:
      ok = writeReg(mb_ph, REG_PH_CAL_FIXED, 7);
      break;
    case CalibCmd::PH_1000:
      ok = writeReg(mb_ph, REG_PH_CAL_FIXED, 10);
      break;
    default:
      break;
  }
  return ok;
}

// =======================
//  INPUT / DEBOUNCE
// =======================
bool debounceRead(uint8_t pin){
  static uint8_t last[64] = {1};
  static uint32_t tLast[64] = {0};
  uint8_t raw = digitalRead(pin);
  uint32_t now = millis();
  if (raw != last[pin] && now - tLast[pin] > 25){
    tLast[pin] = now; last[pin] = raw;
    if (raw == LOW) return true;
  }
  return false;
}

// =======================
//  MENU DATA
// =======================
const char* MAIN_ITEMS[] = {
  "Dashboard",
  "WiFi Manager",
  "Kalibrasi Sensor"
};
const int MAIN_COUNT = sizeof(MAIN_ITEMS)/sizeof(MAIN_ITEMS[0]);

const char* CAL_ITEMS[] = {
  "Kalibrasi pH",
  "Kalibrasi EC",
  "Kalibrasi DO",
  "Kembali"
};
const int CAL_COUNT = sizeof(CAL_ITEMS)/sizeof(CAL_ITEMS[0]);

const char* EC_CAL_ITEMS[] = {
  "Larutan 1.413 mS/cm",
  "Larutan 12.88 mS/cm",
  "Kembali"
};
const int EC_CAL_COUNT = sizeof(EC_CAL_ITEMS)/sizeof(EC_CAL_ITEMS[0]);

const char* DO_CAL_ITEMS[] = {
  "Nol Oksigen (Na2SO3)",
  "Udara (100% saturasi)",
  "Kembali"
};
const int DO_CAL_COUNT = sizeof(DO_CAL_ITEMS)/sizeof(DO_CAL_ITEMS[0]);

const char* PH_CAL_ITEMS[] = {
  "Buffer pH 7.00 (awal)",
  "Buffer pH 4.00",
  "Buffer pH 10.00",
  "Kembali"
};
const int PH_CAL_COUNT = sizeof(PH_CAL_ITEMS)/sizeof(PH_CAL_ITEMS[0]);

enum class CalSensor: uint8_t { PH, EC, DO };
enum class CalWizardStage: uint8_t { PREPARE, WAIT_STABLE, SENDING, RESULT };

struct CalibrationOption {
  CalibCmd cmd;
  CalSensor sensor;
  const char* title;
  const char* instruction1;
  const char* instruction2;
  const char* instruction3;
  uint16_t waitSeconds;
};

static const CalibrationOption CAL_PH_OPTIONS[] = {
  {CalibCmd::PH_700,  CalSensor::PH, "pH 7.00",  "Bilas probe dengan air DI", "Celupkan ke buffer pH 7.00",  "Gunakan 30-50 mL larutan", 60},
  {CalibCmd::PH_400,  CalSensor::PH, "pH 4.00",  "Bilas probe dengan air DI", "Celupkan ke buffer pH 4.00",  "Lakukan setelah kalibrasi pH 7", 60},
  {CalibCmd::PH_1000, CalSensor::PH, "pH 10.00", "Bilas probe dengan air DI", "Celupkan ke buffer pH 10.00", "Langkah ini bersifat opsional", 60}
};

static const CalibrationOption CAL_EC_OPTIONS[] = {
  {CalibCmd::EC_1413,  CalSensor::EC, "EC 1.413 mS/cm", "Bilas probe dengan air DI", "Celup ke larutan 1.413 mS/cm", "Gunakan 30-50 mL larutan", 120},
  {CalibCmd::EC_12880, CalSensor::EC, "EC 12.88 mS/cm", "Bilas probe dengan air DI", "Celup ke larutan 12.88 mS/cm", "Gunakan 30-50 mL larutan", 120}
};

static const CalibrationOption CAL_DO_OPTIONS[] = {
  {CalibCmd::DO_ZERO, CalSensor::DO, "DO Nol Oksigen", "Larutkan 5g Na2SO3/100mL", "Celupkan probe ke larutan", "Pastikan probe terendam", 180},
  {CalibCmd::DO_AIR,  CalSensor::DO, "DO Udara 100%", "Bilas dan keringkan probe", "Letakkan probe di udara bebas", "Jangan sentuh membran probe", 180}
};

const char* WIFI_MGR_ITEMS[] = { "Start Portal", "Back" };
const int WIFI_MGR_COUNT = sizeof(WIFI_MGR_ITEMS)/sizeof(WIFI_MGR_ITEMS[0]);

// =======================
//  DRAW HELPERS (UI)
// =======================
struct DashboardCache {
  char temp[16];
  char do_mg[16];
  char ph[16];
  char sal[16];
  char ec[16];
  char tds[16];
  char status[48];
  bool valid = false;
};

static float salinityToPpt(float salinityPpm){
  if (!isfinite(salinityPpm)) return NAN;
  return salinityPpm / 1000.0f;
}

static void formatDashboardStrings(const DisplayData& d, DashboardCache& out){
  if (isfinite(d.t_ds)) snprintf(out.temp, sizeof(out.temp), "%.2f", d.t_ds);
  else                  snprintf(out.temp, sizeof(out.temp), "--");
  if (d.do_ok)  snprintf(out.do_mg, sizeof(out.do_mg), "%.2f", d.do_mgL);
  else          snprintf(out.do_mg, sizeof(out.do_mg), "--");
  if (d.ph_ok)  snprintf(out.ph, sizeof(out.ph), "%.2f", d.ph);
  else          snprintf(out.ph, sizeof(out.ph), "--");
  if (d.ec_ok)  snprintf(out.sal, sizeof(out.sal), "%.3f", salinityToPpt(d.sal));
  else          snprintf(out.sal, sizeof(out.sal), "--");
  if (d.ec_ok)  snprintf(out.ec, sizeof(out.ec), "%.2f", d.ec);
  else          snprintf(out.ec, sizeof(out.ec), "--");
  if (d.ec_ok)  snprintf(out.tds, sizeof(out.tds), "%.0f", d.tds);
  else          snprintf(out.tds, sizeof(out.tds), "--");

  char wifi[8];
  if (d.wifiOK) snprintf(wifi, sizeof(wifi), "OK");
  else          snprintf(wifi, sizeof(wifi), "E%d", d.wifiStatus);

  if (!deviceUsesBattery()){
    snprintf(out.status, sizeof(out.status), "W:%s S:%s B:DC", wifi, d.sendStatus);
  } else if (d.bat_ok){
    char volts[8];
    snprintf(volts, sizeof(volts), "%.1f", d.bat_v);
    char* decimal = strchr(volts, '.');
    if (decimal) *decimal = ',';
    snprintf(out.status, sizeof(out.status), "W:%s S:%s B:%d%% %sV",
             wifi, d.sendStatus, static_cast<int>(d.bat_pct + 0.5f), volts);
  } else {
    snprintf(out.status, sizeof(out.status), "W:%s S:%s B:EADC", wifi, d.sendStatus);
  }
}

static void drawToastOverlay(){
  if (!toastActive()){
    return;
  }
  tft.fillRect(0, 24, 320, 18, TFT_ORANGE);
  tft.setTextColor(TFT_BLACK, TFT_ORANGE);
  tft.setTextFont(2);
  tft.drawString(toastMsg, 6, 26);
}

static void drawStatusBar(const DisplayData& d){
  tft.fillRect(0, 0, 320, 24, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);

  DashboardCache tmp{};
  formatDashboardStrings(d, tmp);
  const uint8_t font = tft.textWidth(tmp.status, 2) <= 312 ? 2 : 1;
  tft.setTextFont(font);
  tft.drawString(tmp.status, 4, font == 2 ? 4 : 8);
}

static void drawMetricBox(int x, int y, int w, int h, const char* label, const char* unit,
                          const char* value, uint16_t color){
  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.drawRect(x, y, w, h, TFT_DARKGREY);
  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(label, x + 6, y + 4);

  if (unit && unit[0]){
    tft.setTextFont(1);
    const int16_t unitW = tft.textWidth(unit, 1);
    tft.drawString(unit, x + w - unitW - 6, y + 8);
  }

  uint8_t valueFont = 4;
  const int maxTextW = w - 12;
  while (valueFont > 2 && tft.textWidth(value, valueFont) > maxTextW){
    --valueFont;
  }

  int16_t valueW = tft.textWidth(value, valueFont);
  int16_t valueX = x + 6;
  if (valueW < maxTextW){
    valueX = x + (w - valueW) / 2;
  }

  const int valueY = (valueFont >= 4) ? (y + 24) : (y + 28);
  tft.setTextFont(valueFont);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(value, valueX, valueY);
}

static void drawWiFiBusyScreen(const char* line1, const char* line2){
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("WiFi", 160, 72, 4);
  tft.setTextFont(2);
  tft.drawCentreString(line1, 160, 120, 2);
  tft.drawCentreString(line2, 160, 144, 2);
}

static void drawSplashStatic(){
  const uint16_t splashBg = tft.color565(0x19, 0x8A, 0xFF);
  tft.fillScreen(splashBg);

  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, splashBg);
  tft.drawCentreString("Aerasea", 160, 10, 4);

  int logoX = (320 - AERASEA_LOGO_WIDTH) / 2;
  int logoY = 40;
  tft.drawXBitmap(logoX, logoY, aerasea_logo_128x64_inverted_bits,
                  AERASEA_LOGO_WIDTH, AERASEA_LOGO_HEIGHT, TFT_WHITE);

  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, splashBg);
  char ln1[28]; snprintf(ln1, sizeof(ln1), "UID: %s", deviceUid());
  char ln2[20]; snprintf(ln2, sizeof(ln2), "FW : %s", FW_VERSION);
  tft.drawCentreString(ln1, 160, 115, 2);
  tft.drawCentreString(ln2, 160, 135, 2);

  tft.drawRect(40, 190, 240, 14, TFT_WHITE);
}

static void drawSplashDynamic(uint8_t pct, bool wifiOK, bool sendOK){
  const uint16_t splashBg = tft.color565(0x19, 0x8A, 0xFF);

  tft.fillRect(60, 158, 100, 16, splashBg);
  tft.fillRect(160, 158, 100, 16, splashBg);
  tft.setTextColor(TFT_WHITE, splashBg);
  tft.drawCentreString(wifiOK ? "WiFi: OK" : "WiFi: --", 110, 158, 2);
  tft.drawCentreString(sendOK ? "SEND: OK" : "SEND: --", 210, 158, 2);

  tft.fillRect(42, 192, 236, 10, splashBg);
  tft.fillRect(42, 192, map(pct, 0, 100, 0, 236), 10, TFT_WHITE);
}

static void drawDashboardFrame(const DisplayData& d){
  tft.fillScreen(TFT_BLACK);
  drawStatusBar(d);

  const int gap = 6;
  const int colW = (320 - gap * 3) / 2;
  const int rowH = 60;
  const int y0 = 28;

  char buf[16];

  if (isfinite(d.t_ds)) snprintf(buf, sizeof(buf), "%.2f", d.t_ds);
  else                  snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap, y0, colW, rowH, "Temp", "C", buf, TFT_YELLOW);

  if (d.do_ok) snprintf(buf, sizeof(buf), "%.2f", d.do_mgL);
  else         snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap * 2 + colW, y0, colW, rowH, "DO", "mg/L", buf, TFT_CYAN);

  if (d.ph_ok) snprintf(buf, sizeof(buf), "%.2f", d.ph);
  else         snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap, y0 + rowH + gap, colW, rowH, "pH", "", buf, TFT_GREEN);

  if (d.ec_ok) snprintf(buf, sizeof(buf), "%.3f", salinityToPpt(d.sal));
  else          snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap * 2 + colW, y0 + rowH + gap, colW, rowH, "Salinity", "ppt", buf, TFT_ORANGE);

  if (d.ec_ok) snprintf(buf, sizeof(buf), "%.2f", d.ec);
  else         snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap, y0 + (rowH + gap) * 2, colW, rowH, "EC", "mS/cm", buf, TFT_BLUE);

  if (d.ec_ok) snprintf(buf, sizeof(buf), "%.0f", d.tds);
  else         snprintf(buf, sizeof(buf), "--");
  drawMetricBox(gap * 2 + colW, y0 + (rowH + gap) * 2, colW, rowH, "TDS", "mg/L", buf, TFT_MAGENTA);

  drawToastOverlay();
}

static void drawDashboardUpdate(const DisplayData& d, DashboardCache& cache, bool force){
  DashboardCache now{};
  formatDashboardStrings(d, now);

  if (force || !cache.valid || strncmp(cache.status, now.status, sizeof(now.status)) != 0){
    drawStatusBar(d);
  }

  const int gap = 6;
  const int colW = (320 - gap * 3) / 2;
  const int rowH = 60;
  const int y0 = 28;

  if (force || !cache.valid || strncmp(cache.temp, now.temp, sizeof(now.temp)) != 0){
    drawMetricBox(gap, y0, colW, rowH, "Temp", "C", now.temp, TFT_YELLOW);
  }
  if (force || !cache.valid || strncmp(cache.do_mg, now.do_mg, sizeof(now.do_mg)) != 0){
    drawMetricBox(gap * 2 + colW, y0, colW, rowH, "DO", "mg/L", now.do_mg, TFT_CYAN);
  }
  if (force || !cache.valid || strncmp(cache.ph, now.ph, sizeof(now.ph)) != 0){
    drawMetricBox(gap, y0 + rowH + gap, colW, rowH, "pH", "", now.ph, TFT_GREEN);
  }
  if (force || !cache.valid || strncmp(cache.sal, now.sal, sizeof(now.sal)) != 0){
    drawMetricBox(gap * 2 + colW, y0 + rowH + gap, colW, rowH, "Salinity", "ppt", now.sal, TFT_ORANGE);
  }
  if (force || !cache.valid || strncmp(cache.ec, now.ec, sizeof(now.ec)) != 0){
    drawMetricBox(gap, y0 + (rowH + gap) * 2, colW, rowH, "EC", "mS/cm", now.ec, TFT_BLUE);
  }
  if (force || !cache.valid || strncmp(cache.tds, now.tds, sizeof(now.tds)) != 0){
    drawMetricBox(gap * 2 + colW, y0 + (rowH + gap) * 2, colW, rowH, "TDS", "mg/L", now.tds, TFT_MAGENTA);
  }

  cache = now;
  cache.valid = true;
  drawToastOverlay();
}

void drawMenu(const char* title, const char* items[], int nItems, int cursor){
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 24, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextFont(2);
  tft.drawString(title, 6, 4);

  int y = 34;
  const int itemH = 28;
  for (int i = 0; i < nItems; ++i){
    if (i == cursor){
      tft.fillRect(6, y - 4, 308, itemH, TFT_BLUE);
      tft.setTextColor(TFT_WHITE, TFT_BLUE);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.setTextFont(2);
    tft.drawString(items[i], 12, y);
    y += itemH;
  }

  drawToastOverlay();
}

static void drawMenuItem(const char* items[], int index, bool selected){
  const int itemH = 28;
  const int y = 34 + index * itemH;
  tft.fillRect(6, y - 4, 308, itemH, selected ? TFT_BLUE : TFT_BLACK);
  tft.setTextColor(TFT_WHITE, selected ? TFT_BLUE : TFT_BLACK);
  tft.setTextFont(2);
  tft.drawString(items[index], 12, y);
}

static bool calibrationLiveValue(const CalibrationOption& option, const DisplayData& d,
                                 float& value, const char*& unit){
  switch (option.sensor){
    case CalSensor::PH:
      value = d.ph;
      unit = "pH";
      return d.ph_ok && isfinite(value);
    case CalSensor::EC:
      value = d.ec;
      unit = "mS/cm";
      return d.ec_ok && isfinite(value);
    case CalSensor::DO:
      value = d.do_mgL;
      unit = "mg/L";
      return d.do_ok && isfinite(value);
  }
  return false;
}

static void drawWizardInstruction(const char* prefix, const char* text, int16_t y){
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(prefix, 8, y);

  uint8_t size = tft.textWidth(text, 2) <= 282 ? 2 : 1;
  tft.setTextFont(size);
  tft.drawString(text, 28, y + (size == 1 ? 4 : 0));
}

static void drawCalibrationWizard(const CalibrationOption& option, CalWizardStage stage,
                                  const DisplayData& d, uint32_t elapsedSeconds, bool resultOK){
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 28, TFT_NAVY);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString(option.title, 8, 6);

  if (stage == CalWizardStage::PREPARE){
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Langkah 1/3 - Persiapan", 8, 38);
    drawWizardInstruction("1.", option.instruction1, 72);
    drawWizardInstruction("2.", option.instruction2, 102);
    drawWizardInstruction("3.", option.instruction3, 132);
    tft.fillRect(0, 205, 320, 35, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawString("OK: mulai   BACK: batal", 24, 216);
    return;
  }

  if (stage == CalWizardStage::WAIT_STABLE){
    float value = NAN;
    const char* unit = "";
    const bool valid = calibrationLiveValue(option, d, value, unit);
    const uint32_t wait = option.waitSeconds;
    const uint32_t remaining = elapsedSeconds >= wait ? 0 : wait - elapsedSeconds;
    const uint32_t progressSeconds = elapsedSeconds < wait ? elapsedSeconds : wait;
    const uint16_t progress = static_cast<uint16_t>(progressSeconds * 280UL / wait);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Langkah 2/3 - Tunggu stabil", 8, 38);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Nilai sensor saat ini", 8, 70);

    char valueText[32];
    if (valid) snprintf(valueText, sizeof(valueText), "%.2f %s", value, unit);
    else       snprintf(valueText, sizeof(valueText), "Sensor belum terbaca");
    tft.setTextFont(valid ? 4 : 2);
    tft.setTextColor(valid ? TFT_CYAN : TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString(valueText, 160, 96, valid ? 4 : 2);

    tft.drawRect(20, 148, 280, 16, TFT_WHITE);
    tft.fillRect(22, 150, progress > 4 ? progress - 4 : 0, 12, TFT_GREEN);
    tft.setTextFont(2);
    char waitText[40];
    if (remaining > 0) snprintf(waitText, sizeof(waitText), "Tunggu %lu detik", static_cast<unsigned long>(remaining));
    else               snprintf(waitText, sizeof(waitText), "Nilai siap dikalibrasi");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(waitText, 160, 174, 2);

    tft.fillRect(0, 205, 320, 35, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawCentreString(remaining == 0 && valid ? "OK: simpan  BACK: batal" : "BACK: batal", 160, 216, 2);
    return;
  }

  if (stage == CalWizardStage::SENDING){
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("Menyimpan kalibrasi...", 160, 85, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("Jangan cabut sensor", 160, 120, 2);
    return;
  }

  tft.setTextColor(resultOK ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  tft.drawCentreString(resultOK ? "Kalibrasi berhasil" : "Kalibrasi gagal", 160, 72, 2);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(resultOK ? "Bilas probe sebelum digunakan" : "Periksa sensor dan coba lagi", 160, 125, 2);
  tft.fillRect(0, 205, 320, 35, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawCentreString("OK/BACK: kembali", 160, 216, 2);
}

// =======================
//  LED INDICATOR (RGB)
// =======================
struct LedRGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static inline void ledWrite(const LedRGB& c){
  digitalWrite(LED_R_PIN, c.r ? HIGH : LOW);
  digitalWrite(LED_G_PIN, c.g ? HIGH : LOW);
  digitalWrite(LED_B_PIN, c.b ? HIGH : LOW);
}

static void ledInit(){
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  ledWrite({0,0,0});
}

static void setBacklight(bool on){
  gBacklightOn = on;
#if TFT_BACKLIGHT_ON == HIGH
  digitalWrite(TFT_BL, on ? HIGH : LOW);
#else
  digitalWrite(TFT_BL, on ? LOW : HIGH);
#endif
}

static void noteInteraction(){
  gLastInteractionMs = millis();
  if (!gBacklightOn){
    setBacklight(true);
  }
}

static void initDisplay(){
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);
  digitalWrite(TFT_DC, HIGH);
  pinMode(TFT_RST, OUTPUT);

  // Beberapa modul ILI9341 di ESP32-S3 lebih stabil jika di-hard-reset
  // dulu dan dijalankan dengan clock SPI yang lebih konservatif.
  digitalWrite(TFT_RST, HIGH);
  delay(5);
  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
  tft.begin(TFT_SPI_FREQ);
  delay(20);
  tft.setRotation(TFT_ROTATION);
  tft.invertDisplay(false);
  tft.setTextWrap(false);
  tft.fillScreen(TFT_BLACK);
}

// =======================
//  TASK INPUT (4 BUTTON)
// =======================
void TaskInput(void* param){
  auto* args = static_cast<TaskInputArgs*>(param);
  if (!args || !args->qInput){
    vTaskDelete(NULL);
    return;
  }

  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_OK,    INPUT_PULLUP);
  pinMode(BTN_BACK,  INPUT_PULLUP);

  for(;;){
    // Navigasi: UP / DOWN → ENC_DELTA (-1 / +1)
    if (debounceRead(BTN_UP)){
      noteInteraction();
      InputEvent e{ENC_DELTA, -1};
      xQueueSend(args->qInput, &e, 0);
    }
    if (debounceRead(BTN_DOWN)){
      noteInteraction();
      InputEvent e{ENC_DELTA, +1};
      xQueueSend(args->qInput, &e, 0);
    }

    // Aksi: OK & BACK → BTN_PRESS
    if (debounceRead(BTN_OK)){
      noteInteraction();
      InputEvent e{BTN_PRESS, BTN_OK};
      xQueueSend(args->qInput, &e, 0);
    }
    if (debounceRead(BTN_BACK)){
      noteInteraction();
      InputEvent e{BTN_PRESS, BTN_BACK};
      xQueueSend(args->qInput, &e, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =======================
//  TASK UI
// =======================
void TaskUI(void* param){
  auto* args = static_cast<TaskUIArgs*>(param);
  if (!args || !args->qInput || !args->qDisplay || !args->flags ||
      !args->qCalib || !args->qCalibResult){
    vTaskDelete(NULL);
    return;
  }

  DisplayData disp = {};
  DashboardCache dashCache{};
  uint8_t splashPct = 0;
  uint32_t tSplash = millis();
  bool splashDrawn = false;
  bool wasInSplash = true;
  UIState ui = UIState::DASHBOARD;
  UIState lastUi = UIState::DASHBOARD;
  bool lastWifiOK = false;
  bool lastSendOK = false;
  bool lastWiFiBusy = false;
  bool lastToastActive = false;
  char lastToastMsg[sizeof(toastMsg)] = {0};
  char lastSendStatus[16] = {0};
  int lastMenuCursor = menuCursor;
  int lastCalCursor = calCursor;
  int lastEcCalCursor = ecCalCursor;
  int lastDoCalCursor = doCalCursor;
  int lastPhCalCursor = phCalCursor;
  const CalibrationOption* activeCalibration = nullptr;
  UIState calibrationParent = UIState::CALIB;
  CalWizardStage wizardStage = CalWizardStage::PREPARE;
  uint32_t wizardStartedMs = 0;
  uint32_t lastWizardSecond = UINT32_MAX;
  bool wizardResultOK = false;
  uint8_t modbusFailStreak = 0;
  uint8_t sendErrStreak = 0;
  uint32_t ledFlashUntil = 0;
  LedRGB ledFlashColor{0,0,0};

  for(;;){
    bool gotInput = false;
    bool gotDisplay = false;
    bool gotCalibResult = false;
    bool forceRedraw = false;
    uint32_t idleMs = millis() - gLastInteractionMs;
    if (gBacklightOn && idleMs >= BACKLIGHT_TIMEOUT_MS){
      setBacklight(false);
    }

    InputEvent ev;
    while (xQueueReceive(args->qInput, &ev, 0) == pdTRUE){
      gotInput = true;
      if (ev.type == ENC_DELTA){
        int dir = (ev.value > 0) ? 1 : -1;
        switch (ui){
          case UIState::MENU:
            menuCursor = constrain(menuCursor + dir, 0, MAIN_COUNT-1);
            break;
          case UIState::CALIB:
            calCursor = constrain(calCursor + dir, 0, CAL_COUNT-1);
            break;
          case UIState::CAL_EC:
            ecCalCursor = constrain(ecCalCursor + dir, 0, EC_CAL_COUNT-1);
            break;
          case UIState::CAL_DO:
            doCalCursor = constrain(doCalCursor + dir, 0, DO_CAL_COUNT-1);
            break;
          case UIState::CAL_PH:
            phCalCursor = constrain(phCalCursor + dir, 0, PH_CAL_COUNT-1);
            break;
          default:
            break;
        }
      } else if (ev.type == BTN_PRESS){
        if (ev.value == BTN_BACK){
          if (ui == UIState::CAL_WIZARD){
            if (wizardStage != CalWizardStage::SENDING) ui = calibrationParent;
          } else if (ui == UIState::CAL_EC || ui == UIState::CAL_DO || ui == UIState::CAL_PH){
            ui = UIState::CALIB;
          } else if (ui == UIState::CALIB || ui == UIState::WIFI_MGR){
            ui = UIState::MENU;
          } else if (ui == UIState::MENU){
            ui = UIState::DASHBOARD;
          }
          continue;
        }

        if (ui == UIState::DASHBOARD){
          if (ev.value == BTN_OK){
            ui = UIState::MENU;
            showToast("Menu");
          }
        } else if (ui == UIState::MENU){
          if (ev.value == BTN_OK){
            switch (menuCursor){
              case 0: ui = UIState::DASHBOARD;   break;
              case 1: ui = UIState::WIFI_MGR;    break;
              case 2: ui = UIState::CALIB;       break;
            }
          }
        } else if (ui == UIState::WIFI_MGR){
          if (ev.value == BTN_OK){
            xEventGroupSetBits(args->flags, EG_PORTAL_ON);
            showToast("WiFi Portal");
            ui = UIState::DASHBOARD;
          }
        } else if (ui == UIState::CALIB){
          if (ev.value == BTN_OK){
            switch (calCursor){
              case 0: ui = UIState::CAL_PH; break;
              case 1: ui = UIState::CAL_EC; break;
              case 2: ui = UIState::CAL_DO; break;
              case 3: ui = UIState::MENU;   break;
            }
          }
        } else if (ui == UIState::CAL_EC){
          if (ev.value == BTN_OK){
            if (ecCalCursor < 2){
              activeCalibration = &CAL_EC_OPTIONS[ecCalCursor];
              calibrationParent = UIState::CAL_EC;
              wizardStage = CalWizardStage::PREPARE;
              ui = UIState::CAL_WIZARD;
            } else {
              ui = UIState::CALIB;
            }
          }
        } else if (ui == UIState::CAL_DO){
          if (ev.value == BTN_OK){
            if (doCalCursor < 2){
              activeCalibration = &CAL_DO_OPTIONS[doCalCursor];
              calibrationParent = UIState::CAL_DO;
              wizardStage = CalWizardStage::PREPARE;
              ui = UIState::CAL_WIZARD;
            } else {
              ui = UIState::CALIB;
            }
          }
        } else if (ui == UIState::CAL_PH){
          if (ev.value == BTN_OK){
            if (phCalCursor < 3){
              activeCalibration = &CAL_PH_OPTIONS[phCalCursor];
              calibrationParent = UIState::CAL_PH;
              wizardStage = CalWizardStage::PREPARE;
              ui = UIState::CAL_WIZARD;
            } else {
              ui = UIState::CALIB;
            }
          }
        } else if (ui == UIState::CAL_WIZARD && activeCalibration && ev.value == BTN_OK){
          if (wizardStage == CalWizardStage::PREPARE){
            wizardStage = CalWizardStage::WAIT_STABLE;
            wizardStartedMs = millis();
            lastWizardSecond = UINT32_MAX;
          } else if (wizardStage == CalWizardStage::WAIT_STABLE){
            const uint32_t elapsed = (millis() - wizardStartedMs) / 1000;
            float liveValue = NAN;
            const char* liveUnit = "";
            if (elapsed >= activeCalibration->waitSeconds &&
                calibrationLiveValue(*activeCalibration, disp, liveValue, liveUnit)){
              CalibMsg m{activeCalibration->cmd};
              if (xQueueSend(args->qCalib, &m, 0) == pdTRUE){
                wizardStage = CalWizardStage::SENDING;
              }
            }
          } else if (wizardStage == CalWizardStage::RESULT){
            ui = calibrationParent;
          }
        }
      }
    }

    DisplayData tmp;
    if (xQueueReceive(args->qDisplay, &tmp, 0) == pdTRUE){
      disp = tmp;
      gotDisplay = true;

      bool sensorsOK = (!PH_SENSOR_ENABLED || disp.ph_ok) &&
                       (!EC_SENSOR_ENABLED || disp.ec_ok) &&
                       (!NH4_SENSOR_ENABLED || disp.nh4_ok) &&
                       (!DO_SENSOR_ENABLED || disp.do_ok);
      if (sensorsOK){
        modbusFailStreak = 0;
      } else if (modbusFailStreak < 255){
        modbusFailStreak++;
      }

      bool sendErr = (disp.sendStatus[0] == 'E');
      if (sendErr){
        if (sendErrStreak < 255) sendErrStreak++;
      } else {
        sendErrStreak = 0;
      }

      if (strncmp(disp.sendStatus, "OK", 2) == 0 && strncmp(lastSendStatus, "OK", 2) != 0){
        ledFlashUntil = millis() + 300;
        ledFlashColor = {0, 192, 192}; // cyan flash
      }
      strncpy(lastSendStatus, disp.sendStatus, sizeof(lastSendStatus)-1);
      lastSendStatus[sizeof(lastSendStatus)-1] = '\0';
    }

    CalibResult calibResult{};
    if (xQueueReceive(args->qCalibResult, &calibResult, 0) == pdTRUE){
      if (activeCalibration && calibResult.cmd == activeCalibration->cmd){
        wizardResultOK = calibResult.ok;
        wizardStage = CalWizardStage::RESULT;
        gotCalibResult = true;
      }
    }

    EventBits_t flags = xEventGroupGetBits(args->flags);
    bool wifiOK = (flags & EG_WIFI_OK);
    bool sendOK = (strncmp(disp.sendStatus, "OK", 2) == 0);
    bool wifiBusy = (flags & EG_WIFI_BUSY);
    bool toastNow = toastActive();
    bool toastChanged = (toastNow != lastToastActive) ||
                        (strncmp(toastMsg, lastToastMsg, sizeof(toastMsg)) != 0);

    if (toastChanged){
      if (strncmp(toastMsg, "Cal OK", 6) == 0){
        ledFlashUntil = millis() + 350;
        ledFlashColor = {192, 0, 192}; // magenta flash
      } else if (strncmp(toastMsg, "Cal Fail", 8) == 0){
        ledFlashUntil = millis() + 400;
        ledFlashColor = {192, 0, 0}; // red flash
      }
    }

    if (!toastNow && toastChanged){
      forceRedraw = true;
    }

    uint32_t now = millis();
    bool inSplash = (now - tSplash < 2000);
    if (wasInSplash && !inSplash){
      forceRedraw = true;
      dashCache.valid = false;
      splashDrawn = false;
    }
    bool wizardTick = false;
    uint32_t wizardElapsed = 0;
    if (ui == UIState::CAL_WIZARD && activeCalibration && wizardStage == CalWizardStage::WAIT_STABLE){
      wizardElapsed = (now - wizardStartedMs) / 1000;
      if (wizardElapsed != lastWizardSecond){
        lastWizardSecond = wizardElapsed;
        wizardTick = true;
      }
    }
    const bool displayNeedsDraw = gotDisplay && ui != UIState::CAL_WIZARD;
    bool needDraw = forceRedraw || gotInput || displayNeedsDraw || gotCalibResult || toastChanged ||
                    (wifiOK != lastWifiOK) || (sendOK != lastSendOK) ||
                    (wifiBusy != lastWiFiBusy) || wizardTick;

    if (inSplash){
      if (splashPct < 100 && now - tSplash > splashPct*10){
        splashPct++;
        needDraw = true;
      }
      if (!splashDrawn){
        drawSplashStatic();
        splashDrawn = true;
        needDraw = true;
      }
      if (needDraw){
        drawSplashDynamic(splashPct, wifiOK, sendOK);
      }
    } else {
      if (wifiBusy){
        if (needDraw){
          drawWiFiBusyScreen("Radio aktif", "Tunggu koneksi...");
        }
      } else if (needDraw){
        if (ui != lastUi){
          forceRedraw = true;
          lastUi = ui;
          dashCache.valid = false;
        }

        switch (ui){
          case UIState::DASHBOARD:
            if (forceRedraw){
              drawDashboardFrame(disp);
              dashCache.valid = false;
            }
            drawDashboardUpdate(disp, dashCache, forceRedraw);
            break;
          case UIState::MENU:
            if (forceRedraw){
              drawMenu("Main Menu", MAIN_ITEMS, MAIN_COUNT, menuCursor);
              lastMenuCursor = menuCursor;
            } else if (menuCursor != lastMenuCursor){
              drawMenuItem(MAIN_ITEMS, lastMenuCursor, false);
              drawMenuItem(MAIN_ITEMS, menuCursor, true);
              lastMenuCursor = menuCursor;
              drawToastOverlay();
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
          case UIState::CALIB:
            if (forceRedraw){
              drawMenu("Kalibrasi Sensor", CAL_ITEMS, CAL_COUNT, calCursor);
              lastCalCursor = calCursor;
            } else if (calCursor != lastCalCursor){
              drawMenuItem(CAL_ITEMS, lastCalCursor, false);
              drawMenuItem(CAL_ITEMS, calCursor, true);
              lastCalCursor = calCursor;
              drawToastOverlay();
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
          case UIState::CAL_EC:
            if (forceRedraw){
              drawMenu("Cal EC (Auto)", EC_CAL_ITEMS, EC_CAL_COUNT, ecCalCursor);
              lastEcCalCursor = ecCalCursor;
            } else if (ecCalCursor != lastEcCalCursor){
              drawMenuItem(EC_CAL_ITEMS, lastEcCalCursor, false);
              drawMenuItem(EC_CAL_ITEMS, ecCalCursor, true);
              lastEcCalCursor = ecCalCursor;
              drawToastOverlay();
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
          case UIState::CAL_DO:
            if (forceRedraw){
              drawMenu("Cal DO", DO_CAL_ITEMS, DO_CAL_COUNT, doCalCursor);
              lastDoCalCursor = doCalCursor;
            } else if (doCalCursor != lastDoCalCursor){
              drawMenuItem(DO_CAL_ITEMS, lastDoCalCursor, false);
              drawMenuItem(DO_CAL_ITEMS, doCalCursor, true);
              lastDoCalCursor = doCalCursor;
              drawToastOverlay();
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
          case UIState::CAL_PH:
            if (forceRedraw){
              drawMenu("Cal pH (S-PH-01)", PH_CAL_ITEMS, PH_CAL_COUNT, phCalCursor);
              lastPhCalCursor = phCalCursor;
            } else if (phCalCursor != lastPhCalCursor){
              drawMenuItem(PH_CAL_ITEMS, lastPhCalCursor, false);
              drawMenuItem(PH_CAL_ITEMS, phCalCursor, true);
              lastPhCalCursor = phCalCursor;
              drawToastOverlay();
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
          case UIState::CAL_WIZARD:
            if (activeCalibration){
              drawCalibrationWizard(*activeCalibration, wizardStage, disp,
                                    wizardElapsed, wizardResultOK);
            }
            break;
          case UIState::WIFI_MGR:
            if (forceRedraw){
              drawMenu("WiFi Manager", WIFI_MGR_ITEMS, WIFI_MGR_COUNT, 0);
            } else if (toastChanged) {
              drawToastOverlay();
            }
            break;
        }
      }
    }

    if (!toastNow){
      clearToast();
    }

    if (needDraw){
      lastWifiOK = wifiOK;
      lastSendOK = sendOK;
      lastWiFiBusy = wifiBusy;
      lastToastActive = toastNow;
      strncpy(lastToastMsg, toastMsg, sizeof(lastToastMsg)-1);
      lastToastMsg[sizeof(lastToastMsg)-1] = '\0';
    }
    wasInSplash = inSplash;

    // ===== LED indicator =====
    const uint32_t nowLed = millis();
    if (wifiBusy){
      ledWrite({0,0,0});
    } else if (ledFlashUntil > nowLed){
      ledWrite(ledFlashColor);
    } else {
      const bool criticalError = (modbusFailStreak >= 3) || (sendErrStreak >= 2);
      if (criticalError){
        const bool on = ((nowLed / 200) % 2) == 0;
        ledWrite(on ? LedRGB{192,0,0} : LedRGB{0,0,0}); // fast red blink
      } else if (!wifiOK){
        const bool on = ((nowLed / 1000) % 2) == 0;
        ledWrite(on ? LedRGB{0,0,192} : LedRGB{0,0,0}); // slow blue blink
      } else {
        ledWrite({0,192,0}); // solid green
      }
    }

    gCalibrationActive = (ui == UIState::CAL_WIZARD);
    vTaskDelay(pdMS_TO_TICKS(wifiBusy ? 150 : 50));
  }
}

static void otaProgress(uint8_t progress, void* context) {
  const auto* command = static_cast<const OtaCommand*>(context);
  char displayStatus[16];
  snprintf(displayStatus, sizeof(displayStatus), "OTA%u", progress);
  setSendStatus(gStatus, displayStatus);
  if (command) queueOtaStatus(command->assignmentId, command->version, "downloading", progress);
}

// =======================
// TaskOTA (Core 0)
// =======================
void TaskOTA(void* param) {
  auto* args = static_cast<TaskOtaArgs*>(param);
  if (!args || !args->qOtaCommand || !args->qOtaStatus || !args->flags) {
    vTaskDelete(NULL);
    return;
  }

  OtaPendingReport bootReport{};
  if (otaLoadPendingReport(bootReport)) {
    if (otaRunningImagePendingVerify()) {
      queueOtaStatus(bootReport.assignmentId, bootReport.targetVersion, "pending_verify");
      vTaskDelay(pdMS_TO_TICKS(30000));
      if (otaMarkRunningImageValid()) {
        queueOtaStatus(bootReport.assignmentId, bootReport.targetVersion, "success", 100, nullptr, true);
      } else {
        queueOtaStatus(bootReport.assignmentId, bootReport.targetVersion, "failed", -1, "SELF_TEST_FAILED");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
      }
    } else if (strcmp(FW_VERSION, bootReport.targetVersion) == 0) {
      queueOtaStatus(bootReport.assignmentId, bootReport.targetVersion, "success", 100, nullptr, true);
    } else {
      queueOtaStatus(bootReport.assignmentId, bootReport.targetVersion, "rolled_back", -1, "ROLLED_BACK", true);
    }
  }

  OtaCommand command{};
  bool hasCommand = false;
  uint8_t attempts = 0;
  uint32_t nextAttemptAt = 0;
  const uint32_t retryDelayMs[] = {60000, 300000, 1800000};

  for (;;) {
    OtaCommand incoming{};
    if (xQueueReceive(args->qOtaCommand, &incoming, pdMS_TO_TICKS(1000)) == pdTRUE) {
      command = incoming;
      hasCommand = true;
      attempts = 0;
      nextAttemptAt = 0;
    }
    if (!hasCommand || (nextAttemptAt != 0 && static_cast<int32_t>(millis() - nextAttemptAt) < 0)) continue;

    if (strcmp(command.version, FW_VERSION) == 0) {
      queueOtaStatus(command.assignmentId, command.version, "success", 100, nullptr, true);
      hasCommand = false;
      continue;
    }

    const char* deferredReason = nullptr;
    if (WiFi.status() != WL_CONNECTED) deferredReason = "WIFI_OFFLINE";
    else if (WiFi.RSSI() < -75) deferredReason = "WEAK_WIFI";
    else if (deviceUsesBattery() && (!gBatteryValid || gBatteryPercent < 50.0f)) deferredReason = "LOW_BATTERY";
    else if (gCalibrationActive) deferredReason = "CALIBRATION_ACTIVE";

    if (deferredReason) {
      queueOtaStatus(command.assignmentId, command.version, "deferred", -1, deferredReason);
      nextAttemptAt = millis() + 60000;
      continue;
    }

    if (!otaSavePendingReport(command)) {
      queueOtaStatus(command.assignmentId, command.version, "failed", -1, "NVS_FAILED");
      hasCommand = false;
      continue;
    }

    xEventGroupSetBits(args->flags, EG_OTA_ACTIVE);
    queueOtaStatus(command.assignmentId, command.version, "downloading", 0);
    char errorCode[40] = {0};
    const bool ok = otaPerformUpdate(command, otaProgress, &command, errorCode, sizeof(errorCode));
    xEventGroupClearBits(args->flags, EG_OTA_ACTIVE);

    if (ok) {
      setSendStatus(gStatus, "OTA OK");
      queueOtaStatus(command.assignmentId, command.version, "downloaded", 100);
      queueOtaStatus(command.assignmentId, command.version, "rebooting", 100);
      vTaskDelay(pdMS_TO_TICKS(1500));
      ESP.restart();
    }

    otaClearPendingReport();
    setSendStatus(gStatus, "OTA ERR");
    ++attempts;
    if (attempts >= 3) {
      queueOtaStatus(command.assignmentId, command.version, "failed", -1, errorCode);
      hasCommand = false;
    } else {
      queueOtaStatus(command.assignmentId, command.version, "deferred", -1, errorCode);
      nextAttemptAt = millis() + retryDelayMs[attempts - 1];
    }
  }
}

// =======================
// TaskSensors (Core 0)
// =======================
void TaskSensors(void* param){
  auto* args = static_cast<TaskSensorArgs*>(param);
  if (!args || !args->qDisplay || !args->qTelemetry || !args->qCalib ||
      !args->qCalibResult || !args->rs485 || !args->status || !args->flags){
    vTaskDelete(NULL);
    return;
  }

  pinMode(RS485_DE_RE_PIN, OUTPUT); rs485Receive();
  RS485.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  auto bindNode = [](ModbusMaster& mb, uint8_t id){
    mb.begin(id, RS485);
    mb.preTransmission(rs485Transmit);
    mb.postTransmission(rs485Receive);
    mb.idle(modbusIdle);
  };

  // Node sensor modular
  bindNode(mb_ph,  PH_SLAVE_ID);
  bindNode(mb_ec,  EC_SLAVE_ID);
  bindNode(mb_nh4, NH4_SLAVE_ID);
  bindNode(mb_do,  DO_SLAVE_ID);

  DisplayData cur = {};
  cur.t_ds = NAN;
  cur.phT = NAN;
  cur.ecT = NAN;
  cur.nh4T = NAN;
  cur.do_tC = NAN;
  uint8_t modularPhase = 0;
  const TickType_t rsLockTimeout = pdMS_TO_TICKS(100);

  for(;;){
    EventBits_t loopFlags = xEventGroupGetBits(args->flags);
    bool wifiBusy = (loopFlags & EG_WIFI_BUSY);

    const bool otaActive = (loopFlags & EG_OTA_ACTIVE);

    // 1) Proses perintah kalibrasi dari wizard UI.
    CalibMsg msg;
    while (!otaActive && xQueueReceive(args->qCalib, &msg, 0) == pdTRUE){
      CalibResult result{msg.cmd, false};
      if (xSemaphoreTake(args->rs485, rsLockTimeout) == pdTRUE){
        result.ok = handleCalibration(msg.cmd);
        rs485Receive();
        xSemaphoreGive(args->rs485);
      }
      xQueueOverwrite(args->qCalibResult, &result);
    }

    // 2) Poll sensor modular satu per satu agar bus ringan dan watchdog aman
    if (!otaActive && xSemaphoreTake(args->rs485, rsLockTimeout) == pdTRUE){
      readOneModularSensor(cur, modularPhase);
      resolveWaterTemperature(cur);
      rs485Receive();
      xSemaphoreGive(args->rs485);
    }

    float vbat = 0.0f;
    float batPct = 0.0f;
    cur.bat_ok = deviceUsesBattery() && readBattery(vbat, batPct);
    gBatteryValid = cur.bat_ok;
    gBatteryPercent = cur.bat_ok ? batPct : 0.0f;
    if (cur.bat_ok){
      cur.bat_v = vbat;
      cur.bat_pct = batPct;
    } else {
      cur.bat_v = 0.0f;
      cur.bat_pct = 0.0f;
    }

    // 4) Tambahan: status WiFi dan pengiriman MQTT
    const wl_status_t wifiStatus = WiFi.status();
    cur.wifiOK = (wifiStatus == WL_CONNECTED);
    cur.wifiStatus = static_cast<int8_t>(wifiStatus);
    strncpy(cur.sendStatus, args->status->sendStatus, sizeof(cur.sendStatus)-1);
    cur.sendStatus[sizeof(cur.sendStatus)-1] = '\0';
    // 5) Kirim snapshot ke UI & MQTT
    xQueueOverwrite(args->qDisplay, &cur);
    xQueueOverwrite(args->qTelemetry, &cur);

    // 6) Delay sampling
    vTaskDelay(pdMS_TO_TICKS(wifiBusy ? 1000 : 250));   // 4 Hz normal, diperlambat saat WiFi aktif
  }
}

// =======================
// TaskMQTT (Core 0, low)
// =======================
void TaskMQTT(void* param){
  auto* args = static_cast<TaskMqttArgs*>(param);
  if (!args || !args->qTelemetry || !args->qOtaCommand || !args->qOtaStatus ||
      !args->status || !args->flags){
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[BOOT] TaskMQTT start");
  seedClockFromBuild();
  gBootNonce = esp_random();
  gMqtt.begin(gMqttSocket);
  gMqtt.setOptions(45, true, MQTT_TIMEOUT_MS);
  WiFi.mode(WIFI_OFF);

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setConfigPortalBlocking(false);

  uint32_t lastPublish = 0;
  uint32_t lastWiFiAttempt = 0;
  uint32_t bootMs = millis();
  DisplayData snap{};
  bool wifiWasConnected = false;
  String pendingPayload;
  OtaStatusMessage pendingOtaStatus{};
  bool hasPendingOtaStatus = false;

  for(;;){
    EventBits_t f = xEventGroupGetBits(args->flags);

    // ====== WiFi Manager Portal (dipanggil dari menu) ======
    if (f & EG_PORTAL_ON){
      xEventGroupClearBits(args->flags, EG_PORTAL_ON);

      gMqtt.disconnect();
      gMqttSocket.disconnect();
      WiFi.disconnect(true, true);
      vTaskDelay(pdMS_TO_TICKS(200));

      wm.startConfigPortal("Aerasea-Setup");
      uint32_t portalStart = millis();
      while (wm.getConfigPortalActive()){
        wm.process();
        vTaskDelay(pdMS_TO_TICKS(25));
        if (millis() - portalStart > 300000){
          break;
        }
      }

      bool ok = (WiFi.status() == WL_CONNECTED);
      wifiWasConnected = ok;
      if (ok){
        xEventGroupSetBits(args->flags, EG_WIFI_OK);
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        showToast("WiFi OK");
      } else {
        xEventGroupClearBits(args->flags, EG_WIFI_OK);
        showToast("WiFi Fail");
      }

      WiFi.mode(WIFI_STA);
    }

    // ====== Auto reconnect yang lebih ringan ======
    bool wifiNow = (WiFi.status() == WL_CONNECTED);
    uint32_t now = millis();
    if (!wifiNow && (now - bootMs) >= 15000 && (now - lastWiFiAttempt) >= 30000){
      lastWiFiAttempt = now;
      Serial.println("[BOOT] WiFi connect attempt");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(true);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      WiFi.begin();   // pakai kredensial terakhir yang tersimpan
    }

    // ====== Monitor status WiFi biasa (tanpa portal) ======
    wifiNow = (WiFi.status() == WL_CONNECTED);

    if (wifiNow && !wifiWasConnected){
      // Baru saja connect
      wifiWasConnected = true;
      xEventGroupSetBits(args->flags, EG_WIFI_OK);
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    } else if (!wifiNow && wifiWasConnected){
      // Baru saja putus
      wifiWasConnected = false;
      xEventGroupClearBits(args->flags, EG_WIFI_OK);
      gMqtt.disconnect();
      gMqttSocket.disconnect();
    }

    EventBits_t flags = xEventGroupGetBits(args->flags);
    if (gMqtt.isConnected() && !gMqtt.update()){
      setSendStatus(*args->status, "ECON");
    }

    if (!hasPendingOtaStatus && xQueueReceive(args->qOtaStatus, &pendingOtaStatus, 0) == pdTRUE) {
      hasPendingOtaStatus = true;
    }
    if (hasPendingOtaStatus && wifiNow && connectMqtt(*args->status)) {
      const String topic = String("devices/") + deviceUid() + "/ota/status";
      if (gMqtt.publish(topic, buildOtaStatusPayload(pendingOtaStatus), false, 1)) {
        ++gOtaMessageSequence;
        if (pendingOtaStatus.clearPending) otaClearPendingReport();
        hasPendingOtaStatus = false;
      }
    }

    // ====== Kirim data ke broker ======
    if (!(flags & EG_OTA_ACTIVE) && millis() - lastPublish >= PUBLISH_INTERVAL_MS){
      lastPublish = millis();
      if (xQueueReceive(args->qTelemetry, &snap, 0)==pdTRUE){

        // WiFi belum OK: snapshot terbaru akan diambil pada interval berikutnya.
        if (!(flags & EG_WIFI_OK)){
          setSendStatus(*args->status, "--");
        } else {
          publishData(snap, *args->status, pendingPayload);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =======================
// SETUP & LOOP
// =======================
void setup(){
  Serial.begin(115200);
  delay(100);
  Serial.println("[BOOT] setup begin");
  const bool configured = deviceConfigBegin();
  Serial.printf("[CONFIG] %s (%s)\n", configured ? deviceUid() : "UNPROVISIONED",
                deviceUsesBattery() ? "portable" : "panel");

  ledInit();
  Serial.println("[BOOT] LED ready");
  initDisplay();
  Serial.println("[BOOT] TFT ready");
  pinMode(TFT_BL, OUTPUT);
  gLastInteractionMs = millis();
  setBacklight(true);

  if (deviceUsesBattery()){
    analogReadResolution(12);
    analogRead(BAT_ADC_PIN); // Arduino core memetakan channel ADC saat pembacaan pertama.
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    Serial.println("[BOOT] ADC ready");
  } else {
    Serial.println("[BOOT] Power profile: panel DC");
  }

  qInput     = xQueueCreate(16, sizeof(InputEvent));
  qDisplay   = xQueueCreate(1,  sizeof(DisplayData));
  qTelemetry = xQueueCreate(1,  sizeof(DisplayData));
  qCalib     = xQueueCreate(4,  sizeof(CalibMsg));
  qCalibResult = xQueueCreate(1, sizeof(CalibResult));
  qOtaCommand = xQueueCreate(1, sizeof(OtaCommand));
  qOtaStatus = xQueueCreate(12, sizeof(OtaStatusMessage));
  mRS485     = xSemaphoreCreateMutex();
  egFlags    = xEventGroupCreate();
  Serial.println("[BOOT] RTOS objects ready");

  gInputArgs.qInput = qInput;

  gUIArgs.qInput   = qInput;
  gUIArgs.qDisplay = qDisplay;
  gUIArgs.qCalib   = qCalib;
  gUIArgs.qCalibResult = qCalibResult;
  gUIArgs.flags    = egFlags;
  gSensorArgs.qDisplay   = qDisplay;
  gSensorArgs.qTelemetry = qTelemetry;
  gSensorArgs.qCalib     = qCalib;
  gSensorArgs.qCalibResult = qCalibResult;
  gSensorArgs.rs485      = mRS485;
  gSensorArgs.flags      = egFlags;
  gSensorArgs.status     = &gStatus;

  gMqttArgs.qTelemetry = qTelemetry;
  gMqttArgs.qOtaCommand = qOtaCommand;
  gMqttArgs.qOtaStatus = qOtaStatus;
  gMqttArgs.flags      = egFlags;
  gMqttArgs.status     = &gStatus;

  gOtaArgs.qOtaCommand = qOtaCommand;
  gOtaArgs.qOtaStatus = qOtaStatus;
  gOtaArgs.flags = egFlags;

  xTaskCreatePinnedToCore(TaskInput,   "TaskInput",   4096,  &gInputArgs,   5, NULL, 1);
  xTaskCreatePinnedToCore(TaskUI,      "TaskUI",      10240, &gUIArgs,      4, NULL, 1);
  xTaskCreatePinnedToCore(TaskSensors, "TaskSensors", 8192,  &gSensorArgs,  1, NULL, 0);
  xTaskCreatePinnedToCore(TaskMQTT,    "TaskMQTT",    8192,  &gMqttArgs,    1, NULL, 0);
  xTaskCreatePinnedToCore(TaskOTA,     "TaskOTA",     12288, &gOtaArgs,     2, NULL, 0);
  Serial.println("[BOOT] tasks started");
}

void loop(){
  deviceProvisioningPoll(Serial);
  vTaskDelay(pdMS_TO_TICKS(20));
}
