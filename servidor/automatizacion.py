# Automatizacion por presencia de la oficina.
# - Oficina vacia + detecta cualquier sensor de dentro (2-6) -> hay alguien: enciende el aire
#   (IR, ultima configuracion). Si solo detecta la puerta (la abrieron sin entrar), no se enciende
#   nada. Solo se enciende al pasar de vacia a ocupada: si alguien lo apaga a mano estando dentro,
#   no se vuelve a encender.
# - Al entrar dentro del horario de luz (por defecto 18:00 a 06:00, hora de Mexico) tambien se
#   enciende la luz; fuera de ese horario solo el aire. El horario se cambia desde el dashboard
#   con {"luz_inicio":"HH:MM","luz_fin":"HH:MM"} (retained) en oficina/automatizacion/config.
# - Cualquier sensor con movimiento reinicia el contador.
# - 15 minutos sin movimiento -> apaga el aire (IR) y la luz; la oficina queda vacia.
# - Si la placa de sensores se desconecta, no se apaga nada (sin datos no se sabe si hay gente).
# Se activa/desactiva con "ON"/"OFF" (retained) en oficina/automatizacion/set.
import json
import threading
import time
from datetime import datetime
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt

MQTT_BROKER = "localhost"
MQTT_PORT = 1883

TOPIC_MOVIMIENTO = "oficina/movimiento/state"
TOPIC_SENSORES_CONEXION = "oficina/movimiento/conexion"
TOPIC_AUTO_SET = "oficina/automatizacion/set"
TOPIC_AUTO_CONFIG = "oficina/automatizacion/config"
TOPIC_PRESENCIA_STATE = "oficina/presencia/state"
TOPIC_AC_STATE = "oficina/ac/state"
TOPIC_AC_SET = "oficina/ac/set"
TOPIC_LUZ_SET = "oficina/luz/set"

TIEMPO_VACIO_S = 15 * 60
VENTANA_CONFIRMACION_S = 60  # tras la puerta, otro sensor debe detectar en este tiempo
LATIDO_MAXIMO_S = 90  # la placa manda su estado cada 30 s
SENSOR_PUERTA = 0

ZONA_HORARIA = ZoneInfo("America/Mexico_City")
LUZ_INICIO_DEFECTO = "18:00"  # desde las 18:00...
LUZ_FIN_DEFECTO = "06:00"     # ...hasta las 06:00 se enciende tambien la luz al entrar

estado = {
    "activa": False,
    "ocupada": False,
    "ultimo_movimiento": None,  # time.time()
    "sensores": [0, 0, 0, 0, 0],
    "sensores_en_linea": False,
    "ultimo_latido": 0.0,
    "puerta_pendiente_desde": None,  # time.time() de la puerta que espera confirmacion
    "ac": {},
    "luz_inicio": LUZ_INICIO_DEFECTO,
    "luz_fin": LUZ_FIN_DEFECTO,
}
candado = threading.Lock()


def log(mensaje):
    print(f"{datetime.now():%Y-%m-%d %H:%M:%S} {mensaje}", flush=True)


def publicar_presencia(cliente):
    ultimo = estado["ultimo_movimiento"]
    datos = {
        "activa": estado["activa"],
        "ocupada": estado["ocupada"],
        "sensores": estado["sensores"],
        "sensores_en_linea": estado["sensores_en_linea"],
        "esperando_confirmacion": estado["puerta_pendiente_desde"] is not None,
        "ultimo_movimiento": datetime.fromtimestamp(ultimo).isoformat() if ultimo else None,
        "luz_inicio": estado["luz_inicio"],
        "luz_fin": estado["luz_fin"],
    }
    cliente.publish(TOPIC_PRESENCIA_STATE, json.dumps(datos), retain=True)


def a_minutos(hhmm):
    """'18:30' -> 1110. Lanza ValueError si el formato no es valido."""
    horas, minutos = hhmm.split(":")
    horas, minutos = int(horas), int(minutos)
    if not (0 <= horas <= 23 and 0 <= minutos <= 59):
        raise ValueError(hhmm)
    return horas * 60 + minutos


def es_horario_luz():
    ahora = datetime.now(ZONA_HORARIA)
    actual = ahora.hour * 60 + ahora.minute
    inicio = a_minutos(estado["luz_inicio"])
    fin = a_minutos(estado["luz_fin"])
    if inicio == fin:
        return False  # horario vacio: nunca enciende la luz
    if inicio < fin:
        return inicio <= actual < fin  # mismo dia, p. ej. 08:00-14:00
    return actual >= inicio or actual < fin  # cruza la medianoche, p. ej. 18:00-06:00


def aire_disponible():
    return estado["ac"].get("estado") == "ON"


def aire_encendido():
    return aire_disponible() and estado["ac"].get("modo") not in (None, "apagado")


def al_conectar(cliente, userdata, flags, reason_code, properties):
    log(f"Conectado a Mosquitto ({reason_code})")
    for topic in (TOPIC_MOVIMIENTO, TOPIC_SENSORES_CONEXION, TOPIC_AUTO_SET, TOPIC_AUTO_CONFIG, TOPIC_AC_STATE):
        cliente.subscribe(topic)
    with candado:
        publicar_presencia(cliente)


