#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <string>

#include "secrets.h"

namespace {

constexpr char kSensorMac[] = "aa:bb:cc:dd:ee:ff";
constexpr char kGattServiceUuid[] =
    "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char kMeasurementCharacteristicUuid[] =
    "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char kBafaMqttHost[] = "bemfa.com";
constexpr uint16_t kBafaMqttPort = 9501;
constexpr unsigned long kWifiConnectTimeoutMs = 20000UL;
constexpr unsigned long kMqttRetryIntervalMs = 5000UL;
constexpr unsigned long kMeasurementIntervalMs = 600000UL;

struct Measurement {
  float temperatureC = 0.0F;
  uint8_t humidity = 0;
  uint16_t batteryMillivolts = 0;
};

BLEScan* scanner = nullptr;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
bool targetSeen = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastMeasurement = 0;

// 连接本地 WiFi，并在超时后返回，让主循环继续运行。
bool ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("[WIFI] connecting to %s...\n", MITEMP_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(MITEMP_WIFI_SSID, MITEMP_WIFI_PASSWORD);
  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < kWifiConnectTimeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] connection failed, status=%d\n", WiFi.status());
    return false;
  }

  Serial.printf("[WIFI] connected, ip=%s\n",
                WiFi.localIP().toString().c_str());
  return true;
}

// 连接巴法云 MQTT，使用 UID 作为客户端 ID。
bool ensureMqttConnected() {
  if (!ensureWifiConnected()) {
    return false;
  }
  if (mqttClient.connected()) {
    return true;
  }
  if (millis() - lastMqttAttempt < kMqttRetryIntervalMs) {
    return false;
  }

  lastMqttAttempt = millis();
  Serial.printf("[MQTT] connecting to %s:%u...\n", kBafaMqttHost,
                kBafaMqttPort);
  if (!mqttClient.connect(BEMFA_UID)) {
    Serial.printf("[MQTT] connection failed, state=%d\n",
                  mqttClient.state());
    return false;
  }

  Serial.println("[MQTT] connected");
  return true;
}

// 解析 LYWSD03MMC GATT 温湿度特征值。
bool parseGattMeasurement(const std::string& value,
                          Measurement& measurement) {
  if (value.size() < 5) {
    Serial.printf("[GATT] unexpected value length=%u\n",
                  static_cast<unsigned>(value.size()));
    return false;
  }

  const int16_t rawTemperature =
      static_cast<int16_t>(static_cast<uint8_t>(value[0]) |
                           (static_cast<uint8_t>(value[1]) << 8));
  measurement.temperatureC = static_cast<float>(rawTemperature) / 100.0F;
  measurement.humidity = static_cast<uint8_t>(value[2]);
  measurement.batteryMillivolts =
      static_cast<uint16_t>(static_cast<uint8_t>(value[3]) |
                            (static_cast<uint8_t>(value[4]) << 8));
  return true;
}

// 主动连接原厂温湿度计并读取 GATT 温湿度特征值。
bool readGattMeasurement(Measurement& measurement) {
  Serial.printf("[GATT] connecting to %s...\n", kSensorMac);
  BLEClient* client = BLEDevice::createClient();
  if (client == nullptr) {
    Serial.println("[GATT] failed to create BLE client");
    return false;
  }

  const bool connected = client->connect(BLEAddress(kSensorMac));
  if (!connected) {
    Serial.println(
        "[GATT] connection failed; close Mi Home or other BLE apps if connected");
    delete client;
    return false;
  }
  Serial.println("[GATT] connected");

  BLERemoteService* service = client->getService(BLEUUID(kGattServiceUuid));
  if (service == nullptr) {
    Serial.println("[GATT] target service not found");
    client->disconnect();
    delete client;
    return false;
  }

  BLERemoteCharacteristic* characteristic =
      service->getCharacteristic(BLEUUID(kMeasurementCharacteristicUuid));
  if (characteristic == nullptr || !characteristic->canRead()) {
    Serial.println("[GATT] measurement characteristic is not readable");
    client->disconnect();
    delete client;
    return false;
  }

  const std::string value = characteristic->readValue();
  const bool parsed = parseGattMeasurement(value, measurement);
  client->disconnect();
  delete client;
  Serial.println("[GATT] disconnected");
  return parsed;
}

// BLE 广播回调：确认目标设备在范围内。
class AdvertisementCallbacks final : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (advertisedDevice.getAddress().toString() != kSensorMac) {
      return;
    }

    targetSeen = true;
    Serial.printf("[BLE] target seen, rssi=%d\n",
                  advertisedDevice.getRSSI());
  }
};

// 扫描并读取一次米家温湿度计。
bool readSensorMeasurement(Measurement& measurement) {
  targetSeen = false;
  scanner->start(5, false);

  if (!targetSeen) {
    scanner->clearResults();
    Serial.println("[BLE] target not seen in the latest scan");
    return false;
  }

  const bool success = readGattMeasurement(measurement);
  scanner->clearResults();
  if (success) {
    Serial.printf("[MEASUREMENT] temperature=%.1fC humidity=%u%% battery=%umV\n",
                  measurement.temperatureC, measurement.humidity,
                  measurement.batteryMillivolts);
  }
  return success;
}

// 按巴法云传感器格式上传温度、湿度和电压，只更新云端最新值。
bool publishMeasurement(const Measurement& measurement) {
  if (!mqttClient.connected()) {
    return false;
  }

  char topic[80] = {};
  snprintf(topic, sizeof(topic), "%s/up", BEMFA_TOPIC);
  char payload[48] = {};
  snprintf(payload, sizeof(payload), "#%.1f#%u#%u", measurement.temperatureC,
           measurement.humidity, measurement.batteryMillivolts);

  const bool published = mqttClient.publish(topic, payload, true);
  Serial.printf("[MQTT] publish topic=%s payload=%s result=%s\n", topic,
                payload, published ? "ok" : "failed");
  return published;
}

}  // namespace

// 初始化串口、WiFi、MQTT 和 BLE 扫描器。
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("MiTempHumid LYWSD03MMC Bafa Cloud gateway");
  Serial.printf("Target MAC: %s\n", kSensorMac);
  Serial.printf("Bafa topic: %s\n", BEMFA_TOPIC);
  Serial.println("BLE mode: active GATT read; bind key is not required.");

  mqttClient.setServer(kBafaMqttHost, kBafaMqttPort);
  mqttClient.setBufferSize(128);

  BLEDevice::init("");
  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(new AdvertisementCallbacks());
  scanner->setActiveScan(false);
  scanner->setInterval(160);
  scanner->setWindow(80);
}

// 保持云端连接，并每 10 分钟读取一次传感器后上传。
void loop() {
  ensureMqttConnected();
  mqttClient.loop();

  if (scanner != nullptr &&
      (lastMeasurement == 0 ||
       millis() - lastMeasurement >= kMeasurementIntervalMs)) {
    lastMeasurement = millis();
    Measurement measurement;
    if (readSensorMeasurement(measurement)) {
      publishMeasurement(measurement);
    }
  }

  delay(20);
}





