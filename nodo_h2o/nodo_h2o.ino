#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <OneWire.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SenMLHelper.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ======================================================
// TELEMETRIA RTC
// ======================================================
RTC_DATA_ATTR uint32_t rtc_boot_count = 0;
RTC_DATA_ATTR uint32_t rtc_wdt_resets = 0;
RTC_DATA_ATTR uint32_t rtc_wifi_disconnects = 0;
RTC_DATA_ATTR uint32_t rtc_http_errors = 0;

// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE
// ======================================================
const char *FIRMWARE_VERSION_CODE = "1.1.4-h2o";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED Y PORTAL CAUTIVO
// ======================================================
const char *DEFAULT_SSID = "tili";
const char *DEFAULT_PASS = "Ubuntu1234$";

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const char *PREFS_NAMESPACE = "wifi_config";
const char *PREF_SSID = "ssid";
const char *PREF_PASS = "pass";
const char *AP_SSID = "NODO_H2O_SETUP";

String loadedSsid = "";
String loadedPassword = "";

const int WIFI_RESET_PIN = 0; // GPIO 0 (Botón BOOT en ESP32 estándar de 38 pines)
const int TIEMPO_MAX_CONEXION_WIFI = 20000;
const long CONFIG_FETCH_INTERVAL = 60000; // Consulta cada 60s

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA (Nodriza Cloud)
// ======================================================
String backendHost = "192.168.68.89"; // Por defecto IP de la GreenBox Local
int backendPort = 3000;
String endpointRelays = "/relays/status/";
long intervaloEnvioMs = 15000;   // Telemetría periódica cada 15s
long intervaloConsultaMs = 5000; // Polling de relés cada 5s
bool isLoneWolf = false;         // Por defecto modo Enjambre con GreenBox
bool flagActivo = true;

const String NODRIZA_HOST = "nodrizabackend-production.up.railway.app";

// ======================================================
// CONFIGURACIÓN DE MQTT (HiveMQ Cloud TLS)
// ======================================================
const char *mqtt_server = "737baf5d1c9043498f3a99457237bbd7.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char *mqtt_user = "nodriza_app";
const char *mqtt_pass = "Ubuntu1234$";

WiFiClientSecure mqttEspClient;
WiFiClient normalClient;
PubSubClient mqttClient;
String commandTopic;
String telemetryTopic;

// ======================================================
// 3. HARDWARE & PINES (ESP32 Standard WROOM-32 / DevKit)
// ======================================================
String boxSerialId;

// 🌊 Sensor de Nivel
const int PIN_FLOAT = 19; // GPIO 19 (Entrada flotador con pull-up interno)

// ⚡ Sensores Analógicos (ADC1)
const int PH_PIN = 32;  // GPIO 32 (ADC1_CH4)
const int TDS_PIN = 34; // GPIO 34 (ADC1_CH6)

// 🌡️ Sensor de Temperatura de Agua
const int ONE_WIRE_BUS = 4; // GPIO 4 para DS18B20
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// 🔌 Módulo de 8 Relés
const int NUM_RELAYS = 8;
const int relayPins[NUM_RELAYS] = {13, 14, 27, 26, 25, 16, 17, 18};
bool relayStates[NUM_RELAYS] = {false, false, false, false, false, false, false, false};

// Mapeo Estandarizado de Actuadores
const char *RELAY_LABELS[NUM_RELAYS] = {"llenado", "ph_up", "ph_down", "base_a", "base_b", "base_c", "aux", "mezcladora"};
const char *RELAY_KEYS[NUM_RELAYS] = {
    "relay_fill", "relay_ph_up", "relay_ph_down", "relay_base_a", "relay_base_b", "relay_base_c", "relay_aux", "relay_mixer"
};

// Polaridad de Relés (Lógica Active LOW para este módulo de relés)
const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

// Lógica de Flotador (LOW = Tanque Lleno con Pull-up interno)
const int FLOAT_LLENO = LOW;

// ======================================================
// RUTINA DE AUTO-LLENADO NATIVO (Fill Tank)
// ======================================================
bool isAutoFilling = false;
int fillCycles = 0;
const int MAX_FILL_CYCLES = 20; // Límite de seguridad
unsigned long lastFillActionTime = 0;
const unsigned long FILL_ON_TIME = 3000;   // 3s encendido
const unsigned long FILL_OFF_TIME = 10000; // 10s apagado estabilizando boya
bool isFillRelayOn = false;

// Parámetros ADC y Muestreo Estable
const int ADC_MAX_VALUE = 4095;
const float ADC_VOLTAGE_REF = 3.3f;
const int NUM_MUESTRAS = 25; // Filtro de mediana con rechazo de cuartiles

// ======================================================
// 4. PARÁMETROS DE CALIBRACIÓN Y VARIABLES DE MEDICIÓN
// ======================================================
float ph_v4 = 0.40f;
float ph_v7 = 1.10f;
float ph_slope = -5.70f;
float ph_offset = 14.5f;
float tds_k_value = 1.0f;

// Variables de Medición
int ph_raw_filtered = 0;
float ph_voltage = 0.0f;
float ph_value = 7.0f;

int tds_raw_filtered = 0;
float tds_voltage = 0.0f;
float tds_value = 0.0f;   // TDS en ppm
float ec_value = 0.0f;    // Conductividad Eléctrica en uS/cm
float water_temp = 25.0f; // Temperatura de agua nominal (°C)

// Timers del ciclo
unsigned long tiempoUltimaMuestra = 0;
unsigned long tiempoUltimaConsultaRelays = 0;
unsigned long lastConfigFetch = 0;

// Estado de Pulsos Nativos
unsigned long pulseEndTimes[NUM_RELAYS] = {0, 0, 0, 0, 0, 0, 0, 0};
bool isPulseActive[NUM_RELAYS] = {false, false, false, false, false, false, false, false};

// ======================================================
// 5. DECLARACIONES DE FUNCIONES
// ======================================================
void configurar_hardware();
void leer_sensores_agua();
void calcular_calibracion_ph();
void guardarCalibracionPh(float v4, float v7);
void guardarCalibracionTds(float k);
bool isTankFull();
void setRelay(int index, bool on);
void applyRelayCommand(int index, bool requestedOn);
void ejecutarPulsoAsync(int index, float seconds);
void apagarTodosLosRelays();
void processRelayPayload(const String &payloadStr);
void mqttCallback(char *topic, byte *payload, unsigned int length);
void reconnectMqtt();
void publicarTelemetriaMqtt();

bool conectar_wifi();
void resetWifiStack();
void enviar_post();
void consultar_servidor_remoto();
void sendTelemetry();
void logMessage(String level, String msg);

// NVS y Portal Cautivo
void saveCredentials(const String &ssid, const String &password);
bool loadCredentials();
void clearCredentials();
void startConfigPortal();
void handleRoot();
void handleSave();
bool probarCredencialesWifi(const String &ssid, const String &password,
                            int &estadoFinal);

// Remote Config y OTA
void obtener_remote_config();
bool check_for_update();

// ======================================================
// CONTROL DE RELÉS
// ======================================================

void setRelay(int index, bool on) {
  if (index < 0 || index >= NUM_RELAYS)
    return;
  relayStates[index] = on;
  digitalWrite(relayPins[index], on ? RELAY_ON : RELAY_OFF);
  Serial.printf("🔌 Relay %d (%s / %s): %s\n", index + 1, RELAY_LABELS[index],
                RELAY_KEYS[index], on ? "ON" : "OFF");
}

void applyRelayCommand(int index, bool requestedOn) {
  if (index < 0 || index >= NUM_RELAYS)
    return;
  setRelay(index, requestedOn);
}

