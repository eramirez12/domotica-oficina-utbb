# Ayuda al Arduino a encontrar la Pi aunque cambie de IP.
# 1) El Arduino manda "DOMOTICA?" por broadcast UDP al puerto 5006.
# 2) La Pi se conecta por TCP al Arduino (puerto 5007) y le manda "DOMOTICA_PI";
#    el Arduino toma la IP de quien se conecto.
# Se responde por TCP porque el firmware WiFi del UNO R4 (0.4.1) no entrega los paquetes UDP recibidos.
import socket
import threading
import time

PUERTO_PREGUNTA = 5006
PUERTO_ARDUINO = 5007


def avisar_al_arduino(ip):
    for intento in range(3):
        try:
            with socket.create_connection((ip, PUERTO_ARDUINO), timeout=3) as conexion:
                conexion.sendall(b"DOMOTICA_PI\n")
                time.sleep(0.5)  # dar tiempo a que el Arduino lea antes de cerrar
            print(f"Avise a {ip}", flush=True)
            return
        except OSError as error:
            print(f"Intento {intento + 1} con {ip} fallo: {error}", flush=True)
            time.sleep(0.5)


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", PUERTO_PREGUNTA))
print(f"Escuchando busquedas en UDP {PUERTO_PREGUNTA}", flush=True)

while True:
    datos, origen = sock.recvfrom(64)
    if datos.strip() == b"DOMOTICA?":
        threading.Thread(target=avisar_al_arduino, args=(origen[0],), daemon=True).start()
