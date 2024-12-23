#include <MqttClient.h>
#include <MqttWiFiClient.h>
#include <MqttWriter.h>
#include "HaHelper.h"

extern FileData fsSettings;

class MqttTask : public Task {
public:
  MqttTask(bool _enabled = false, unsigned long _interval = 0) : Task(_enabled, _interval) {
    this->wifiClient = new MqttWiFiClient();
    this->client = new MqttClient(this->wifiClient);
    this->writer = new MqttWriter(this->client, 256);
    this->haHelper = new HaHelper();
  }

  ~MqttTask() {
    delete this->haHelper;

    if (this->client != nullptr) {
      if (this->client->connected()) {
        this->client->stop();

        if (this->connected) {
          this->onDisconnect();
          this->connected = false;
        }
      }

      delete this->client;
    }

    delete this->writer;
    delete this->wifiClient;
  }

  void disable() {
    this->client->stop();

    if (this->connected) {
      this->onDisconnect();
      this->connected = false;
    }
    
    Task::disable();

    Log.sinfoln(FPSTR(L_MQTT), F("Disabled"));
  }

  void enable() {
    Task::enable();

    Log.sinfoln(FPSTR(L_MQTT), F("Enabled"));
  }

  inline bool isConnected() {
    return this->connected;
  }

  inline void resetPublishedSettingsTime() {
    this->prevPubSettingsTime = 0;
  }

  inline void resetPublishedVarsTime() {
    this->prevPubVarsTime = 0;
  }

protected:
  MqttWiFiClient* wifiClient = nullptr;
  MqttClient* client = nullptr;
  HaHelper* haHelper = nullptr;
  MqttWriter* writer = nullptr;
  UnitSystem currentUnitSystem = UnitSystem::METRIC;
  bool currentHomeAssistantDiscovery = false;
  unsigned short readyForSendTime = 30000;
  unsigned long lastReconnectTime = 0;
  unsigned long connectedTime = 0;
  unsigned long disconnectedTime = 0;
  unsigned long prevPubVarsTime = 0;
  unsigned long prevPubSettingsTime = 0;
  bool connected = false;
  bool newConnection = false;

  #if defined(ARDUINO_ARCH_ESP32)
  const char* getTaskName() override {
    return "Mqtt";
  }
  
  /*BaseType_t getTaskCore() override {
    return 1;
  }*/

  int getTaskPriority() override {
    return 2;
  }
  #endif

  inline bool isReadyForSend() {
    return millis() - this->connectedTime > this->readyForSendTime;
  }

  void setup() {
    Log.sinfoln(FPSTR(L_MQTT), F("Started"));

    // wificlient settings
    #ifdef ARDUINO_ARCH_ESP8266
    this->wifiClient->setSync(true);
    this->wifiClient->setNoDelay(true);
    #endif

    // client settings
    this->client->setKeepAliveInterval(15000);
    this->client->setTxPayloadSize(256);
    #ifdef ARDUINO_ARCH_ESP8266
    this->client->setConnectionTimeout(1000);
    #else
    this->client->setConnectionTimeout(3000);
    #endif

    this->client->onMessage([this] (void*, size_t length) {
      String topic = this->client->messageTopic();
      if (!length || length > 2048 || !topic.length()) {
        return;
      }

      uint8_t payload[length];
      for (size_t i = 0; i < length && this->client->available(); i++) {
        payload[i] = this->client->read();
      }
      
      this->onMessage(topic.c_str(), payload, length);
    });

    // writer settings
    #ifdef ARDUINO_ARCH_ESP32
    this->writer->setYieldCallback([this] {
      this->delay(10);
    });
    #endif

    this->writer->setPublishEventCallback([this] (const char* topic, size_t written, size_t length, bool result) {
      Log.straceln(FPSTR(L_MQTT), F("%s publish %u of %u bytes to topic: %s"), result ? F("Successfully") : F("Failed"), written, length, topic);

      #ifdef ARDUINO_ARCH_ESP8266
      ::delay(0);
      #endif

      //this->client->poll();
      this->delay(250);
    });

    #ifdef ARDUINO_ARCH_ESP8266
    this->writer->setFlushEventCallback([this] (size_t, size_t) {
      ::delay(0);

      if (this->wifiClient->connected()) {
        this->wifiClient->flush();
      }

      ::delay(0);
    });
    #endif

    // ha helper settings
    this->haHelper->setDevicePrefix(settings.mqtt.prefix);
    this->haHelper->setDeviceVersion(BUILD_VERSION);
    this->haHelper->setDeviceModel(PROJECT_NAME);
    this->haHelper->setDeviceName(PROJECT_NAME);
    this->haHelper->setWriter(this->writer);

    sprintf(buffer, CONFIG_URL, WiFi.localIP().toString().c_str());
    this->haHelper->setDeviceConfigUrl(buffer);
  }

