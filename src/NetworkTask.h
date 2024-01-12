#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include "lwip/etharp.h"
#elif defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#endif
#include <Connection.h>


class NetworkTask : public Task {
public:
  NetworkTask(bool _enabled = false, unsigned long _interval = 0) : Task(_enabled, _interval) {
    Connection::setup(this->useDhcp);
  }

  NetworkTask* setHostname(const char* value) {
    this->hostname = value;
    return this;
  }

  NetworkTask* setApCredentials(const char* ssid, const char* password = nullptr, byte channel = 0) {
    this->apName = ssid;
    this->apPassword = password;
    this->apChannel = channel;

    return this;
  }

  NetworkTask* setStaCredentials(const char* ssid = nullptr, const char* password = nullptr, byte channel = 0) {
    this->staSsid = ssid;
    this->staPassword = password;
    this->staChannel = channel;

    return this;
  }

  NetworkTask* setUseDhcp(bool value) {
    this->useDhcp = value;
    Connection::setup(this->useDhcp);

    return this;
  }

  NetworkTask* setStaticConfig(const char* ip, const char* gateway, const char* subnet, const char* dns) {
    this->staticIp.fromString(ip);
    this->staticGateway.fromString(gateway);
    this->staticSubnet.fromString(subnet);
    this->staticDns.fromString(dns);

    return this;
  }

  NetworkTask* setStaticConfig(IPAddress &ip, IPAddress &gateway, IPAddress &subnet, IPAddress &dns) {
    this->staticIp = ip;
    this->staticGateway = gateway;
    this->staticSubnet = subnet;
    this->staticDns = dns;

    return this;
  }

  bool hasStaCredentials() {
    return this->staSsid != nullptr;
  }

  bool isConnected() {
    return this->isStaEnabled() && Connection::getStatus() == Connection::Status::CONNECTED;
  }

  bool isConnecting() {
    return this->isStaEnabled() && Connection::getStatus() == Connection::Status::CONNECTING;
  }

  bool isStaEnabled() {
    return (WiFi.getMode() & WIFI_STA) != 0;
  }

  bool isApEnabled() {
    return (WiFi.getMode() & WIFI_AP) != 0;
  }

  bool hasApClients() {
    if (!this->isApEnabled()) {
      return false;
    }

    return WiFi.softAPgetStationNum() > 0;
  }

  short int getRssi() {
    return WiFi.RSSI();
  }

  IPAddress getApIp() {
    return WiFi.softAPIP();
  }

  IPAddress getStaIp() {
    return WiFi.localIP();
  }

  IPAddress getStaSubnet() {
    return WiFi.subnetMask();
  }

  IPAddress getStaGateway() {
    return WiFi.gatewayIP();
  }

  IPAddress getStaDns() {
    return WiFi.dnsIP();
  }

  String getStaMac() {
    return WiFi.macAddress();
  }

  const char* getStaSsid() {
    return this->staSsid;
  }

  const char* getStaPassword() {
    return this->staPassword;
  }

  byte getStaChannel() {
    return this->staChannel;
  }

  bool resetWifi() {
    WiFi.persistent(false);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);

    #ifdef ARDUINO_ARCH_ESP8266
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    if (wifi_softap_dhcps_status() == DHCP_STARTED) {
      wifi_softap_dhcps_stop();
    }
    #elif defined(ARDUINO_ARCH_ESP32)
    WiFi.setSleep(WIFI_PS_NONE);
    #endif
    WiFi.softAPdisconnect();

    #ifdef ARDUINO_ARCH_ESP8266
    if (wifi_station_dhcpc_status() == DHCP_STARTED) {
      wifi_station_dhcpc_stop();
    }
    #endif
    WiFi.disconnect(false, true);

    return WiFi.mode(WIFI_OFF);
  }

  void reconnect() {
    this->reconnectFlag = true;
  }

  bool connect(bool force = false, unsigned int timeout = 1000u) {
    if (this->isConnected() && !force) {
      return true;
    }

    if (force && !this->isApEnabled()) {
      this->resetWifi();

    } else {
      #ifdef ARDUINO_ARCH_ESP8266
      if (wifi_station_dhcpc_status() == DHCP_STARTED) {
        wifi_station_dhcpc_stop();
      }
      #endif

      WiFi.disconnect(false, true);
    }

    if (!this->hasStaCredentials()) {
      return false;
    }

    this->delay(200);

    #ifdef ARDUINO_ARCH_ESP32
    if (this->setWifiHostname(this->hostname)) {
      Log.straceln(FPSTR(L_NETWORK), F("Set hostname '%s': success"), this->hostname);

    } else {
      Log.serrorln(FPSTR(L_NETWORK), F("Set hostname '%s': fail"), this->hostname);
    }
    #endif

    if (!WiFi.mode((WiFiMode_t)(WiFi.getMode() | WIFI_STA))) {
      return false;
    }

    this->delay(200);

    #ifdef ARDUINO_ARCH_ESP8266
    if (this->setWifiHostname(this->hostname)) {
      Log.straceln(FPSTR(L_NETWORK), F("Set hostname '%s': success"), this->hostname);

    } else {
      Log.serrorln(FPSTR(L_NETWORK), F("Set hostname '%s': fail"), this->hostname);
    }

    this->delay(200);
    #endif

    if (!this->useDhcp) {
      WiFi.config(this->staticIp, this->staticGateway, this->staticSubnet, this->staticDns);
    }

    WiFi.begin(this->staSsid, this->staPassword, this->staChannel);

    unsigned long beginConnectionTime = millis();
    while (millis() - beginConnectionTime < timeout) {
      this->delay(100);

      if (WiFi.status() == WL_CONNECTED) {
        return true;
      }
    }

    return false;
  }

  static byte rssiToSignalQuality(short int rssi) {
    return constrain(map(rssi, -100, -50, 0, 100), 0, 100);
  }

