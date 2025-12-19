# Test de Comandos MQTT

## Requisitos

Instalar la librería paho-mqtt:
```bash
pip install paho-mqtt
```

## Uso

Ejecutar el script de prueba:
```bash
python test_mqtt_command.py
```

## Configuración

Editar las variables al inicio del script `test_mqtt_command.py`:

```python
BROKER_HOST = "localhost"           # Dirección del broker MQTT
BROKER_PORT = 1883                  # Puerto del broker
COMMAND_TOPIC = "datalogger/commands"  # Topic para enviar comandos
RESPONSE_TOPIC = "datalogger/telemetry/response"  # Topic para recibir respuestas
DEVICE_ID = "FJACFFBI"              # ID del dispositivo destino
COMMAND = "help"                    # Comando a ejecutar
```

## Comportamiento

El script:
1. Se conecta al broker MQTT localhost
2. Se suscribe al topic de respuestas
3. Envía el comando `help` al dispositivo con ID `FJACFFBI`
4. Espera la respuesta (timeout: 10 segundos)
5. Valida la respuesta recibida
6. Muestra el resultado del test

## Ejemplos de uso

### Probar comando STATS
Editar en el script:
```python
COMMAND = "stats"
```

### Probar comando CONFIG
Editar en el script:
```python
COMMAND = "config"
```

### Cambiar Device ID
Editar en el script:
```python
DEVICE_ID = "OTRO_DEVICE_ID"
```

## Salida esperada

Si todo funciona correctamente, deberías ver:

```
[✓] Conectado al broker MQTT en localhost:1883
[✓] Suscrito al topic: datalogger/telemetry/response
[✓] Suscripción confirmada (QoS: 1)
[→] Enviando comando a 'datalogger/commands':
    {"deviceId": "FJACFFBI", "command": "help", "args": ""}
[✓] Comando enviado exitosamente
[→] Esperando respuesta... (timeout: 10s)
[→] Respuesta recibida en 'datalogger/telemetry/response':
    {"type":"command_response","id":"test_1234567890","command":"help",...}
[✓] TEST EXITOSO - Respuesta recibida
[✓] Estado: OK
[✓] Comando coincidente: help
[✓] ID de correlación coincidente: test_1234567890
[✓] Datos recibidos
[✓] RESPUESTA VÁLIDA
```

