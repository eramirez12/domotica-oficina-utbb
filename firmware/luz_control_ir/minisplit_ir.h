// Generador de tramas IR del control del minisplit.
// Protocolo tipo Electra: 13 bytes, LSB primero, encabezado 9000/4500 us.
// Descifrado a partir de 20 capturas del control original (2026-09-28).
#pragma once
#include <stdint.h>

const uint8_t MODO_AUTO       = 0x00;
const uint8_t MODO_FRIO       = 0x20;
const uint8_t MODO_SECO       = 0x40;
const uint8_t MODO_CALOR      = 0x80;
const uint8_t MODO_VENTILADOR = 0xC0;

const uint8_t VENT_AUTO  = 0;
const uint8_t VENT_BAJA  = 1;
const uint8_t VENT_MEDIA = 2;
const uint8_t VENT_ALTA  = 3;
const uint8_t VENT_TURBO = 4;

// Byte 11: tecla que "se presiono" en el control
const uint8_t TECLA_TEMP  = 0x40;
const uint8_t TECLA_VENT  = 0x44;
const uint8_t TECLA_POWER = 0x45;
const uint8_t TECLA_MODO  = 0x46;

const uint8_t TEMP_MIN = 16;
const uint8_t TEMP_MAX = 30;

const uint8_t AC_BYTES = 13;
const uint16_t AC_RAW_LEN = 2 + AC_BYTES * 8 * 2 + 1;  // encabezado + 104 bits + pulso final = 211

inline void armarTramaAc(uint8_t t[AC_BYTES], bool encendido, uint8_t modo,
                         uint8_t temperatura, uint8_t ventilador, uint8_t tecla) {
  const uint8_t VENT_BYTE[] = {0xA0, 0x60, 0x40, 0x20, 0x20};  // auto, baja, media, alta, turbo(=alta + bandera)

  if (temperatura < TEMP_MIN) temperatura = TEMP_MIN;
  if (temperatura > TEMP_MAX) temperatura = TEMP_MAX;
  if (ventilador > VENT_TURBO) ventilador = VENT_AUTO;
  if (modo == MODO_SECO) ventilador = VENT_BAJA;  // el control original fuerza baja en seco
  bool sinTemperatura = (modo == MODO_AUTO || modo == MODO_VENTILADOR);

  for (uint8_t i = 0; i < AC_BYTES; i++) t[i] = 0;
  t[0] = 0xC3;
  t[1] = (sinTemperatura ? 0 : (uint8_t)((temperatura - 8) << 3)) | 0x07;
  t[2] = 0xE0;
  t[4] = VENT_BYTE[ventilador];
  t[5] = (ventilador == VENT_TURBO) ? 0x40 : 0x00;
  t[6] = modo;
  t[9] = (encendido ? 0x20 : 0x00) | (modo == MODO_CALOR ? 0x10 : 0x00);
  t[11] = tecla;

  uint8_t suma = 0;
  for (uint8_t i = 0; i < AC_BYTES - 1; i++) suma += t[i];
  t[12] = suma;
}

// Convierte la trama en tiempos crudos (us) para IrSender.sendRaw(). Devuelve cuantos valores escribio.
inline uint16_t tramaARaw(const uint8_t t[AC_BYTES], uint16_t raw[AC_RAW_LEN]) {
  uint16_t n = 0;
  raw[n++] = 9000;
  raw[n++] = 4500;
  for (uint8_t i = 0; i < AC_BYTES; i++) {
    for (uint8_t b = 0; b < 8; b++) {
      raw[n++] = 600;
      raw[n++] = ((t[i] >> b) & 1) ? 1650 : 550;
    }
  }
  raw[n++] = 600;
  return n;
}
