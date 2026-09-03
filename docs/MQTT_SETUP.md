# MQTT Notes

MQTT is the messaging protocol used to move telemetry from the ESP32 to a remote broker.

## Roles

- **Publisher:** ESP32
- **Broker:** MQTT server
- **Subscriber:** MQTTX, backend service, database bridge, dashboard, etc.
- **Topic:** named message channel
- **Payload:** telemetry data sent on that topic

Current development topic:

```text
chillsense/test/telemetry
```

The ESP32 publishes JSON telemetry to the broker. MQTTX subscribes to the same topic and receives the messages.

```text
ESP32 -> Wi-Fi -> Internet -> MQTT broker -> MQTTX / cloud service
```

The project currently uses MQTT over TLS (`mqtts://`) for encrypted transport.

Public MQTT brokers are useful for testing but can be unstable and should not carry sensitive or production data. The final project should use a private/cloud broker with authentication.