void ejecutarPulsoAsync(int index, float seconds) {
  if (index < 0 || index >= NUM_RELAYS)
    return;
  unsigned long ms = (unsigned long)(seconds * 1000.0f);
  if (ms < 100)
    ms = 100; // Mínimo 100ms
  if (ms > 60000)
    ms = 60000; // Máximo 60s

  Serial.printf("💧 Iniciando pulso RELAY %d: %lu ms\n", index, ms);
  applyRelayCommand(index, true);
  pulseEndTimes[index] = millis() + ms;
  isPulseActive[index] = true;
  publicarTelemetriaMqtt();
}

void apagarTodosLosRelays() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    setRelay(i, false);
  }
}

void guardarCalibracionPh(float v4, float v7) {
  ph_v4 = v4;
  ph_v7 = v7;
  calcular_calibracion_ph();
  Preferences prefs;
  prefs.begin("cal_h2o", false);
  prefs.putFloat("ph_v4", ph_v4);
  prefs.putFloat("ph_v7", ph_v7);
  prefs.end();
  Serial.printf("💾 [CALIBRACIÓN] pH guardado en NVS: v4=%.3f, v7=%.3f (slope=%.3f, offset=%.3f)\n",
                ph_v4, ph_v7, ph_slope, ph_offset);
}

void guardarCalibracionTds(float k) {
  tds_k_value = k;
  Preferences prefs;
  prefs.begin("cal_h2o", false);
  prefs.putFloat("tds_k", tds_k_value);
  prefs.end();
  Serial.printf("💾 [CALIBRACIÓN] TDS guardado en NVS: Factor K=%.3f\n", tds_k_value);
}

void processRelayPayload(const String &payloadStr) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (!err) {
    int cambios = 0;

    // 0. Comandos Globales / Acciones Especiales (Calibración, Auto-Llenado, Pulsos)
    if (doc.containsKey("action")) {
      String act = doc["action"].as<String>();
      act.toUpperCase();

      if (act == "CAL_PH") {
        int point = doc.containsKey("point") ? doc["point"].as<int>()
                                             : (doc.containsKey("ph") ? doc["ph"].as<int>() : 7);
        float volt = doc.containsKey("voltage")
                         ? doc["voltage"].as<float>()
                         : (doc.containsKey("volt") ? doc["volt"].as<float>() : ph_voltage);
        if (point == 4) {
          guardarCalibracionPh(volt, ph_v7);
        } else if (point == 7) {
          guardarCalibracionPh(ph_v4, volt);
        }
        publicarTelemetriaMqtt();
        return;
      } else if (act == "CAL_TDS") {
        float k = doc.containsKey("k")
                      ? doc["k"].as<float>()
                      : (doc.containsKey("value") ? doc["value"].as<float>() : 1.0f);
        if (k > 0.0f) {
          guardarCalibracionTds(k);
          publicarTelemetriaMqtt();
        }
        return;
      } else if (act == "FILL_TANK" || act == "FILL") {
        if (!isAutoFilling) {
          isAutoFilling = true;
          fillCycles = 0;
          lastFillActionTime = millis();
          isFillRelayOn = false;
          Serial.println(F("🌊 Iniciando rutina nativa de AUTO-LLENADO"));
        }
        return;
      } else if (act == "PULSE") {
        int idx = doc.containsKey("relayIndex")
                      ? doc["relayIndex"].as<int>()
                      : (doc.containsKey("relay")
                             ? doc["relay"].as<int>()
                             : (doc.containsKey("index") ? doc["index"].as<int>()
                                                         : doc["channel"] | 0));
        float sec = doc.containsKey("duration") ? doc["duration"].as<float>() : 15.0f;
        ejecutarPulsoAsync(idx, sec);
        return;
      }
    }

    // 1. Formato SenML: { "e": [{ "n": "relay:0", "vb": true }] }
    if (doc.containsKey("e")) {
      JsonArray eArray = doc["e"].as<JsonArray>();
      for (JsonObject item : eArray) {
        String name = item["n"] | "";
        int targetIdx = -1;

        if (name.startsWith("relay:")) {
          targetIdx = name.substring(6).toInt();
        } else {
          for (int r = 0; r < NUM_RELAYS; r++) {
            if (name.equalsIgnoreCase(RELAY_KEYS[r]) ||
                name.equalsIgnoreCase(RELAY_LABELS[r])) {
              targetIdx = r;
              break;
            }
          }
        }

        if (targetIdx >= 0 && targetIdx < NUM_RELAYS) {
          bool val = item.containsKey("vb") ? item["vb"].as<bool>()
                                            : (item["v"].as<float>() > 0);
          if (val != relayStates[targetIdx]) {
            cambios++;
            applyRelayCommand(targetIdx, val);
            esp_task_wdt_reset();
            delay(50);
          }
        }
      }
    }
    // 2. Formato Individual Directo: { "relayIndex": 0, "state": true }
    else if (doc.containsKey("relayIndex") || doc.containsKey("relay") ||
             doc.containsKey("index") || doc.containsKey("channel")) {
      int idx =
          doc.containsKey("relayIndex")
              ? doc["relayIndex"].as<int>()
              : (doc.containsKey("relay")
                     ? doc["relay"].as<int>()
                     : (doc.containsKey("index") ? doc["index"].as<int>()
                                                 : doc["channel"].as<int>()));

      if (idx >= 0 && idx < NUM_RELAYS) {
        bool val = false;
        if (doc.containsKey("state"))
          val = doc["state"].as<bool>();
        else if (doc.containsKey("status"))
          val = doc["status"].as<bool>();
        else if (doc.containsKey("value"))
          val = doc["value"].as<bool>();
        else if (doc.containsKey("val"))
          val = doc["val"].as<bool>();
        else if (doc.containsKey("on"))
          val = doc["on"].as<bool>();
        else if (doc.containsKey("action")) {
          String act = doc["action"].as<String>();
          act.toUpperCase();
          if (act == "FILL_TANK" || act == "FILL") {
            if (!isAutoFilling) {
              isAutoFilling = true;
              fillCycles = 0;
              lastFillActionTime = millis();
              isFillRelayOn = false;
              Serial.println(F("🌊 Iniciando rutina nativa de AUTO-LLENADO"));
            }
            return;
          } else if (act == "PULSE") {
            float sec = doc.containsKey("duration")
                            ? doc["duration"].as<float>()
                            : 15.0f;
            ejecutarPulsoAsync(idx, sec);
            return;
          } else if (act == "ON" || act == "TRUE" || act == "1" ||
                     act == "ENABLE")
            val = true;
          else if (act == "OFF" || act == "FALSE" || act == "0") {
            isPulseActive[idx] = false;
            val = false;
            if (idx == 0) {
              isAutoFilling = false;
              isFillRelayOn = false;
            }
          }
        }

        if (doc.containsKey("duration") && val) {
          float sec = doc["duration"].as<float>();
          if (sec > 0) {
            ejecutarPulsoAsync(idx, sec);
            return;
          }
        }

        if (val != relayStates[idx]) {
          cambios++;
          applyRelayCommand(idx, val);
          esp_task_wdt_reset();
          delay(50);
        }
      }
    }
    // 3. Formato Nombre/Target: { "name": "relay_fill", "state": true }
    else if (doc.containsKey("name") || doc.containsKey("target")) {
      String name = doc.containsKey("name") ? doc["name"].as<String>()
                                            : doc["target"].as<String>();
      int targetIdx = -1;
      if (name.startsWith("relay:")) {
        targetIdx = name.substring(6).toInt();
      } else {
        for (int r = 0; r < NUM_RELAYS; r++) {
          if (name.equalsIgnoreCase(RELAY_KEYS[r]) ||
              name.equalsIgnoreCase(RELAY_LABELS[r])) {
            targetIdx = r;
            break;
          }
        }
      }

      if (targetIdx >= 0 && targetIdx < NUM_RELAYS) {
        bool val = doc.containsKey("state") ? doc["state"].as<bool>()
                                            : (doc["value"] | false);
        if (val != relayStates[targetIdx]) {
          cambios++;
          applyRelayCommand(targetIdx, val);
          esp_task_wdt_reset();
          delay(50);
        }
      }
    }
    // 4. Formato Arreglos o Mapas: { "relays": [true, false, ...] }
    else {
      JsonVariant rVar =
          doc.containsKey("relays")
              ? doc["relays"]
              : (doc.containsKey("status") ? doc["status"] : doc["data"]);

      if (rVar.is<JsonArray>()) {
        JsonArray arr = rVar.as<JsonArray>();
        for (int i = 0; i < NUM_RELAYS && i < (int)arr.size(); i++) {
          bool val = false;
          if (arr[i].is<bool>())
            val = arr[i].as<bool>();
          else if (arr[i].is<int>() || arr[i].is<float>())
            val = (arr[i].as<int>() > 0);
          else if (arr[i].is<const char *>()) {
            String sVal = arr[i].as<String>();
            val =
                (sVal == "true" || sVal == "1" || sVal == "ON" || sVal == "on");
          }
          if (val != relayStates[i]) {
            cambios++;
            applyRelayCommand(i, val);
            esp_task_wdt_reset();
            delay(50);
          }
        }
      }
    }

    if (cambios > 0) {
      Serial.printf(F("🔌 ✅ %d relé(s) de agua conmutados vía comando.\n"),
                    cambios);
      publicarTelemetriaMqtt();
    }
  } else {
    String pUpper = payloadStr;
    pUpper.trim();
    pUpper.toUpperCase();

    // Comandos directos de Calibración
    if (pUpper.startsWith("CAL_PH")) {
      // Formatos: CAL_PH:4:2.95, CAL_PH:7:2.50, CAL_PH:4, CAL_PH:7
      int firstColon = pUpper.indexOf(':');
      if (firstColon != -1) {
        int secondColon = pUpper.indexOf(':', firstColon + 1);
        int point = pUpper.substring(firstColon + 1, secondColon != -1 ? secondColon : pUpper.length()).toInt();
        float volt = (secondColon != -1) ? pUpper.substring(secondColon + 1).toFloat() : ph_voltage;
        if (point == 4) {
          guardarCalibracionPh(volt, ph_v7);
        } else if (point == 7) {
          guardarCalibracionPh(ph_v4, volt);
        }
        publicarTelemetriaMqtt();
      }
      return;
    } else if (pUpper.startsWith("CAL_TDS")) {
      // Formato: CAL_TDS:1.15
      int colon = pUpper.indexOf(':');
      if (colon != -1) {
        float k = pUpper.substring(colon + 1).toFloat();
        if (k > 0.0f) {
          guardarCalibracionTds(k);
          publicarTelemetriaMqtt();
        }
      }
      return;
    } else if (pUpper == "FILL_TANK" || pUpper == "FILL") {
      if (!isAutoFilling) {
        isAutoFilling = true;
        fillCycles = 0;
        lastFillActionTime = millis();
        isFillRelayOn = false;
        Serial.println(F("🌊 Iniciando rutina nativa de AUTO-LLENADO"));
      }
      return;
    } else if (pUpper == "ON" || pUpper == "ALL_ON" || pUpper == "ON_ALL") {
      for (int i = 0; i < NUM_RELAYS; i++)
        applyRelayCommand(i, true);
      Serial.println(F("🔌 ✅ TODOS LOS RELÉS ENCENDIDOS"));
      publicarTelemetriaMqtt();
    } else if (pUpper == "OFF" || pUpper == "ALL_OFF" || pUpper == "OFF_ALL") {
      for (int i = 0; i < NUM_RELAYS; i++)
        applyRelayCommand(i, false);
      Serial.println(F("🔌 ✅ TODOS LOS RELÉS APAGADOS"));
      publicarTelemetriaMqtt();
    } else if (pUpper.startsWith("RELAY:") || pUpper.startsWith("RELAY_") ||
               pUpper.startsWith("PUMP:") || pUpper.startsWith("PUMP_")) {
      // Formatos: RELAY:0:ON, RELAY:0:OFF, RELAY:1:PULSE:5.0
      char sep = pUpper.indexOf(':') != -1 ? ':' : '_';
      int firstSep = pUpper.indexOf(sep);
      int secondSep = pUpper.indexOf(sep, firstSep + 1);

      if (firstSep != -1 && secondSep != -1) {
        int idx = pUpper.substring(firstSep + 1, secondSep).toInt();
        String action = pUpper.substring(secondSep + 1);

        if (action.startsWith("PULSE")) {
          int thirdSep = action.indexOf(sep);
          float sec = (thirdSep != -1) ? action.substring(thirdSep + 1).toFloat() : 15.0f;
          ejecutarPulsoAsync(idx, sec);
        } else {
          bool state = (action == "ON" || action == "1" || action == "TRUE" || action == "ENABLE");
          if (idx >= 0 && idx < NUM_RELAYS) {
            applyRelayCommand(idx, state);
            Serial.printf("🔌 ✅ RELAY %d -> %s\n", idx + 1, state ? "ON" : "OFF");
            publicarTelemetriaMqtt();
          }
        }
      }
    }
  }
}

