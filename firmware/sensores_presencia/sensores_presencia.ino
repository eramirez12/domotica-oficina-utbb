// Placa de sensores de presencia: lee 6 PIR HC-SR501 y avisa a la Raspberry Pi por MQTT.
// La Pi decide (servicio domotica-automatizacion): enciende el aire cuando entra la primera
// persona y apaga aire y luz tras 15 minutos sin movimiento.
#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include <EEPROM.h>

// ---------- Configuracion: llenar con tus datos ----------
// Nombre y contrasena de la red se definen en secrets.h (copia secrets.example.h).
#include "secrets.h"
// IP con la que se empieza a buscar la Raspberry Pi. Si la Pi cambia de IP, la placa
// la encuentra sola (servicio domotica-descubrimiento) y guarda la nueva.
const IPAddress BROKER_INICIAL(192, 168, 88, 5);  // IP fija de la Pi en la red del sistema
const int   MQTT_PORT     = 1883;
const int   PUERTO_PREGUNTA  = 5006;  // UDP: la placa pregunta "DOMOTICA?"
const int   PUERTO_RESPUESTA = 5007;  // TCP: la Pi se conecta y responde "DOMOTICA_PI"
const unsigned long ESPERA_RESPUESTA_MS = 4000;
// -----------------------------------------------------------

const char* TOPIC_MOVIMIENTO = "oficina/movimiento/state";     // {"sensores":[1,0,0,0,0]}, al cambiar y cada 30 s
const char* TOPIC_CONEXION   = "oficina/movimiento/conexion";  // "online" / "offline" (last will)

// Sensor 1 es el de la puerta. Se leen por analogico porque el HC-SR501 entrega 3.3V
// y en la UNO R4 (logica de 5V) eso queda en el limite de un HIGH digital.
const int NUM_SENSORES = 6;
const int PINES_SENSOR[NUM_SENSORES] = {A0, A1, A2, A3, A4, A5};
// Umbral por sensor (0-1023 con referencia de 5V). ~1.65V para A0-A3. Los sensores 5 y 6 van en
// A4 y A5, que en la UNO R4 tienen pull-up de placa (son tambien SDA/SCL), asi que no bajan a 0V:
// sensor 5 (A4) reposo ~1.6V / detectando ~4.0V -> umbral ~3.0V;
// sensor 6 (A5) reposo ~0.85V / detectando ~3.8V -> umbral ~2.35V.
const int UMBRAL_ADC[NUM_SENSORES] = {340, 340, 340, 340, 614, 480};

const unsigned long CALENTAMIENTO_MS = 60000;  // el HC-SR501 da falsos disparos el primer minuto
const unsigned long INTERVALO_LATIDO_MS = 30000;
const unsigned long REINTENTO_RED_MS = 5000; // reconectar pronto: mientras no hay conexion no llegan los avisos de movimiento

const int EE_MARCA  = 0;
const int EE_BROKER = 1;  // 4 bytes: ultima IP donde se encontro la Pi
const uint8_t MARCA_EEPROM = 0x5A;

IPAddress brokerIp = BROKER_INICIAL;

WiFiUDP udpDescubrimiento;
WiFiServer servidorDescubrimiento(PUERTO_RESPUESTA);
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

bool sensores[NUM_SENSORES] = {false};
unsigned long ultimoDiagnostico = 0;

// Muestra por el puerto serie el voltaje de todos los pines analogicos (para revisar el cableado)
void imprimirVoltajes() {
  if (millis() - ultimoDiagnostico < 3000) return;
  ultimoDiagnostico = millis();
  const int PINES[] = {A0, A1, A2, A3, A4, A5};
  Serial.print("Voltajes:");
  for (int i = 0; i < 6; i++) {
    Serial.print(" A");
    Serial.print(i);
    Serial.print("=");
    Serial.print(analogRead(PINES[i]) * 5.0 / 1023.0, 2);
    Serial.print("V");
  }
  Serial.println();
}
unsigned long ultimoLatido = 0;
unsigned long ultimoIntentoRed = 0;

String estadoJson() {
  String json = "{\"sensores\":[";
  for (int i = 0; i < NUM_SENSORES; i++) {
    json += sensores[i] ? "1" : "0";
    if (i < NUM_SENSORES - 1) json += ",";
  }
  json += "],\"calentando\":";
  json += millis() < CALENTAMIENTO_MS ? "true" : "false";
  json += ",\"rssi\":" + String(WiFi.RSSI());  // intensidad de la senal WiFi en dBm
  json += "}";
  return json;
}

void publicarMovimiento() {
  if (!mqttClient.connected()) return;
  String json = estadoJson();
  mqttClient.beginMessage(TOPIC_MOVIMIENTO, json.length(), false);
  mqttClient.print(json);
  mqttClient.endMessage();
  ultimoLatido = millis();
}

void leerSensores() {
  bool cambio = false;
  for (int i = 0; i < NUM_SENSORES; i++) {
    bool activo = analogRead(PINES_SENSOR[i]) > UMBRAL_ADC[i];
    if (millis() < CALENTAMIENTO_MS) activo = false;
    if (activo != sensores[i]) {
      sensores[i] = activo;
      cambio = true;
      Serial.print("Sensor ");
      Serial.print(i + 1);
      Serial.println(activo ? (i == 0 ? ": movimiento (puerta)" : ": movimiento") : ": sin movimiento");
    }
  }
  if (cambio || millis() - ultimoLatido > INTERVALO_LATIDO_MS) {
    publicarMovimiento();
  }
}

