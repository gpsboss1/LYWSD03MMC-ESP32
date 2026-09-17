#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "secrets.h"

namespace {

constexpr char kSensorMacPrefix[] = "a4:c1:38:";
constexpr uint16_t kMiBeaconServiceUuid = 0xfe95;
constexpr uint16_t kEnvironmentalSensingServiceUuid = 0x181a;
constexpr char kGattServiceUuid[] =
    "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char kMeasurementCharacteristicUuid[] =
    "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char kBafaMqttHost[] = "bemfa.com";
constexpr uint16_t kBafaMqttPort = 9501;
constexpr unsigned long kWifiConnectTimeoutMs = 20000UL;
constexpr unsigned long kMqttRetryIntervalMs = 5000UL;
constexpr uint32_t kDefaultMeasurementIntervalMinutes = 60U;
constexpr uint32_t kMinimumMeasurementIntervalMinutes = 1U;
constexpr uint32_t kMaximumMeasurementIntervalMinutes = 1440U;
constexpr uint8_t kBleReadMaxAttempts = 3U;
constexpr unsigned long kBleRetryDelayMs = 1000UL;

struct Measurement {
  float temperatureC = 0.0F;
  uint8_t humidity = 0;
  uint16_t batteryMillivolts = 0;
};

BLEScan* scanner = nullptr;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences preferences;
bool targetSeen = false;
std::string selectedSensorMac;
int strongestSensorRssi = -127;
bool autoUpdateEnabled = true;
bool immediateMeasurementRequested = false;
bool measurementPublishPending = false;
uint32_t measurementIntervalMinutes = kDefaultMeasurementIntervalMinutes;
Measurement pendingMeasurement;
unsigned long lastMqttAttempt = 0;
unsigned long lastMeasurement = 0;

// 将命令字符串首尾的空白字符移除，兼容 APP 自动附加的换行。
void trimCommand(char* command) {
  size_t start = 0;
  while (command[start] == ' ' || command[start] == '\t' ||
         command[start] == '\r' || command[start] == '\n') {
    ++start;
  }

  if (start > 0) {
    memmove(command, command + start, strlen(command + start) + 1);
  }

  size_t length = strlen(command);
  while (length > 0 &&
         (command[length - 1] == ' ' || command[length - 1] == '\t' ||
          command[length - 1] == '\r' || command[length - 1] == '\n')) {
    command[--length] = '\0';
  }
}

// 将自动更新设置保存到 NVS，设备重启后仍保留用户配置。
void saveSettings() {
  preferences.putBool("auto", autoUpdateEnabled);
  preferences.putUInt("interval", measurementIntervalMinutes);
}

// 从 NVS 读取自动更新设置，异常值恢复为安全的默认配置。
void loadSettings() {
  preferences.begin("settings", false);
  autoUpdateEnabled = preferences.getBool("auto", true);
  measurementIntervalMinutes =
      preferences.getUInt("interval", kDefaultMeasurementIntervalMinutes);

  if (measurementIntervalMinutes < kMinimumMeasurementIntervalMinutes ||
      measurementIntervalMinutes > kMaximumMeasurementIntervalMinutes) {
    measurementIntervalMinutes = kDefaultMeasurementIntervalMinutes;
    saveSettings();
  }
}

// 解析 Every:分钟数命令，并检查数值范围和尾随字符。
bool parseEveryCommand(const char* command, uint32_t& minutes) {
  constexpr char kEveryPrefix[] = "Every:";
  if (strncmp(command, kEveryPrefix, strlen(kEveryPrefix)) != 0) {
    return false;
  }

  const char* valueStart = command + strlen(kEveryPrefix);
  if (*valueStart == '\0') {
    return false;
  }

  char* valueEnd = nullptr;
  const unsigned long parsed = strtoul(valueStart, &valueEnd, 10);
  if (*valueEnd != '\0' || parsed < kMinimumMeasurementIntervalMinutes ||
      parsed > kMaximumMeasurementIntervalMinutes) {
    return false;
  }

  minutes = static_cast<uint32_t>(parsed);
  return true;
}

// 请求立即读取一次传感器，供 MQTT 命令统一触发即时回传。
void requestImmediateMeasurement() {
  immediateMeasurementRequested = true;
}

// 忽略设备自己发布的 /up 数据，避免把传感器数据误判为控制命令。
bool isMeasurementTopic(const char* topic) {
  char measurementTopic[80] = {};
  snprintf(measurementTopic, sizeof(measurementTopic), "%s/up", BEMFA_TOPIC);
  return strcmp(topic, measurementTopic) == 0;
}

// 接收巴法云自定义消息，只更新控制状态，避免在 MQTT 回调中执行 BLE 操作。
void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  if (isMeasurementTopic(topic)) {
    return;
  }