// ======================================================
// MQTT CALLBACK Y TELEMETRÍA (HiveMQ TLS)
// ======================================================
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();
  Serial.printf("📥 [MQTT H2O Comando] %s\n", message.c_str());
  processRelayPayload(message);
}

void publicarTelemetriaMqtt() {
  if (!mqttClient.connected())
    return;

  DynamicJsonDocument doc(768);
  doc["nodeId"] = boxSerialId;
  doc["nodeType"] = "NODO_H2O";
  doc["firmwareVersion"] = FIRMWARE_VERSION_CODE;
  doc["ip"] = WiFi.localIP().toString();
  doc["localIp"] = WiFi.localIP().toString();
  doc["ph"] = ph_value;
  doc["ph_voltage"] = ph_voltage;
  doc["tds"] = tds_value;
  doc["tds_voltage"] = tds_voltage;
  doc["ec"] = ec_value;
  doc["water_temp"] = water_temp;
  doc["tank_full"] = isTankFull();

  JsonArray rArr = doc.createNestedArray("relays");
  for (int i = 0; i < NUM_RELAYS; i++) {
    rArr.add(relayStates[i]);
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  mqttClient.publish(telemetryTopic.c_str(), jsonStr.c_str());
  Serial.printf("📤 [MQTT H2O Telemetría] Publicado a %s (IP: %s)\n",
                telemetryTopic.c_str(), WiFi.localIP().toString().c_str());
}

void reconnectMqtt() {
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      
      int currentPort = (!isLoneWolf && backendHost != "") ? 1883 : mqtt_port;

      if (!isLoneWolf && backendHost != "") {
        mqttClient.setClient(normalClient);
        mqttClient.setServer(backendHost.c_str(), currentPort);
        Serial.printf("Conectando a MQTT Local: %s:%d...\n", backendHost.c_str(), currentPort);
      } else {
        mqttEspClient.setInsecure();
        mqttClient.setClient(mqttEspClient);
        mqttClient.setServer(mqtt_server, currentPort);
        Serial.printf("Conectando a MQTT Cloud: %s:%d...\n", mqtt_server, currentPort);
      }

      Serial.flush(); // Asegurar que el mensaje salga antes de conectar

      String clientId = "ESP32_H2O_" + boxSerialId;
      bool connected = false;

      if (!isLoneWolf && backendHost != "") {
        connected = mqttClient.connect(clientId.c_str());
      } else {
        connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
      }

      if (connected) {
        Serial.println(F("¡Conectado a MQTT!"));
        mqttClient.subscribe(commandTopic.c_str());
        mqttClient.subscribe("nodos/esp32/broadcast/command");
        Serial.printf("Suscrito a: %s\n", commandTopic.c_str());
        publicarTelemetriaMqtt();
      } else {
        Serial.printf("Fallo MQTT, rc=%d\n", mqttClient.state());
        if (isLoneWolf || backendHost == "") {
          char err_buf[100];
          if (mqttEspClient.lastError(err_buf, 100) < 0) {
            Serial.printf("TLS Error: %s\n", err_buf);
          }
        }
      }
    }
  }
}

