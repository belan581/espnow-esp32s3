# ESP-NOW Master-Slave LED Control

Sistema de control de LEDs mediante ESP-NOW para ESP32-S3 Super Mini.

## Descripción

Este proyecto implementa un sistema maestro-esclavo donde un ESP32-S3 maestro puede:
1. **Emparejar** esclavos manteniendo presionado un botón durante 3 segundos a 4cm de distancia
2. **Controlar** los LEDs de todos los esclavos emparejados con un botón

## Hardware Requerido

- **ESP32-S3 Super Mini** (1 maestro + N esclavos)
- **LED RGB WS2812B** en GPIO 48 (integrado en algunos modelos)
- **Botones** (solo maestro):
  - GPIO 1: Botón de pairing
  - GPIO 7: Botón de control de LED

## Arquitectura del Sistema

```
┌─────────────────┐                    ┌─────────────────┐
│   MAESTRO       │                    │    ESCLAVO 1    │
│                 │  ESP-NOW Pairing   │                 │
│  - GPIO1: Pair  │◄──────────────────►│  - Escucha      │
│  - GPIO7: Ctrl  │                    │  - Responde     │
│  - GPIO48: LED  │  ESP-NOW Control   │  - GPIO48: LED  │
│                 │───────────────────►│                 │
└─────────────────┘                    └─────────────────┘
         │                                                
         │                              ┌─────────────────┐
         │                              │    ESCLAVO 2    │
         └─────────────────────────────►│  - GPIO48: LED  │
                                        └─────────────────┘
```

## Componentes Creados

### 1. **button**
- Manejo de botones GPIO con debounce
- Detección de presión corta y larga
- Callbacks para eventos de botón

### 2. **led_rgb**
- Control de LEDs WS2812B mediante RMT
- Funciones de blink y color sólido
- Soporte para múltiples LEDs

### 3. **espnow_manager**
- Gestión de comunicación ESP-NOW
- Manejo de peers (dispositivos emparejados)
- Envío/recepción de mensajes

### 4. **pairing**
- Proceso de emparejamiento maestro-esclavo
- Verificación de distancia mediante RSSI
- Confirmación LED con blink

## Configuración

### Mediante `idf.py menuconfig`

```bash
idf.py menuconfig
```

Navega a: **Example Configuration** →

- **Device Role**: Master o Slave
- **GPIO Pairing Button**: GPIO 1 (maestro)
- **GPIO Control Button**: GPIO 7 (maestro)
- **GPIO LED RGB**: GPIO 48
- **Pairing RSSI Threshold**: -30 dBm (~4cm)
- **Pairing Long Press Time**: 3000 ms
- **Pairing Confirmation Duration**: 5000 ms

## Compilación y Flasheo

### Configurar como MAESTRO:
```bash
idf.py menuconfig
# Seleccionar "Device Role" -> "Master"
idf.py build
idf.py -p COM<X> flash monitor
```

### Configurar como ESCLAVO:
```bash
idf.py menuconfig
# Seleccionar "Device Role" -> "Slave"
idf.py build
idf.py -p COM<Y> flash monitor
```

## Uso del Sistema

### 1. Proceso de Pairing

**En el Maestro:**
1. Acercar el maestro a ~4cm del esclavo
2. Mantener presionado el **botón GPIO1** durante **3 segundos**
3. El maestro envía solicitud de pairing
4. Al recibir respuesta del esclavo, ambos LEDs parpadean en **azul** (maestro) y **cyan** (esclavo) durante 5 segundos
5. Esclavo envía mensaje "READY"
6. Pairing completado

**Indicadores LED:**
- **Maestro**: Blink azul durante 5 segundos
- **Esclavo**: Blink cyan durante 5 segundos

### 2. Control de LEDs

**En el Maestro:**
1. Presionar **botón GPIO7** → LEDs de todos los esclavos se **encienden** (verde)
2. Presionar nuevamente → LEDs de todos los esclavos se **apagan**

**Estado:**
- Los LEDs permanecen encendidos/apagados hasta nueva presión del botón

## Flujo de Mensajes

