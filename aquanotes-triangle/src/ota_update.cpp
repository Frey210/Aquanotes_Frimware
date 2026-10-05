#include "ota_update.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>

#include "mqtt_ca.h"
#include "ota_signing_key.h"

namespace {
constexpr uint32_t OTA_MAX_IMAGE_SIZE = 0x330000;
constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 15000;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 4096;
constexpr const char* NVS_NAMESPACE = "aquanotes_ota";

void setError(char* destination, size_t size, const char* value) {
  if (!destination || size == 0) return;
  strncpy(destination, value, size - 1);
  destination[size - 1] = '\0';
}

bool copyJsonString(JsonVariantConst value, char* destination, size_t size) {
  const char* source = value.as<const char*>();
  if (!source || strlen(source) == 0 || strlen(source) >= size) return false;
  strncpy(destination, source, size - 1);
  destination[size - 1] = '\0';
  return true;
}

bool isSha256(const char* value) {
  if (!value || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

void digestToHex(const uint8_t digest[32], char output[65]) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = HEX_DIGITS[digest[i] >> 4];
    output[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0f];
  }
  output[64] = '\0';
}

bool verifySignature(const uint8_t digest[32], const char* encodedSignature) {
  if (!encodedSignature || !encodedSignature[0]) return false;
  uint8_t signature[96];
  size_t signatureLength = 0;
  if (mbedtls_base64_decode(signature, sizeof(signature), &signatureLength,
                            reinterpret_cast<const unsigned char*>(encodedSignature),
                            strlen(encodedSignature)) != 0) {
    return false;
  }

  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(
      &key, reinterpret_cast<const unsigned char*>(OTA_SIGNING_PUBLIC_KEY),
      strlen(OTA_SIGNING_PUBLIC_KEY) + 1);
  const int verified = parsed == 0
                           ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, 32,
                                               signature, signatureLength)
                           : parsed;
  mbedtls_pk_free(&key);
  return verified == 0;
}

int compareIdentifier(const String& left, const String& right) {
  const bool leftNumeric = left.length() > 0 && left.indexOf('-') < 0 &&
                           left.toInt() >= 0 && String(left.toInt()) == left;
  const bool rightNumeric = right.length() > 0 && right.indexOf('-') < 0 &&
                            right.toInt() >= 0 && String(right.toInt()) == right;
  if (leftNumeric && rightNumeric) {
    const long a = left.toInt();
    const long b = right.toInt();
    return a == b ? 0 : (a < b ? -1 : 1);
  }
  if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
  const int compared = left.compareTo(right);
  return compared == 0 ? 0 : (compared < 0 ? -1 : 1);
}

bool splitVersion(const char* value, int numbers[3], String& prerelease) {
  if (!value) return false;
  String version(value);
  if (version.startsWith("v")) version.remove(0, 1);
  const int plus = version.indexOf('+');
  if (plus >= 0) version.remove(plus);
  const int dash = version.indexOf('-');
  if (dash >= 0) {
    prerelease = version.substring(dash + 1);
    version.remove(dash);
  } else {
    prerelease = "";
  }
  int first = version.indexOf('.');
  int second = version.indexOf('.', first + 1);
  if (first <= 0 || second <= first + 1 || version.indexOf('.', second + 1) >= 0) return false;
  const String parts[] = {
      version.substring(0, first),
      version.substring(first + 1, second),
      version.substring(second + 1),
  };
  for (size_t i = 0; i < 3; ++i) {
    if (parts[i].length() == 0) return false;
    for (size_t j = 0; j < parts[i].length(); ++j) {
      if (!isDigit(parts[i][j])) return false;
    }
    numbers[i] = parts[i].toInt();
  }
  return true;
}
}  // namespace

bool otaParseCommand(const char* payload, size_t length, OtaCommand& command,
                     char* errorCode, size_t errorCodeSize) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload, length);
  if (error) {
    setError(errorCode, errorCodeSize, "INVALID_MANIFEST");
    return false;
  }
  if (document["schema"].as<int>() != 1) {
    setError(errorCode, errorCodeSize, "UNSUPPORTED_MANIFEST");
    return false;
  }

  command.assignmentId = document["assignment_id"].as<uint32_t>();
  command.releaseId = document["release_id"].as<uint32_t>();
  command.size = document["size"].as<uint32_t>();
  command.mandatory = document["mandatory"] | false;
  command.rollback = document["rollback"] | false;
  if (command.assignmentId == 0 || command.releaseId == 0 || command.size == 0 ||
      command.size >= OTA_MAX_IMAGE_SIZE ||
      !copyJsonString(document["version"], command.version, sizeof(command.version)) ||
      !copyJsonString(document["url"], command.url, sizeof(command.url)) ||
      !copyJsonString(document["download_token"], command.token, sizeof(command.token)) ||
      !copyJsonString(document["sha256"], command.sha256, sizeof(command.sha256)) ||
      !copyJsonString(document["signature"], command.signature, sizeof(command.signature)) ||
      !copyJsonString(document["signing_key_id"], command.signingKeyId, sizeof(command.signingKeyId)) ||
      strcmp(command.signingKeyId, OTA_SIGNING_KEY_ID) != 0 ||
      !isSha256(command.sha256)) {
    setError(errorCode, errorCodeSize, "INVALID_MANIFEST");
    return false;
  }
  setError(errorCode, errorCodeSize, "");
  return true;
}

