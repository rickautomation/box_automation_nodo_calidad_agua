/*
 * SKETCH: nodo_h2o.ino
 * OBJETIVO: Versión final con CONFIGURACIÓN WIFI VÍA PORTAL CAUTIVO, 
 * Configuración Dinámica desde Firebase y Actualización OTA.
 * * MEJORA: Implementación de la función "Reset de Credenciales"
 * usando el botón físico BOOT (GPIO 9) del ESP32-C3 Mini.
 */

#include <WiFi.h>              
#include <HTTPClient.h>        
#include <ArduinoJson.h>       
#include <Update.h>            
#include <WiFiClientSecure.h>  
#include <Preferences.h>        
#include <WebServer.h>          
#include <DNSServer.h>          
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>


// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE (DEFINE LA VERSIÓN ACTUAL)
// ======================================================
const char* FIRMWARE_VERSION_CODE = "1.0.5"; // Versión incrementada

// ======================================================
// 1. CONFIGURACIÓN DE RED, FIREBASE Y PORTAL CAUTIVO
// ======================================================

// ⚠️ REEMPLAZAR CON TUS CLAVES Y HOST
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8"; 
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";                   

// 🔑 CREDENCIALES POR DEFECTO PARA FORZAR CONEXIÓN INICIAL 🔑
const char* DEFAULT_SSID = "tili";         
const char* DEFAULT_PASS = "Ubuntu1234$"; 


// 🛠️ VARIABLES GLOBALES PARA EL PORTAL CAUTIVO Y NVS
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

// Claves de almacenamiento persistente
const char* PREFS_NAMESPACE = "wifi_config";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";
const char* AP_SSID = "NODO_H2O_SETUP"; // SSID del Punto de Acceso para configuración

// Variables de credenciales leídas o ingresadas
String loadedSsid = "";
String loadedPassword = "";

// 🛠️ PIN DE RESETEO DE WIFI: USAMOS EL BOTÓN FÍSICO "BOOT" (GPIO 9)
const int WIFI_RESET_PIN = 9; 


// ======================================================
// 2. VARIABLES DE CONFIGURACIÓN DINÁMICA (LEÍDAS DE FIREBASE)
// ======================================================
// Valores por defecto (Fallback) 
String backendHost = "192.168.68.54";    
int backendPort = 3000;                  
String endpointCalidadAgua = "/sensor-data/arduino/batch"; 
long intervaloEnvioMs = 60000;           
bool flagActivo = true;                  
String latestFirmwareVersion = "0.0.0";  
String remoteFirmwareVersion = "0.0.0"; 
String firmwareUrl = "";                 

const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";


// ======================================================
// 3. DATOS DEL DISPOSITIVO Y SENSORES 
// ======================================================
const char* BOX_SERIAL_ID = "eea11eb7-e5eb-45d7-be52-69ff8d15e6e-AGUA"; 
const char* NODE_TYPE_KEY = "NODO_H2O"; 

const int PH_PIN = 5;       
const int TDS_PIN = 4;      

const int TIEMPO_MAX_CONEXION_WIFI = 20000; 

// Parámetros ADC y Calibración
const int ADC_MAX_VALUE = 4095;
const float ADC_VOLTAGE_REF = 3.3; 
const float PH_V4 = 0.4;    
const float PH_V7 = 1.1;    
float ph_slope = 0;         
float ph_offset = 0;        

// Variables de Lectura
int ph_raw = 0;
float ph_voltage = 0.0;
float ph_value = 0.0; 
int tds_raw = 0;
float tds_voltage = 0.0;
float tds_value = 0.0; 


// ======================================================
// 4. GESTIÓN DEL TIEMPO 
// ======================================================
unsigned long lastConfigFetch = 0; 
const long CONFIG_FETCH_INTERVAL = 60000; 

// Declaraciones de funciones
void configurar_adc();
void calcular_calibracion_ph();
void leer_sensores_agua();
bool conectar_wifi();
void enviar_post_batch();
bool obtener_remote_config(); 
int compareVersions(String current, String remote);
bool check_for_update();
void perform_update();

// 🛠️ Funciones del Portal Cautivo y NVS
void saveCredentials(const String& ssid, const String& password);
bool loadCredentials();
void clearCredentials(); 
void startConfigPortal();
void handleRoot();
void handleSave();
void logMessage(String level, String msg);



