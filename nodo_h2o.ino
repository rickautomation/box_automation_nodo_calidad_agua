/*
 * SKETCH: arduino_remote_config_c5_final.ino
 * OBJETIVO: Versión final del código con configuración dinámica totalmente funcional
 * y corrección de pines/host, implementando la estructura jerárquica de la RTDB.
 * * * CORRECCIONES:
 * 1. PINES: PH en GPIO 5 y TDS en GPIO 4.
 * 2. CONFIG: Se corrigió el RTDB_HOST para incluir '-default-rtdb'.
 * 3. CONFIG: La ruta de lectura se corrigió a '/.json' para la configuración en la raíz.
 * 4. PARSING: Lógica de JSON actualizada para leer configuración anidada:
 * - Parámetros generales: bajo "remote_config"
 * - Intervalo de envío: bajo "firmware_updates/[NODE_TYPE_KEY]"
 */

#include <WiFi.h>              
#include <HTTPClient.h>        
#include <ArduinoJson.h>       

// ======================================================
// 1. CONFIGURACIÓN DE LA RED WIFI Y FIREBASE
// ======================================================
// ⚠️ MODIFICAR CON TU RED
const char* ssid = "tili";         
const char* password = "Ubuntu1234$"; 

// ⚠️ REEMPLAZAR CON TUS CLAVES Y HOST
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8"; 
// 🟢 HOST CORREGIDO: Usando el dominio completo de Firebase RTDB
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";                   

// ======================================================
// 2. VARIABLES DE CONFIGURACIÓN DINÁMICA (LEÍDAS DE FIREBASE)
// ======================================================
// Valores por defecto (Fallback) 
String backendHost = "192.168.68.54";    
int backendPort = 3000;                  
String endpointCalidadAgua = "/sensor-data/arduino/batch"; 
long intervaloEnvioMs = 60000;           // Este valor se sobrescribe con la config de NODO_AGUA
bool flagActivo = true;                  

// 🟢 URL BASE: Apunta a la raíz para leer todo el objeto de configuración
const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";


// ======================================================
// 3. DATOS DEL DISPOSITIVO Y SENSORES (Parámetros)
// ======================================================
const char* BOX_SERIAL_ID = "eea11eb7-e5eb-45d7-be52-69ff8d15e6e-AGUA"; 
// 🔑 CLAVE ÚNICA DEL NODO en la sección 'firmware_updates' de Firebase.
// Si este nodo es 'NODO_SUELO' u otro, DEBES cambiar esta constante.
const char* NODE_TYPE_KEY = "NODO_AGUA"; 

// 🟢 PINES CORREGIDOS DEFINITIVAMENTE: 
const int PH_PIN = 5;       // PH en GPIO 5
const int TDS_PIN = 4;      // TDS en GPIO 4

const int TIEMPO_MAX_CONEXION_WIFI = 20000; 

// Parámetros ADC y Calibración
const int ADC_MAX_VALUE = 4095;
const float ADC_VOLTAGE_REF = 3.3; 
// ⚠️ CALIBRACIÓN PH: Reemplaza con tus valores reales
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
const long CONFIG_FETCH_INTERVAL = 60000; // 60 segundos

// Declaraciones de funciones
void configurar_adc();
void calcular_calibracion_ph();
void leer_sensores_agua();
bool conectar_wifi();
void enviar_post_batch();
bool obtener_remote_config(); 


// ======================================================
// SETUP: Inicialización de Sensores y Config
// ======================================================
void setup() {
  Serial.begin(115200); 
  delay(1000); 
  
  configurar_adc();
  calcular_calibracion_ph();

  Serial.println(F("\n--- 💧 Nodo de Monitoreo de Agua con Configuración Dinámica (REST) ---"));
  Serial.printf(F("ID: %s\n"), BOX_SERIAL_ID);
  
  if (conectar_wifi()) {
      obtener_remote_config();
      lastConfigFetch = millis();
  }
}

// ----------------------------------------------------
// FUNCIONES DE CONFIGURACIÓN VÍA REST API
// ----------------------------------------------------