  constexpr size_t kCommandBufferSize = 48;
  if (length >= kCommandBufferSize) {
    Serial.printf("[CMD] command too long, length=%u\n", length);
    return;
  }

  char command[kCommandBufferSize] = {};
  memcpy(command, payload, length);
  command[length] = '\0';
  trimCommand(command);
  Serial.printf("[CMD] topic=%s command=%s\n", topic, command);

  if (strcmp(command, "Auto:on") == 0) {
    autoUpdateEnabled = true;
    requestImmediateMeasurement();
    saveSettings();
    Serial.println("[CMD] automatic updates enabled");
    return;
  }

  if (strcmp(command, "Auto:off") == 0) {
    autoUpdateEnabled = false;
    requestImmediateMeasurement();
    saveSettings();
    Serial.println("[CMD] automatic updates disabled");
    return;
  }

  uint32_t requestedIntervalMinutes = 0;
  if (parseEveryCommand(command, requestedIntervalMinutes)) {
    measurementIntervalMinutes = requestedIntervalMinutes;
    lastMeasurement = millis();
    requestImmediateMeasurement();
    saveSettings();
    Serial.printf("[CMD] update interval=%lu minutes\n",
                  static_cast<unsigned long>(measurementIntervalMinutes));
    return;
  }

  if (strcmp(command, "Update") == 0) {
    requestImmediateMeasurement();
    Serial.println("[CMD] one-shot update requested");
    return;
  }

  if (strcmp(command, "Status") == 0) {
    requestImmediateMeasurement();
    Serial.printf("[CMD] status auto=%s interval=%lu minutes\n",
                  autoUpdateEnabled ? "on" : "off",
                  static_cast<unsigned long>(measurementIntervalMinutes));
    return;
  }

  Serial.println("[CMD] unknown command");
}

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
  const bool subscribed = mqttClient.subscribe(BEMFA_TOPIC);
  Serial.printf("[MQTT] subscribe topic=%s result=%s\n", BEMFA_TOPIC,
                subscribed ? "ok" : "failed");

  char commandTopic[80] = {};
  snprintf(commandTopic, sizeof(commandTopic), "%s/set", BEMFA_TOPIC);
  const bool setSubscribed = mqttClient.subscribe(commandTopic);
  Serial.printf("[MQTT] subscribe topic=%s result=%s\n", commandTopic,
                setSubscribed ? "ok" : "failed");
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

// 主动连接发现到的温湿度计并读取 GATT 温湿度特征值。
bool readGattMeasurement(const std::string& sensorMac,
                         Measurement& measurement) {
  Serial.printf("[GATT] connecting to %s...\n", sensorMac.c_str());
  BLEClient* client = BLEDevice::createClient();
  if (client == nullptr) {
    Serial.println("[GATT] failed to create BLE client");
    return false;
  }

  const bool connected = client->connect(BLEAddress(sensorMac));
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

// 判断广播是否可能来自 LYWSD03MMC，减少误连接其他 BLE 设备的机会。
bool isLikelySensorAdvertisement(BLEAdvertisedDevice& advertisedDevice) {
  const std::string address = advertisedDevice.getAddress().toString();
  if (address.rfind(kSensorMacPrefix, 0) == 0) {
    return true;
  }

  if (advertisedDevice.haveServiceUUID() &&
      advertisedDevice.isAdvertisingService(BLEUUID(kGattServiceUuid))) {
    return true;
  }

  BLEUUID miBeaconUuid(kMiBeaconServiceUuid);
  BLEUUID environmentalSensingUuid(kEnvironmentalSensingServiceUuid);
  for (int index = 0; index < advertisedDevice.getServiceDataUUIDCount();
       ++index) {
    BLEUUID serviceDataUuid = advertisedDevice.getServiceDataUUID(index);
    if (serviceDataUuid.equals(miBeaconUuid) ||
        serviceDataUuid.equals(environmentalSensingUuid)) {
      return true;
    }
  }

  if (advertisedDevice.haveName()) {
    const std::string name = advertisedDevice.getName();
    return name.find("LYWSD03MMC") != std::string::npos;
  }

  return false;
}

// BLE 广播回调：记录 RSSI 最强的兼容温湿度计。
class AdvertisementCallbacks final : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!isLikelySensorAdvertisement(advertisedDevice)) {
      return;
    }

    const int rssi = advertisedDevice.getRSSI();
    if (!targetSeen || rssi > strongestSensorRssi) {
      targetSeen = true;
      strongestSensorRssi = rssi;
      selectedSensorMac = advertisedDevice.getAddress().toString();
      Serial.printf("[BLE] sensor candidate=%s, rssi=%d\n",
                    selectedSensorMac.c_str(), strongestSensorRssi);
    }
  }
};