// ======================================================
// SETUP: Inicialización de Sensores y Config
// ======================================================
void setup() {
  Serial.begin(115200); 
  delay(1000); 
  
  configurar_adc();
  calcular_calibracion_ph();

  latestFirmwareVersion = String(FIRMWARE_VERSION_CODE); 
  
  Serial.println(F("\n--- 💧 Nodo de Monitoreo de Agua ---"));
  Serial.printf(F("VERSIÓN ACTUAL (Local): %s\n"), latestFirmwareVersion.c_str());
  
  // Iniciar Hardware Watchdog (30 segundos) - API v3.x (ESP-IDF v5)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);

  preferences.begin(PREFS_NAMESPACE, false);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  delay(100); 


  
  // 3. LÓGICA DE RESET MEJORADA: Si BOOT (GPIO 9) está presionado al inicio, forzar AP
  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    Serial.println(F("🚨 BOTÓN BOOT DETECTADO (GPIO 9 LOW) AL INICIO. BORRANDO CREDENCIALES..."));
    clearCredentials(); 
    Serial.println(F("📡 Iniciando Portal Cautivo INMEDIATAMENTE..."));
    // Al llamar a startConfigPortal(), el código se detendrá en el bucle del portal, 
    // y no continuará con la lógica de conexión normal.
    startConfigPortal(); 
    // Si el portal termina (por un reinicio), el setup comenzará de nuevo.
  }
  
  // -- A partir de aquí solo se ejecuta si el botón BOOT NO fue presionado --
  
  // 4. INTENTAR CARGAR CREDENCIALES GUARDADAS
  bool credentialsLoaded = loadCredentials();
  
  // 🔑 LÓGICA DE FALLBACK: Si no hay credenciales, fuerza las predeterminadas y las guarda
  if (!credentialsLoaded) {
      Serial.println(F("🟡 INFO: No hay credenciales guardadas. Forzando credenciales por defecto..."));
      // Guardar las credenciales por defecto para el primer intento de conexión
      saveCredentials(DEFAULT_SSID, DEFAULT_PASS); 
      loadCredentials();
      credentialsLoaded = true; 
  }
  
  if (credentialsLoaded && conectar_wifi()) {
      ArduinoOTA.begin();
      logMessage("INFO", "✅ Conexión Wi-Fi exitosa con credenciales guardadas.");
      obtener_remote_config();
      check_for_update();
      lastConfigFetch = millis();
  } else {
      logMessage("ERROR", "❌ Fallo al conectar con credenciales.");
      startConfigPortal();
  }
}


// ======================================================
// FUNCIONES DEL PORTAL CAUTIVO Y NVS
// ======================================================

/**
 * @brief Guarda SSID y Password en la memoria NVS.
 */
void saveCredentials(const String& ssid, const String& password) {
  preferences.putString(PREF_SSID, ssid);
  preferences.putString(PREF_PASS, password);
  loadedSsid = ssid;
  loadedPassword = password;
  Serial.printf(F("💾 Credenciales guardadas: SSID = %s\n"), ssid.c_str());
}

/**
 * @brief Carga SSID y Password de la memoria NVS.
 * @return true si se encontraron credenciales válidas.
 */
bool loadCredentials() {
  loadedSsid = preferences.getString(PREF_SSID, "");
  loadedPassword = preferences.getString(PREF_PASS, "");
  
  if (loadedSsid.length() > 0) {
    Serial.printf(F("📝 Credenciales cargadas: SSID = %s\n"), loadedSsid.c_str());
    return true;
  }
  return false;
}

/**
 * @brief Borra las credenciales de Wi-Fi de la NVS (SSID y PASS).
 */
void clearCredentials() {
    preferences.remove(PREF_SSID);
    preferences.remove(PREF_PASS);
    loadedSsid = "";
    loadedPassword = "";
    Serial.println(F("🗑️ CREDENCIALES BORRADAS DE NVS."));
}


/**
 * @brief Inicializa el Access Point y el Servidor Web para la configuración.
 */
