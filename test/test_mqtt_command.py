#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script de prueba para enviar comandos MQTT al DataLogger
Requiere: pip install paho-mqtt
"""

import paho.mqtt.client as mqtt
import json
import time
import sys
import os

# Fix encoding for Windows console
if sys.platform == 'win32':
    import codecs
    sys.stdout = codecs.getwriter('utf-8')(sys.stdout.buffer, 'strict')
    sys.stderr = codecs.getwriter('utf-8')(sys.stderr.buffer, 'strict')

# Configuración
BROKER_HOST = "localhost"
BROKER_PORT = 1883
COMMAND_TOPIC = "datalogger/commands"
RESPONSE_TOPIC = "datalogger/telemetry/response"
DEVICE_ID = "FJACFFBI"
COMMAND = "help"

# Variables globales
response_received = False
response_data = None

def on_connect(client, userdata, flags, rc):
    """Callback cuando se conecta al broker"""
    if rc == 0:
        print(f"[OK] Conectado al broker MQTT en {BROKER_HOST}:{BROKER_PORT}")
        # Suscribirse al topic de respuestas
        client.subscribe(RESPONSE_TOPIC)
        print(f"[OK] Suscrito al topic: {RESPONSE_TOPIC}")
    else:
        print(f"[ERROR] Error al conectar. Codigo: {rc}")
        sys.exit(1)

def on_disconnect(client, userdata, rc):
    """Callback cuando se desconecta del broker"""
    print(f"[!] Desconectado del broker")

def on_message(client, userdata, msg):
    """Callback cuando se recibe un mensaje"""
    global response_received, response_data
    
    try:
        payload = msg.payload.decode('utf-8')
        print(f"\n[->] Respuesta recibida en '{msg.topic}':")
        print(f"    {payload}")
        
        # Parsear JSON
        data = json.loads(payload)
        response_data = data
        response_received = True
        
        # Verificar que sea una respuesta de comando
        if data.get("type") == "command_response":
            print(f"\n[OK] Tipo: Respuesta de comando")
            print(f"    Comando: {data.get('command', 'N/A')}")
            print(f"    Estado: {data.get('status', 'N/A')}")
            print(f"    Mensaje: {data.get('message', 'N/A')}")
            if 'id' in data:
                print(f"    ID correlacion: {data['id']}")
        else:
            print(f"[!] Advertencia: El mensaje no es una respuesta de comando")
            
    except json.JSONDecodeError as e:
        print(f"[ERROR] Error al parsear JSON: {e}")
        print(f"    Payload: {payload}")
    except Exception as e:
        print(f"[ERROR] Error al procesar mensaje: {e}")

def on_subscribe(client, userdata, mid, granted_qos):
    """Callback cuando se completa la suscripción"""
    print(f"[OK] Suscripcion confirmada (QoS: {granted_qos[0]})")

def send_command(client, device_id, command, args="", request_id=None):
    """Envía un comando por MQTT"""
    cmd_json = {
        "deviceId": device_id,
        "command": command,
        "args": args
    }
    
    if request_id:
        cmd_json["id"] = request_id
    
    cmd_str = json.dumps(cmd_json)
    print(f"\n[->] Enviando comando a '{COMMAND_TOPIC}':")
    print(f"    {cmd_str}")
    
    result = client.publish(COMMAND_TOPIC, cmd_str, qos=1)
    
    if result.rc == mqtt.MQTT_ERR_SUCCESS:
        print(f"[OK] Comando enviado exitosamente")
        return True
    else:
        print(f"[ERROR] Error al enviar comando. Codigo: {result.rc}")
        return False

def main():
    global response_received, response_data
    
    print("=" * 60)
    print("Test de Comandos MQTT - DataLogger")
    print("=" * 60)
    print(f"Broker: {BROKER_HOST}:{BROKER_PORT}")
    print(f"Topic comandos: {COMMAND_TOPIC}")
    print(f"Topic respuestas: {RESPONSE_TOPIC}")
    print(f"Device ID: {DEVICE_ID}")
    print(f"Comando: {COMMAND}")
    print("=" * 60)
    
    # Crear cliente MQTT (usando callback_api_version para evitar warning)
    try:
        client = mqtt.Client(client_id="test_client", clean_session=True, callback_api_version=mqtt.CallbackAPIVersion.VERSION1)
    except AttributeError:
        # Fallback para versiones antiguas
        client = mqtt.Client(client_id="test_client", clean_session=True)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.on_subscribe = on_subscribe
    
    try:
        # Conectar al broker
        print(f"\n[->] Conectando al broker...")
        client.connect(BROKER_HOST, BROKER_PORT, 60)
        
        # Iniciar loop en background
        client.loop_start()
        
        # Esperar un momento para que la conexión se establezca
        time.sleep(1)
        
        # Enviar comando
        request_id = f"test_{int(time.time())}"
        if not send_command(client, DEVICE_ID, COMMAND, "", request_id):
            client.loop_stop()
            client.disconnect()
            sys.exit(1)
        
        # Esperar respuesta (timeout de 10 segundos)
        print(f"\n[->] Esperando respuesta... (timeout: 10s)")
        timeout = 10
        elapsed = 0
        while not response_received and elapsed < timeout:
            time.sleep(0.5)
            elapsed += 0.5
            if elapsed % 2 == 0:
                print(f"    Esperando... ({elapsed:.0f}s)")
        
        # Detener loop
        client.loop_stop()
        client.disconnect()
        
        # Verificar resultado
        print("\n" + "=" * 60)
        if response_received:
            print("[OK] TEST EXITOSO - Respuesta recibida")
            print("=" * 60)
            
            # Validar respuesta
            if response_data:
                if response_data.get("status") == "ok":
                    print(f"[OK] Estado: OK")
                    if response_data.get("command") == COMMAND:
                        print(f"[OK] Comando coincidente: {COMMAND}")
                    else:
                        print(f"[!] Advertencia: Comando no coincide")
                    
                    if response_data.get("id") == request_id:
                        print(f"[OK] ID de correlacion coincidente: {request_id}")
                    else:
                        print(f"[!] Advertencia: ID no coincide")
                    
                    if "data" in response_data:
                        print(f"[OK] Datos recibidos")
                    else:
                        print(f"[!] Advertencia: No hay datos en la respuesta")
                    
                    print("\n[OK] RESPUESTA VALIDA")
                    return 0
                else:
                    print(f"[ERROR] TEST FALLIDO - Estado de error: {response_data.get('status')}")
                    if "error" in response_data:
                        print(f"    Error: {response_data['error']}")
                    return 1
        else:
            print("[ERROR] TEST FALLIDO - No se recibio respuesta en el tiempo esperado")
            print("=" * 60)
            return 1
            
    except KeyboardInterrupt:
        print("\n\n[!] Interrumpido por el usuario")
        client.loop_stop()
        client.disconnect()
        return 1
    except Exception as e:
        print(f"\n[ERROR] Error: {e}")
        client.loop_stop()
        client.disconnect()
        return 1

if __name__ == "__main__":
    exit_code = main()
    sys.exit(exit_code)

