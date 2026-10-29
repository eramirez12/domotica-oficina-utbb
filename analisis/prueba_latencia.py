#!/usr/bin/env python3
"""
Prueba de latencia de comando del sistema domótico (para la tesis).

Mide el tiempo desde que el servidor publica una orden de luz
(oficina/luz/set) hasta que la placa actuadora confirma el nuevo estado
(oficina/luz/state). Repite la prueba N veces y calcula las medidas de
tendencia central y de dispersión.

Uso en la Raspberry Pi (con la oficina vacía, porque la luz parpadea):
    python3 prueba_latencia.py            # 30 repeticiones, broker local
    python3 prueba_latencia.py 30 127.0.0.1

Al terminar deja la luz como estaba y genera:
  latencias.csv   un renglón por repetición
  y en pantalla: n, media, mediana, moda, desviación estándar, mínimo, máximo, rango, CV
"""
import csv
import statistics as st
import sys
import threading
import time

import paho.mqtt.client as mqtt

N = int(sys.argv[1]) if len(sys.argv) > 1 else 30
BROKER = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
T_SET, T_STATE = "oficina/luz/set", "oficina/luz/state"
PAUSA = 2.0        # segundos entre repeticiones
TIEMPO_MAX = 5.0   # segundos sin confirmación = prueba perdida

estado = {"valor": None}
llego = threading.Event()


def on_message(client, userdata, msg):
    estado["valor"] = msg.payload.decode().strip().upper()
    llego.set()


try:  # paho-mqtt 2.x
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="prueba-latencia")
except AttributeError:  # paho-mqtt 1.x
    c = mqtt.Client(client_id="prueba-latencia")
c.on_message = on_message
c.connect(BROKER, 1883, 30)
c.subscribe(T_STATE)
c.loop_start()

# Estado inicial (el tema es retenido, así que llega al suscribirse)
llego.wait(3)
inicial = estado["valor"] or "OFF"
print(f"Estado inicial de la luz: {inicial}. Repeticiones: {N}")

resultados = []
actual = inicial
for i in range(1, N + 1):
    objetivo = "OFF" if actual == "ON" else "ON"
    llego.clear()
    t0 = time.perf_counter()
    c.publish(T_SET, objetivo)
    ok = False
    while time.perf_counter() - t0 < TIEMPO_MAX:
        if llego.wait(TIEMPO_MAX) and estado["valor"] == objetivo:
            ok = True
            break
        llego.clear()
    lat = (time.perf_counter() - t0) if ok else None
    resultados.append((i, objetivo, round(lat, 4) if lat else ""))
    print(f"{i:2d}  {objetivo:3s}  " + (f"{lat:.3f} s" if lat else "SIN CONFIRMACIÓN"))
    actual = objetivo if ok else actual
    time.sleep(PAUSA)

if actual != inicial:  # dejar la luz como estaba
    c.publish(T_SET, inicial)
    time.sleep(1)
c.loop_stop()
c.disconnect()

with open("latencias.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["repeticion", "orden", "latencia_s"])
    w.writerows(resultados)

v = [r[2] for r in resultados if r[2] != ""]
perdidas = N - len(v)
print("\n--- Resumen ---")
print(f"Repeticiones: {N}   confirmadas: {len(v)}   perdidas: {perdidas}")
if len(v) >= 2:
    media, de = st.mean(v), st.stdev(v)
    redondeo = [round(x, 2) for x in v]
    modas = st.multimode(redondeo)
    moda = modas[0] if len(modas) == 1 and redondeo.count(modas[0]) > 1 else "sin moda"
    print(f"Media:    {media:.3f} s")
    print(f"Mediana:  {st.median(v):.3f} s")
    print(f"Moda:     {moda} (redondeo a 0.01 s)")
    print(f"DE:       {de:.3f} s")
    print(f"Mínimo:   {min(v):.3f} s   Máximo: {max(v):.3f} s   Rango: {max(v) - min(v):.3f} s")
    print(f"CV:       {100 * de / media:.1f} %")
print("Archivo: latencias.csv")
