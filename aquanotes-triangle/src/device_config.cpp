#include "device_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>

#ifndef AQUANOTES_GENERIC_BUILD
#include "secrets.h"
#endif

namespace {

constexpr char NVS_NAMESPACE[] = "device_cfg";
constexpr uint32_t CONFIG_SCHEMA = 1;
constexpr uint32_t PROVISION_WINDOW_MS = 120000;
constexpr size_t MAX_LINE_LENGTH = 384;

DeviceConfig current{};
uint32_t bootMs = 0;
uint32_t challenge = 0;
bool provisioningActive = false;
char inputLine[MAX_LINE_LENGTH + 1] = {0};
size_t inputLength = 0;

bool validUid(const char* uid) {
  if (!uid) return false;
  const size_t length = strlen(uid);
  if (length < 3 || length > 36 || !isalnum(static_cast<unsigned char>(uid[0]))) return false;
  for (size_t i = 1; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(uid[i]);
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

bool validPassword(const char* password) {
  if (!password) return false;
  const size_t length = strlen(password);
  if (length < 16 || length >= sizeof(current.mqttPassword)) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(password[i]);
    if (c < 0x21 || c > 0x7e) return false;
  }
  return true;
}

bool loadRuntime(DeviceConfig& target) {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, true)) return false;
  const uint32_t schema = preferences.getUInt("schema", 0);
  const String uid = preferences.getString("uid", "");
  const String password = preferences.getString("mqtt_password", "");
  const bool panel = preferences.getBool("panel", false);
  preferences.end();
  if (schema != CONFIG_SCHEMA || !validUid(uid.c_str()) || !validPassword(password.c_str())) return false;

  strlcpy(target.uid, uid.c_str(), sizeof(target.uid));
  strlcpy(target.mqttPassword, password.c_str(), sizeof(target.mqttPassword));
  target.panel = panel;
  target.provisioned = true;
  return true;
}

bool saveRuntime(const DeviceConfig& source) {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return false;
  preferences.putUInt("schema", 0);
  const bool valuesWritten =
      preferences.putString("uid", source.uid) == strlen(source.uid) &&
      preferences.putString("mqtt_password", source.mqttPassword) == strlen(source.mqttPassword) &&
      preferences.putBool("panel", source.panel) == sizeof(bool);
  const bool committed = valuesWritten && preferences.putUInt("schema", CONFIG_SCHEMA) == sizeof(uint32_t);
  preferences.end();
  return committed;
}

bool loadCompileTimeFallback(DeviceConfig& target) {
#ifdef AQUANOTES_GENERIC_BUILD
  return false;
#else
  if (!validUid(DEVICE_UID) || !validPassword(MQTT_PASSWORD)) return false;
  strlcpy(target.uid, DEVICE_UID, sizeof(target.uid));
  strlcpy(target.mqttPassword, MQTT_PASSWORD, sizeof(target.mqttPassword));
#ifdef DEVICE_POWER_SOURCE_PANEL
  target.panel = DEVICE_POWER_SOURCE_PANEL == 1;
#else
  target.panel = false;
#endif
  target.provisioned = true;
  return true;
#endif
}

void printReady(Stream& serial) {
  const uint64_t mac = ESP.getEfuseMac();
  char macText[18];
  snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X",
           static_cast<unsigned>((mac >> 40) & 0xff), static_cast<unsigned>((mac >> 32) & 0xff),
           static_cast<unsigned>((mac >> 24) & 0xff), static_cast<unsigned>((mac >> 16) & 0xff),
           static_cast<unsigned>((mac >> 8) & 0xff), static_cast<unsigned>(mac & 0xff));
  challenge = esp_random();
  provisioningActive = true;
  serial.printf("AQPROV READY %s %08lX\n", macText, static_cast<unsigned long>(challenge));
}

