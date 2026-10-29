// Prueba del receptor KY-022: imprime en el monitor serie lo que recibe.
// Conexion: S -> D4, centro -> 5V, "-" -> GND
#define RAW_BUFFER_LENGTH 750     // tramas largas de aire acondicionado
#define RECORD_GAP_MICROS 12000
#include <IRremote.hpp>

const int PIN_IR_RX = 4;

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  IrReceiver.begin(PIN_IR_RX, DISABLE_LED_FEEDBACK);
  Serial.println("Receptor listo en D4. Apunta un control y presiona un boton.");
}

unsigned long ultimoAviso = 0;

void loop() {
  if (millis() - ultimoAviso > 3000) {
    ultimoAviso = millis();
    Serial.println("esperando senal...");
  }
  if (!IrReceiver.decode()) return;

  if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_WAS_OVERFLOW) {
    Serial.println("Trama demasiado larga (overflow), sube RAW_BUFFER_LENGTH");
  } else {
    Serial.println("---- Senal recibida ----");
    IrReceiver.printIRResultShort(&Serial);
    Serial.print("Pulsos: ");
    Serial.println(IrReceiver.decodedIRData.rawlen - 1);
    IrReceiver.printIRResultRawFormatted(&Serial, true);
  }
  IrReceiver.resume();
}