void startConfigPortal() {
  // Configura el ESP como Access Point (AP)
  WiFi.mode(WIFI_AP);
  // IP del AP: 192.168.4.1
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  
  WiFi.softAPConfig(localIP, gateway, subnet);
  WiFi.softAP(AP_SSID);
  
  Serial.printf(F("AP creado. Conéctate a '%s' para configurar.\n"), AP_SSID);
  Serial.println(F("IP del portal: 192.168.4.1"));

  // Iniciar DNS (redirige todas las peticiones a la IP del portal)
  dnsServer.start(53, "*", localIP);
  
  // Rutas del servidor web
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  // Bucle infinito del portal (se sale con ESP.restart() en handleSave)
  while (true) {
    esp_task_wdt_reset();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }
}


/**
 * @brief Sirve la página HTML del formulario.
 */
void handleRoot() {
  String html = R"raw(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Configuracion NODO H2O</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #f4f7f6; }
    .container { max-width: 400px; margin: auto; padding: 25px; background: #ffffff; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
    h1 { color: #00796B; margin-bottom: 20px; font-size: 24px; }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 12px;
      margin: 10px 0 20px 0;
      display: inline-block;
      border: 1px solid #ccc;
      border-radius: 6px;
      box-sizing: border-box;
      font-size: 16px;
    }
    input[type="submit"] {
      background-color: #00796B;
      color: white;
      padding: 14px 20px;
      margin: 8px 0;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      width: 100%;
      font-size: 18px;
      transition: background-color 0.3s;
    }
    input[type="submit"]:hover { background-color: #004D40; }
    .footer { margin-top: 20px; color: #757575; font-size: 14px; }
    .logo { color: #00796B; font-size: 30px; margin-bottom: 10px; }
  </style>
</head>
<body>
<div class="container">
  <div class="logo">💧</div>
  <h1>Configura tu Nodo H2O</h1>
  <p>Conéctate a tu red Wi-Fi para que el nodo pueda enviar datos.</p>
  <p style="font-size: 12px; color: #B00020; font-weight: bold;">
    MANTÉN PRESIONADO BOOT AL INICIAR para borrar credenciales y entrar aquí.
  </p>
  <form method="POST" action="/save">
    <label for="ssid">SSID (Nombre de la Red):</label>
    <input type="text" id="ssid" name="ssid" required placeholder="MiRedWiFi">
    <label for="password">Contraseña:</label>
    <input type="password" id="password" name="password" placeholder="Dejar vacío si no tiene clave">
    <input type="submit" value="Guardar y Conectar">
  </form>
  <div class="footer">Version Firmware: )raw" + String(FIRMWARE_VERSION_CODE) + R"raw(</div>
</div>
</body>
</html>
)raw";
  server.send(200, "text/html", html);
}

/**
 * @brief Procesa el formulario, guarda las credenciales y reinicia.
 */
void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  
  if (newSsid.length() > 0) {
    saveCredentials(newSsid, newPassword);
    
    String successHtml = R"raw(
      <!DOCTYPE html><html><head><meta http-equiv="refresh" content="5;url=/" /></head><body>
      <div style="text-align: center; margin-top: 50px;">
        <h1>✅ Credenciales Guardadas</h1>
        <p>Intentando conectar a la red: <strong>)raw" + newSsid + R"raw(</strong></p>
        <p>El nodo se reiniciará en 5 segundos para aplicar la nueva configuración.</p>
      </div>
      </body></html>
    )raw";
    server.send(200, "text/html", successHtml);
    
    // Finaliza el servidor, libera la memoria y reinicia
    server.stop();
    dnsServer.stop();
    Serial.println(F("🔄 Reiniciando ESP32..."));
    ESP.restart();
  } else {
    server.send(200, "text/html", "<h1>❌ ERROR: SSID vacío.</h1><p>Vuelve al portal e introduce un nombre de red válido.</p>");
  }
}


// ======================================================
// FUNCIONES DE CONEXIÓN WIFI 
// ======================================================


void resetWifiStack() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
}
bool conectar_wifi() {
  Serial.print(F("\n📡 Encendiendo Wi-Fi y conectando..."));
  resetWifiStack();
  WiFi.mode(WIFI_STA);
  
  // Usar las credenciales cargadas/guardadas
  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio < TIEMPO_MAX_CONEXION_WIFI)) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ WiFi Conectado. IP: %s\n"), WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.printf(F("\n❌ Falló la conexión a WiFi después de %d ms.\n"), TIEMPO_MAX_CONEXION_WIFI);
    return false;
  }
}