// 扫描并读取一次米家温湿度计。
bool readSensorMeasurement(Measurement& measurement) {
  for (uint8_t attempt = 1; attempt <= kBleReadMaxAttempts; ++attempt) {
    targetSeen = false;
    strongestSensorRssi = -127;
    selectedSensorMac.clear();
    scanner->start(5, false);

    bool success = false;
    if (!targetSeen) {
      Serial.printf("[BLE] compatible sensor not seen, attempt %u/%u\n", attempt,
                    kBleReadMaxAttempts);
    } else {
      Serial.printf("[BLE] strongest sensor=%s, rssi=%d\n",
                    selectedSensorMac.c_str(), strongestSensorRssi);
      success = readGattMeasurement(selectedSensorMac, measurement);
    }

    scanner->clearResults();
    if (success) {
      Serial.printf(
          "[MEASUREMENT] temperature=%.1fC humidity=%u%% battery=%umV\n",
          measurement.temperatureC, measurement.humidity,
          measurement.batteryMillivolts);
      return true;
    }

    if (attempt < kBleReadMaxAttempts) {
      Serial.printf("[BLE] read failed, retrying in %lu ms\n",
                    kBleRetryDelayMs);
      delay(kBleRetryDelayMs);
    }
  }

  Serial.println("[BLE] all read attempts failed");
  return false;
}

// 按巴法云传感器格式上传温度、湿度、电压、Auto 状态和 Every 间隔。
bool publishMeasurement(const Measurement& measurement) {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] publish skipped, not connected");
    return false;
  }

  char topic[80] = {};
  snprintf(topic, sizeof(topic), "%s/up", BEMFA_TOPIC);
  char payload[96] = {};
  snprintf(payload, sizeof(payload), "#%.1f#%u#%u#Auto %s#Every %lu",
           measurement.temperatureC, measurement.humidity,
           measurement.batteryMillivolts, autoUpdateEnabled ? "on" : "off",
           static_cast<unsigned long>(measurementIntervalMinutes));

  const bool published = mqttClient.publish(topic, payload, true);
  Serial.printf("[MQTT] publish topic=%s payload=%s result=%s\n", topic,
                payload, published ? "ok" : "failed");
  return published;
}

// MQTT 重连后补发读取期间暂存的最新测量值。
void publishPendingMeasurement() {
  if (!measurementPublishPending || !mqttClient.connected()) {
    return;
  }

  if (publishMeasurement(pendingMeasurement)) {
    measurementPublishPending = false;
  }
}

}  // namespace

// 初始化串口、WiFi、MQTT 和 BLE 扫描器。
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("LYWSD03MMC-ESP32 gateway");
  Serial.println("Target sensor: strongest compatible BLE device");
  Serial.printf("Bafa topic: %s\n", BEMFA_TOPIC);
  Serial.println("BLE mode: active GATT read; bind key is not required.");
  loadSettings();
  Serial.printf("Auto update: %s, interval=%lu minutes\n",
                autoUpdateEnabled ? "on" : "off",
                static_cast<unsigned long>(measurementIntervalMinutes));

  mqttClient.setServer(kBafaMqttHost, kBafaMqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(128);

  BLEDevice::init("");
  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(new AdvertisementCallbacks());
  scanner->setActiveScan(false);
  scanner->setInterval(160);
  scanner->setWindow(80);
}

// 保持云端连接，并按照命令配置定时或按需读取传感器后上传。
void loop() {
  ensureMqttConnected();
  mqttClient.loop();
  publishPendingMeasurement();

  const unsigned long intervalMs =
      static_cast<unsigned long>(measurementIntervalMinutes) * 60000UL;
  const bool intervalDue =
      autoUpdateEnabled &&
      (lastMeasurement == 0 || millis() - lastMeasurement >= intervalMs);
  if (scanner != nullptr && (immediateMeasurementRequested || intervalDue)) {
    immediateMeasurementRequested = false;
    lastMeasurement = millis();
    Measurement measurement;
    if (readSensorMeasurement(measurement)) {
      pendingMeasurement = measurement;
      measurementPublishPending = true;
      publishPendingMeasurement();
    }
  }

  delay(20);
}