```
MAESTRO                                  ESCLAVO
   │                                        │
   │──── PAIRING_REQUEST (broadcast) ─────►│
   │                                        │ (Verifica RSSI)
   │◄──── PAIRING_RESPONSE ────────────────│
   │                                        │
   │ (Agrega peer)                (Agrega peer)
   │                                        │
   │◄──── READY ───────────────────────────│
   │                                        │
   │ (Blink azul 5s)            (Blink cyan 5s)
   │                                        │
   │──── LED_ON/LED_OFF ───────────────────►│
   │                                        │ (Enciende/apaga LED)
```

## Estructura de Archivos

```
espnow/
├── CMakeLists.txt
├── README.md
├── sdkconfig
├── components/
│   ├── button/
│   │   ├── CMakeLists.txt
│   │   ├── button.c
│   │   └── include/button.h
│   ├── led_rgb/
│   │   ├── CMakeLists.txt
│   │   ├── led_rgb.c
│   │   └── include/led_rgb.h
│   ├── espnow_manager/
│   │   ├── CMakeLists.txt
│   │   ├── espnow_manager.c
│   │   └── include/espnow_manager.h
│   └── pairing/
│       ├── CMakeLists.txt
│       ├── pairing.c
│       └── include/pairing.h
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    └── espnow_example_main.c
```

## Mensajes ESP-NOW

### Tipos de Mensajes
- `ESPNOW_MSG_PAIRING_REQUEST`: Solicitud de pairing (maestro → broadcast)
- `ESPNOW_MSG_PAIRING_RESPONSE`: Respuesta de pairing (esclavo → maestro)
- `ESPNOW_MSG_READY`: Esclavo listo (esclavo → maestro)
- `ESPNOW_MSG_LED_ON`: Encender LED (maestro → esclavos)
- `ESPNOW_MSG_LED_OFF`: Apagar LED (maestro → esclavos)

## Solución de Problemas

### Pairing no funciona
- Verificar que la distancia sea menor a 4cm
- Ajustar `PAIRING_RSSI_THRESHOLD` en menuconfig (valores más negativos = mayor distancia)
- Verificar que el botón esté conectado correctamente a GPIO1

### LEDs no encienden
- Verificar que el LED WS2812B esté conectado a GPIO48
- Revisar alimentación del LED
- Verificar en logs si los mensajes se están enviando/recibiendo

### No recibe mensajes
- Ambos dispositivos deben estar en el mismo canal WiFi (configurado en menuconfig)
- Verificar en monitor serial que el pairing se completó correctamente

## Logs de Depuración

Los logs incluyen:
- Estado de inicialización de componentes
- Eventos de pairing
- Mensajes ESP-NOW enviados/recibidos
- Estado de botones y LEDs

Ejemplo:
```
I (1234) main: ESP-NOW Master-Slave LED Control
I (1235) main: Initializing as MASTER
I (1240) led_rgb: LED RGB initialized on GPIO 48 with 1 LEDs
I (1245) espnow_mgr: ESP-NOW manager initialized as MASTER, MAC: XX:XX:XX:XX:XX:XX
I (1250) pairing: Pairing initialized as MASTER (RSSI threshold: -30 dBm)
I (1255) button: Button initialized on GPIO 1
I (1260) button: Button initialized on GPIO 7
I (1265) main: Master initialized successfully
```

## Notas Técnicas

- **RSSI Threshold**: -30 dBm aproximadamente 4cm (puede variar según entorno)
- **Canal WiFi**: Por defecto canal 1 (configurable)
- **Máximo de peers**: 20 dispositivos (configurable en `espnow_manager.h`)
- **LED RGB**: Usa protocolo WS2812B (GRB format)

## Licencia

Este proyecto está basado en los ejemplos de ESP-IDF y está disponible bajo licencia CC0 / Public Domain.

## Autor

Desarrollado para ESP32-S3 Super Mini usando ESP-IDF v5.x

* Start WiFi.
* Initialize ESPNOW.
* Register ESPNOW sending or receiving callback function.
* Add ESPNOW peer information.
* Send and receive ESPNOW data.

This example need at least two ESP devices:

* In order to get the MAC address of the other device, Device1 firstly send broadcast ESPNOW data with 'state' set as 0.
* When Device2 receiving broadcast ESPNOW data from Device1 with 'state' as 0, adds Device1 into the peer list.
  Then start sending broadcast ESPNOW data with 'state' set as 1.
