# Domótica para la oficina de SEED – UTBB

Código del sistema de domótica descrito en la tesis *Implementación de un sistema de domótica para la automatización de las condiciones ambientales de una oficina de la Universidad Tecnológica de Bahía de Banderas, 2026* (Maestría en Gestión del Mantenimiento 4.0, UTBB).

El sistema enciende y apaga la iluminación y el aire acondicionado tipo minisplit según la ocupación de la oficina, y registra la temperatura interior.

## Arquitectura

| Componente | Hardware | Carpeta |
|---|---|---|
| Placa de sensores | Arduino UNO R4 WiFi, 6 sensores PIR HC-SR501 | `firmware/sensores_presencia` |
| Placa actuadora | Arduino UNO R4 WiFi, emisor IR, relé de la iluminación, DHT11 | `firmware/luz_control_ir` |
| Servidor | Raspberry Pi 4: Mosquitto (MQTT), Flask, SQLite | `servidor` |
| Panel web | React (versión compilada) | `servidor/static_dist` |
| Protocolo IR | Sketches de prueba y tramas capturadas del control original | `herramientas_ir` |
| Análisis | Exportación de datos y prueba de latencia | `analisis` |

Las placas se comunican con el servidor por MQTT a través de una red WiFi dedicada (MikroTik hAP). El minisplit se controla con su protocolo infrarrojo (compatible con el protocolo Electra de IRremoteESP8266).

## Configuración

Las contraseñas de la red WiFi no se incluyen en el repositorio. En cada sketch, copia `secrets.example.h` como `secrets.h` y llena tus datos.

### Firmware (Arduino IDE)

Placa: Arduino UNO R4 WiFi. Bibliotecas usadas: IRremote 4.7.1, ArduinoMqttClient 0.1.8, ArduinoJson 7.4.3, DHT sensor library 1.4.7 y Adafruit Unified Sensor 1.1.15.

La placa actuadora tiene un segundo relé (pin 8) en serie con la alimentación del minisplit, que permanece energizado y solo se usa de forma manual desde el panel para cortar la energía del aire. La automatización controla el aire únicamente por infrarrojo. **Precaución:** el minisplit consume 9.6 A, cerca del límite de 10 A del relé; para replicarlo se recomienda un contactor de al menos 20 A.

### Servidor (Raspberry Pi)

```bash
sudo apt install mosquitto
cd servidor
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
python3 descubrimiento.py &     # responde a las placas que buscan al servidor (UDP 5006 / TCP 5007)
python3 automatizacion.py &     # reglas de ocupación
python3 app.py                  # API REST y panel web en el puerto 5000
```

`static_dist` contiene el panel web ya compilado, que Flask sirve en `/`.

## Cita

Si usas este código, cita la tesis y este repositorio (ver `CITATION.cff`).

## Licencia

MIT. Ver `LICENSE`.
