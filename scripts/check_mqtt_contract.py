from pathlib import Path


source = (Path(__file__).parents[1] / "aquanotes-triangle" / "src" / "main.cpp").read_text(encoding="utf-8")

for required in (
    'MQTT_HOST  = "mqtt.farlabs.my.id"',
    'MQTT_PATH  = "/mqtt"',
    'String("devices/") + DEVICE_UID + "/telemetry"',
    '\\"message_id\\"',
    '\\"uid\\"',
    '\\"suhu\\"',
    '\\"ph\\"',
    '\\"do\\"',
    '\\"tds\\"',
    '\\"ammonia\\"',
    '\\"salinitas\\"',
    'gMqtt.publish(topic, pendingPayload, false, 1)',
):
    assert required in source, f"Kontrak MQTT hilang: {required}"

assert "HTTPClient" not in source
assert "timestamp" not in source
print("MQTT firmware contract: OK")
