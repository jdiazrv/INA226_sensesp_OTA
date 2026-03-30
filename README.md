# INA226 SenseSP OTA (ESP8266)

Firmware para ESP8266 (WeMos D1 mini Lite) que mide tension/corriente con INA226, publica por UDP en formato SignalK, permite configuracion web y OTA, y funciona con ciclos de deep sleep para ahorro de bateria.

Version actual del firmware:
- v1.9 (build 109)

## Funcionalidad principal
- Medicion con INA226 por I2C.
- Publicacion UDP a SignalK (`<basePath>.voltage` y `<basePath>.current`).
- Configuracion web en `/` y estado en `/status`.
- Modo OTA web (`/pre-update` y `/update`).
- Seleccion de modo de operacion: `auto`, `battery`, `maintenance`.
- Persistencia de configuracion en EEPROM con CRC.
- Estado RTC para recuperar fallos de WiFi entre ciclos.

## Requisitos de hardware
- ESP8266 (probado con WeMos D1 mini Lite).
- INA226 (direccion por defecto `0x40`).
- OLED SSD1306 I2C 128x64 (opcional).
- LEDs externos:
  - D6 (verde)
  - D7 (rojo)
  - Logica inversa: `LOW = encendido`, `HIGH = apagado`.

I2C configurado en:
- SDA: D2 (GPIO4)
- SCL: D1 (GPIO5)

## Endpoints web
- `GET /`:
  - Formulario de configuracion.
  - Accesos rapidos a mantenimiento y update.
- `POST /config`:
  - Guarda configuracion.
- `GET /status`:
  - Estado JSON completo del equipo.
- `POST /maintenance`:
  - `enable=1`: fuerza mantenimiento en RTC y reinicia.
  - `enable=0`: limpia mantenimiento forzado.
- `GET /pre-update`:
  - Entra en modo update.
- `GET/POST /update`:
  - Subida OTA de `.bin`.

## Todas las opciones de configuracion
Estas opciones se editan en la web (`/`) y se guardan en EEPROM.

1. `Pantalla Activa` (`enableDisplay`)
- Tipo: checkbox (0/1)
- Efecto:
  - `1`: mantiene la OLED activa segun la logica de pantalla.
  - `0`: la OLED se apaga fisicamente fuera de ventanas de arranque/uso temporal.

2. `Deep Sleep` (`enableDeepSleep`)
- Tipo: checkbox (0/1)
- Efecto:
  - `1`: habilita ciclos de reposo profundo.
  - `0`: no entra en deep sleep.
- Nota:
  - Antes de dormir, el firmware espera telemetria INA valida tras despertar (con timeout de seguridad).

3. `LEDs Activos` (`enableLEDs`)
- Tipo: checkbox (0/1)
- Efecto:
  - `1`: permite indicaciones por LEDs.
  - `0`: fuerza apagado continuo de D6/D7 para minimo consumo y modo sigiloso.

4. `Modo de operacion` (`preferredMode`)
- Tipo: selector (`auto`, `battery`, `maintenance`)
- Efecto:
  - `auto`:
    - Opera como bateria por defecto.
    - Solo entra en mantenimiento por:
      - mantenimiento forzado en RTC, o
      - fallos STA consecutivos (umbral interno actual: 3).
  - `battery`:
    - Fuerza ciclo rapido sin AP fallback.
  - `maintenance`:
    - Fuerza modo mantenimiento (web/AP para diagnostico).

5. `Tiempo Sleep (seg)` (`sleepTime`)
- Tipo: numero entero
- Rango valido: 15 a 300
- Efecto: tiempo de deep sleep entre ciclos, en segundos.

6. `Hostname` (`hostname`)
- Tipo: texto
- Restricciones:
  - solo letras, numeros y `-`
  - sin `-` al inicio ni al final
- Efecto:
  - hostname WiFi/mDNS del dispositivo.

7. `Shunt (Ohm)` (`shuntResistance`)
- Tipo: decimal
- Rango valido: `0.0001` a `1.0`
- Efecto:
  - calibracion del INA226 para calcular corriente correctamente.

8. `SignalK Source ID` (`sourceId`)
- Tipo: texto
- Restricciones: letras, numeros, `.`, `-`, `_`
- Efecto:
  - valor de `$source` en los mensajes SignalK.

9. `SignalK Path` (`basePath`)
- Tipo: texto
- Restricciones: letras, numeros, `.`, `-`, `_`
- Efecto:
  - base para publicar:
    - `<basePath>.voltage`
    - `<basePath>.current`

10. `SignalK Host/IP` (`signalKHost`)
- Tipo: texto
- Acepta:
  - IPv4 (ejemplo `192.168.1.50`)
  - hostname DNS/mDNS (ejemplo `lysmarine.local`)
- Efecto:
  - destino de resolucion DNS para el envio UDP.

## Funcionamiento detallado de LEDs
El proyecto usa logica inversa:
- `LED_ON = LOW`
- `LED_OFF = HIGH`

LEDs de usuario:
- D6 (verde)
- D7 (rojo)

Comportamientos:
1. Durante busqueda WiFi en setup (cuando corresponde mostrar progreso)
- Parpadeo del verde por intentos.
- Rojo apagado.

2. Arranque manual (no deep sleep), primeros ~30s
- Si INA esta disponible:
  - verde encendido fijo, rojo apagado.
- Si INA falla:
  - rojo encendido fijo, verde apagado.

3. Tras ventana de diagnostico o en despertares deep sleep
- ambos apagados (modo sigilo).

4. En modo AP de mantenimiento (sin STA)
- ambos encendidos fijos (requiere atencion/configuracion).

5. Destello tras envio UDP correcto
- verde si envio normal (INA disponible).
- rojo si envio de alarma por fallo de sensor.
- duracion aprox: 30 ms (no bloqueante).

6. Si `LEDs Activos` esta desmarcado
- ambos siempre apagados, sin excepciones de estado operativo.

LED interno de placa (`LED_BUILTIN`):
- Existe logica de heartbeat, actualmente desactivada por defecto (`allowBuiltinHeartbeat = false`).

## Logica de telemetria tras despertar
Para evitar enviar `0V` espurio tras wake:
- El firmware no publica telemetria normal hasta tener lectura INA valida (`V > 0.01`).
- Si INA aun no estabiliza, omite envio normal ese ciclo.
- Deep sleep espera estabilizacion INA y usa timeout de seguridad para no drenar bateria indefinidamente.

## Compilacion (PlatformIO)
Desde la raiz del proyecto:

```bash
platformio run -e d1_mini_lite
```

## Carga OTA (si ya esta en red)
Ejemplo:

```bash
platformio run --target upload --environment d1_mini_lite --upload-port <IP_DEL_DISPOSITIVO>
```

## Notas de operacion
- Si `signalKHost` no resuelve DNS, el firmware reintenta automaticamente.
- Si el INA226 falla por I2C, se marca no disponible y se intenta recuperar sin reiniciar MCU.
- En fallo de sensor se puede enviar notificacion de alarma SignalK en `notifications.<basePath>.sensor_error`.