// Un solo intento: si la red no esta, los sensores se siguen leyendo y se reintenta despues
bool conectarWiFi() {
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);
  if (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.println("WiFi no disponible, se reintenta en un momento");
    return false;
  }
  // El DHCP tarda un poco despues de asociarse; sin IP no se puede hablar con la Pi
  unsigned long inicio = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - inicio < 5000) delay(100);
  Serial.println("WiFi conectado, IP: " + WiFi.localIP().toString());
  return true;
}

// 1) Manda "DOMOTICA?" por broadcast UDP; 2) la Pi (servicio domotica-descubrimiento) se conecta
// por TCP a esta placa y manda "DOMOTICA_PI"; su IP es la de quien se conecto.
// La respuesta llega por TCP porque el firmware WiFi 0.4.1 del R4 no entrega los UDP recibidos.
bool buscarBroker() {
  Serial.println("Buscando la Raspberry Pi en la red...");
  servidorDescubrimiento.begin(PUERTO_RESPUESTA);

  if (udpDescubrimiento.begin(PUERTO_PREGUNTA + 10)) {
    IPAddress local = WiFi.localIP();
    IPAddress mascara = WiFi.subnetMask();
    IPAddress broadcastRed(local[0] | ~mascara[0], local[1] | ~mascara[1],
                           local[2] | ~mascara[2], local[3] | ~mascara[3]);
    IPAddress destinos[] = {broadcastRed, IPAddress(255, 255, 255, 255)};
    for (IPAddress destino : destinos) {
      udpDescubrimiento.beginPacket(destino, PUERTO_PREGUNTA);
      udpDescubrimiento.print("DOMOTICA?");
      udpDescubrimiento.endPacket();
    }
    udpDescubrimiento.stop();
  }

  bool encontrada = false;
  unsigned long inicio = millis();
  while (millis() - inicio < ESPERA_RESPUESTA_MS && !encontrada) {
    WiFiClient cliente = servidorDescubrimiento.accept();
    if (cliente) {
      unsigned long inicioLectura = millis();
      while (!cliente.available() && millis() - inicioLectura < 1000) delay(10);
      String mensaje = cliente.readStringUntil('\n');
      if (mensaje == "DOMOTICA_PI") {
        brokerIp = cliente.remoteIP();
        encontrada = true;
      }
      cliente.stop();
    }
  }
  servidorDescubrimiento.end();

  if (!encontrada) {
    Serial.println("La Pi no respondio");
    return false;
  }
  Serial.println("Pi encontrada en " + brokerIp.toString());
  EEPROM.update(EE_MARCA, MARCA_EEPROM);
  for (int i = 0; i < 4; i++) EEPROM.update(EE_BROKER + i, brokerIp[i]);
  return true;
}

bool conectarMqtt() {
  Serial.print("Conectando a broker MQTT: ");
  Serial.println(brokerIp.toString());

  // Si esta placa se desconecta, el broker avisa a la Pi para que no apague nada por error
  mqttClient.beginWill(TOPIC_CONEXION, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  if (!mqttClient.connect(brokerIp, MQTT_PORT)) {
    Serial.println("Broker no disponible, se reintenta en un momento");
    return false;
  }
  Serial.println("MQTT conectado");
  mqttClient.beginMessage(TOPIC_CONEXION, true, 1); // retained
  mqttClient.print("online");
  mqttClient.endMessage();
  publicarMovimiento();
  return true;
}

// Revisa la conexion sin bloquear: si se cayo, reintenta cada REINTENTO_RED_MS
void mantenerConexion() {
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) return;
  if (ultimoIntentoRed != 0 && millis() - ultimoIntentoRed < REINTENTO_RED_MS) return;
  ultimoIntentoRed = millis();

  if (WiFi.status() != WL_CONNECTED && !conectarWiFi()) return;
  if (mqttClient.connected()) return;
  if (conectarMqtt()) return;
  // La IP guardada no respondio: probar la IP fija de la Pi en la red del sistema
  if (!(brokerIp == BROKER_INICIAL)) {
    brokerIp = BROKER_INICIAL;
    if (conectarMqtt()) {
      EEPROM.update(EE_MARCA, MARCA_EEPROM);
      for (int i = 0; i < 4; i++) EEPROM.update(EE_BROKER + i, brokerIp[i]);  // la proxima vez se conecta directo
      return;
    }
  }
  // Tampoco respondio: tal vez la Pi cambio de IP
  if (buscarBroker()) conectarMqtt();
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_SENSORES; i++) pinMode(PINES_SENSOR[i], INPUT);

  if (EEPROM.read(EE_MARCA) == MARCA_EEPROM) {
    brokerIp = IPAddress(EEPROM.read(EE_BROKER), EEPROM.read(EE_BROKER + 1),
                         EEPROM.read(EE_BROKER + 2), EEPROM.read(EE_BROKER + 3));
  }

  mqttClient.setId("sensores-presencia");
  mqttClient.setConnectionTimeout(3000); // no quedarse esperando a un broker que no responde
  Serial.println("Sensores en calentamiento (60 s)...");
  mantenerConexion();
}

void loop() {
  mantenerConexion();
  if (mqttClient.connected()) {
    mqttClient.poll();
  }
  leerSensores();
  imprimirVoltajes();
}