  void loop() {
    if (settings.mqtt.interval > 120) {
      settings.mqtt.interval = 5;
      fsSettings.update();
    }

    if (this->connected && !this->client->connected()) {
      this->connected = false;
      this->onDisconnect();

    } else if (!this->connected && millis() - this->lastReconnectTime >= MQTT_RECONNECT_INTERVAL) {
      Log.sinfoln(FPSTR(L_MQTT), F("Connecting to %s:%u..."), settings.mqtt.server, settings.mqtt.port);

      this->haHelper->setDevicePrefix(settings.mqtt.prefix);
      this->client->stop();
      this->client->setId(networkSettings.hostname);
      this->client->setUsernamePassword(settings.mqtt.user, settings.mqtt.password);
      this->client->connect(settings.mqtt.server, settings.mqtt.port);
      this->lastReconnectTime = millis();
      this->yield();

    } else if (!this->connected && this->client->connected()) {
      this->connected = true;
      this->onConnect();
    }

    if (!this->connected) {
      return;
    }

    this->client->poll();

    // delay for publish data
    if (!this->isReadyForSend()) {
      return;
    }

    #ifdef ARDUINO_ARCH_ESP8266
    ::delay(0);
    #endif

    // publish variables and status
    if (this->newConnection || millis() - this->prevPubVarsTime > (settings.mqtt.interval * 1000u)) {
      this->writer->publish(this->haHelper->getDeviceTopic("status").c_str(), "online", false);
      this->publishVariables(this->haHelper->getDeviceTopic("state").c_str());
      this->prevPubVarsTime = millis();
    }

    // publish settings
    if (this->newConnection || millis() - this->prevPubSettingsTime > (settings.mqtt.interval * 10000u)) {
      this->publishSettings(this->haHelper->getDeviceTopic("settings").c_str());
      this->prevPubSettingsTime = millis();
    }

    // publish ha entities if not published
    if (settings.mqtt.homeAssistantDiscovery) {
      if (this->newConnection || !this->currentHomeAssistantDiscovery || this->currentUnitSystem != settings.system.unitSystem) {
        this->publishHaEntities();
        this->publishNonStaticHaEntities(true);
        this->currentHomeAssistantDiscovery = true;
        this->currentUnitSystem = settings.system.unitSystem;

      } else {
        // publish non static ha entities
        this->publishNonStaticHaEntities();
      }

    } else if (this->currentHomeAssistantDiscovery) {
      this->currentHomeAssistantDiscovery = false;
    }

    if (this->newConnection) {
      this->newConnection = false;
    }
  }

  void onConnect() {
    this->connectedTime = millis();
    this->newConnection = true;
    unsigned long downtime = (millis() - this->disconnectedTime) / 1000;
    Log.sinfoln(FPSTR(L_MQTT), F("Connected (downtime: %u s.)"), downtime);

    this->client->subscribe(this->haHelper->getDeviceTopic("settings/set").c_str());
    this->client->subscribe(this->haHelper->getDeviceTopic("state/set").c_str());
  }

  void onDisconnect() {
    this->disconnectedTime = millis();

    unsigned long uptime = (millis() - this->connectedTime) / 1000;
    Log.swarningln(FPSTR(L_MQTT), F("Disconnected (reason: %d uptime: %u s.)"), this->client->connectError(), uptime);
  }

  void onMessage(const char* topic, uint8_t* payload, size_t length) {
    if (!length) {
      return;
    }

    if (settings.system.logLevel >= TinyLogger::Level::TRACE) {
      Log.strace(FPSTR(L_MQTT_MSG), F("Topic: %s\r\n>  "), topic);
      if (Log.lock()) {
        for (size_t i = 0; i < length; i++) {
          if (payload[i] == 0) {
            break;
          } else if (payload[i] == 13) {
            continue;
          } else if (payload[i] == 10) {
            Log.print("\r\n>  ");
          } else {
            Log.print((char) payload[i]);
          }
        }
        Log.print("\r\n\n");
        Log.flush();
        Log.unlock();
      }
    }

    JsonDocument doc;
    DeserializationError dErr = deserializeJson(doc, payload, length);
    if (dErr != DeserializationError::Ok) {
      Log.swarningln(FPSTR(L_MQTT_MSG), F("Error on deserialization: %s"), dErr.f_str());
      return;

    } else if (doc.isNull() || !doc.size()) {
      Log.swarningln(FPSTR(L_MQTT_MSG), F("Not valid json"));
      return;
    }
    doc.shrinkToFit();

    if (this->haHelper->getDeviceTopic("state/set").equals(topic)) {
      this->writer->publish(this->haHelper->getDeviceTopic("state/set").c_str(), nullptr, 0, true);
      
      if (jsonToVars(doc, vars)) {
        this->resetPublishedVarsTime();
      }

    } else if (this->haHelper->getDeviceTopic("settings/set").equals(topic)) {
      this->writer->publish(this->haHelper->getDeviceTopic("settings/set").c_str(), nullptr, 0, true);
      
      if (safeJsonToSettings(doc, settings)) {
        this->resetPublishedSettingsTime();
        fsSettings.update();
      }
    }
  }