// ======================================================
// TELEMETRÍA RTC Y CONFIGURACIÓN HARDWARE
// ======================================================

void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) +
                 "/api/health/metrics";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/senml+json");

    DynamicJsonDocument doc(512);
    JsonArray eArray = SenMLHelper::createPack(
        doc, boxSerialId, "NODO_H2O", FIRMWARE_VERSION_CODE,
        WiFi.localIP().toString().c_str());

    doc["boxSerialId"] = boxSerialId;
    doc["ip"] = WiFi.localIP().toString();
    doc["boot_count"] = rtc_boot_count;
    doc["wdt_resets"] = rtc_wdt_resets;
    doc["wifi_disconnects"] = rtc_wifi_disconnects;
    doc["http_errors"] = rtc_http_errors;

    String jsonStr;
    serializeJson(doc, jsonStr);
    int code = http.POST(jsonStr);
    if (code <= 0)
      rtc_http_errors++;
    http.end();
  }
}

void configurar_hardware() {
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  pinMode(PIN_FLOAT, INPUT_PULLUP);

  pinMode(PH_PIN, INPUT);
  pinMode(TDS_PIN, INPUT);

  sensors.begin();

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
    relayStates[i] = false;
  }

  analogSetAttenuation(ADC_11db);
  calcular_calibracion_ph();
}

void setup() {
  Serial.begin(115200);

#if defined(ESP32)
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON) {
    rtc_boot_count = 0;
    rtc_wdt_resets = 0;
    rtc_wifi_disconnects = 0;
    rtc_http_errors = 0;
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
             reason == ESP_RST_PANIC) {
    rtc_wdt_resets++;
  }
  rtc_boot_count++;

  delay(500);

  configurar_hardware();

  preferences.begin(PREFS_NAMESPACE, true);
  backendHost = preferences.getString("bHost", "192.168.68.89");
  backendPort = preferences.getInt("bPort", 3000);
  isLoneWolf = preferences.getBool("isLoneWolf", false);
  preferences.end();

  if (backendHost == "" || backendHost == "localhost" || backendHost == "127.0.0.1") {
    backendHost = "192.168.68.89";
    backendPort = 3000;
    isLoneWolf = false;
    preferences.begin(PREFS_NAMESPACE, false);
    preferences.putString("bHost", backendHost);
    preferences.putInt("bPort", backendPort);
    preferences.putBool("isLoneWolf", false);
    preferences.end();
  }

  // Cargar calibraciones de pH y TDS de NVS
  preferences.begin("cal_h2o", true);
  ph_v4 = preferences.getFloat("ph_v4", 0.40f);
  ph_v7 = preferences.getFloat("ph_v7", 1.10f);
  tds_k_value = preferences.getFloat("tds_k", 1.0f);
  preferences.end();
  calcular_calibracion_ph();

  resetWifiStack();
  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  if (mac == "00:00:00:00:00:00" || mac == "") {
    uint64_t chipid = ESP.getEfuseMac();
    uint32_t high = (uint32_t)(chipid >> 32);
    uint32_t low = (uint32_t)chipid;
    char macStr[20];
    snprintf(macStr, sizeof(macStr), "%04X%08X", (uint16_t)high, low);
    boxSerialId = String(macStr);
  } else {
    boxSerialId = mac;
    boxSerialId.replace(":", "");
  }
  boxSerialId.toUpperCase();

  commandTopic = "nodos/esp32/" + boxSerialId + "/command";
  telemetryTopic = "nodos/esp32/" + boxSerialId + "/telemetry";

  logMessage("INFO", "\n--- 💧 Nodo H2O: Calidad de Agua & Dosificación (v" +
                         String(FIRMWARE_VERSION_CODE) + ") 💧 ---");
  logMessage("INFO", "🆔 ID: " + boxSerialId);

  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    logMessage("WARNING", "🚨 Botón BOOT detectado. Borrando credenciales...");
    clearCredentials();
    startConfigPortal();
  }

  bool credentialsLoaded = loadCredentials();
  if (!credentialsLoaded) {
    saveCredentials(DEFAULT_SSID, DEFAULT_PASS);
    loadCredentials();
    credentialsLoaded = true;
  }

  if (credentialsLoaded && conectar_wifi()) {
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(768);

    ArduinoOTA.begin();
    logMessage("INFO", "✅ Conexión Wi-Fi establecida. IP: " +
                           WiFi.localIP().toString());

    // Servidor HTTP local para diagnósticos y control manual en puerto 80
    server.on("/", HTTP_GET, []() {
      String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Nodo H2O</title>";
      html += "<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:20px;}";
      html += ".card{background:#161b22;padding:20px;border-radius:10px;max-width:520px;margin:auto;box-shadow:0 4px 12px rgba(0,0,0,0.5);}";
      html += "h1{color:#58a6ff;margin-bottom:5px;}h3{color:#8b949e;margin-top:0;}span{color:#3fb950;font-weight:bold;}";
      html += ".metric{background:#21262d;padding:10px;border-radius:6px;margin:8px 0;}";
      html += "</style></head><body><div class='card'>";
      html += "<h1>💧 Nodo Calidad de Agua</h1><h3>Ecosistema Nodriza</h3>";
      html += "<div class='metric'><b>ID:</b> <span>" + boxSerialId + "</span> | <b>Fw:</b> <span>" + String(FIRMWARE_VERSION_CODE) + "</span></div>";
      html += "<div class='metric'><b>IP:</b> <span>" + WiFi.localIP().toString() + "</span> | <b>Modo:</b> <span>" + (isLoneWolf ? "Lobo Solitario" : "Enjambre") + "</span></div>";
      html += "<div class='metric'><b>pH:</b> <span>" + String(ph_value, 2) + "</span> (" + String(ph_voltage, 2) + "V)</div>";
      html += "<div class='metric'><b>TDS / EC:</b> <span>" + String(tds_value, 0) + " ppm</span> | <span>" + String(ec_value, 0) + " µS/cm</span></div>";
      html += "<div class='metric'><b>Temp Agua:</b> <span>" + String(water_temp, 1) + " °C</span></div>";
      html += "<div class='metric'><b>Boya Nivel:</b> <span>" + String(isTankFull() ? "LLENO" : "BAJO") + "</span></div>";
      html += "</div></body></html>";
      server.send(200, "text/html", html);
    });

    server.on("/status", HTTP_GET, []() {
      DynamicJsonDocument doc(1024);
      doc["nodeId"] = boxSerialId;
      doc["nodeType"] = "NODO_H2O";
      doc["firmwareVersion"] = FIRMWARE_VERSION_CODE;
      doc["ip"] = WiFi.localIP().toString();
      doc["ph"] = ph_value;
      doc["phVoltage"] = ph_voltage;
      doc["tds"] = tds_value;
      doc["tdsVoltage"] = tds_voltage;
      doc["ec"] = ec_value;
      doc["waterTemp"] = water_temp;
      doc["tankFull"] = isTankFull();
      JsonArray rArr = doc.createNestedArray("relays");
      for (int i = 0; i < NUM_RELAYS; i++) {
        rArr.add(relayStates[i]);
      }
      String res;
      serializeJson(doc, res);
      server.send(200, "application/json", res);
    });

    server.on("/relay", HTTP_GET, []() {
      if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        bool state = (server.hasArg("state") && (server.arg("state") == "on" || server.arg("state") == "1" || server.arg("state") == "true"));
        if (idx >= 0 && idx < NUM_RELAYS) {
          applyRelayCommand(idx, state);
          server.send(200, "application/json", "{\"status\":\"ok\",\"relay\":" + String(idx) + ",\"state\":" + String(state ? "true" : "false") + "}");
          return;
        }
      }
      server.send(400, "application/json", "{\"error\":\"invalid idx or state\"}");
    });

    server.on("/ping", HTTP_GET, []() {
      server.send(200, "application/json", "{\"status\":\"ok\",\"nodeId\":\"" + boxSerialId + "\",\"role\":\"NODO_H2O\"}");
    });

    server.begin();
    logMessage("INFO", "🌐 Servidor HTTP local iniciado en puerto 80");

    obtener_remote_config();
    check_for_update();
    reconnectMqtt();
    leer_sensores_agua();
    enviar_post();
    lastConfigFetch = millis();
  } else {
    logMessage("WARNING",
               "❌ Falló conexión inicial. Entrando a Portal Cautivo...");
    startConfigPortal();
  }

  // Iniciar Hardware Watchdog (30 segundos) una vez completado el aprovisionamiento
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true};
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);
}