void handleProvisioningJson(Stream& serial, const char* line) {
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, line);
  if (parseError) {
    serial.println("AQPROV ERROR INVALID_JSON");
    return;
  }

  const uint32_t schema = document["schema"] | 0;
  const uint32_t suppliedChallenge = document["nonce"] | 0;
  const char* uid = document["uid"] | "";
  const char* password = document["mqtt_password"] | "";
  const char* powerProfile = document["power_profile"] | "";
  if (schema != CONFIG_SCHEMA || suppliedChallenge != challenge) {
    serial.println("AQPROV ERROR INVALID_CHALLENGE");
    return;
  }
  if (!validUid(uid)) {
    serial.println("AQPROV ERROR INVALID_UID");
    return;
  }
  if (!validPassword(password)) {
    serial.println("AQPROV ERROR INVALID_PASSWORD");
    return;
  }
  if (strcmp(powerProfile, "portable") != 0 && strcmp(powerProfile, "panel") != 0) {
    serial.println("AQPROV ERROR INVALID_POWER_PROFILE");
    return;
  }

  DeviceConfig candidate{};
  strlcpy(candidate.uid, uid, sizeof(candidate.uid));
  strlcpy(candidate.mqttPassword, password, sizeof(candidate.mqttPassword));
  candidate.panel = strcmp(powerProfile, "panel") == 0;
  candidate.provisioned = true;
  if (!saveRuntime(candidate)) {
    serial.println("AQPROV ERROR NVS_WRITE_FAILED");
    return;
  }

  DeviceConfig verified{};
  if (!loadRuntime(verified) || strcmp(verified.uid, candidate.uid) != 0 ||
      strcmp(verified.mqttPassword, candidate.mqttPassword) != 0 || verified.panel != candidate.panel) {
    serial.println("AQPROV ERROR NVS_VERIFY_FAILED");
    return;
  }

  current = verified;
  provisioningActive = false;
  serial.printf("AQPROV STORED %s %s\n", current.uid, current.panel ? "panel" : "portable");
}

void handleLine(Stream& serial, const char* line) {
  if (strcmp(line, "AQUANOTES PROVISION 1") == 0) {
    if (millis() - bootMs > PROVISION_WINDOW_MS) {
      serial.println("AQPROV ERROR WINDOW_CLOSED");
      return;
    }
    printReady(serial);
    return;
  }
  if (strcmp(line, "AQUANOTES REBOOT") == 0) {
    if (!deviceIsProvisioned()) {
      serial.println("AQPROV ERROR NOT_PROVISIONED");
      return;
    }
    serial.println("AQPROV REBOOTING");
    serial.flush();
    delay(100);
    ESP.restart();
  }
  if (provisioningActive && line[0] == '{') handleProvisioningJson(serial, line);
}

}  // namespace

bool deviceConfigBegin() {
  bootMs = millis();
  if (loadRuntime(current)) return true;
  if (!loadCompileTimeFallback(current)) {
    current = {};
    strlcpy(current.uid, "UNPROVISIONED", sizeof(current.uid));
    return false;
  }
  if (!saveRuntime(current)) Serial.println("[CONFIG] fallback aktif; gagal menyimpan ke NVS");
  return true;
}

const DeviceConfig& deviceConfig() { return current; }
const char* deviceUid() { return current.uid; }
const char* deviceMqttPassword() { return current.mqttPassword; }
bool deviceUsesBattery() { return !current.panel; }
bool deviceIsProvisioned() { return current.provisioned; }

void deviceProvisioningPoll(Stream& serial) {
  while (serial.available() > 0) {
    const char c = static_cast<char>(serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      inputLine[inputLength] = '\0';
      if (inputLength > 0) handleLine(serial, inputLine);
      inputLength = 0;
      continue;
    }
    if (inputLength >= MAX_LINE_LENGTH) {
      inputLength = 0;
      provisioningActive = false;
      serial.println("AQPROV ERROR LINE_TOO_LONG");
      continue;
    }
    inputLine[inputLength++] = c;
  }
}

