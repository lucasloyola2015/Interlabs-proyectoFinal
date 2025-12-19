#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script para obtener el Device ID del ESP32 mediante comando config
"""

import paho.mqtt.client as mqtt
import json
import time
import sys

# Configuración
BROKER_HOST = "localhost"
BROKER_PORT = 1883
COMMAND_TOPIC = "datalogger/commands"
RESPONSE_TOPIC = "datalogger/telemetry/response"

# Intentar diferentes posibles Device IDs basados en el MAC conocido
# MAC: f0:24:f9:0c:55:b8 -> últimos 4 bytes: f9:0c:55:b8
# Posibles IDs: F90C55B8, FJAC55B8, etc. (con transformación 0-9 -> A-J)
POSSIBLE_IDS = [
    "F90C55B8",  # Sin transformación
    "FJAC55B8",  # Con transformación parcial
    "FJACFFBI",  # El que el usuario mencionó
]

response_received = False
device_id_found = None

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[OK] Conectado al broker")
        client.subscribe(RESPONSE_TOPIC)

def on_message(client, userdata, msg):
    global response_received, device_id_found
    try:
        payload = msg.payload.decode('utf-8')
        data = json.loads(payload)
        
        if data.get("type") == "command_response" and data.get("command") == "config":
            if data.get("status") == "ok" and "data" in data:
                config_data = data["data"]
                if isinstance(config_data, dict) and "device" in config_data:
                    device_id_found = config_data["device"].get("id", "")
                    print(f"\n[OK] Device ID encontrado: {device_id_found}")
                    response_received = True
    except:
        pass

def try_device_id(client, device_id):
    global response_received
    response_received = False
    
    cmd_json = {
        "deviceId": device_id,
        "command": "config",
        "args": "",
        "id": f"get_id_{int(time.time())}"
    }
    
    print(f"  Probando Device ID: {device_id}...")
    client.publish(COMMAND_TOPIC, json.dumps(cmd_json), qos=1)
    
    timeout = 5
    elapsed = 0
    while not response_received and elapsed < timeout:
        time.sleep(0.5)
        elapsed += 0.5
    
    return response_received and device_id_found == device_id

def main():
    print("Buscando Device ID del ESP32...")
    print("Probando diferentes IDs posibles...\n")
    
    client = mqtt.Client(client_id="get_id_client", clean_session=True)
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        client.connect(BROKER_HOST, BROKER_PORT, 60)
        client.loop_start()
        time.sleep(1)
        
        for device_id in POSSIBLE_IDS:
            if try_device_id(client, device_id):
                print(f"\n[OK] Device ID correcto: {device_id}")
                print(f"\nUsa este ID en el script de prueba:")
                print(f'DEVICE_ID = "{device_id}"')
                client.loop_stop()
                client.disconnect()
                return 0
        
        print("\n[ERROR] No se encontro el Device ID correcto")
        print("Verifica:")
        print("1. Que el ESP32 este conectado al broker MQTT")
        print("2. Que el handler de comandos este activo")
        print("3. Que los topics sean correctos")
        
        client.loop_stop()
        client.disconnect()
        return 1
        
    except Exception as e:
        print(f"[ERROR] {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())