  void publishHaEntities() {
    // heating
    this->haHelper->publishSwitchHeating(false);
    this->haHelper->publishSwitchHeatingTurbo(false);
    this->haHelper->publishInputHeatingHysteresis(settings.system.unitSystem);
    this->haHelper->publishInputHeatingTurboFactor(false);
    this->haHelper->publishInputHeatingMinTemp(settings.system.unitSystem);
    this->haHelper->publishInputHeatingMaxTemp(settings.system.unitSystem);
    this->haHelper->publishSensorHeatingSetpoint(settings.system.unitSystem, false);
    this->haHelper->publishSensorBoilerHeatingMinTemp(settings.system.unitSystem, false);
    this->haHelper->publishSensorBoilerHeatingMaxTemp(settings.system.unitSystem, false);

    // pid
    this->haHelper->publishSwitchPid();
    this->haHelper->publishInputPidFactorP(false);
    this->haHelper->publishInputPidFactorI(false);
    this->haHelper->publishInputPidFactorD(false);
    this->haHelper->publishInputPidDt(false);
    this->haHelper->publishInputPidMinTemp(settings.system.unitSystem, false);
    this->haHelper->publishInputPidMaxTemp(settings.system.unitSystem, false);

    // equitherm
    this->haHelper->publishSwitchEquitherm();
    this->haHelper->publishInputEquithermFactorN(false);
    this->haHelper->publishInputEquithermFactorK(false);
    this->haHelper->publishInputEquithermFactorT(false);

    // states
    this->haHelper->publishStateStatus();
    this->haHelper->publishStateEmergency();
    this->haHelper->publishStateOtStatus();
    this->haHelper->publishStateHeating();
    this->haHelper->publishStateFlame();
    this->haHelper->publishStateFault();
    this->haHelper->publishStateDiagnostic();
    this->haHelper->publishStateExtPump(false);

    // sensors
    this->haHelper->publishSensorModulation();
    this->haHelper->publishSensorPressure(settings.system.unitSystem, false);
    this->haHelper->publishSensorPower();
    this->haHelper->publishSensorFaultCode();
    this->haHelper->publishSensorDiagnosticCode();
    this->haHelper->publishSensorRssi(false);
    this->haHelper->publishSensorUptime(false);
    this->haHelper->publishOutdoorSensorConnected();
    this->haHelper->publishOutdoorSensorRssi(false);
    this->haHelper->publishOutdoorSensorBattery(false);
    this->haHelper->publishOutdoorSensorHumidity(false);
    this->haHelper->publishIndoorSensorConnected();
    this->haHelper->publishIndoorSensorRssi(false);
    this->haHelper->publishIndoorSensorBattery(false);
    this->haHelper->publishIndoorSensorHumidity(false);

    // temperatures
    this->haHelper->publishSensorHeatingTemp(settings.system.unitSystem);
    this->haHelper->publishSensorHeatingReturnTemp(settings.system.unitSystem, false);
    this->haHelper->publishSensorExhaustTemp(settings.system.unitSystem, false);

    // buttons
    this->haHelper->publishButtonRestart(false);
    this->haHelper->publishButtonResetFault();
    this->haHelper->publishButtonResetDiagnostic();
  }