* When Device1 receiving broadcast ESPNOW data with 'state' as 1, compares the local magic number with that in the data.
  If the local one is bigger than that one, stop sending broadcast ESPNOW data and starts sending unicast ESPNOW data to Device2.
* If Device2 receives unicast ESPNOW data, also stop sending broadcast ESPNOW data.

In practice, if the MAC address of the other device is known, it's not required to send/receive broadcast ESPNOW data first,
just add the device into the peer list and send/receive unicast ESPNOW data.

There are a lot of "extras" on top of ESPNOW data, such as type, state, sequence number, CRC and magic in this example. These "extras" are
not required to use ESPNOW. They are only used to make this example to run correctly. However, it is recommended that users add some "extras"
to make ESPNOW data more safe and more reliable.

## How to use example

### Configure the project

```
idf.py menuconfig
```

* Set WiFi mode (station or SoftAP) under Example Configuration Options.
* Set ESPNOW primary master key under Example Configuration Options.
  This parameter must be set to the same value for sending and recving devices.
* Set ESPNOW local master key under Example Configuration Options.
  This parameter must be set to the same value for sending and recving devices.
* Set Channel under Example Configuration Options.
  The sending device and the recving device must be on the same channel.
* Set Send count and Send delay under Example Configuration Options.
* Set Send len under Example Configuration Options.
* Set Enable Long Range Options.
  When this parameter is enabled, the ESP32 device will send data at the PHY rate of 512Kbps or 256Kbps
  then the data can be transmitted over long range between two ESP32 devices.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

Here is the example of ESPNOW receiving device console output.

```
I (898) phy: phy_version: 3960, 5211945, Jul 18 2018, 10:40:07, 0, 0
I (898) wifi: mode : sta (30:ae:a4:80:45:68)
I (898) espnow_example: WiFi started
I (898) ESPNOW: espnow [version: 1.0] init
I (5908) espnow_example: Start sending broadcast data
I (6908) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (7908) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (52138) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (52138) espnow_example: Receive 0th broadcast data from: 30:ae:a4:0c:34:ec, len: 200
I (53158) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (53158) espnow_example: Receive 1th broadcast data from: 30:ae:a4:0c:34:ec, len: 200
I (54168) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (54168) espnow_example: Receive 2th broadcast data from: 30:ae:a4:0c:34:ec, len: 200
I (54168) espnow_example: Receive 0th unicast data from: 30:ae:a4:0c:34:ec, len: 200
I (54678) espnow_example: Receive 1th unicast data from: 30:ae:a4:0c:34:ec, len: 200
I (55668) espnow_example: Receive 2th unicast data from: 30:ae:a4:0c:34:ec, len: 200
```

Here is the example of ESPNOW sending device console output.

```
I (915) phy: phy_version: 3960, 5211945, Jul 18 2018, 10:40:07, 0, 0
I (915) wifi: mode : sta (30:ae:a4:0c:34:ec)
I (915) espnow_example: WiFi started
I (915) ESPNOW: espnow [version: 1.0] init
I (5915) espnow_example: Start sending broadcast data
I (5915) espnow_example: Receive 41th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (5915) espnow_example: Receive 42th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (5925) espnow_example: Receive 44th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (5935) espnow_example: Receive 45th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (6965) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (6965) espnow_example: Receive 46th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (7975) espnow_example: send data to ff:ff:ff:ff:ff:ff
I (7975) espnow_example: Receive 47th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (7975) espnow_example: Start sending unicast data
I (7975) espnow_example: send data to 30:ae:a4:80:45:68
I (9015) espnow_example: send data to 30:ae:a4:80:45:68
I (9015) espnow_example: Receive 48th broadcast data from: 30:ae:a4:80:45:68, len: 200
I (10015) espnow_example: send data to 30:ae:a4:80:45:68
I (16075) espnow_example: send data to 30:ae:a4:80:45:68
I (17075) espnow_example: send data to 30:ae:a4:80:45:68
I (24125) espnow_example: send data to 30:ae:a4:80:45:68
```

## Troubleshooting

If ESPNOW data can not be received from another device, maybe the two devices are not
on the same channel or the primary key and local key are different.

In real application, if the receiving device is in station mode only and it connects to an AP,
modem sleep should be disabled. Otherwise, it may fail to revceive ESPNOW data from other devices.
