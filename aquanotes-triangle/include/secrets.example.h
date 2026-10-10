#pragma once

// Hanya untuk build transisi perangkat lama. Build generic memakai provisioning NVS via USB.
// Username broker harus sama dengan UID perangkat.
#define DEVICE_UID "AER2023AQ0000"
#define MQTT_PASSWORD "ganti-dengan-password-perangkat"
// 0 = portable (baterai), 1 = panel (power supply DC 24/7)
#define DEVICE_POWER_SOURCE_PANEL 0