// ----------------------------------------------------
// FUNCIONES DE CONFIGURACIÓN VÍA REST API Y OTA (SIN CAMBIOS FUNCIONALES)
// ----------------------------------------------------

// Compara versiones en formato "X.Y.Z"
int compareVersions(String current, String remote) {
  int cur_v[3] = {0, 0, 0};
  int rem_v[3] = {0, 0, 0};

  sscanf(current.c_str(), "%d.%d.%d", &cur_v[0], &cur_v[1], &cur_v[2]);
  sscanf(remote.c_str(), "%d.%d.%d", &rem_v[0], &rem_v[1], &rem_v[2]);

  for (int i = 0; i < 3; i++) {
    if (cur_v[i] < rem_v[i]) return -1;
    if (cur_v[i] > rem_v[i]) return 1;
  }
  return 0; 
}

// Verifica si la versión remota es superior a la versión actual
bool check_for_update() {
  if (remoteFirmwareVersion.isEmpty() || remoteFirmwareVersion == "0.0.0") {
    Serial.println(F("🟡 OTA Skip: Versión remota no válida."));
    return false;
  }

  int comparison = compareVersions(latestFirmwareVersion, remoteFirmwareVersion);

  if (comparison < 0) {
    Serial.printf(F("🔴 📢 ACTUALIZACIÓN REQUERIDA: Versión local %s -> Remota %s\n"), latestFirmwareVersion.c_str(), remoteFirmwareVersion.c_str());
    if (!firmwareUrl.isEmpty()) {
      perform_update();
      return true;
    } else {
      Serial.println(F("❌ ERROR OTA: URL de firmware vacía. No se puede actualizar."));
      return false;
    }
  } else {
    Serial.printf(F("✅ OTA: La versión actual (%s) está al día.\n"), latestFirmwareVersion.c_str());
    return false;
  }
}

// Realiza la descarga y flasheo del firmware
void perform_update() {
  Serial.printf(F("🚀 Iniciando actualización OTA desde: %s\n"), firmwareUrl.c_str());
  
  if (!firmwareUrl.startsWith("https://")) {
      Serial.println(F("❌ ERROR: La URL del firmware no es HTTPS. Se requiere HTTPS para OTA."));
      return;
  }

  WiFiClientSecure client;
  client.setInsecure(); 
  
  HTTPClient http;
  
  if (http.begin(client, firmwareUrl)) {
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      int contentLength = http.getSize();
      Serial.printf(F("Tamaño del nuevo firmware: %d bytes.\n"), contentLength);
      
      bool canBegin = Update.begin(contentLength);
      
      if (canBegin) {
        Serial.println(F("Iniciando proceso de flasheo..."));
        
        WiFiClient* stream = http.getStreamPtr(); 
        size_t written = Update.writeStream(*stream);
        
        if (written == contentLength) {
          Serial.printf(F("Descarga y escritura completada: %d bytes.\n"), written);
        } else {
          Serial.printf(F("❌ Error de escritura: Escrito %zu de %d bytes.\n"), written, contentLength);
        }
        
        if (Update.end()) {
          Serial.println(F("✅ Actualización finalizada exitosamente. Reiniciando..."));
          ESP.restart(); 
        } else {
          Serial.printf(F("❌ Error al finalizar la actualización. Error: %d. Mensaje: %s\n"), Update.getError(), Update.errorString());
        }
      } else {
        Serial.println(F("❌ ERROR: No hay suficiente espacio para la actualización."));
      }
    } else {
      Serial.printf(F("❌ ERROR HTTP (%d): No se pudo descargar el archivo de firmware. URL: %s\n"), httpCode, firmwareUrl.c_str());
    }
    http.end();
  } else {
    Serial.println(F("❌ ERROR: No se pudo conectar a la URL de firmware."));
  }
}