protected:
  const unsigned int reconnectInterval = 5000;
  const unsigned int failedConnectTimeout = 30000; // 120000
  const unsigned int connectionTimeout = 15000;
  const unsigned int resetConnectionTimeout = 60000;

  const char* hostname = "esp";
  const char* apName = "ESP";
  const char* apPassword = nullptr;
  byte apChannel = 1;

  const char* staSsid = nullptr;
  const char* staPassword = nullptr;
  byte staChannel = 0;

  bool useDhcp = true;
  IPAddress staticIp;
  IPAddress staticGateway;
  IPAddress staticSubnet;
  IPAddress staticDns;

  bool connected = false;
  bool reconnectFlag = false;
  unsigned long prevArpGratuitous = 0;
  unsigned long prevReconnectingTime = 0;
  unsigned long connectedTime = 0;
  unsigned long disconnectedTime = 0;

  const char* getTaskName() {
    return "Wifi";
  }

  /*int getTaskCore() {
    return 1;
  }*/

  int getTaskPriority() {
    return 0;
  }

  void setup() {
    this->resetWifi();
  }

  void loop() {
    if (this->isConnected() && !this->hasStaCredentials()) {
      Log.sinfoln(FPSTR(L_NETWORK), F("Reset"));
      this->resetWifi();

    } else if (this->isConnected() && !this->reconnectFlag) {
      if (!this->connected) {
        this->connectedTime = millis();
        this->connected = true;

        Log.sinfoln(
          FPSTR(L_NETWORK),
          F("Connected, downtime: %lu s., IP: %s, RSSI: %hhd"),
          (millis() - this->disconnectedTime) / 1000,
          WiFi.localIP().toString().c_str(),
          WiFi.RSSI()
        );
      }

      if (this->isApEnabled() && millis() - this->connectedTime > this->reconnectInterval && !this->hasApClients()) {
        Log.sinfoln(FPSTR(L_NETWORK), F("Stop AP because connected, start only STA"));

        WiFi.mode(WIFI_STA);
        return;
      }

      #ifdef ARDUINO_ARCH_ESP8266
      if (millis() - this->prevArpGratuitous > 60000) {
        this->stationKeepAliveNow();
        this->prevArpGratuitous = millis();
      }
      #endif

    } else {
      if (this->connected) {
        this->disconnectedTime = millis();
        this->connected = false;

        Log.sinfoln(
          FPSTR(L_NETWORK),
          F("Disconnected, reason: %d, uptime: %lu s."),
          Connection::getDisconnectReason(),
          (millis() - this->connectedTime) / 1000
        );
      }

      if (!this->hasStaCredentials() && !this->isApEnabled()) {
        Log.sinfoln(FPSTR(L_NETWORK), F("No STA credentials, start AP"));

        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(this->apName, this->apPassword, this->apChannel);

      } else if (!this->isApEnabled() && millis() - this->disconnectedTime > this->failedConnectTimeout) {
        Log.sinfoln(FPSTR(L_NETWORK), F("Disconnected for a long time, start AP"));

        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(this->apName, this->apPassword, this->apChannel);

      } else if (this->isConnecting() && millis() - this->prevReconnectingTime > this->resetConnectionTimeout) {
        Log.swarningln(FPSTR(L_NETWORK), F("Connection timeout, reset wifi..."));
        this->resetWifi();

      } else if (!this->isConnecting() && (!this->prevReconnectingTime || millis() - this->prevReconnectingTime > this->reconnectInterval)) {
        if (this->hasStaCredentials()) {
          Log.sinfoln(FPSTR(L_NETWORK), F("Try connect..."));

          this->prevReconnectingTime = millis();
          this->connect(true, this->connectionTimeout);
          this->reconnectFlag = false;
        }
      }
    }
  }

  bool setWifiHostname(const char* hostname) {
    if (!this->isHostnameValid(hostname)) {
      return false;
    }

    if (strcmp(WiFi.getHostname(), hostname) == 0) {
      return true;
    }

    return WiFi.setHostname(hostname);
  }

  #ifdef ARDUINO_ARCH_ESP8266
  /**
   * @brief
   * https://github.com/arendst/Tasmota/blob/e6515883f0ee5451931b6280ff847b117de5a231/tasmota/tasmota_support/support_wifi.ino#L1196
   */
  static void stationKeepAliveNow(void) {
    for (netif* interface = netif_list; interface != nullptr; interface = interface->next) {
      if (
        (interface->flags & NETIF_FLAG_LINK_UP)
        && (interface->flags & NETIF_FLAG_UP)
        && interface->num == STATION_IF
        && (!ip4_addr_isany_val(*netif_ip4_addr(interface)))
        ) {
        etharp_gratuitous(interface);
        ::optimistic_yield(1000);
        break;
      }
    }
  }
  #endif

  /**
   * @brief check RFC compliance
   *
   * @param value
   * @return true
   * @return false
   */
  static bool isHostnameValid(const char* value) {
    size_t len = strlen(value);
    if (len > 24) {
      return false;
    } else if (value[len - 1] == '-') {
      return false;
    }

    for (size_t i = 0; i < len; i++) {
      if (!isalnum(value[i]) && value[i] != '-') {
        return false;
      }
    }

    return true;
  }
};