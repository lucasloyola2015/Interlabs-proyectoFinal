#!/usr/bin/env python3
"""Ejemplo mínimo de cliente MQTT para enviar comandos y recibir respuestas"""

import paho.mqtt.client as mqtt
import json
import time

# Configuración
BROKER = "localhost"
PORT = 1883
COMMAND_TOPIC = "datalogger/commands"
RESPONSE_TOPIC = "datalogger/telemetry/response"
DEVICE_ID = "FJACFFBI"

# Variable para capturar la respuesta
respuesta = None

def on_connect(client, userdata, flags, rc):
    """Callback cuando se conecta al broker"""
    print(f"✓ Conectado al broker (código: {rc})")
    # IMPORTANTE: Suscribirse al topic de respuestas ANTES de enviar comandos
    client.subscribe(RESPONSE_TOPIC, qos=1)
    print(f"✓ Suscrito a: {RESPONSE_TOPIC}")

def on_message(client, userdata, msg):
    """Callback cuando se recibe un mensaje"""
    global respuesta
    print(f"✓ Respuesta recibida en: {msg.topic}")
    respuesta = msg.payload.decode()
    print(f"\nRespuesta:\n{respuesta}")

# Crear cliente
client = mqtt.Client(client_id="mi_cliente", clean_session=True)
client.on_connect = on_connect
client.on_message = on_message

# Conectar
print(f"Conectando a {BROKER}:{PORT}...")
client.connect(BROKER, PORT, 60)

# Iniciar loop en background (esto permite recibir mensajes)
client.loop_start()

# Esperar a que se establezca la conexión
time.sleep(1)

# Preparar comando (SIN campo 'id' - es opcional)
comando = {
    "deviceId": DEVICE_ID,
    "command": "help",
    "args": ""
}

# Enviar comando
print(f"\nEnviando comando a '{COMMAND_TOPIC}':")
print(json.dumps(comando, indent=2))
client.publish(COMMAND_TOPIC, json.dumps(comando), qos=1)

# Esperar respuesta (máximo 10 segundos)
print("\nEsperando respuesta...")
for i in range(10):
    if respuesta:
        break
    time.sleep(1)
    print(f"  Esperando... ({i+1}s)")

# Detener y desconectar
client.loop_stop()
client.disconnect()

# Mostrar resultado final
if respuesta:
    print("\n✓ ÉXITO: Respuesta recibida")
else:
    print("\n✗ ERROR: No se recibió respuesta")
    print("\nVerifica:")
    print("  1. Que el ESP32 esté conectado al broker")
    print("  2. Que el deviceId sea correcto")
    print("  3. Que estés suscrito al topic:", RESPONSE_TOPIC)