bool obtener_remote_config() {
  Serial.println(F("\n--- Obteniendo Configuración Dinámica (Vía REST API) ---"));
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("❌ WiFi no conectado. Saltando fetch de config."));
    return false;
  }
  
  String fullUrl = RTDB_CONFIG_URL_BASE + "?auth=" + String(API_KEY); 
  long oldIntervaloEnvioMs = intervaloEnvioMs; 

  HTTPClient http;
  http.begin(fullUrl); 
  http.setTimeout(3000);
  
  int httpCode = http.GET();
  
  if (httpCode == 200) { 
    Serial.printf(F("✅ Configuración obtenida. Código HTTP: %d\n"), httpCode);
    String payload = http.getString();
    
    DynamicJsonDocument doc(1536); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.printf(F("❌ Fallo al parsear JSON: %s\n"), error.c_str());
      http.end();
      return false;
    }

    JsonObject remoteConfig = doc[F("remote_config")];
    if (remoteConfig.isNull()) {
        Serial.println(F("❌ Fallo: Objeto 'remote_config' no encontrado. Usando fallbacks."));
    } else {
        if (remoteConfig.containsKey(F("backend_host")) && remoteConfig[F("backend_host")].is<String>()) {
          backendHost = remoteConfig[F("backend_host")].as<String>();
        }
        if (remoteConfig.containsKey(F("backend_port")) && remoteConfig[F("backend_port")].is<int>()) {
          backendPort = remoteConfig[F("backend_port")].as<int>();
        }
        if (remoteConfig.containsKey(F("endpoint_calidad_agua")) && remoteConfig[F("endpoint_calidad_agua")].is<String>()) {
          endpointCalidadAgua = remoteConfig[F("endpoint_calidad_agua")].as<String>();
        }
        if (remoteConfig.containsKey(F("flag_activo")) && remoteConfig[F("flag_activo")].is<bool>()) {
          flagActivo = remoteConfig[F("flag_activo")].as<bool>();
        }
    }

    JsonObject nodeConfig = doc[F("firmware_updates")][NODE_TYPE_KEY];
    if (nodeConfig.isNull()) {
        Serial.printf(F("❌ Fallo: Configuración de nodo '%s' no encontrada bajo 'firmware_updates'. Usando fallbacks.\n"), NODE_TYPE_KEY);
    } else {
        if (nodeConfig.containsKey(F("intervalo_envio_ms")) && nodeConfig[F("intervalo_envio_ms")].is<long>()) {
          long newIntervalo = nodeConfig[F("intervalo_envio_ms")].as<long>();
          if (newIntervalo != oldIntervaloEnvioMs) {
             intervaloEnvioMs = newIntervalo;
             Serial.printf(F("🟢 LOG: INTERVALO ACTUALIZADO: Nuevo valor remoto = %ld ms\n"), intervaloEnvioMs);
          } 
        }

        if (nodeConfig.containsKey(F("latest_firmware_version")) && nodeConfig[F("latest_firmware_version")].is<String>()) {
          remoteFirmwareVersion = nodeConfig[F("latest_firmware_version")].as<String>();
        }

        if (nodeConfig.containsKey(F("firmware_url")) && nodeConfig[F("firmware_url")].is<String>()) {
          firmwareUrl = nodeConfig[F("firmware_url")].as<String>();
        }
    }


    Serial.println(F("------------------------------------------"));
    Serial.println(F("Configuración Dinámica Aplicada:"));
    Serial.printf(F("Intervalo (ms) FINAL: %ld\n"), intervaloEnvioMs);
    Serial.printf(F("Ver. Remota OTA: %s\n"), remoteFirmwareVersion.c_str());
    Serial.println(F("------------------------------------------"));
    
    http.end();
    return true;
  } else {
    Serial.printf(F("❌ Fallo al obtener la configuración (HTTP Code: %d). Usando valores por defecto.\n"), httpCode);
    http.end();
    return false;
  }
}