  bool publishNonStaticHaEntities(bool force = false) {
    static byte _heatingMinTemp, _heatingMaxTemp, _dhwMinTemp, _dhwMaxTemp = 0;
    static bool _noRegulators, _editableOutdoorTemp, _editableIndoorTemp, _dhwPresent = false;

    bool published = false;
    bool noRegulators = !settings.opentherm.nativeHeatingControl && !settings.pid.enable && !settings.equitherm.enable;
    byte heatingMinTemp = 0;
    byte heatingMaxTemp = 0;
    bool editableOutdoorTemp = settings.sensors.outdoor.type == SensorType::MANUAL;
    bool editableIndoorTemp = settings.sensors.indoor.type == SensorType::MANUAL;

    if (noRegulators) {
      heatingMinTemp = settings.heating.minTemp;
      heatingMaxTemp = settings.heating.maxTemp;

    } else {
      heatingMinTemp = convertTemp(THERMOSTAT_INDOOR_MIN_TEMP, UnitSystem::METRIC, settings.system.unitSystem);
      heatingMaxTemp = convertTemp(THERMOSTAT_INDOOR_MAX_TEMP, UnitSystem::METRIC, settings.system.unitSystem);
    }

    if (force || _dhwPresent != settings.opentherm.dhwPresent) {
      _dhwPresent = settings.opentherm.dhwPresent;

      if (_dhwPresent) {
        this->haHelper->publishSwitchDhw(false);
        this->haHelper->publishSensorBoilerDhwMinTemp(settings.system.unitSystem, false);
        this->haHelper->publishSensorBoilerDhwMaxTemp(settings.system.unitSystem, false);
        this->haHelper->publishInputDhwMinTemp(settings.system.unitSystem);
        this->haHelper->publishInputDhwMaxTemp(settings.system.unitSystem);
        this->haHelper->publishStateDhw();
        this->haHelper->publishSensorDhwTemp(settings.system.unitSystem);
        this->haHelper->publishSensorDhwFlowRate(settings.system.unitSystem);

      } else {
        this->haHelper->deleteSwitchDhw();
        this->haHelper->deleteSensorBoilerDhwMinTemp();
        this->haHelper->deleteSensorBoilerDhwMaxTemp();
        this->haHelper->deleteInputDhwMinTemp();
        this->haHelper->deleteInputDhwMaxTemp();
        this->haHelper->deleteStateDhw();
        this->haHelper->deleteSensorDhwTemp();
        this->haHelper->deleteInputDhwTarget();
        this->haHelper->deleteClimateDhw();
        this->haHelper->deleteSensorDhwFlowRate();
      }

      published = true;
    }

    if (force || _noRegulators != noRegulators || _heatingMinTemp != heatingMinTemp || _heatingMaxTemp != heatingMaxTemp) {
      _heatingMinTemp = heatingMinTemp;
      _heatingMaxTemp = heatingMaxTemp;
      _noRegulators = noRegulators;

      this->haHelper->publishInputHeatingTarget(settings.system.unitSystem, heatingMinTemp, heatingMaxTemp, false);
      this->haHelper->publishClimateHeating(
        settings.system.unitSystem,
        heatingMinTemp,
        heatingMaxTemp,
        noRegulators ? HaHelper::TEMP_SOURCE_HEATING : HaHelper::TEMP_SOURCE_INDOOR
      );

      published = true;
    }

    if (_dhwPresent && (force || _dhwMinTemp != settings.dhw.minTemp || _dhwMaxTemp != settings.dhw.maxTemp)) {
      _dhwMinTemp = settings.dhw.minTemp;
      _dhwMaxTemp = settings.dhw.maxTemp;

      this->haHelper->publishInputDhwTarget(settings.system.unitSystem, settings.dhw.minTemp, settings.dhw.maxTemp, false);
      this->haHelper->publishClimateDhw(settings.system.unitSystem, settings.dhw.minTemp, settings.dhw.maxTemp);

      published = true;
    }

    if (force || _editableOutdoorTemp != editableOutdoorTemp) {
      _editableOutdoorTemp = editableOutdoorTemp;

      if (editableOutdoorTemp) {
        this->haHelper->deleteSensorOutdoorTemp();
        this->haHelper->publishInputOutdoorTemp(settings.system.unitSystem);
      } else {
        this->haHelper->deleteInputOutdoorTemp();
        this->haHelper->publishSensorOutdoorTemp(settings.system.unitSystem);
      }

      published = true;
    }

    if (force || _editableIndoorTemp != editableIndoorTemp) {
      _editableIndoorTemp = editableIndoorTemp;

      if (editableIndoorTemp) {
        this->haHelper->deleteSensorIndoorTemp();
        this->haHelper->publishInputIndoorTemp(settings.system.unitSystem);
      } else {
        this->haHelper->deleteInputIndoorTemp();
        this->haHelper->publishSensorIndoorTemp(settings.system.unitSystem);
      }

      published = true;
    }

    return published;
  }

  bool publishSettings(const char* topic) {
    JsonDocument doc;
    safeSettingsToJson(settings, doc);
    doc.shrinkToFit();

    return this->writer->publish(topic, doc, true);
  }

  bool publishVariables(const char* topic) {
    JsonDocument doc;
    varsToJson(vars, doc);
    doc.shrinkToFit();

    return this->writer->publish(topic, doc, true);
  }
};