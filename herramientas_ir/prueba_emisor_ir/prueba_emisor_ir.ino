// Prueba del emisor KY-005: manda un codigo NEC cada 2 segundos.
// Conexion: S -> D3, "-" -> GND, centro sin conectar
// Verificacion: mira el LED con la camara del celular (se ve morado parpadeando)
#include <IRremote.hpp>

const int PIN_IR_TX = 3;

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  IrSender.begin(PIN_IR_TX);
  Serial.println("Emisor listo en D3. Enviando cada 2 segundos...");
}

void loop() {
  IrSender.sendNEC(0x00, 0x45, 0);
  Serial.println("Enviado NEC direccion 0x00 comando 0x45");
  delay(2000);
}
