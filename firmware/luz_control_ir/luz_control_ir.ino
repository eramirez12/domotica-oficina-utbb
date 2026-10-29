#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include <DHT.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <IRremote.hpp>
#include "minisplit_ir.h"

// ---------- Configuracion: llenar con tus datos ----------
// Nombre y contrasena de la red se definen en secrets.h (copia secrets.example.h).
#include "secrets.h"
// IP con la que se empieza a buscar la Raspberry Pi. Si la Pi cambia de IP, el Arduino
// la encuentra solo (broadcast UDP al servicio domotica-descubrimiento) y guarda la nueva.
const IPAddress BROKER_INICIAL(192, 168, 88, 5);  // IP fija de la Pi en la red del sistema
const int   MQTT_PORT     = 1883;
const int   PUERTO_PREGUNTA  = 5006;  // UDP: el Arduino pregunta "DOMOTICA?"
const int   PUERTO_RESPUESTA = 5007;  // TCP: la Pi se conecta y responde "DOMOTICA_PI"
const unsigned long ESPERA_RESPUESTA_MS = 4000;
// -----------------------------------------------------------

const char* TOPIC_SET   = "oficina/luz/set";    // comandos que llegan de la Pi
const char* TOPIC_STATE = "oficina/luz/state";  // estado que publica el Arduino
const char* TOPIC_CLIMA = "oficina/clima/lectura";  // temperatura + humedad
const char* TOPIC_AC_SET   = "oficina/ac/set";
const char* TOPIC_AC_STATE = "oficina/ac/state";

const int PIN_RELAY    = 7;
const int PIN_BUTTON   = 2;
const int PIN_IR_TX    = 3;  // KY-005 (emisor infrarrojo)
const int PIN_DHT      = 5;
const int PIN_RELAY_AC = 8;

// La mayoria de los modulos de rele baratos son activos en LOW.
// Si algo queda invertido (se apaga cuando deberia encender), cambia esto a false.
const bool RELAY_ACTIVE_LOW = true;

// true = el aire tiene energia. Con el cableado por NO (normalmente abierto),
// el rele debe estar activado para dar corriente al aire.
bool acEncendido = true;

// Ultimo estado enviado al minisplit por infrarrojo
bool irConocido = false;   // false hasta que se mande el primer comando
bool irEncendido = false;
uint8_t irModo = MODO_FRIO;
uint8_t irTemperatura = 24;
uint8_t irVentilador = VENT_AUTO;

// Posiciones en la EEPROM para recordar estados entre reinicios
const int EE_MARCA        = 0;
const int EE_LUZ          = 1;
const int EE_IR_CONOCIDO  = 2;
const int EE_IR_ENCENDIDO = 3;
const int EE_IR_MODO      = 4;
const int EE_IR_TEMP      = 5;
const int EE_IR_VENT      = 6;
const int EE_BROKER       = 7;  // 4 bytes: ultima IP donde se encontro la Pi
const uint8_t MARCA_EEPROM = 0xA5;

IPAddress brokerIp = BROKER_INICIAL;

WiFiUDP udpDescubrimiento;
WiFiServer servidorDescubrimiento(PUERTO_RESPUESTA);
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
DHT dht(PIN_DHT, DHT11);

unsigned long ultimoIntentoRed = 0;
const unsigned long REINTENTO_RED_MS = 5000; // reconectar pronto: mientras no hay conexion no llegan los avisos de movimiento

unsigned long ultimaLecturaClima = 0;
const unsigned long INTERVALO_CLIMA_MS = 10000; // leer cada 10 segundos

bool luzEncendida = false;
bool ultimoEstadoBoton = HIGH;
bool estadoEstableBoton = HIGH;
unsigned long ultimoCambioBoton = 0;
const unsigned long DEBOUNCE_MS = 50;

void setRele(bool encender) {
  luzEncendida = encender;
  bool nivelPin = RELAY_ACTIVE_LOW ? !encender : encender;
  digitalWrite(PIN_RELAY, nivelPin ? HIGH : LOW);
  EEPROM.update(EE_LUZ, encender ? 1 : 0);
  publicarEstado();
}

void publicarEstado() {
  mqttClient.beginMessage(TOPIC_STATE, true); // retained
  mqttClient.print(luzEncendida ? "ON" : "OFF");
  mqttClient.endMessage();
}

