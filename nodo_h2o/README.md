# 💧 Nodo de Calidad de Agua & Dosificación (ESP32)

**Versión Firmware:** `1.1.4-h2o`  
**Microcontrolador:** ESP32 Standard DevKit (WROOM-32 / NodeMCU-32S de 30 o 38 pines)  
**Código Fuente:** [`firmware/nodo_h2o/nodo_h2o.ino`](file:///Users/dariancampos/Documents/Code/nodriza_backend/firmware/nodo_h2o/nodo_h2o.ino)  
**Sketch Arduino IDE:** [`nodo_h2o.ino`](file:///Users/dariancampos/Documents/Arduino/box_automation_nodo_calidad_agua/nodo_h2o/nodo_h2o.ino)

---

## 📌 Descripción General

El **Nodo de Calidad de Agua** es el módulo central del ecosistema Nodriza encargado de:
1. **Monitoreo Físico-Químico en Tiempo Real:** Medición de pH, Electroconductividad / Sólidos Disueltos (TDS/EC en ppm y µS/cm), Temperatura de Agua (°C con DS18B20 OneWire) y Estado de Nivel del Tanque/Reservorio (Boya flotadora magnética).
2. **Control Automatizado de 8 Relés:** Válvula/Bomba de llenado, bombas peristálticas de corrección de pH (UP / DOWN), 3 líneas independientes de dosificación de nutrientes (Bases A, B y C / CalMag), aireador/oxigenador auxiliar y bomba de recirculación/mezclado.
3. **Rutinas Inteligentes Autónomas:**
   - **Auto-Llenado de Tanque (`FILL_TANK`):** Inyección por pulsos seguros con pausas de estabilización mecánica de la boya y límite de seguridad contra rebalse o falla de sensor.
   - **Dosificación por Pulsos Temporizados (`PULSE`):** Micro-inyecciones precisas de ácidos/bases y fertilizantes en segundos sin bloqueo del bucle principal.
   - **Calibración Persistente en NVS:** Ajuste fino de 2 puntos para sonda de pH (pH 4.0 y pH 7.0) y factor multiplicador $K$ para la sonda TDS/EC, guardados de forma permanente en la memoria flash.
4. **Homologación de Estándares Nodriza:** SenML RFC 8428 con etiquetas de pines GPIO físicos, WebServer local en puerto 80 (`/status`, `/relay`, `/ping`), doble canal de transporte (HTTP Enjambre local + MQTT TLS Cloud), y protección integral contra bloqueos NVS y cuelgues (Watchdog TWDT a 30s).

---

## 📋 Mapeo de Pines Estandarizado

> [!IMPORTANT]
> Este nodo utiliza un **ESP32 WROOM-32 estándar** (no un ESP32-C3) debido a la cantidad requerida de GPIOs (8 salidas de relé, 2 entradas analógicas de alta precisión ADC1, bus OneWire y entrada digital de flotador).

### 1. Sensores y Entradas
| Periférico | Pin Físico ESP32 | Modo / Configuración | Notas de Conexión Eléctrica |
| :--- | :--- | :--- | :--- |
| **Sonda pH** (pH-4502C) | **GPIO 32** | `ADC1_CH4` (Input) | Salida analógica `Po` del módulo. Alimentar módulo con 5V y GND común. |
| **Sonda TDS / EC** (Gravity) | **GPIO 34** | `ADC1_CH6` (Input) | Salida analógica `AOUT`. Alimentar módulo con 3.3V y GND común. |
| **Temp. Agua** (DS18B20) | **GPIO 4** | OneWire Bus | **Pull-up obligatorio de 4.7 kΩ** entre el cable de Datos (amarillo/blanco) y 3.3V. |
| **Boya de Nivel** (Flotador) | **GPIO 19** | `INPUT_PULLUP` | Contacto seco reed switch. Un terminal a GPIO 19, el otro a `GND`. (`LOW = LLENO`). |
| **Botón Reset Wi-Fi** | **GPIO 0** | `INPUT_PULLUP` | Pulsador `BOOT` integrado en la placa. Mantener presionado al arrancar para borrar NVS. |

### 2. Módulo de 8 Relés (Lógica Active LOW: `LOW = ENCENDIDO`, `HIGH = APAGADO`)
| Canal | Pin GPIO | Clave SenML | Etiqueta / Nombre | Función Principal |
| :---: | :---: | :--- | :--- | :--- |
| **CH 0** | **GPIO 13** | `relay_fill` | `llenado` | Electroválvula o Bomba de ingreso de agua de red/ósmosis. |
| **CH 1** | **GPIO 14** | `relay_ph_up` | `ph_up` | Bomba peristáltica correctora de pH UP (básico). |
| **CH 2** | **GPIO 27** | `relay_ph_down` | `ph_down` | Bomba peristáltica correctora de pH DOWN (ácido). |
| **CH 3** | **GPIO 26** | `relay_base_a` | `base_a` | Bomba peristáltica de fertilizante Base A (Micro). |
| **CH 4** | **GPIO 25** | `relay_base_b` | `base_b` | Bomba peristáltica de fertilizante Base B (Bloom). |
| **CH 5** | **GPIO 16** | `relay_base_c` | `base_c` | Bomba peristáltica de fertilizante Base C (Grow / CalMag). |
| **CH 6** | **GPIO 17** | `relay_aux` | `aux` | Bomba de aireación / Piedra difusora de oxígeno. |
| **CH 7** | **GPIO 18** | `relay_mixer` | `mezcladora` | Bomba sumergible de recirculación / homogenización de mezcla. |

---

## ⚡ Esquema Eléctrico y Diagrama de Conexión

```text
                                  ESP32 WROOM-32 (30/38 Pines)
                                 ┌────────────────────────────┐
               [ 5V Fuente ]────>│ VIN                    GND ├────┬──> [ GND Común ]
                                 │                            │    │
                                 │                   (ADC1_4) │    │
    Módulo pH-4502C [ Po ]──────>│ GPIO 32                    │    │
    Módulo TDS [ AOUT ]─────────>│ GPIO 34           (ADC1_6) │    │
                                 │                            │    │
    DS18B20 [ DATA ]────────────>│ GPIO 4                     │    │
             │                   │                            │    │
             └──[ R 4.7kΩ ]──┐   │                            │    │
                             │   │                            │    │
    Boya Nivel (Cable 1)────────>│ GPIO 19                    │    │
    Boya Nivel (Cable 2)─────────┼────────────────────────────┼────┤
                                 │                            │    │
    [ 3.3V ]─────────────────┴──>│ 3V3                        │    │
                                 │                            │    │
                                 │ GPIO 13 ────> IN 0 (Llenado)    │
                                 │ GPIO 14 ────> IN 1 (pH UP)      │
                                 │ GPIO 27 ────> IN 2 (pH DOWN)    │  Módulo de 8 Relés
                                 │ GPIO 26 ────> IN 3 (Base A)     │  Optoacoplados
                                 │ GPIO 25 ────> IN 4 (Base B)     │  (Alimentado a 5V)
                                 │ GPIO 16 ────> IN 5 (Base C)     │
                                 │ GPIO 17 ────> IN 6 (Aux/Aire)   │
                                 │ GPIO 18 ────> IN 7 (Mezcladora) │
                                 └────────────────────────────┘
```

> [!TIP]
> **Alimentación del Módulo de Relés:** Conectar `VCC` a una fuente estable de `5V` (no al pin de 3.3V del ESP32). Para aislamiento galvánico óptimo, remover el jumper `JD-VCC/VCC` y alimentar la bobina de los relés con una fuente de 5V separada.

---

## 🧪 Calibración de Sensores

### 1. Sonda de pH (Calibración en 2 Puntos)
La calibración calcula la ecuación de recta $y = m \cdot x + b$:
- **Buffer pH 7.0:** Sumergir la sonda en solución buffer 7.0, esperar estabilización y enviar:
  ```text
  CAL_PH:7
  ```
  *(O especificar voltaje exacto: `CAL_PH:7:1.10`)*
- **Buffer pH 4.0:** Enjuagar con agua destilada, sumergir en solución 4.0 y enviar:
  ```text
  CAL_PH:4
  ```
  *(O especificar voltaje exacto: `CAL_PH:4:0.40`)*
El firmware calcula automáticamente la pendiente (`slope`) y el offset y los almacena en el namespace `cal_h2o` de la NVS.

### 2. Sonda TDS / Conductividad Eléctrica (Factor K)
Ajuste multiplicador contra solución patrón o medidor de mano calibrado:
```text
CAL_TDS:1.15
```
*(Multiplica la lectura de TDS por 1.15 y persiste el factor en NVS).*

---

## 📦 Contrato SenML (RFC 8428)

El nodo reporta periódicamente a la GreenBox Local (`/sensor-data/water`) y a Nodriza Cloud (`/api/sync`) utilizando el estándar SenML con nombres normalizados y referencia física de hardware:

```json
{
  "bn": "urn:dev:mac:48F6EE230524:",
  "bt": 128450,
  "ver": 1,
  "e": [
    { "n": "ph", "u": "pH", "v": 6.25, "gpio": "GPIO32", "label": "Sonda pH" },
    { "n": "ph_volt", "u": "V", "v": 1.215, "gpio": "GPIO32", "label": "Voltaje pH" },
    { "n": "tds", "u": "ppm", "v": 640.0, "gpio": "GPIO34", "label": "Sonda TDS" },
    { "n": "tds_volt", "u": "V", "v": 0.820, "gpio": "GPIO34", "label": "Voltaje TDS" },
    { "n": "ec", "u": "uS/cm", "v": 1280.0, "gpio": "GPIO34", "label": "Conductividad" },
    { "n": "water_temp", "u": "Cel", "v": 21.5, "gpio": "GPIO4", "label": "Temp Agua" },
    { "n": "tank_full", "vb": true, "gpio": "GPIO19", "label": "Boya Nivel" },
    { "n": "relay_fill", "vb": false, "gpio": "GPIO13", "label": "llenado" },
    { "n": "relay_ph_up", "vb": false, "gpio": "GPIO14", "label": "ph_up" },
    { "n": "relay_ph_down", "vb": false, "gpio": "GPIO27", "label": "ph_down" },
    { "n": "relay_base_a", "vb": false, "gpio": "GPIO26", "label": "base_a" },
    { "n": "relay_base_b", "vb": false, "gpio": "GPIO25", "label": "base_b" },
    { "n": "relay_base_c", "vb": false, "gpio": "GPIO16", "label": "base_c" },
    { "n": "relay_aux", "vb": false, "gpio": "GPIO17", "label": "aux" },
    { "n": "relay_mixer", "vb": false, "gpio": "GPIO18", "label": "mezcladora" }
  ],
  "boxSerialId": "48F6EE230524",
  "ip": "192.168.68.105",
  "localIp": "192.168.68.105",
  "firmwareVersion": "1.1.4-h2o",
  "ph": 6.25,
  "ph_voltage": 1.215,
  "tds": 640.0,
  "tds_voltage": 0.820,
  "ec": 1280.0,
  "water_temp": 21.5,
  "tank_full": true,
  "relays": [false, false, false, false, false, false, false, false]
}
```

---

## 📡 API de Control y Comandos (MQTT / HTTP)

### 1. Tópicos MQTT
- **Comandos:** `nodos/esp32/<MAC>/command`
- **Telemetría:** `nodos/esp32/<MAC>/telemetry`
- **Broadcast:** `nodos/esp32/broadcast/command`

### 2. Formatos de Comandos Aceptados

#### Control Directo de Relé:
- **String Plano:** `RELAY:1:ON` o `RELAY:1:OFF`
- **SenML JSON:**
  ```json
  { "e": [{ "n": "relay_ph_up", "vb": true }] }
  ```
- **JSON Individual:**
  ```json
  { "relayIndex": 1, "state": true }
  ```

#### Dosificación por Pulso Temporizado (No bloqueante):
- **String Plano:** `RELAY:1:PULSE:5.0` *(Enciende el relé 1 por 5 segundos y se auto-apaga).*
- **JSON:**
  ```json
  { "action": "PULSE", "relay": 1, "duration": 5.0 }
  ```

#### Rutina de Auto-Llenado de Tanque:
- **String Plano:** `FILL_TANK` o `FILL`
- **JSON:**
  ```json
  { "action": "FILL_TANK" }
  ```
*(Inicia ciclos de 3s ON / 10s OFF monitoreando la boya hasta que detecte nivel alto o alcance 20 ciclos máximos de seguridad).*

#### Calibración Remota:
- **pH:** `CAL_PH:7` o `CAL_PH:4:0.40`
- **TDS:** `CAL_TDS:1.10`

---

## 🌐 Endpoints HTTP Locales (Diagnóstico en Puerto 80)

| Ruta | Método | Descripción |
| :--- | :---: | :--- |
| `http://<IP_NODO>/` | `GET` | Dashboard web oscuro responsive con lecturas en vivo y estado general. |
| `http://<IP_NODO>/status` | `GET` | JSON estructurado con todos los valores físico-químicos y estado de los 8 relés. |
| `http://<IP_NODO>/relay?idx=0&state=on` | `GET` | Conmutación manual rápida de relés (`idx` 0 a 7, `state`: `on`/`off`). |
| `http://<IP_NODO>/ping` | `GET` | Respuesta inmediata `{"status":"ok","nodeId":"...","role":"NODO_H2O"}`. |

---

## 🛠️ Guía de Compilación y Flasheo (Arduino IDE)

### 1. Configuración de la Placa
- **Placa:** `ESP32 Dev Module` (o `NodeMCU-32S`)
- **CPU Frequency:** `240MHz (WiFi/BT)`
- **Flash Frequency:** `80MHz`
- **Flash Mode:** `QIO`
- **Partition Scheme:** `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`
- **Upload Speed:** `921600`
- **Core Debug Level:** `None` (o `Info`)

### 2. Librerías Requeridas en el Gestor de Bibliotecas
- `ArduinoJson` (versión 6.21.x)
- `PubSubClient` (por Nick O'Leary)
- `OneWire` (por Paul Stoffregen)
- `DallasTemperature` (por Miles Burton)

### 3. Primer Arranque y Portal Cautivo
1. Al encender por primera vez (o manteniendo presionado el botón `BOOT/GPIO 0`), el ESP32 levantará un Access Point Wi-Fi:
   - **SSID:** `NODO_H2O_SETUP`
   - **IP del Portal:** `http://192.168.4.1`
2. Conectarse desde el celular o laptop y abrir el navegador.
3. Ingresar el SSID y Contraseña de la red Wi-Fi local de 2.4 GHz.
4. El nodo probará la conexión en vivo y persistirá los datos en memoria NVS antes de reiniciar en modo operativo normal.