void checkPulses() {
  unsigned long currentMillis = millis();
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (isPulseActive[i] && currentMillis >= pulseEndTimes[i]) {
      Serial.printf("💧 Fin del pulso RELAY %d\n", i);
      applyRelayCommand(i, false);
      isPulseActive[i] = false;
    }
  }
}

void handleAutoFillTask() {
  if (!isAutoFilling)
    return;

  unsigned long currentMillis = millis();

  // Si sobrepasamos el máximo de ciclos, algo está mal (boya sucia, sin presión
  // de agua, etc.)
  if (fillCycles >= MAX_FILL_CYCLES) {
    Serial.println("❌ [CRÍTICO] Timeout en AUTO-LLENADO (Max Ciclos "
                   "alcanzado). Abortando.");
    applyRelayCommand(0, false);
    isAutoFilling = false;
    isFillRelayOn = false;
    return;
  }

  if (isFillRelayOn) {
    // Estamos llenando, chequear si ya pasaron los 3s
    if (currentMillis - lastFillActionTime >= FILL_ON_TIME) {
      applyRelayCommand(0, false); // Apagar válvula
      isFillRelayOn = false;
      lastFillActionTime = currentMillis;
      Serial.println("💧 Fin de pulso de llenado. Esperando estabilización...");
    }
  } else {
    // Estamos en descanso, chequear si ya pasaron los 10s
    if (currentMillis - lastFillActionTime >= FILL_OFF_TIME) {
      // 10s han pasado. Revisar el flotador.
      int floatValue = digitalRead(PIN_FLOAT);
      if (floatValue == FLOAT_LLENO) {
        Serial.println("✅ Tanque Lleno. Rutina de AUTO-LLENADO terminada.");
        isAutoFilling = false;
        fillCycles = 0;
      } else {
        Serial.printf("⚠️ Tanque bajo. Iniciando ciclo %d de %d...\n",
                      fillCycles + 1, MAX_FILL_CYCLES);
        applyRelayCommand(0, true); // Encender válvula
        isFillRelayOn = true;
        lastFillActionTime = currentMillis;
        fillCycles++;
      }
    }
  }
}

void loop() {
  handleAutoFillTask();
  checkPulses();
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  server.handleClient();

  reconnectMqtt();
  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  unsigned long tiempoActual = millis();

  // 1. Polling rápido de estado de relés desde la GreenBox Local
  if (tiempoActual - tiempoUltimaConsultaRelays >= intervaloConsultaMs) {
    tiempoUltimaConsultaRelays = tiempoActual;
    consultar_servidor_remoto();
  }

  // 2. Fetch de Configuración Dinámica y OTA (cada 60s)
  if (tiempoActual - lastConfigFetch >= CONFIG_FETCH_INTERVAL) {
    if (conectar_wifi()) {
      obtener_remote_config();
      check_for_update();
      lastConfigFetch = tiempoActual;
    }
  }

  // 3. Muestreo y Envío de Mediciones periódicas (SenML y MQTT)
  if (tiempoActual - tiempoUltimaMuestra >= intervaloEnvioMs) {
    tiempoUltimaMuestra = tiempoActual;

    leer_sensores_agua();

    if (flagActivo && conectar_wifi()) {
      enviar_post();
      publicarTelemetriaMqtt();
    }
  }

  // 4. Envío periódico de Telemetría RTC (cada 1 hora)
  static unsigned long lastTelemetry = 0;
  if (tiempoActual - lastTelemetry >= 3600000 || lastTelemetry == 0) {
    lastTelemetry = (tiempoActual == 0) ? 1 : tiempoActual;
    sendTelemetry();
  }
}

// ======================================================
// LECTURA Y FILTRADO ULTRA ESTABLE DE SENSORES
// ======================================================

bool isTankFull() {
  int countLleno = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(PIN_FLOAT) == FLOAT_LLENO)
      countLleno++;
    delay(2);
  }
  return (countLleno >= 3);
}

void calcular_calibracion_ph() {
  if (ph_v4 != ph_v7) {
    ph_slope = (4.0f - 7.0f) / (ph_v4 - ph_v7);
    ph_offset = 7.0f - (ph_slope * ph_v7);
  } else {
    ph_slope = -5.70f;
    ph_offset = 14.5f;
  }
  Serial.printf(F("🧪 CALIBRACIÓN pH -> Pendiente: %.3f, Offset: %.3f\n"),
                ph_slope, ph_offset);
}