const char* nombreModo(uint8_t modo) {
  switch (modo) {
    case MODO_AUTO:       return "auto";
    case MODO_SECO:       return "seco";
    case MODO_CALOR:      return "calor";
    case MODO_VENTILADOR: return "ventilador";
    default:              return "frio";
  }
}

const char* nombreVentilador(uint8_t vent) {
  const char* nombres[] = {"auto", "baja", "media", "alta", "turbo"};
  return vent <= VENT_TURBO ? nombres[vent] : "auto";
}

// Devuelve -1 si el texto no es un modo valido
int modoDesdeTexto(const String& texto) {
  if (texto == "auto")       return MODO_AUTO;
  if (texto == "frio")       return MODO_FRIO;
  if (texto == "seco")       return MODO_SECO;
  if (texto == "calor")      return MODO_CALOR;
  if (texto == "ventilador") return MODO_VENTILADOR;
  return -1;
}

int ventiladorDesdeTexto(const String& texto) {
  if (texto == "auto")  return VENT_AUTO;
  if (texto == "baja")  return VENT_BAJA;
  if (texto == "media") return VENT_MEDIA;
  if (texto == "alta")  return VENT_ALTA;
  if (texto == "turbo") return VENT_TURBO;
  return -1;
}

// "estado" es la energia (rele). "modo" es "apagado" cuando el minisplit esta apagado por IR,
// y se omite si todavia no se ha mandado ningun comando IR.
void publicarEstadoAc() {
  String json = "{\"estado\":\"";
  json += acEncendido ? "ON" : "OFF";
  json += "\"";
  if (irConocido) {
    json += ",\"modo\":\"";
    json += irEncendido ? nombreModo(irModo) : "apagado";
    json += "\",\"temperatura\":" + String(irTemperatura);
    json += ",\"ventilador\":\"";
    json += nombreVentilador(irVentilador);
    json += "\"";
  }
  json += "}";
  mqttClient.beginMessage(TOPIC_AC_STATE, json.length(), true); // retained
  mqttClient.print(json);
  mqttClient.endMessage();
}

void setReleAc(bool encender) {
  acEncendido = encender;
  bool nivelPin = RELAY_ACTIVE_LOW ? !encender : encender;
  digitalWrite(PIN_RELAY_AC, nivelPin ? HIGH : LOW);
  publicarEstadoAc();
}

void guardarEstadoIr() {
  EEPROM.update(EE_IR_CONOCIDO, irConocido ? 1 : 0);
  EEPROM.update(EE_IR_ENCENDIDO, irEncendido ? 1 : 0);
  EEPROM.update(EE_IR_MODO, irModo);
  EEPROM.update(EE_IR_TEMP, irTemperatura);
  EEPROM.update(EE_IR_VENT, irVentilador);
}

void enviarIrAc(uint8_t tecla) {
  uint8_t trama[AC_BYTES];
  static uint16_t raw[AC_RAW_LEN];
  armarTramaAc(trama, irEncendido, irModo, irTemperatura, irVentilador, tecla);
  uint16_t n = tramaARaw(trama, raw);
  IrSender.sendRaw(raw, n, 38);

  irConocido = true;
  guardarEstadoIr();
  Serial.print("IR enviado: ");
  Serial.print(irEncendido ? nombreModo(irModo) : "apagado");
  Serial.print(" ");
  Serial.print(irTemperatura);
  Serial.print("C ventilador ");
  Serial.println(nombreVentilador(irVentilador));
  publicarEstadoAc();
}

void aplicarEstadoEnergia(const String& estado) {
  if (estado == "TOGGLE") {
    setReleAc(!acEncendido);
  } else if (estado == "OFF") {
    setReleAc(false);
  } else if (estado == "ON") {
    setReleAc(true);
  }
}

