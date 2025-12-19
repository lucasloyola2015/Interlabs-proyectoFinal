#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script de diagnostico MQTT - Verifica conexion y topics
"""

import paho.mqtt.client as mqtt
import json
import time
import sys

BROKER_HOST = "localhost"
BROKER_PORT = 1883

messages_received = []

def on_connect(client, userdata, flags, rc):
    print(f"[OK] Conectado al broker MQTT")
    # Suscribirse a todos los topics (#)
    client.subscribe("#")

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode('utf-8', errors='ignore')
    print(f"\n[MENSAJE RECIBIDO]")
    print(f"  Topic: {topic}")
    print(f"  Payload: {payload[:200]}")  # Primeros 200 chars
    messages_received.append((topic, payload))

def main():
    print("=" * 60)
    print("Diagnostico MQTT - DataLogger")
    print("=" * 60)
    print(f"Broker: {BROKER_HOST}:{BROKER_PORT}")
    print("Suscrito a todos los topics (#)")
    print("Esperando mensajes por 30 segundos...")
    print("=" * 60)
    
    client = mqtt.Client(client_id="diagnose_client", clean_session=True)
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        client.connect(BROKER_HOST, BROKER_PORT, 60)
        client.loop_start()
        
        # Enviar un comando de prueba mientras escuchamos
        time.sleep(2)
        print("\n[->] Enviando comando de prueba...")
        test_cmd = {
            "deviceId": "FJACFFBI",
            "command": "help",
            "args": ""
        }
        client.publish("datalogger/commands", json.dumps(test_cmd), qos=1)
        print("    Comando enviado")
        
        # Esperar mensajes
        timeout = 30
        elapsed = 0
        while elapsed < timeout:
            time.sleep(1)
            elapsed += 1
            if elapsed % 5 == 0:
                print(f"    Esperando... ({elapsed}s)")
        
        client.loop_stop()
        client.disconnect()
        
        print("\n" + "=" * 60)
        print(f"Total de mensajes recibidos: {len(messages_received)}")
        if len(messages_received) > 0:
            print("\nMensajes recibidos:")
            for topic, payload in messages_received:
                print(f"  - {topic}: {payload[:100]}")
        else:
            print("\n[ERROR] No se recibieron mensajes")
            print("\nPosibles causas:")
            print("1. El ESP32 no esta conectado al broker")
            print("2. El ESP32 no tiene configurado el host MQTT correctamente")
            print("3. El ESP32 no esta publicando en estos topics")
        
        return 0 if len(messages_received) > 0 else 1
        
    except Exception as e:
        print(f"\n[ERROR] {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())

