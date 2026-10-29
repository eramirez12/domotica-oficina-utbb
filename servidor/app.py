import json
import re
import sqlite3
import threading
from datetime import datetime

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, send_from_directory

MQTT_BROKER = "localhost"
MQTT_PORT = 1883

TOPIC_LUZ_STATE = "oficina/luz/state"
TOPIC_LUZ_SET = "oficina/luz/set"
TOPIC_CLIMA_LECTURA = "oficina/clima/lectura"
TOPIC_AC_STATE = "oficina/ac/state"
TOPIC_AC_SET = "oficina/ac/set"
TOPIC_PRESENCIA_STATE = "oficina/presencia/state"  # lo publica automatizacion.py
TOPIC_AUTO_SET = "oficina/automatizacion/set"
TOPIC_AUTO_CONFIG = "oficina/automatizacion/config"

DB_PATH = "domotica.db"
STATIC_DIST = "static_dist"

app = Flask(__name__, static_folder=STATIC_DIST, static_url_path="/")

estado_actual = {
    "luz": "OFF",
    "clima": {"temperatura": None, "humedad": None},
    "ac": {"estado": "OFF", "modo": None, "temperatura": None, "ventilador": None},
    "presencia": {},
}
estado_lock = threading.Lock()


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS luz_eventos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            estado TEXT NOT NULL,
            timestamp TEXT NOT NULL
        )
        """
    )
    conn.execute("DROP TABLE IF EXISTS temperatura_lecturas")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS clima_lecturas (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            temperatura REAL NOT NULL,
            humedad REAL NOT NULL,
            timestamp TEXT NOT NULL
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS ac_eventos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            estado TEXT NOT NULL,
            modo TEXT,
            temperatura INTEGER,
            ventilador TEXT,
            timestamp TEXT NOT NULL
        )
        """
    )
    conn.commit()
    conn.close()


def on_connect(client, userdata, flags, rc):
    print("Conectado a Mosquitto con codigo", rc)
    client.subscribe(TOPIC_LUZ_STATE)
    client.subscribe(TOPIC_CLIMA_LECTURA)
    client.subscribe(TOPIC_AC_STATE)
    client.subscribe(TOPIC_PRESENCIA_STATE)


def on_message(client, userdata, msg):
    payload = msg.payload.decode().strip()
    ahora = datetime.now().isoformat()

    # La presencia cambia muy seguido: solo se guarda en memoria, no en la base de datos
    if msg.topic == TOPIC_PRESENCIA_STATE:
        try:
            datos = json.loads(payload)
        except ValueError:
            return
        with estado_lock:
            estado_actual["presencia"] = datos
        return

    conn = get_db()

    if msg.topic == TOPIC_LUZ_STATE:
        with estado_lock:
            estado_actual["luz"] = payload
        conn.execute(
            "INSERT INTO luz_eventos (estado, timestamp) VALUES (?, ?)",
            (payload, ahora),
        )
        print(f"Estado de luz actualizado: {payload}")

    elif msg.topic == TOPIC_CLIMA_LECTURA:
        try:
            datos = json.loads(payload)
            temperatura = float(datos["temperatura"])
            humedad = float(datos["humedad"])
        except (ValueError, KeyError, TypeError):
            print(f"Lectura de clima invalida: {payload}")
            conn.close()
            return
        with estado_lock:
            estado_actual["clima"] = {"temperatura": temperatura, "humedad": humedad}
        conn.execute(
            "INSERT INTO clima_lecturas (temperatura, humedad, timestamp) VALUES (?, ?, ?)",
            (temperatura, humedad, ahora),
        )
        print(f"Clima actualizado: {temperatura} C, {humedad}% humedad")

    elif msg.topic == TOPIC_AC_STATE:
        try:
            datos = json.loads(payload)
        except ValueError:
            print(f"Estado de AC invalido: {payload}")
            conn.close()
            return
        nuevo_estado = {
            "estado": datos.get("estado", "OFF"),
            "modo": datos.get("modo"),
            "temperatura": datos.get("temperatura"),
            "ventilador": datos.get("ventilador"),
        }
        with estado_lock:
            estado_actual["ac"] = nuevo_estado
        conn.execute(
            "INSERT INTO ac_eventos (estado, modo, temperatura, ventilador, timestamp) VALUES (?, ?, ?, ?, ?)",
            (
                nuevo_estado["estado"],
                nuevo_estado["modo"],
                nuevo_estado["temperatura"],
                nuevo_estado["ventilador"],
                ahora,
            ),
        )
        print(f"Estado de AC actualizado: {nuevo_estado}")

    conn.commit()
    conn.close()


mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message


@app.route("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.route("/api/luz/state")
def api_luz_state():
    with estado_lock:
        return jsonify({"luz": estado_actual["luz"]})


@app.route("/api/luz/set", methods=["POST"])
def api_luz_set():
    comando = request.json.get("comando")  # "ON", "OFF" o "TOGGLE"
    if comando not in ("ON", "OFF", "TOGGLE"):
        return jsonify({"error": "comando invalido"}), 400
    mqtt_client.publish(TOPIC_LUZ_SET, comando)
    return jsonify({"enviado": comando})


@app.route("/api/luz/historial")
def api_luz_historial():
    conn = get_db()
    filas = conn.execute(
        "SELECT estado, timestamp FROM luz_eventos ORDER BY id DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return jsonify([dict(f) for f in filas])


@app.route("/api/clima/actual")
def api_clima_actual():
    with estado_lock:
        return jsonify(estado_actual["clima"])


@app.route("/api/clima/historial")
def api_clima_historial():
    conn = get_db()
    filas = conn.execute(
        "SELECT temperatura, humedad, timestamp FROM clima_lecturas ORDER BY id DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return jsonify([dict(f) for f in filas])


@app.route("/api/ac/state")
def api_ac_state():
    with estado_lock:
        return jsonify(estado_actual["ac"])


@app.route("/api/ac/set", methods=["POST"])
def api_ac_set():
    datos = request.json or {}
    mqtt_client.publish(TOPIC_AC_SET, json.dumps(datos))
    return jsonify({"enviado": datos})


@app.route("/api/ac/historial")
def api_ac_historial():
    conn = get_db()
    filas = conn.execute(
        "SELECT estado, modo, temperatura, ventilador, timestamp FROM ac_eventos ORDER BY id DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return jsonify([dict(f) for f in filas])


@app.route("/api/automatizacion")
def api_automatizacion():
    with estado_lock:
        return jsonify(estado_actual["presencia"])


@app.route("/api/automatizacion/set", methods=["POST"])
def api_automatizacion_set():
    activa = (request.json or {}).get("activa")
    if not isinstance(activa, bool):
        return jsonify({"error": "activa debe ser true o false"}), 400
    mqtt_client.publish(TOPIC_AUTO_SET, "ON" if activa else "OFF", retain=True)
    return jsonify({"enviado": activa})


HORA_VALIDA = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")


@app.route("/api/automatizacion/horario", methods=["POST"])
def api_automatizacion_horario():
    datos = request.json or {}
    inicio, fin = datos.get("luz_inicio"), datos.get("luz_fin")
    if not (isinstance(inicio, str) and isinstance(fin, str) and HORA_VALIDA.match(inicio) and HORA_VALIDA.match(fin)):
        return jsonify({"error": "las horas deben tener formato HH:MM"}), 400
    config = {"luz_inicio": inicio, "luz_fin": fin}
    mqtt_client.publish(TOPIC_AUTO_CONFIG, json.dumps(config), retain=True)
    return jsonify({"enviado": config})


if __name__ == "__main__":
    init_db()
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()
    app.run(host="0.0.0.0", port=5000)