// ----------------------------------------------------
// FUNCIÓN DE ENVÍO POST 
// ----------------------------------------------------
void enviar_post_batch() {
  if (!flagActivo) {
    Serial.println(F("❌ Envío omitido: Flag de envío inactiva (configuración remota)."));
    return;
  }
  
  Serial.println(F("📦 Preparando JSON y envío POST..."));
  DynamicJsonDocument doc(1024); 
  
  doc["boxSerialId"] = BOX_SERIAL_ID;
  JsonArray dataArray = doc.createNestedArray("data");
  
  // 1. DATOS DE pH (Valor de pH CALCULADO) - PIN 5
  JsonObject phValueItem = dataArray.createNestedObject();
  phValueItem["arduinoPin"] = String(PH_PIN);
  phValueItem["raw"] = (int)(ph_value * 100); 
  phValueItem["unit"] = "ph";
  phValueItem["key"] = "ph_value";

  // 2. DATOS DE pH (Voltaje) - PIN 5
  JsonObject phVoltItem = dataArray.createNestedObject();
  phVoltItem["arduinoPin"] = String(PH_PIN);
  phVoltItem["raw"] = (int)(ph_voltage * 1000); 
  phVoltItem["unit"] = "mV";
  phVoltItem["key"] = "ph_voltaje";

  // 3. DATOS DE TDS (Voltaje) - PIN 4
  JsonObject tdsVoltItem = dataArray.createNestedObject();
  tdsVoltItem["arduinoPin"] = String(TDS_PIN); 
  tdsVoltItem["raw"] = (int)(tds_voltage * 1000); 
  tdsVoltItem["unit"] = "mV";
  tdsVoltItem["key"] = "tds_voltaje";
  
  String jsonBuffer;
  serializeJson(doc, jsonBuffer);
  
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + endpointCalidadAgua; 
    
    Serial.printf(F("URL de envío: %s\n"), url.c_str());
    
    http.setConnectTimeout(15000); 
    
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    
    int httpResponseCode = http.POST(jsonBuffer);
    
    if (httpResponseCode == 200) {
      Serial.printf(F("✅ POST exitoso. Código: %d\n"), httpResponseCode);
    } else {
      Serial.printf(F("❌ Error en el POST. Código: %d.\n"), httpResponseCode);
    }
    http.end();
  }
}

// ----------------------------------------------------
// FUNCIONES DE LECTURA Y CALIBRACIÓN
// ----------------------------------------------------

void configurar_adc() {
  // Configuración por defecto para ESP32-C3 (12 bits)
}

void calcular_calibracion_ph() {
    ph_slope = (4.0 - 7.0) / (PH_V4 - PH_V7);
    ph_offset = 7.0 - (ph_slope * PH_V7);
    
    Serial.printf(F("CALIBRACIÓN PH: Pendiente: %.2f, Intersección: %.2f\n"), ph_slope, ph_offset);
}


void leer_sensores_agua() {
  
  // Oversampling de pH (10 lecturas)
  long sum_ph = 0;
  for (int i=0; i<10; i++) {
    sum_ph += analogRead(PH_PIN);
    delay(2);
  }
  ph_raw = sum_ph / 10;
  ph_voltage = (float)ph_raw * (ADC_VOLTAGE_REF / ADC_MAX_VALUE);
  ph_value = ph_slope * ph_voltage + ph_offset;

  if (ph_raw <= 5 || ph_raw >= ADC_MAX_VALUE - 5) {
      logMessage("WARNING", "⚠️ ALERTA PH: Lectura en extremos. Revisa conexión.");
  }
  
  // Oversampling de TDS (10 lecturas)
  long sum_tds = 0;
  for (int i=0; i<10; i++) {
    sum_tds += analogRead(TDS_PIN);
    delay(2);
  }
  tds_raw = sum_tds / 10;
  tds_voltage = (float)tds_raw * (ADC_VOLTAGE_REF / ADC_MAX_VALUE);
  tds_value = 0.0; 
  
  if (tds_raw == 0) {
      logMessage("ERROR", "❌ ERROR TDS: Lectura RAW es CERO (0).");
  }

  
  Serial.printf(F("   PH: %.2f pH / TDS: %.3f V\n"), ph_value, tds_voltage);
}


void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();

  unsigned long tiempoActual = millis();
  static unsigned long lastRun = 0;
  
  if (tiempoActual - lastConfigFetch >= CONFIG_FETCH_INTERVAL) {
      if (conectar_wifi()) {
          obtener_remote_config(); 
          check_for_update();
      }
      lastConfigFetch = tiempoActual;
  }

  if (tiempoActual - lastRun >= intervaloEnvioMs) {
    lastRun = tiempoActual;

    leer_sensores_agua();

    if (conectar_wifi()) {
        enviar_post_batch();
    } else {
        logMessage("ERROR", "❌ Fallo la conexión WiFi al enviar.");
    }
  }
}

void logMessage(String level, String msg) {
  Serial.println("[" + level + "] " + msg);
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + "/sensor-data/logs";
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