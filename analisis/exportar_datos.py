#!/usr/bin/env python3
"""
Exporta la base de datos del sistema domótico a CSV para el análisis de la tesis.

Uso en la Raspberry Pi:
    python3 exportar_datos.py                      # usa ~/domotica-server/domotica.db
    python3 exportar_datos.py /ruta/a/domotica.db  # otra ubicación

Genera la carpeta export_AAAAMMDD_HHMM con:
  - un archivo CSV por cada tabla (luz_eventos, clima_lecturas, ac_eventos, ...)
  - resumen.txt con las columnas, el número de registros y el periodo que cubre cada tabla
y la comprime en export_AAAAMMDD_HHMM.zip para enviarla.

Solo lee la base de datos; no modifica nada.
"""
import csv
import os
import shutil
import sqlite3
import sys
from datetime import datetime

db_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/domotica-server/domotica.db")
if not os.path.exists(db_path):
    sys.exit(f"No encontré la base de datos en {db_path}. Pásala como argumento.")

out_dir = "export_" + datetime.now().strftime("%Y%m%d_%H%M")
os.makedirs(out_dir, exist_ok=True)

# Abrir en modo solo lectura para no interferir con el servicio en ejecución
con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
cur = con.cursor()
tables = [r[0] for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")]

TIME_HINTS = ("fecha", "hora", "time", "ts", "timestamp", "creado", "registro")
lines = [f"Base de datos: {db_path}", f"Exportado: {datetime.now().isoformat(timespec='seconds')}", ""]

for t in tables:
    cols = [r[1] for r in cur.execute(f'PRAGMA table_info("{t}")')]
    rows = cur.execute(f'SELECT * FROM "{t}"').fetchall()
    with open(os.path.join(out_dir, f"{t}.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols)
        w.writerows(rows)
    lines.append(f"Tabla {t}: {len(rows)} registros")
    lines.append(f"  Columnas: {', '.join(cols)}")
    for c in cols:
        if any(h in c.lower() for h in TIME_HINTS):
            lo, hi = cur.execute(f'SELECT MIN("{c}"), MAX("{c}") FROM "{t}"').fetchone()
            lines.append(f"  Periodo ({c}): {lo}  →  {hi}")
    lines.append("")

con.close()
with open(os.path.join(out_dir, "resumen.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

zip_path = shutil.make_archive(out_dir, "zip", out_dir)
print("\n".join(lines))
print(f"Listo: {zip_path}")