int otaCompareVersions(const char* left, const char* right) {
  int leftNumbers[3] = {0};
  int rightNumbers[3] = {0};
  String leftPre;
  String rightPre;
  if (!splitVersion(left, leftNumbers, leftPre) || !splitVersion(right, rightNumbers, rightPre)) {
    return String(left).compareTo(String(right));
  }
  for (size_t i = 0; i < 3; ++i) {
    if (leftNumbers[i] != rightNumbers[i]) return leftNumbers[i] < rightNumbers[i] ? -1 : 1;
  }
  if (leftPre.isEmpty() != rightPre.isEmpty()) return leftPre.isEmpty() ? 1 : -1;
  if (leftPre.isEmpty()) return 0;

  int leftStart = 0;
  int rightStart = 0;
  while (leftStart < static_cast<int>(leftPre.length()) || rightStart < static_cast<int>(rightPre.length())) {
    int leftEnd = leftPre.indexOf('.', leftStart);
    int rightEnd = rightPre.indexOf('.', rightStart);
    if (leftEnd < 0) leftEnd = leftPre.length();
    if (rightEnd < 0) rightEnd = rightPre.length();
    if (leftStart >= static_cast<int>(leftPre.length())) return -1;
    if (rightStart >= static_cast<int>(rightPre.length())) return 1;
    const int compared = compareIdentifier(leftPre.substring(leftStart, leftEnd),
                                           rightPre.substring(rightStart, rightEnd));
    if (compared != 0) return compared;
    leftStart = leftEnd + 1;
    rightStart = rightEnd + 1;
  }
  return 0;
}

bool otaPerformUpdate(const OtaCommand& command, OtaProgressCallback callback,
                      void* context, char* errorCode, size_t errorCodeSize) {
  WiFiClientSecure client;
  client.setCACert(MQTT_ROOT_CA);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(DOWNLOAD_IDLE_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, command.url)) {
    setError(errorCode, errorCodeSize, "HTTP_BEGIN_FAILED");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + command.token);
  http.addHeader("Accept-Encoding", "identity");

  const int code = http.GET();
  if (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN) {
    http.end();
    setError(errorCode, errorCodeSize, "AUTH_FAILED");
    return false;
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    setError(errorCode, errorCodeSize, code < 0 ? "HTTP_TIMEOUT" : "HTTP_FAILED");
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength <= 0 || static_cast<uint32_t>(contentLength) != command.size) {
    http.end();
    setError(errorCode, errorCodeSize, "SIZE_MISMATCH");
    return false;
  }
  if (!Update.begin(command.size, U_FLASH)) {
    http.end();
    setError(errorCode, errorCodeSize, "NO_OTA_SLOT");
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  NetworkClient* stream = http.getStreamPtr();
  uint8_t buffer[DOWNLOAD_BUFFER_SIZE];
  uint32_t written = 0;
  uint32_t lastDataAt = millis();
  uint8_t lastProgress = 255;
  bool downloadOk = true;

  while (written < command.size) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected() || millis() - lastDataAt > DOWNLOAD_IDLE_TIMEOUT_MS) {
        downloadOk = false;
        setError(errorCode, errorCodeSize, "HTTP_TIMEOUT");
        break;
      }
      delay(10);
      continue;
    }
    const size_t wanted = min(available, min(sizeof(buffer), static_cast<size_t>(command.size - written)));
    const int received = stream->readBytes(buffer, wanted);
    if (received <= 0) continue;
    lastDataAt = millis();
    mbedtls_sha256_update(&sha, buffer, received);
    if (Update.write(buffer, received) != static_cast<size_t>(received)) {
      downloadOk = false;
      setError(errorCode, errorCodeSize, "FLASH_WRITE_FAILED");
      break;
    }
    written += received;
    const uint8_t progress = static_cast<uint8_t>((written * 100ULL) / command.size);
    if (callback && (lastProgress == 255 || progress >= lastProgress + 5 || progress == 100)) {
      callback(progress, context);
      lastProgress = progress;
    }
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  http.end();

  if (!downloadOk || written != command.size) {
    Update.abort();
    if (downloadOk) setError(errorCode, errorCodeSize, "SIZE_MISMATCH");
    return false;
  }
  char actualSha[65];
  digestToHex(digest, actualSha);
  if (strcasecmp(actualSha, command.sha256) != 0) {
    Update.abort();
    setError(errorCode, errorCodeSize, "HASH_MISMATCH");
    return false;
  }
  if (!verifySignature(digest, command.signature)) {
    Update.abort();
    setError(errorCode, errorCodeSize, "SIGNATURE_INVALID");
    return false;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    setError(errorCode, errorCodeSize, "FLASH_WRITE_FAILED");
    return false;
  }
  setError(errorCode, errorCodeSize, "");
  return true;
}

bool otaRunningImagePendingVerify() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool otaMarkRunningImageValid() {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool otaSavePendingReport(const OtaCommand& command) {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return false;
  const bool ok = preferences.putUInt("assignment", command.assignmentId) == sizeof(uint32_t) &&
                  preferences.putString("version", command.version) > 0;
  preferences.end();
  return ok;
}

bool otaLoadPendingReport(OtaPendingReport& report) {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, true)) return false;
  report.assignmentId = preferences.getUInt("assignment", 0);
  const String version = preferences.getString("version", "");
  preferences.end();
  if (report.assignmentId == 0 || version.isEmpty() || version.length() >= sizeof(report.targetVersion)) {
    return false;
  }
  strncpy(report.targetVersion, version.c_str(), sizeof(report.targetVersion) - 1);
  report.targetVersion[sizeof(report.targetVersion) - 1] = '\0';
  return true;
}

void otaClearPendingReport() {
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return;
  preferences.clear();
  preferences.end();
}
