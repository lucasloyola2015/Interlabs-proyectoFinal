#include "MqttClient.h"
#include "config/ConfigManager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include <cstring>
#include <ctime>

static const char *TAG = "MqttClient";

// Tiempo entre intentos de reconexión (ms)
static const uint32_t RECONNECT_DELAY_MS = 5000;
static const uint32_t MAX_RECONNECT_DELAY_MS = 60000;

MqttClient::MqttClient()
    : m_client(nullptr), m_state(State::DISCONNECTED), m_autoReconnect(true),
      m_reconnectAttempts(0), m_lastReconnectAttempt(0), m_port(1883),
      m_qos(1), m_useAuth(false) {
  m_host[0] = '\0';
  m_username[0] = '\0';
  m_password[0] = '\0';
  m_topicPub[0] = '\0';
  m_topicSub[0] = '\0';
  m_clientId[0] = '\0';
}

MqttClient::~MqttClient() {
  if (m_client) {
    disconnect();
    esp_mqtt_client_destroy(m_client);
    m_client = nullptr;
  }
}

esp_err_t MqttClient::init() {
  // Cargar configuración desde ConfigManager
  esp_err_t ret = reloadConfig();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error al cargar configuración MQTT: %s", esp_err_to_name(ret));
    return ret;
  }

  // Verificar que tenemos configuración válida
  if (strlen(m_host) == 0) {
    ESP_LOGE(TAG, "Host MQTT no configurado");
    return ESP_ERR_INVALID_ARG;
  }

  if (m_port == 0) {
    ESP_LOGE(TAG, "Puerto MQTT no configurado");
    return ESP_ERR_INVALID_ARG;
  }

  // Construir URI del broker (sin credenciales en la URI)
  char uri[128];
  snprintf(uri, sizeof(uri), "mqtt://%s:%d", m_host, m_port);

  ESP_LOGI(TAG, "Inicializando cliente MQTT: %s (puerto %d)", m_host, m_port);

  // Configurar cliente MQTT
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = uri;
  mqtt_cfg.session.keepalive = 60;
  mqtt_cfg.session.disable_clean_session = false;
  mqtt_cfg.session.last_will.topic = nullptr; // Sin Last Will por ahora

  // Configurar autenticación si está habilitada
  if (m_useAuth && strlen(m_username) > 0) {
    mqtt_cfg.credentials.username = m_username;
    if (strlen(m_password) > 0) {
      mqtt_cfg.credentials.authentication.password = m_password;
    }
  }

  // Configurar ID del cliente si está disponible
  if (strlen(m_clientId) > 0) {
    mqtt_cfg.credentials.client_id = m_clientId;
  } else {
    // Generar ID único basado en MAC
    ConfigManager::FullConfig config;
    if (ConfigManager::getConfig(&config) == ESP_OK) {
      if (strlen(config.device.id) > 0) {
        snprintf(m_clientId, sizeof(m_clientId), "datalogger_%s",
                 config.device.id);
        mqtt_cfg.credentials.client_id = m_clientId;
      }
    }
  }

  // Crear cliente MQTT
  m_client = esp_mqtt_client_init(&mqtt_cfg);
  if (!m_client) {
    ESP_LOGE(TAG, "Error al crear cliente MQTT");
    return ESP_FAIL;
  }

  // Registrar handler de eventos
  esp_mqtt_client_register_event(m_client, MQTT_EVENT_ANY, mqttEventHandler,
                                 this);

  ESP_LOGI(TAG, "Cliente MQTT inicializado correctamente");
  return ESP_OK;
}