// Payloads aceptados en oficina/ac/set:
//   "ON" / "OFF" / "TOGGLE" o {"estado":"ON"}        -> energia del aire (rele)
//   {"encendido":"ON","modo":"frio","temperatura":24,"ventilador":"auto"} -> control IR (cualquier combinacion de campos)
void manejarComandoAc(const String& payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    aplicarEstadoEnergia(payload);
    return;
  }

  if (doc["estado"].is<const char*>()) {
    aplicarEstadoEnergia(doc["estado"].as<String>());
  }

  bool cambioPower = false, cambioModo = false, cambioVent = false, cambioTemp = false;

  if (doc["encendido"].is<const char*>()) {
    String valor = doc["encendido"].as<String>();
    if (valor == "ON" || valor == "OFF" || valor == "TOGGLE") {
      irEncendido = (valor == "TOGGLE") ? !irEncendido : (valor == "ON");
      cambioPower = true;
    }
  }
  if (doc["modo"].is<const char*>()) {
    int modo = modoDesdeTexto(doc["modo"].as<String>());
    if (modo >= 0) {
      irModo = modo;
      cambioModo = true;
    }
  }
  if (doc["ventilador"].is<const char*>()) {
    int vent = ventiladorDesdeTexto(doc["ventilador"].as<String>());
    if (vent >= 0) {
      irVentilador = vent;
      cambioVent = true;
    }
  }
  if (doc["temperatura"].is<int>()) {
    irTemperatura = constrain(doc["temperatura"].as<int>(), TEMP_MIN, TEMP_MAX);
    cambioTemp = true;
  }

  if (cambioPower)     enviarIrAc(TECLA_POWER);
  else if (cambioModo) enviarIrAc(TECLA_MODO);
  else if (cambioVent) enviarIrAc(TECLA_VENT);
  else if (cambioTemp) enviarIrAc(TECLA_TEMP);
}

// Un solo intento: si la red no esta, el loop sigue (boton, clima) y se reintenta despues
bool conectarWiFi() {
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);
  if (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.println("WiFi no disponible, se reintenta en un momento");
    return false;
  }
  Serial.println("WiFi conectado, IP: " + WiFi.localIP().toString());
  return true;
}

void alRecibirMqtt(int tamanoMensaje) {
  String topic = mqttClient.messageTopic();
  String payload = "";
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }
  payload.trim();
  Serial.println("Comando recibido en " + topic + ": " + payload);

  if (topic == TOPIC_AC_SET) {
    manejarComandoAc(payload);
    return;
  }

  if (topic != TOPIC_SET) return;

  if (payload == "ON") {
    setRele(true);
  } else if (payload == "OFF") {
    setRele(false);
  } else if (payload == "TOGGLE") {
    setRele(!luzEncendida);
  }
}

void leerYPublicarClima() {
  float temperatura = dht.readTemperature();
  float humedad = dht.readHumidity();

  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("Error leyendo el DHT11");
    return;
  }

  String payload = "{\"temperatura\":" + String(temperatura, 1) + ",\"humedad\":" + String(humedad, 1) +
                   ",\"rssi\":" + String(WiFi.RSSI()) + "}";  // rssi: intensidad de la senal WiFi en dBm
  mqttClient.beginMessage(TOPIC_CLIMA);
  mqttClient.print(payload);
  mqttClient.endMessage();

  Serial.println("Clima publicado: " + payload);
}

// 1) Manda "DOMOTICA?" por broadcast UDP; 2) la Pi (servicio domotica-descubrimiento) se conecta
// por TCP a este Arduino y manda "DOMOTICA_PI"; su IP es la de quien se conecto.
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
    revisarBoton(); // el boton sigue respondiendo mientras se espera
  }
  servidorDescubrimiento.end();

  if (!encontrada) {
    Serial.println("La Pi no respondio");
    return false;
  }
  Serial.println("Pi encontrada en " + brokerIp.toString());
  for (int i = 0; i < 4; i++) EEPROM.update(EE_BROKER + i, brokerIp[i]);
  return true;
}

bool conectarMqtt() {
  Serial.print("Conectando a broker MQTT: ");
  Serial.println(brokerIp.toString());
  if (!mqttClient.connect(brokerIp, MQTT_PORT)) {
    Serial.println("Broker no disponible, se reintenta en un momento");
    return false;
  }
  Serial.println("MQTT conectado");
  mqttClient.onMessage(alRecibirMqtt);
  int suscrito = mqttClient.subscribe("oficina/+/set"); // luz y ac en una sola suscripcion
  Serial.println(suscrito ? "Suscrito a oficina/+/set" : "Fallo la suscripcion");
  publicarEstado(); // anuncia el estado inicial al reconectar
  publicarEstadoAc();
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
      for (int i = 0; i < 4; i++) EEPROM.update(EE_BROKER + i, brokerIp[i]);  // la proxima vez se conecta directo
      return;
    }
  }
  // Tampoco respondio: tal vez la Pi cambio de IP
  if (buscarBroker()) conectarMqtt();
}