bool obtener_remote_config() {
  Serial.println(F("\n--- Obteniendo Configuración Dinámica (Vía REST API) ---"));
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("❌ WiFi no conectado. Saltando fetch de config."));
    return false;
  }
  
  // La clave de autenticación se añade para asegurar el acceso de lectura
  String fullUrl = RTDB_CONFIG_URL_BASE + "?auth=" + String(API_KEY); 
  
  Serial.printf(F("Nodo a buscar en RTDB: %s\n"), NODE_TYPE_KEY);
  
  // Guardamos el intervalo anterior para reportar si hubo un cambio
  long oldIntervaloEnvioMs = intervaloEnvioMs; 

  HTTPClient http;
  http.begin(fullUrl); 
  
  int httpCode = http.GET();
  
  if (httpCode == 200) { 
    Serial.printf(F("✅ Configuración obtenida. Código HTTP: %d\n"), httpCode);
    String payload = http.getString();
    
    // ⬆️ Aumentamos el buffer a 1024 para la estructura anidada y completa
    DynamicJsonDocument doc(1024); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.printf(F("❌ Fallo al parsear JSON: %s\n"), error.c_str());
      http.end();
      return false;
    }

    // 1. Acceso a la configuración GENERAL bajo "remote_config"
    JsonObject remoteConfig = doc[F("remote_config")];
    if (remoteConfig.isNull()) {
        Serial.println(F("❌ Fallo: Objeto 'remote_config' no encontrado. Usando fallbacks."));
    } else {
        // A. CONFIGURACIÓN GENERAL (Bajo /remote_config)
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


    // 2. Acceso a la configuración ESPECÍFICA del nodo bajo "firmware_updates/[NODE_TYPE_KEY]"
    JsonObject nodeConfig = doc[F("firmware_updates")][NODE_TYPE_KEY];
    if (nodeConfig.isNull()) {
        Serial.printf(F("❌ Fallo: Configuración de nodo '%s' no encontrada bajo 'firmware_updates'. Usando fallbacks.\n"), NODE_TYPE_KEY);
    } else {
        // B. INTERVALO ESPECÍFICO DEL NODO
        if (nodeConfig.containsKey(F("intervalo_envio_ms")) && nodeConfig[F("intervalo_envio_ms")].is<long>()) {
          long newIntervalo = nodeConfig[F("intervalo_envio_ms")].as<long>();
          
          if (newIntervalo != oldIntervaloEnvioMs) {
             intervaloEnvioMs = newIntervalo;
             Serial.printf(F("🟢 LOG: INTERVALO ACTUALIZADO: Nuevo valor remoto = %ld ms\n"), intervaloEnvioMs);
          } 
        }
    }


    Serial.println(F("------------------------------------------"));
    Serial.println(F("Configuración Dinámica Aplicada:"));
    Serial.printf(F("Host: %s\n"), backendHost.c_str());
    Serial.printf(F("Endpoint: %s\n"), endpointCalidadAgua.c_str());
    Serial.printf(F("Intervalo (ms) FINAL: %ld\n"), intervaloEnvioMs);
    Serial.printf(F("Flag Activa: %s\n"), flagActivo ? "SI" : "NO");
    Serial.println(F("------------------------------------------"));
    
    http.end();
    return true;
  } else {
    // Si falla, se usa la configuración por defecto (fallback)
    Serial.printf(F("❌ Fallo al obtener la configuración (HTTP Code: %d). Usando valores por defecto.\n"), httpCode);
    http.end();
    return false;
  }
}