void leer_sensores_agua() {
  // 0. TEMPERATURA DEL AGUA (DS18B20)
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  if (tempC != DEVICE_DISCONNECTED_C && tempC > -10.0f) {
    water_temp = tempC;
  }

  // 1. MUESTREO DE pH (Filtro de Mediana y Rechazo de Cuartiles)
  int phMuestras[NUM_MUESTRAS];
  for (int i = 0; i < NUM_MUESTRAS; i++) {
    phMuestras[i] = analogRead(PH_PIN);
    delay(4);
  }

  for (int i = 0; i < NUM_MUESTRAS - 1; i++) {
    for (int j = i + 1; j < NUM_MUESTRAS; j++) {
      if (phMuestras[i] > phMuestras[j]) {
        int temp = phMuestras[i];
        phMuestras[i] = phMuestras[j];
        phMuestras[j] = temp;
      }
    }
  }

  int descarte = NUM_MUESTRAS / 4;
  long sumPh = 0;
  int validSamples = NUM_MUESTRAS - (descarte * 2);
  for (int i = descarte; i < NUM_MUESTRAS - descarte; i++) {
    sumPh += phMuestras[i];
  }
  ph_raw_filtered = sumPh / validSamples;
  ph_voltage =
      (float)ph_raw_filtered * (ADC_VOLTAGE_REF / (float)ADC_MAX_VALUE);
  ph_value = (ph_slope * ph_voltage) + ph_offset;
  ph_value = constrain(ph_value, 0.0f, 14.0f);

  // 2. MUESTREO DE TDS (Filtro de Mediana y Rechazo de Cuartiles)
  int tdsMuestras[NUM_MUESTRAS];
  for (int i = 0; i < NUM_MUESTRAS; i++) {
    tdsMuestras[i] = analogRead(TDS_PIN);
    delay(4);
  }

  for (int i = 0; i < NUM_MUESTRAS - 1; i++) {
    for (int j = i + 1; j < NUM_MUESTRAS; j++) {
      if (tdsMuestras[i] > tdsMuestras[j]) {
        int temp = tdsMuestras[i];
        tdsMuestras[i] = tdsMuestras[j];
        tdsMuestras[j] = temp;
      }
    }
  }

  long sumTds = 0;
  for (int i = descarte; i < NUM_MUESTRAS - descarte; i++) {
    sumTds += tdsMuestras[i];
  }
  tds_raw_filtered = sumTds / validSamples;
  tds_voltage =
      (float)tds_raw_filtered * (ADC_VOLTAGE_REF / (float)ADC_MAX_VALUE);

  // Compensación de Temperatura para TDS
  float tempCoefficient = 1.0f + 0.02f * (water_temp - 25.0f);
  float compensationVoltage = tds_voltage / tempCoefficient;

  float tdsCalc =
      (133.42f * pow(compensationVoltage, 3) -
       255.86f * pow(compensationVoltage, 2) + 857.39f * compensationVoltage) *
      0.5f;
  if (tdsCalc < 0.0f)
    tdsCalc = 0.0f;
  tds_value = tdsCalc * tds_k_value;
  ec_value = tds_value * 2.0f;

  Serial.printf(F("📊 [MEDICIONES H2O] Tanque: %s | Temp: %.1fC | pH: %.2f "
                  "(%.3fV) | TDS: "
                  "%.1f ppm | EC: %.1f uS/cm\n"),
                isTankFull() ? "LLENO" : "BAJO", water_temp, ph_value,
                ph_voltage, tds_value, ec_value);
}

// ======================================================
// CONSULTA PERIÓDICA DE RELÉS (HTTP POLLING)
// ======================================================

void consultar_servidor_remoto() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  String url;
  WiFiClientSecure client;
  HTTPClient http;

  if (backendHost.length() > 0) {
    url = "http://" + backendHost + ":" + String(backendPort) + endpointRelays +
          boxSerialId;
    http.begin(url);
  } else {
    client.setInsecure();
    url = "https://" + NODRIZA_HOST + "/api/nodes/" + boxSerialId +
          "/config?ip=" + WiFi.localIP().toString() +
          "&fw=" + FIRMWARE_VERSION_CODE;
    http.begin(client, url);
  }

  http.setTimeout(2500);
  http.addHeader("X-Local-IP", WiFi.localIP().toString());
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    processRelayPayload(body);
  }
  http.end();
}

// ======================================================
// TRANSMISIÓN DE DATOS (GREENBOX LOCAL / NODRIZA CLOUD)
// ======================================================

void enviar_post() {
  logMessage("INFO", "📦 Transmitiendo telemetría SenML...");

  HTTPClient http;
  String url;
  String ipStr = WiFi.localIP().toString();

  if (!isLoneWolf && backendHost != "") {
    // 1. Modo Enjambre: Reporte a GreenBox Local
    url = "http://" + backendHost + ":" + String(backendPort) +
          "/sensor-data/water";

    DynamicJsonDocument doc(2048);
    JsonArray eArray = SenMLHelper::createPack(
        doc, boxSerialId, "NODO_H2O", FIRMWARE_VERSION_CODE, ipStr.c_str());

    SenMLHelper::addNumber(eArray, "ph", ph_value, "pH", "GPIO32", "Sonda pH");
    SenMLHelper::addNumber(eArray, "ph_volt", ph_voltage, "V", "GPIO32",
                           "Voltaje pH");
    SenMLHelper::addNumber(eArray, "tds", tds_value, "ppm", "GPIO34",
                           "Sonda TDS");
    SenMLHelper::addNumber(eArray, "tds_volt", tds_voltage, "V", "GPIO34",
                           "Voltaje TDS");
    SenMLHelper::addNumber(eArray, "ec", ec_value, "uS/cm", "GPIO34",
                           "Conductividad");
    SenMLHelper::addNumber(eArray, "water_temp", water_temp, "Cel", "GPIO4",
                           "Temp Agua");
    SenMLHelper::addBoolean(eArray, "tank_full", isTankFull(), "GPIO19",
                            "Flotador Nivel");

    for (int i = 0; i < NUM_RELAYS; i++) {
      String gpioLabel = "GPIO" + String(relayPins[i]);
      SenMLHelper::addBoolean(eArray, RELAY_KEYS[i], relayStates[i],
                              gpioLabel.c_str(), RELAY_LABELS[i]);
    }

    doc["boxSerialId"] = boxSerialId;
    doc["ip"] = ipStr;
    doc["localIp"] = ipStr;
    doc["firmwareVersion"] = FIRMWARE_VERSION_CODE;
    doc["ph"] = ph_value;
    doc["ph_voltage"] = ph_voltage;
    doc["tds"] = tds_value;
    doc["tds_voltage"] = tds_voltage;
    doc["ec"] = ec_value;
    doc["water_temp"] = water_temp;
    doc["tank_full"] = isTankFull();

    JsonArray rArr = doc.createNestedArray("relays");
    for (int i = 0; i < NUM_RELAYS; i++) {
      rArr.add(relayStates[i]);
    }

    String jsonBuffer;
    serializeJson(doc, jsonBuffer);

    http.begin(url);
    http.setTimeout(4000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Local-IP", ipStr);

    int httpResponseCode = http.POST(jsonBuffer);
    Serial.printf(F("📡 [GreenBox Local] POST -> Código: %d (IP: %s)\n"),
                  httpResponseCode, ipStr.c_str());
    if (httpResponseCode == 200) {
      String responseBody = http.getString();
      if (responseBody.length() > 2) {
        processRelayPayload(responseBody);
      }
    } else if (httpResponseCode <= 0) {
      rtc_http_errors++;
    }
    http.end();
  } else {
    // 2. Modo Lobo Solitario: Reporte directo a Nodriza Cloud
    url = "https://" + NODRIZA_HOST + "/api/sync";

    DynamicJsonDocument doc(2048);
    doc["carrierId"] = boxSerialId;
    doc["type"] = "ESP32";
    doc["nodeType"] = "NODO_H2O";
    doc["firmwareVersion"] = FIRMWARE_VERSION_CODE;
    doc["ip"] = ipStr;
    doc["localIp"] = ipStr;
    doc["local_ip"] = ipStr;

    JsonArray readings = doc.createNestedArray("readings");
    JsonObject reading = readings.createNestedObject();
    reading["nodeId"] = boxSerialId;
    reading["ip"] = ipStr;
    reading["ph"] = ph_value;
    reading["ph_voltage"] = ph_voltage;
    reading["tds"] = tds_value;
    reading["tds_voltage"] = tds_voltage;
    reading["ec"] = ec_value;
    reading["water_temp"] = water_temp;
    reading["tank_full"] = isTankFull();
    JsonArray rArrRead = reading.createNestedArray("relays");
    for (int i = 0; i < NUM_RELAYS; i++) {
      rArrRead.add(relayStates[i]);
    }

    // Paquete SenML
    JsonArray eArray = SenMLHelper::createPack(
        doc, boxSerialId, "NODO_H2O", FIRMWARE_VERSION_CODE, ipStr.c_str());
    SenMLHelper::addNumber(eArray, "ph", ph_value, "pH", "GPIO32", "Sonda pH");
    SenMLHelper::addNumber(eArray, "ph_volt", ph_voltage, "V", "GPIO32",
                           "Voltaje pH");
    SenMLHelper::addNumber(eArray, "tds", tds_value, "ppm", "GPIO34",
                           "Sonda TDS");
    SenMLHelper::addNumber(eArray, "tds_volt", tds_voltage, "V", "GPIO34",
                           "Voltaje TDS");
    SenMLHelper::addNumber(eArray, "ec", ec_value, "uS/cm", "GPIO34",
                           "Conductividad");
    SenMLHelper::addNumber(eArray, "water_temp", water_temp, "Cel", "GPIO4",
                           "Temp Agua");
    SenMLHelper::addBoolean(eArray, "tank_full", isTankFull(), "GPIO19",
                            "Flotador Nivel");

    for (int i = 0; i < NUM_RELAYS; i++) {
      String gpioLabel = "GPIO" + String(relayPins[i]);
      SenMLHelper::addBoolean(eArray, RELAY_KEYS[i], relayStates[i],
                              gpioLabel.c_str(), RELAY_LABELS[i]);
    }

    JsonArray rArr = doc.createNestedArray("relays");
    for (int i = 0; i < NUM_RELAYS; i++) {
      rArr.add(relayStates[i]);
    }

    String jsonBuffer;
    serializeJson(doc, jsonBuffer);

    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, url);
    http.setTimeout(6000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Local-IP", ipStr);

    int httpResponseCode = http.POST(jsonBuffer);
    Serial.printf(F("🌐 [Nodriza Cloud Directo] POST -> Código: %d (IP: %s)\n"),
                  httpResponseCode, ipStr.c_str());
    if (httpResponseCode <= 0)
      rtc_http_errors++;
    http.end();
  }
}

// ======================================================
// GESTIÓN DE RED Y CONEXIÓN WIFI
// ======================================================

void resetWifiStack() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
}