void cargarEstadosGuardados() {
  if (EEPROM.read(EE_MARCA) != MARCA_EEPROM) {
    // Primera vez: valores por defecto
    EEPROM.write(EE_MARCA, MARCA_EEPROM);
    EEPROM.write(EE_LUZ, 0);
    guardarEstadoIr();
    return;
  }
  luzEncendida = EEPROM.read(EE_LUZ) == 1;
  irConocido = EEPROM.read(EE_IR_CONOCIDO) == 1;
  irEncendido = EEPROM.read(EE_IR_ENCENDIDO) == 1;
  irModo = EEPROM.read(EE_IR_MODO);
  irTemperatura = constrain(EEPROM.read(EE_IR_TEMP), TEMP_MIN, TEMP_MAX);
  irVentilador = EEPROM.read(EE_IR_VENT);
  if (irVentilador > VENT_TURBO) irVentilador = VENT_AUTO;
  if (modoDesdeTexto(nombreModo(irModo)) != irModo) irModo = MODO_FRIO;

  // La IP guardada solo vale si ya se escribio alguna vez (EEPROM vacia = 0xFF)
  uint8_t primerByte = EEPROM.read(EE_BROKER);
  if (primerByte != 0x00 && primerByte != 0xFF) {
    brokerIp = IPAddress(EEPROM.read(EE_BROKER), EEPROM.read(EE_BROKER + 1),
                         EEPROM.read(EE_BROKER + 2), EEPROM.read(EE_BROKER + 3));
  }
}

// Compara el generador contra tramas capturadas del control original
void autopruebaIr() {
  const uint8_t ESPERADO[][AC_BYTES] = {
    {0xC3, 0x77, 0xE0, 0x00, 0x20, 0x40, 0x20, 0x00, 0x00, 0x20, 0x00, 0x45, 0xFF},  // frio 22 turbo, encender
    {0xC3, 0x97, 0xE0, 0x00, 0xA0, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x44, 0x5E},  // frio 26 ventilador auto
    {0xC3, 0x67, 0xE0, 0x00, 0x20, 0x40, 0x80, 0x00, 0x00, 0x30, 0x00, 0x46, 0x60},  // calor 20 turbo
    {0xC3, 0x07, 0xE0, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x46, 0x50},  // auto, ventilador media
  };
  uint8_t t[4][AC_BYTES];
  armarTramaAc(t[0], true, MODO_FRIO, 22, VENT_TURBO, TECLA_POWER);
  armarTramaAc(t[1], true, MODO_FRIO, 26, VENT_AUTO, TECLA_VENT);
  armarTramaAc(t[2], true, MODO_CALOR, 20, VENT_TURBO, TECLA_MODO);
  armarTramaAc(t[3], true, MODO_AUTO, 24, VENT_MEDIA, TECLA_MODO);
  int bien = 0;
  for (int c = 0; c < 4; c++) {
    if (memcmp(t[c], ESPERADO[c], AC_BYTES) == 0) bien++;
  }
  Serial.print("Autoprueba IR: ");
  Serial.print(bien);
  Serial.println(bien == 4 ? "/4 OK" : "/4 FALLA");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_RELAY_AC, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  dht.begin();
  IrSender.begin(PIN_IR_TX);

  cargarEstadosGuardados();
  setRele(luzEncendida); // la luz vuelve al estado que tenia antes de reiniciar
  setReleAc(true); // aire arranca con energia

  autopruebaIr();
  mqttClient.setConnectionTimeout(3000); // no quedarse esperando a un broker que no responde
  mantenerConexion();
}

void revisarBoton() {
  bool estadoActual = digitalRead(PIN_BUTTON);

  if (estadoActual != ultimoEstadoBoton) {
    ultimoCambioBoton = millis();
  }

  // Boton presionado = LOW (INPUT_PULLUP, conectado a GND)
  if ((millis() - ultimoCambioBoton) > DEBOUNCE_MS && estadoActual != estadoEstableBoton) {
    estadoEstableBoton = estadoActual;
    if (estadoEstableBoton == LOW) {
      setRele(!luzEncendida);
    }
  }

  ultimoEstadoBoton = estadoActual;
}

void loop() {
  mantenerConexion();
  if (mqttClient.connected()) {
    mqttClient.poll();
  }
  revisarBoton(); // funciona aunque no haya red

  if (millis() - ultimaLecturaClima > INTERVALO_CLIMA_MS) {
    ultimaLecturaClima = millis();
    leerYPublicarClima();
  }
}
