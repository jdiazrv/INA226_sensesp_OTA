# Releases

## v1.9 (build 109) - current
Fecha: 2026-03-30

Cambios principales:
- Host de SignalK configurable (IP u hostname) desde `/config`.
- Modo `auto` diferenciado de `maintenance`:
  - `auto` ya no fuerza mantenimiento por arranque en frio.
- Correccion de wake/deep sleep:
  - se evita publicar telemetria normal sin muestra INA valida.
  - se espera estabilizacion INA antes de dormir (con timeout de seguridad).
- Endurecimiento general de modo bateria vs mantenimiento y controles web.

## v1.8 (build 108) - previous
Cambios principales:
- Separacion explicita entre modo bateria y modo mantenimiento.
- Conexion WiFi rapida en ciclos de bateria (timeout corto, sin AP fallback automatico).
- Contador de fallos STA persistente en RTC para decidir entrada a mantenimiento.
- `/status` expone `powerMode`, `maintenanceMode` y `wifiFastConnectEnabled`.
