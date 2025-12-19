# Comandos MQTT - Documentación

## Formato JSON para Comandos

### Solicitud de Comando

Los comandos se envían al topic de suscripción configurado (`topicSub`, por defecto `datalogger/commands`).

**IMPORTANTE**: El campo `deviceId` es **REQUERIDO** para seguridad. Solo el dispositivo cuyo ID coincida ejecutará el comando. Si el `deviceId` no coincide o falta, el comando será ignorado.

#### Formato Básico (deviceId REQUERIDO)

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "stats",
  "args": ""
}
```

#### Comando con Argumentos

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "read",
  "args": "0 256"
}
```

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "baud",
  "args": "9600"
}
```

#### Comando con ID de Correlación (Opcional)

```json
{
  "deviceId": "AABBCCDDEEFF",
  "command": "stats",
  "args": "",
  "id": "req_12345"
}
```

#### Campos del JSON de Solicitud

- `deviceId` (string, **REQUERIDO**): ID único del dispositivo destino. Solo ese dispositivo ejecutará el comando.
- `command` (string, **REQUERIDO**): Nombre del comando a ejecutar.
- `args` (string, opcional): Argumentos del comando (vacío "" si no hay argumentos).
- `id` (string, opcional): ID de correlación para asociar la respuesta con la solicitud.

### Respuesta del Comando

Las respuestas se publican en el topic de publicación configurado con el sufijo `/response` (por ejemplo, `datalogger/telemetry/response`).

#### Respuesta Exitosa

```json
{
  "type": "command_response",
  "id": "req_12345",
  "command": "stats",
  "status": "ok",
  "message": "STATS_DATA",
  "data": {
    "flash": {
      "usedBytes": 1048576,
      "partitionSize": 4194304,
      "freeBytes": 3145728,
      "wrapCount": 0,
      "totalWritten": 1048576,
      "usedPercent": 25.0
    },
    "transport": {
      "totalBytesReceived": 2048,
      "burstCount": 5,
      "overflowCount": 0,
      "burstActive": false
    },
    "pipeline": {
      "bytesWrittenToFlash": 1048576,
      "bytesDropped": 0,
      "writeOperations": 256,
      "flushOperations": 10,
      "running": true
    }
  }
}
```

#### Respuesta de Error

```json
{
  "type": "command_response",
  "id": "req_12345",
  "command": "format",
  "status": "error",
  "message": "PERMISSION_DENIED",
  "error": "Command not allowed from this medium"
}
```

#### Respuesta sin ID

Si no se proporcionó un ID en la solicitud:

```json
{
  "type": "command_response",
  "command": "config",
  "status": "ok",
  "message": "CONFIG_DATA",
  "data": {
    "device": {
      "name": "DataLogger",
      "id": "AABBCCDDEEFF",
      "type": 0
    },
    "network": {
      "lan": {
        "enabled": true,
        "staticIp": "192.168.29.10"
      },
      "wlanOp": {
        "enabled": false,
        "ssid": ""
      },
      "wlanSafe": {
        "ssid": "DataLogger-AP",
        "channel": 6
      }
    }
  }
}
```

## Comandos Disponibles

### Comandos Permitidos desde MQTT

- `stats` - Obtener estadísticas del sistema
- `config` - Obtener configuración del dispositivo
- `help` - Listar comandos disponibles

### Comandos NO Permitidos desde MQTT (Seguridad)

Los siguientes comandos requieren acceso directo (DEBUG o WEB) por seguridad:
- `format` / `erase` - Formatear memoria flash
- `read` - Leer datos de la flash
- `baud` - Cambiar velocidad UART
- `reset` / `reboot` - Reiniciar el dispositivo

## Ejemplos de Uso

### Obtener Estadísticas

**Publicar en topic `datalogger/commands`:**
```json
{
  "command": "stats"
}
```

**Respuesta en topic `datalogger/telemetry/response`:**
```json
{
  "type": "command_response",
  "command": "stats",
  "status": "ok",
  "message": "STATS_DATA",
  "data": { ... }
}
```

### Obtener Configuración

**Publicar en topic `datalogger/commands`:**
```json
{
  "command": "config",
  "id": "get_config_001"
}
```

### Comando con ID de Correlación

**Publicar:**
```json
{
  "command": "stats",
  "args": "",
  "id": "monitoring_20240101_120000"
}
```

**Respuesta:**
```json
{
  "type": "command_response",
  "id": "monitoring_20240101_120000",
  "command": "stats",
  "status": "ok",
  "message": "STATS_DATA",
  "data": { ... }
}
```

## Integración

El sistema de comandos MQTT se integra automáticamente cuando:
1. Se inicializa `MqttManager`
2. Se llama a `MqttCommandHandler::init()` con la instancia de `MqttClient`
3. El cliente MQTT se suscribe al topic de comandos configurado

Ver `src/utils/MqttCommandHandler.cpp` para más detalles de implementación.

## Logs

Todos los comandos ejecutados vía MQTT se registran en la UART de debug con el prefijo `[MQTT]`, por ejemplo:

```
[MQTT] Executing command: stats
[MQTT] Command stats executed successfully: STATS_DATA
```