esp_err_t MqttClient::reloadConfig() {
  ConfigManager::FullConfig config;
  esp_err_t ret = ConfigManager::getConfig(&config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error al cargar configuración: %s", esp_err_to_name(ret));
    return ret;
  }

  // Copiar configuración MQTT
  strncpy(m_host, config.mqtt.host, sizeof(m_host) - 1);
  m_host[sizeof(m_host) - 1] = '\0';
  m_port = config.mqtt.port;
  m_qos = config.mqtt.qos;
  if (m_qos > 2) m_qos = 1; // Validar QoS (0, 1 o 2)
  m_useAuth = config.mqtt.useAuth;

  if (m_useAuth) {
    strncpy(m_username, config.mqtt.username, sizeof(m_username) - 1);
    m_username[sizeof(m_username) - 1] = '\0';
    strncpy(m_password, config.mqtt.password, sizeof(m_password) - 1);
    m_password[sizeof(m_password) - 1] = '\0';
  } else {
    m_username[0] = '\0';
    m_password[0] = '\0';
  }

  // Copy topics ensuring null termination
  memset(m_topicPub, 0, sizeof(m_topicPub));
  strncpy(m_topicPub, config.mqtt.topicPub, sizeof(m_topicPub) - 1);
  m_topicPub[sizeof(m_topicPub) - 1] = '\0';
  
  memset(m_topicSub, 0, sizeof(m_topicSub));
  strncpy(m_topicSub, config.mqtt.topicSub, sizeof(m_topicSub) - 1);
  m_topicSub[sizeof(m_topicSub) - 1] = '\0';

  // Generar client ID si no existe
  if (strlen(m_clientId) == 0 && strlen(config.device.id) > 0) {
    snprintf(m_clientId, sizeof(m_clientId), "datalogger_%s", config.device.id);
  }

  ESP_LOGI(TAG, "Configuracion MQTT cargada: %s:%d, QoS=%d, Pub=[%s], Sub=[%s]",
           m_host, m_port, m_qos, m_topicPub, m_topicSub);

  return ESP_OK;
}

esp_err_t MqttClient::connect() {
  if (!m_client) {
    ESP_LOGE(TAG, "Cliente MQTT no inicializado");
    return ESP_ERR_INVALID_STATE;
  }

  if (m_state == State::CONNECTED || m_state == State::CONNECTING) {
    ESP_LOGW(TAG, "Cliente MQTT ya está conectado o conectando");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Conectando al broker MQTT %s:%d...", m_host, m_port);
  m_state = State::CONNECTING;
  m_reconnectAttempts = 0;

  esp_err_t ret = esp_mqtt_client_start(m_client);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error al iniciar cliente MQTT: %s", esp_err_to_name(ret));
    m_state = State::ERROR;
    return ret;
  }

  return ESP_OK;
}

esp_err_t MqttClient::disconnect() {
  if (!m_client) {
    return ESP_ERR_INVALID_STATE;
  }

  if (m_state == State::DISCONNECTED) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Desconectando del broker MQTT...");
  m_autoReconnect = false; // Deshabilitar reconexión automática
  esp_err_t ret = esp_mqtt_client_stop(m_client);
  m_state = State::DISCONNECTED;

  return ret;
}

esp_err_t MqttClient::publish(const uint8_t *payload, size_t payloadLen,
                              int qos, bool retain) {
  if (strlen(m_topicPub) == 0) {
    ESP_LOGE(TAG, "Topic de publicación no configurado");
    return ESP_ERR_INVALID_ARG;
  }
  // Si no se especifica QoS (valor -1), usar el de la configuración
  if (qos < 0) {
    qos = m_qos;
  }
  return publish(m_topicPub, payload, payloadLen, qos, retain);
}

esp_err_t MqttClient::publish(const char *topic, const uint8_t *payload,
                              size_t payloadLen, int qos, bool retain) {
  if (!m_client || m_state != State::CONNECTED) {
    ESP_LOGW(TAG, "Cliente MQTT no conectado, no se puede publicar");
    return ESP_ERR_INVALID_STATE;
  }

  if (!topic || !payload) {
    ESP_LOGE(TAG, "Topic o payload inválido");
    return ESP_ERR_INVALID_ARG;
  }

  // Si no se especifica QoS (valor -1), usar el de la configuración
  if (qos < 0) {
    qos = m_qos;
  }

  int msg_id = esp_mqtt_client_publish(m_client, topic, (const char *)payload,
                                       payloadLen, qos, retain);
  if (msg_id < 0) {
    ESP_LOGE(TAG, "Error al publicar mensaje en %s", topic);
    return ESP_FAIL;
  }

  ESP_LOGD(TAG, "Mensaje publicado en %s (ID: %d, tamaño: %zu, QoS: %d)",
           topic, msg_id, payloadLen, qos);
  return ESP_OK;
}

esp_err_t MqttClient::subscribe() {
  if (strlen(m_topicSub) == 0) {
    ESP_LOGE(TAG, "Topic de suscripción no configurado");
    return ESP_ERR_INVALID_ARG;
  }
  return subscribe(m_topicSub, m_qos);
}

