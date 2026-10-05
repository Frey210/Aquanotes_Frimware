#pragma once

#include <Arduino.h>

struct OtaCommand {
  uint32_t assignmentId = 0;
  uint32_t releaseId = 0;
  uint32_t size = 0;
  bool mandatory = false;
  bool rollback = false;
  char version[64] = {0};
  char url[256] = {0};
  char token[64] = {0};
  char sha256[65] = {0};
  char signature[160] = {0};
  char signingKeyId[33] = {0};
};

struct OtaPendingReport {
  uint32_t assignmentId = 0;
  char targetVersion[64] = {0};
};

using OtaProgressCallback = void (*)(uint8_t progress, void* context);

bool otaParseCommand(const char* payload, size_t length, OtaCommand& command,
                     char* errorCode, size_t errorCodeSize);
int otaCompareVersions(const char* left, const char* right);
bool otaPerformUpdate(const OtaCommand& command, OtaProgressCallback callback,
                      void* context, char* errorCode, size_t errorCodeSize);

bool otaRunningImagePendingVerify();
bool otaMarkRunningImageValid();
bool otaSavePendingReport(const OtaCommand& command);
bool otaLoadPendingReport(OtaPendingReport& report);
void otaClearPendingReport();