bool conectar_wifi() {
  if (loadedSsid.length() == 0)
    return false;
  if (WiFi.status() == WL_CONNECTED)
    return true;

  Serial.print(F("\n📡 Conectando a Wi-Fi: "));
  Serial.println(loadedSsid);
  resetWifiStack();
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - inicio < TIEMPO_MAX_CONEXION_WIFI)) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ WiFi Conectado. IP: %s\n"),
                  WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.println(F("\n❌ Falló la conexión a WiFi."));
    rtc_wifi_disconnects++;
    return false;
  }
}

void saveCredentials(const String &ssid, const String &password) {
  preferences.begin(PREFS_NAMESPACE, false);
  preferences.putString(PREF_SSID, ssid);
  preferences.putString(PREF_PASS, password);
  preferences.end();
  loadedSsid = ssid;
  loadedPassword = password;
  Serial.printf(F("💾 Credenciales guardadas: SSID = %s\n"), ssid.c_str());
}

bool loadCredentials() {
  preferences.begin(PREFS_NAMESPACE, true);
  loadedSsid = preferences.getString(PREF_SSID, "");
  loadedPassword = preferences.getString(PREF_PASS, "");
  preferences.end();
  return loadedSsid.length() > 0;
}

void clearCredentials() {
  preferences.begin(PREFS_NAMESPACE, false);
  preferences.remove(PREF_SSID);
  preferences.remove(PREF_PASS);
  preferences.end();
  loadedSsid = "";
  loadedPassword = "";
  Serial.println(F("🗑️ Credenciales borradas de NVS."));
}