esp_err_t MqttClient::subscribe(const char *topic, int qos) {
  if (!m_client || m_state != State::CONNECTED) {
    ESP_LOGW(TAG, "Cliente MQTT no conectado, no se puede suscribir");
    return ESP_ERR_INVALID_STATE;
  }

  if (!topic) {
    ESP_LOGE(TAG, "Topic inválido");
    return ESP_ERR_INVALID_ARG;
  }

  // Si no se especifica QoS (valor -1), usar el de la configuración
  if (qos < 0) {
    qos = m_qos;
  }

  int msg_id = esp_mqtt_client_subscribe(m_client, topic, qos);
  if (msg_id < 0) {
    ESP_LOGE(TAG, "Error al suscribirse a %s", topic);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Suscrito a %s (QoS: %d, ID: %d)", topic, qos, msg_id);
  return ESP_OK;
}

esp_err_t MqttClient::unsubscribe() {
  if (strlen(m_topicSub) == 0) {
    ESP_LOGE(TAG, "Topic de suscripción no configurado");
    return ESP_ERR_INVALID_ARG;
  }
  return unsubscribe(m_topicSub);
}

esp_err_t MqttClient::unsubscribe(const char *topic) {
  if (!m_client || m_state != State::CONNECTED) {
    ESP_LOGW(TAG, "Cliente MQTT no conectado, no se puede desuscribir");
    return ESP_ERR_INVALID_STATE;
  }

  if (!topic) {
    ESP_LOGE(TAG, "Topic inválido");
    return ESP_ERR_INVALID_ARG;
  }

  int msg_id = esp_mqtt_client_unsubscribe(m_client, topic);
  if (msg_id < 0) {
    ESP_LOGE(TAG, "Error al desuscribirse de %s", topic);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Desuscrito de %s (ID: %d)", topic, msg_id);
  return ESP_OK;
}

void MqttClient::mqttEventHandler(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data) {
  MqttClient *client = static_cast<MqttClient *>(handler_args);
  if (client) {
    client->handleMqttEvent(event_id, event_data);
  }
}

void MqttClient::handleMqttEvent(int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Conectado al broker MQTT");
    m_state = State::CONNECTED;
    m_reconnectAttempts = 0;

    // Notificar callback de conexión
    if (m_connectionCallback) {
      m_connectionCallback(true);
    }

    // Suscribirse automáticamente al topic configurado
    if (strlen(m_topicSub) > 0) {
      subscribe();
    }
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Desconectado del broker MQTT");
    m_state = State::DISCONNECTED;

    // Notificar callback de conexión
    if (m_connectionCallback) {
      m_connectionCallback(false);
    }

    // Intentar reconectar si está habilitado
    if (m_autoReconnect) {
      attemptReconnect();
    }
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "Suscrito correctamente (msg_id=%d)", event->msg_id);
    break;

  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "Desuscrito correctamente (msg_id=%d)", event->msg_id);
    break;

  case MQTT_EVENT_PUBLISHED:
    ESP_LOGD(TAG, "Mensaje publicado (msg_id=%d)", event->msg_id);
    break;

  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "Mensaje recibido en %.*s (tamaño: %d)", event->topic_len,
             event->topic, event->data_len);

    // Llamar callback si está configurado
    if (m_messageCallback) {
      // Crear strings null-terminated
      char topic[128];
      size_t topicLen = event->topic_len < sizeof(topic) - 1
                            ? event->topic_len
                            : sizeof(topic) - 1;
      memcpy(topic, event->topic, topicLen);
      topic[topicLen] = '\0';

      m_messageCallback(topic, (const uint8_t *)event->data, event->data_len);
    }
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "Error MQTT: %s", esp_err_to_name(event->error_handle->error_type));
    m_state = State::ERROR;

    // Intentar reconectar si está habilitado
    if (m_autoReconnect) {
      attemptReconnect();
    }
    break;

  default:
    ESP_LOGD(TAG, "Evento MQTT: %ld", event_id);
    break;
  }
}

void MqttClient::attemptReconnect() {
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

  // Calcular delay exponencial
  uint32_t delay = RECONNECT_DELAY_MS;
  if (m_reconnectAttempts > 0) {
    delay = RECONNECT_DELAY_MS * (1 << (m_reconnectAttempts - 1));
    if (delay > MAX_RECONNECT_DELAY_MS) {
      delay = MAX_RECONNECT_DELAY_MS;
    }
  }

  // Verificar si ha pasado suficiente tiempo desde el último intento
  if (now - m_lastReconnectAttempt < delay) {
    return; // Aún no es momento de reconectar
  }

  m_lastReconnectAttempt = now;
  m_reconnectAttempts++;

  ESP_LOGW(TAG, "Intentando reconectar al broker MQTT (intento %lu)...",
           m_reconnectAttempts);

  // Reiniciar cliente
  if (m_client) {
    esp_mqtt_client_stop(m_client);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Esperar un segundo
    esp_mqtt_client_start(m_client);
  }
}