def al_recibir(cliente, userdata, msg):
    payload = msg.payload.decode(errors="replace").strip()
    ahora = time.time()
    with candado:
        if msg.topic == TOPIC_AC_STATE:
            try:
                estado["ac"] = json.loads(payload)
            except ValueError:
                pass
            return

        if msg.topic == TOPIC_AUTO_CONFIG:
            try:
                config = json.loads(payload)
                inicio, fin = config["luz_inicio"], config["luz_fin"]
                a_minutos(inicio)
                a_minutos(fin)
            except (ValueError, KeyError, TypeError, AttributeError):
                log(f"Configuracion invalida, se ignora: {payload}")
                return
            if (inicio, fin) != (estado["luz_inicio"], estado["luz_fin"]):
                estado["luz_inicio"], estado["luz_fin"] = inicio, fin
                log(f"Horario de luz: {inicio} a {fin}")
            publicar_presencia(cliente)
            return

        if msg.topic == TOPIC_AUTO_SET:
            activa = payload.upper() == "ON"
            if activa != estado["activa"]:
                estado["activa"] = activa
                # al activar se arranca con la oficina ocupada: si no hay nadie, se apaga en TIEMPO_VACIO_S
                estado["ocupada"] = activa
                estado["puerta_pendiente_desde"] = None
                estado["ultimo_movimiento"] = ahora if activa else estado["ultimo_movimiento"]
                log("Automatizacion activada" if activa else "Automatizacion desactivada")
            publicar_presencia(cliente)
            return

        if msg.topic == TOPIC_SENSORES_CONEXION:
            estado["sensores_en_linea"] = payload == "online"
            if payload == "online":
                estado["ultimo_latido"] = ahora  # que no se marque como caida antes de su primer reporte
            else:
                log("Placa de sensores desconectada: no se apagara nada hasta que vuelva")
            publicar_presencia(cliente)
            return

        if msg.topic == TOPIC_MOVIMIENTO:
            try:
                datos = json.loads(payload)
                sensores = [1 if s else 0 for s in datos["sensores"]]
            except (ValueError, KeyError, TypeError):
                log(f"Estado de sensores invalido: {payload}")
                return
            if datos.get("calentando"):
                return

            anteriores = estado["sensores"]
            estado["sensores"] = sensores
            estado["sensores_en_linea"] = True
            estado["ultimo_latido"] = ahora
            if any(sensores) or any(anteriores):
                # tambien cuando el ultimo sensor se apaga: el tiempo de vacio cuenta desde ese momento
                estado["ultimo_movimiento"] = ahora

            if estado["activa"] and not estado["ocupada"]:
                puerta_detecto = sensores[SENSOR_PUERTA] and not anteriores[SENSOR_PUERTA]
                if puerta_detecto:
                    if estado["puerta_pendiente_desde"] is None:
                        log(f"Puerta detecto movimiento: espero confirmacion de otro sensor ({VENTANA_CONFIRMACION_S} s)")
                    estado["puerta_pendiente_desde"] = ahora

                dentro = [i + 1 for i, s in enumerate(sensores) if s and i != SENSOR_PUERTA]
                if dentro:
                    estado["ocupada"] = True
                    pendiente = estado["puerta_pendiente_desde"]
                    estado["puerta_pendiente_desde"] = None
                    if pendiente is not None and ahora - pendiente <= VENTANA_CONFIRMACION_S:
                        motivo = f"entro alguien (puerta + sensor {dentro[0]})"
                    else:
                        motivo = f"hay alguien dentro (sensor {dentro[0]})"
                    if aire_disponible() and not aire_encendido():
                        log(f"Oficina ocupada, {motivo}: enciendo el aire")
                        cliente.publish(TOPIC_AC_SET, json.dumps({"encendido": "ON"}))
                    else:
                        log(f"Oficina ocupada, {motivo}; el aire ya estaba encendido o sin energia")
                    if es_horario_luz():
                        log(f"Dentro del horario de luz ({estado['luz_inicio']}-{estado['luz_fin']}): enciendo la luz")
                        cliente.publish(TOPIC_LUZ_SET, "ON")
            publicar_presencia(cliente)


def revisar_vacio(cliente):
    while True:
        time.sleep(5)
        ahora = time.time()
        with candado:
            if estado["sensores_en_linea"] and ahora - estado["ultimo_latido"] > LATIDO_MAXIMO_S:
                estado["sensores_en_linea"] = False
                log("La placa de sensores dejo de reportar: no se apagara nada hasta que vuelva")
                publicar_presencia(cliente)

            pendiente = estado["puerta_pendiente_desde"]
            if pendiente is not None and ahora - pendiente > VENTANA_CONFIRMACION_S:
                estado["puerta_pendiente_desde"] = None
                log("Solo detecto la puerta (la abrieron sin entrar): no enciendo el aire")
                publicar_presencia(cliente)

            if not (estado["activa"] and estado["ocupada"] and estado["sensores_en_linea"]):
                continue
            if any(estado["sensores"]):
                estado["ultimo_movimiento"] = ahora  # alguien sigue detectado
                continue
            if ahora - (estado["ultimo_movimiento"] or ahora) < TIEMPO_VACIO_S:
                continue

            estado["ocupada"] = False
            log(f"{TIEMPO_VACIO_S // 60} minutos sin movimiento: apago aire y luz")
            if aire_encendido():
                cliente.publish(TOPIC_AC_SET, json.dumps({"encendido": "OFF"}))
            cliente.publish(TOPIC_LUZ_SET, "OFF")
            publicar_presencia(cliente)


cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="automatizacion-presencia")
cliente.on_connect = al_conectar
cliente.on_message = al_recibir
cliente.connect(MQTT_BROKER, MQTT_PORT, 60)
threading.Thread(target=revisar_vacio, args=(cliente,), daemon=True).start()
cliente.loop_forever()
