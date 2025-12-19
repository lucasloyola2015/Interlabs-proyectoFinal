#!/usr/bin/env python3
"""Script de diagnóstico rápido para comandos MQTT"""

import paho.mqtt.client as mqtt
import json
import time

BROKER_HOST = "localhost"
BROKER_PORT = 1883
COMMAND_TOPIC = "datalogger/commands"
RESPONSE_TOPIC = "datalogger/telemetry/response"
DEVICE_ID = "FJACFFBI"

response_received = False
response_data = None

def on_connect(client, userdata, flags, rc):
    print(f"[OK] Conectado al broker (rc={rc})")
    print(f"[->] Suscribiendo a: {RESPONSE_TOPIC}")
    client.subscribe(RESPONSE_TOPIC, qos=1)

def on_subscribe(client, userdata, mid, granted_qos):
    print(f"[OK] Suscrito (mid={mid}, qos={granted_qos[0]})")

def on_message(client, userdata, msg):
    global response_received, response_data
    print(f"\n[OK] Mensaje recibido en topic: {msg.topic}")
    print(f"[OK] Payload: {msg.payload.decode()}")
    response_received = True
    response_data = msg.payload.decode()

def main():
    global response_received
    
    print("=" * 60)
    print("Diagnóstico MQTT - Comando Help")
    print("=" * 60)
    print(f"Broker: {BROKER_HOST}:{BROKER_PORT}")
    print(f"Topic comandos: {COMMAND_TOPIC}")
    print(f"Topic respuestas: {RESPONSE_TOPIC}")
    print(f"Device ID: {DEVICE_ID}")
    print("=" * 60)
    
    # Crear cliente
    client = mqtt.Client(client_id="debug_client", clean_session=True)
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    
    # Conectar
    print(f"\n[->] Conectando a {BROKER_HOST}:{BROKER_PORT}...")
    try:
        client.connect(BROKER_HOST, BROKER_PORT, 60)
    except Exception as e:
        print(f"[ERROR] Error de conexion: {e}")
        return
    
    # Iniciar loop
    client.loop_start()
    
    # Esperar conexión
    time.sleep(2)
    
    # Preparar comando (SIN campo 'id')
    cmd = {
        "deviceId": DEVICE_ID,
        "command": "help",
        "args": ""
    }
    cmd_str = json.dumps(cmd)
    
    print(f"\n[->] Enviando comando:")
    print(f"    {cmd_str}")
    
    # Publicar
    result = client.publish(COMMAND_TOPIC, cmd_str, qos=1)
    if result.rc == mqtt.MQTT_ERR_SUCCESS:
        print(f"[OK] Comando publicado (mid={result.mid})")
    else:
        print(f"[ERROR] Error al publicar: {result.rc}")
    
    # Esperar respuesta
    print(f"\n[->] Esperando respuesta (timeout: 10s)...")
    for i in range(10):
        if response_received:
            break
        print(f"    Esperando... ({i+1}s)", end="\r")
        time.sleep(1)
    
    print()  # Nueva línea
    
    # Detener y desconectar
    client.loop_stop()
    client.disconnect()
    
    # Resultado
    print("\n" + "=" * 60)
    if response_received:
        print("[OK] TEST EXITOSO - Respuesta recibida")
        print("=" * 60)
        try:
            data = json.loads(response_data)
            print(f"Estado: {data.get('status', 'N/A')}")
            print(f"Comando: {data.get('command', 'N/A')}")
            print(f"Mensaje: {data.get('message', 'N/A')}")
        except:
            print(f"Datos: {response_data}")
    else:
        print("[ERROR] TEST FALLIDO - No se recibió respuesta")
        print("=" * 60)
        print("\nPosibles causas:")
        print("1. El ESP32 no está conectado al broker MQTT")
        print("2. El deviceId no coincide")
        print("3. El ESP32 no está procesando el comando")
        print("4. La respuesta se publica en un topic diferente")
        print("\nVerifica los logs del ESP32 para más detalles.")

if __name__ == "__main__":
    main()

