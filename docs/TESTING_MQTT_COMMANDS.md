# Pruebas de Comandos MQTT

## Pre-requisitos

1. **ESP32 debe estar conectado al broker MQTT**
   - Verifica que la configuración MQTT esté correcta en la configuración del dispositivo
   - El dispositivo debe estar conectado (ver logs: `"MQTT connected - Activating command handler"`)

2. **Cliente MQTT para enviar comandos**
   - Puedes usar: `mosquitto_pub`, MQTT Explorer, MQTT.fx, o cualquier cliente MQTT

## Configuración MQTT

Verifica en la configuración del dispositivo:
- `mqtt.host`: Dirección del broker (ej: `192.168.1.100` o `mqtt.example.com`)
- `mqtt.port`: Puerto del broker (típicamente `1883`)
- `mqtt.topicSub`: Topic para recibir comandos (por defecto: `datalogger/commands`)
- `mqtt.topicPub`: Topic base para publicar respuestas (por defecto: `datalogger/telemetry`)

## Formato JSON para Comando HELP

**IMPORTANTE**: Todos los comandos MQTT DEBEN incluir el campo `deviceId` para dirigir el comando al dispositivo específico. Sin este campo, el comando será ignorado por seguridad.

### Opción 1: Formato Básico (con deviceId requerido)

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "help",
  "args": ""
}
```

### Opción 2: Con ID de Correlación

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "help",
  "args": "",
  "id": "test_001"
}
```

### Obtener el Device ID

El `deviceId` es el ID único del dispositivo. Puedes obtenerlo:
- Desde la configuración del dispositivo (campo `device.id`)
- Desde la página web de configuración
- Desde el comando `config` ejecutado previamente
- En los logs del ESP32 durante la inicialización

## Cómo Enviar el Comando

### Usando mosquitto_pub (línea de comandos)

**Reemplaza `AABBCCDDEEFF` con el ID real de tu dispositivo:**

```bash
mosquitto_pub -h <BROKER_IP> -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"help","args":""}'
```

Ejemplo con broker en `192.168.1.100`:
```bash
mosquitto_pub -h 192.168.1.100 -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"help","args":""}'
```

### Usando mosquitto_pub con ID de correlación

```bash
mosquitto_pub -h 192.168.1.100 -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"help","args":"","id":"test_help_001"}'
```

### Suscribirse para Ver la Respuesta

En otra terminal, suscríbete al topic de respuestas:
```bash
mosquitto_sub -h 192.168.1.100 -p 1883 -t "datalogger/telemetry/response"
```

O si configuraste un topic personalizado:
```bash
mosquitto_sub -h 192.168.1.100 -p 1883 -t "<topicPub>/response"
```

## Respuesta Esperada

Cuando ejecutes el comando `help`, deberías recibir una respuesta como:

```json
{
  "type": "command_response",
  "command": "help",
  "status": "ok",
  "message": "HELP",
  "data": "Available commands:\n  format - Erase flash and reset statistics\n  erase - Erase flash and reset statistics (alias)\n  stats - Get system statistics\n  read - Read data from flash (usage: read <offset> <length>)\n  baud - Get or set UART baudrate (usage: baud [rate])\n  config - Get device configuration\n  reset - Reboot the system\n  reboot - Reboot the system (alias)\n  help - Show available commands\n"
}
```

Si enviaste con ID:
```json
{
  "type": "command_response",
  "id": "test_help_001",
  "command": "help",
  "status": "ok",
  "message": "HELP",
  "data": "..."
}
```

## Logs en UART de Debug

En la consola serial del ESP32 deberías ver:

```
[MQTT] Executing command: help
[MQTT] Command help executed successfully: HELP
```

## Otros Comandos de Prueba

### Comando STATS

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "stats",
  "args": ""
}
```

```bash
mosquitto_pub -h 192.168.1.100 -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"stats","args":""}'
```

### Comando CONFIG

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "config",
  "args": ""
}
```

```bash
mosquitto_pub -h 192.168.1.100 -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"config","args":""}'
```

## Troubleshooting

### No recibo respuesta

1. **Verifica que MQTT esté conectado:**
   - Busca en los logs: `"MQTT connected - Activating command handler"`
   - Si no aparece, verifica la configuración de red y broker

2. **Verifica el topic de suscripción:**
   - Asegúrate de estar suscrito al topic correcto: `<topicPub>/response`

3. **Verifica permisos del comando:**
   - El comando `help` está permitido desde MQTT
   - Los comandos `format`, `reset`, etc. NO están permitidos desde MQTT por seguridad

### Comando no reconocido

- Verifica que el JSON esté bien formateado
- Verifica que el campo `command` esté presente y escrito correctamente
- Los comandos son case-sensitive: `"help"` funciona, `"Help"` no

### Handler no activo

Si ves en los logs: `"Handler not active or not initialized"`:
- Verifica que MQTT esté conectado
- Verifica que `MqttCommandHandler::init()` haya sido llamado
- Revisa los logs de inicialización

## Ejemplo Completo de Prueba

**Terminal 1 - Suscripción a respuestas:**
```bash
mosquitto_sub -h 192.168.1.100 -p 1883 -t "datalogger/telemetry/response" -v
```

**Terminal 2 - Envío de comando:**
```bash
mosquitto_pub -h 192.168.1.100 -p 1883 -t "datalogger/commands" -m '{"deviceId":"AABBCCDDEEFF","command":"help","args":""}'
```

**Terminal 1 debería mostrar:**
```
datalogger/telemetry/response {"type":"command_response","command":"help","status":"ok","message":"HELP","data":"Available commands:\n..."}
```

