#pragma once

#include <Arduino.h>

struct DeviceConfig {
  char uid[37];
  char mqttPassword[96];
  bool panel;
  bool provisioned;
};

bool deviceConfigBegin();
const DeviceConfig& deviceConfig();
const char* deviceUid();
const char* deviceMqttPassword();
bool deviceUsesBattery();
bool deviceIsProvisioned();
void deviceProvisioningPoll(Stream& serial);