bool probarCredencialesWifi(const String &ssid, const String &password,
                            int &estadoFinal) {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long inicio = millis();
  rtc_wifi_disconnects++;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < TIEMPO_MAX_CONEXION_WIFI) {
    esp_task_wdt_reset();
    delay(300);
    dnsServer.processNextRequest();
    server.handleClient();
  }

  estadoFinal = WiFi.status();
  if (estadoFinal == WL_CONNECTED) {
    Serial.printf(F("✅ Prueba WiFi OK. IP: %s\n"),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.printf(F("❌ Prueba WiFi falló. Estado: %d\n"), estadoFinal);
  WiFi.disconnect(true);
  return false;
}

void startConfigPortal() {
  apagarTodosLosRelays();
  resetWifiStack();
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  IPAddress localIP(192, 168, 4, 1);
  WiFi.softAPConfig(localIP, localIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, NULL, 6, 0, 4);

  Serial.printf(F("📡 Portal activo. Red: '%s' -> http://192.168.4.1\n"),
                AP_SSID);
  dnsServer.start(53, "*", localIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/hotspot-detect.html", []() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  unsigned long portalStart = millis();
  while (millis() - portalStart < 600000) { // 10 min timeout
    esp_task_wdt_reset();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }

  Serial.println(F("⏳ Timeout del Portal Cautivo. Reiniciando nodo..."));
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  String html = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
  <title>Nodo Calidad de Agua</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      margin: 0; padding: 16px;
      background: #0a0a0a; color: #e8e8e8;
      min-height: 100vh; display: flex; align-items: center; justify-content: center;
    }
    .card {
      width: 100%; max-width: 420px;
      background: #12181a; border: 1px solid #1c3b44;
      border-radius: 16px; padding: 24px 20px;
      box-shadow: 0 8px 32px rgba(0, 229, 255, 0.12);
    }
    .logo { font-size: 40px; text-align: center; margin-bottom: 8px; }
    h1 { color: #00e5ff; font-size: 22px; text-align: center; margin: 0 0 8px 0; }
    .sub { text-align: center; color: #9eb8bf; font-size: 14px; margin-bottom: 20px; line-height: 1.4; }
    label { display: block; color: #00e5ff; font-size: 13px; font-weight: 600; margin: 12px 0 6px 0; }
    input[type="text"], input[type="password"] {
      width: 100%; padding: 14px 12px; font-size: 16px;
      background: #0a0a0a; color: #fff;
      border: 1px solid #1c3b44; border-radius: 10px;
    }
    input:focus { outline: none; border-color: #00e5ff; box-shadow: 0 0 0 2px rgba(0,229,255,0.2); }
    .btn {
      width: 100%; margin-top: 20px; padding: 15px;
      background: linear-gradient(135deg, #00e5ff, #007791);
      color: #000; font-size: 17px; font-weight: 700;
      border: none; border-radius: 10px; cursor: pointer;
    }
    .hint {
      margin-top: 16px; padding: 10px; border-radius: 8px;
      background: #0d1e24; border-left: 3px solid #00e5ff;
      font-size: 12px; color: #a4c9d4; line-height: 1.5;
    }
    .footer { text-align: center; margin-top: 18px; font-size: 12px; color: #526f78; }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">💧</div>
    <h1>Nodo Calidad de Agua</h1>
    <p class="sub">Configuración Wi-Fi para telemetría SenML y control remoto de relés.</p>
    <form method="POST" action="/save" enctype="application/x-www-form-urlencoded">
      <label for="ssid">Nombre de la red (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="MiRedWiFi" autocomplete="off" autocapitalize="none" spellcheck="false">
      <label for="password">Contraseña Wi-Fi</label>
      <input type="password" id="password" name="password" placeholder="Contraseña de la red" autocomplete="new-password" autocapitalize="none">
      <input class="btn" type="submit" value="Probar y Guardar">
    </form>
    <div class="hint">
      Usa red <strong>2.4 GHz</strong>. Mantén <strong>BOOT (GPIO 0)</strong> al encender para volver a este portal.
    </div>
    <div class="footer">Firmware v)raw" +
                String(FIRMWARE_VERSION_CODE) + R"raw(</div>
  </div>
</body>
</html>
)raw";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  if (newPassword.length() == 0)
    newPassword = server.arg("pass");
  if (newPassword.length() == 0)
    newPassword = server.arg("p");
  newSsid.trim();
  newPassword.trim();

  Serial.printf(F("📥 Portal recibió: SSID='%s', pass_len=%d\n"),
                newSsid.c_str(), newPassword.length());

  if (newSsid.length() == 0) {
    server.send(400, "text/html",
                "<html><body "
                "style='background:#0a0a0a;color:#fff;text-align:center;"
                "padding:40px;'><h1 style='color:#ff5252;'>SSID vacío</h1><a "
                "style='color:#00e5ff;' href='/'>Volver</a></body></html>");
    return;
  }

  int estadoFinal = WL_DISCONNECTED;
  bool conecto = probarCredencialesWifi(newSsid, newPassword, estadoFinal);

  if (!conecto) {
    String errorHtml = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Error WiFi</title>
  <style>
    body { font-family: sans-serif; background:#0a0a0a; color:#e8e8e8; text-align:center; padding:32px 20px; }
    h1 { color:#ff5252; font-size:22px; }
    p { color:#bdbdbd; line-height:1.6; }
    .code { color:#00e5ff; font-weight:bold; }
    a { display:inline-block; margin-top:20px; padding:14px 24px; background:#00e5ff; color:#000; text-decoration:none; border-radius:10px; font-weight:700; }
  </style>
</head>
<body>
  <h1>❌ No se pudo conectar</h1>
  <p>SSID: <strong>)raw" +
                       newSsid + R"raw(</strong></p>
  <p>Estado WiFi: <span class="code">)raw" +
                       String(estadoFinal) + R"raw(</span></p>
  <p>Revisa que el SSID y contraseña sean correctos y pertenezcan a una red <strong>2.4 GHz</strong>.</p>
  <a href='/'>Intentar de nuevo</a>
</body>
</html>
)raw";
    server.send(200, "text/html", errorHtml);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    return;
  }

  saveCredentials(newSsid, newPassword);

  String successHtml = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guardado</title>
  <style>
    body { font-family: sans-serif; background:#0a0a0a; color:#e8e8e8; text-align:center; padding:40px 20px; }
    h1 { color:#00e5ff; }
    p { color:#bdbdbd; line-height:1.6; }
    .ssid { color:#00e5ff; font-weight:bold; }
  </style>
</head>
<body>
  <h1>✅ Conexión exitosa</h1>
  <p>Red: <span class="ssid">)raw" +
                       newSsid + R"raw(</span></p>
  <p>Credenciales guardadas correctamente. Reiniciando nodo...</p>
</body>
</html>
)raw";
  server.send(200, "text/html", successHtml);
  delay(1500);
  server.stop();
  dnsServer.stop();
  resetWifiStack();
  ESP.restart();
}

// ======================================================
// CONFIGURACIÓN DINÁMICA Y OTA (NODRIZA)
// ======================================================

void obtener_remote_config() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  esp_task_wdt_reset();

  Serial.println(F("📥 Consultando configuración dinámica..."));

  String url;
  if (!isLoneWolf && backendHost != "") {
    url = "http://" + backendHost + ":" + String(backendPort) + "/api/nodes/" + boxSerialId +
          "/config?ip=" + WiFi.localIP().toString() + "&fw=" + FIRMWARE_VERSION_CODE;
  } else {
    url = "https://" + NODRIZA_HOST + "/api/nodes/" + boxSerialId +
          "/config?ip=" + WiFi.localIP().toString() + "&fw=" + FIRMWARE_VERSION_CODE;
  }

  HTTPClient http;
  http.setTimeout(5000);
  
  if (url.startsWith("https")) {
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
  } else {
    http.begin(url);
  }

  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      isLoneWolf = doc["remote_config"]["is_lone_wolf"] | false;
      if (!isLoneWolf && doc["remote_config"]["backend_host"] &&
          !doc["remote_config"]["backend_host"].isNull()) {
        backendHost = doc["remote_config"]["backend_host"].as<String>();
        backendPort = doc["remote_config"]["backend_port"] | 3000;

        Preferences prefs;
        prefs.begin(PREFS_NAMESPACE, false);
        prefs.putString("bHost", backendHost);
        prefs.putInt("bPort", backendPort);
        prefs.putBool("isLoneWolf", false);
        prefs.end();

        Serial.println(
            F("✅ Asignado a GreenBox (Enjambre). Guardado en NVS."));
        Serial.printf(F("   -> IP GreenBox Local: %s:%d\n"),
                      backendHost.c_str(), backendPort);
      } else {
        backendHost = "";
        isLoneWolf = true;
        Preferences prefs;
        prefs.begin(PREFS_NAMESPACE, false);
        prefs.putString("bHost", "");
        prefs.putBool("isLoneWolf", true);
        prefs.end();
        Serial.println(F(
            "🐺 Modo Lobo Solitario (Reporta directamente a Nodriza Cloud)."));
      }

      if (doc["remote_config"]["intervalo_envio_ms"]) {
        intervaloEnvioMs =
            doc["remote_config"]["intervalo_envio_ms"].as<long>();
      }
      if (doc["remote_config"]["flag_activo"]) {
        flagActivo = doc["remote_config"]["flag_activo"].as<bool>();
      }
    }
  } else {
    Serial.printf(
        F("❌ Error al obtener config (Código HTTP: %d)\n"), code);
  }
  http.end();
  esp_task_wdt_reset();
}

bool check_for_update() {
  if (WiFi.status() != WL_CONNECTED)
    return false;
  esp_task_wdt_reset();
  Serial.println(F("[OTA] Buscando actualizaciones..."));

  String otaUrl;
  if (!isLoneWolf && backendHost != "") {
    otaUrl = "http://" + backendHost + ":" + String(backendPort) + "/api/ota/check/" + boxSerialId +
             "?ip=" + WiFi.localIP().toString() + "&fw=" + FIRMWARE_VERSION_CODE;
  } else {
    otaUrl = "https://" + NODRIZA_HOST + "/api/ota/check/" + boxSerialId +
             "?ip=" + WiFi.localIP().toString() + "&fw=" + FIRMWARE_VERSION_CODE;
  }

  esp_task_wdt_delete(NULL);
  t_httpUpdate_return ret;
  
  if (otaUrl.startsWith("https")) {
    WiFiClientSecure otaClient;
    otaClient.setInsecure();
    ret = httpUpdate.update(otaClient, otaUrl, FIRMWARE_VERSION_CODE);
  } else {
    WiFiClient client;
    ret = httpUpdate.update(client, otaUrl, FIRMWARE_VERSION_CODE);
  }

  if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println(F("[OTA] Firmware al día (304 No Updates)."));
  } else if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("[OTA] Error (%d): %s\n", httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
  } else if (ret == HTTP_UPDATE_OK) {
    Serial.println(
        F("🚀 ¡Actualización OTA completada con éxito! Reiniciando..."));
    ESP.restart();
  }
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();
  return false;
}

void logMessage(String level, String msg) {
  Serial.println("[" + level + "] " + msg);
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) +
                 "/sensor-data/logs";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512);
    doc["boxSerialId"] = boxSerialId;
    doc["level"] = level;
    doc["message"] = msg;

    String jsonStr;
    serializeJson(doc, jsonStr);
    http.POST(jsonStr);
    http.end();
  }
}