// ----------------------------------------------------
// FUNCIÓN DE CONEXIÓN WIFI 
// ----------------------------------------------------
bool conectar_wifi() {
  Serial.print(F("\n📡 Encendiendo Wi-Fi para enviar datos..."));
  WiFi.disconnect(true); 
  WiFi.mode(WIFI_OFF);
  
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  WiFi.begin(ssid, password);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio < TIEMPO_MAX_CONEXION_WIFI)) {
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ WiFi Conectado. IP: %s\n"), WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.println(F("\n❌ Fallo la conexión a WiFi."));
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
    // URL completa para el backend (usa el endpoint dinámico)
    String url = "http://" + backendHost + ":" + String(backendPort) + endpointCalidadAgua; 
    
    Serial.printf(F("URL de envío: %s\n"), url.c_str());
    
    http.setConnectTimeout(15000); 
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    int httpResponseCode = http.POST(jsonBuffer);
    
    if (httpResponseCode == 200) {
      Serial.printf(F("✅ POST exitoso. Código: %d\n"), httpResponseCode);
    } else {
      // Ahora este error 404/500 será del backend, no de la configuración de ruta.
      Serial.printf(F("❌ Error en el POST. Código: %d. Revise el log del servidor de Python.\n"), httpResponseCode);
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
    // Calculo simple de pendiente y offset para el pH
    ph_slope = (4.0 - 7.0) / (PH_V4 - PH_V7);
    ph_offset = 7.0 - (ph_slope * PH_V7);
    
    Serial.println(F("------------------------------------------"));
    Serial.println(F("CALIBRACIÓN PH (Valores por defecto):"));
    Serial.printf(F("Pendiente (Slope): %.2f\n"), ph_slope);
    Serial.printf(F("Intersección (Offset): %.2f\n"), ph_offset);
    Serial.println(F("------------------------------------------"));
}


void leer_sensores_agua() {
  Serial.println(F("💧 Iniciando lectura de sensores..."));
  
  // 1. LECTURA pH (GPIO 5)
  delay(10); 
  ph_raw = analogRead(PH_PIN);
  ph_voltage = (float)ph_raw * (ADC_VOLTAGE_REF / ADC_MAX_VALUE);
  ph_value = ph_slope * ph_voltage + ph_offset;

  if (ph_raw <= 5 || ph_raw >= ADC_MAX_VALUE - 5) {
      Serial.println(F("⚠️ ALERTA PH: Lectura en extremos. Revisa conexión y calibración."));
  }
  
  // 2. LECTURA TDS (GPIO 4)
  delay(10); 
  tds_raw = analogRead(TDS_PIN);
  tds_voltage = (float)tds_raw * (ADC_VOLTAGE_REF / ADC_MAX_VALUE);
  tds_value = 0.0; // TDS real requiere compensación de temperatura
  
  // ⚠️ MENSAJE DE ALERTA: Si la lectura es cero (0V)
  if (tds_raw == 0) {
      Serial.printf(F("❌ ERROR TDS: Lectura RAW es CERO (0). Verifica la conexión física en el pin GPIO %d.\n"), TDS_PIN);
  }
  
  Serial.println(F("   -----------------------------------"));
  // Los logs de la consola ahora reflejan correctamente el pin 5 para PH y 4 para TDS
  Serial.printf(F("   PH (Pin %d): %d / %.3f V -> %.2f pH\n"), PH_PIN, ph_raw, ph_voltage, ph_value);
  Serial.printf(F("   TDS (Pin %d): %d / %.3f V\n"), TDS_PIN, tds_raw, tds_voltage);
  Serial.println(F("   -----------------------------------"));
}


// ----------------------------------------------------
// BUCLE PRINCIPAL (LÓGICA DE TIEMPO Y CONFIGURACIÓN)
// ----------------------------------------------------
void loop() {
  
  // 1. LECTURA DE SENSORES
  leer_sensores_agua();

  bool connected = false;
  
  // 2. CONEXIÓN 
  if (conectar_wifi()) {
    connected = true;
  }
  
  if (connected) {
      
      // 3. VERIFICAR SI ES TIEMPO DE CONFIGURACIÓN
      if (millis() - lastConfigFetch >= CONFIG_FETCH_INTERVAL) {
          obtener_remote_config(); 
          lastConfigFetch = millis();
      }
      
      // 4. ENVÍO DE DATOS
      enviar_post_batch();
      
      // 5. DESCONEXIÓN 
      Serial.println(F("\n🔌 Desconectando Wi-Fi..."));
      WiFi.disconnect(true); 
      WiFi.mode(WIFI_OFF);
      
  } else {
    Serial.println(F("❌ No se pudo establecer conexión. Saltando envío y usando el intervalo anterior."));
  }
  
  // 6. ESPERA
  Serial.printf(F("💤 Entrando en espera por %ld ms (Intervalo Remoto).\n"), intervaloEnvioMs);
  delay(intervaloEnvioMs); 
  
  Serial.println(F("\n🔄 Despertando y reiniciando ciclo..."));
}