#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include <cstddef>
#include <cstdint>
#include <functional>

/**
 * @brief Cliente MQTT para DataLogger
 * 
 * Gestiona la conexión, publicación y suscripción a un broker MQTT.
 * La configuración (IP, puerto, topics, autenticación) se obtiene desde NVS
 * mediante ConfigManager.
 */
class MqttClient {
public:
  /**
   * @brief Callback para mensajes recibidos
   * @param topic Topic del mensaje
   * @param payload Datos del mensaje
   * @param payloadLen Longitud de los datos
   */
  using MessageCallback = std::function<void(const char *topic, const uint8_t *payload, size_t payloadLen)>;

  /**
   * @brief Callback para eventos de conexión
   * @param connected true si está conectado, false si desconectado
   */
  using ConnectionCallback = std::function<void(bool connected)>;

  /**
   * @brief Estado del cliente MQTT
   */
  enum class State {
    DISCONNECTED,  ///< Desconectado
    CONNECTING,    ///< Conectando
    CONNECTED,     ///< Conectado
    ERROR          ///< Error
  };

  /**
   * @brief Constructor
   */
  MqttClient();

  /**
   * @brief Destructor
   */
  ~MqttClient();

  /**
   * @brief Inicializa el cliente MQTT
   * Carga la configuración desde ConfigManager y prepara el cliente
   * @return ESP_OK en éxito
   */
  esp_err_t init();

  /**
   * @brief Inicia la conexión al broker MQTT
   * @return ESP_OK en éxito
   */
  esp_err_t connect();

  /**
   * @brief Desconecta del broker MQTT
   * @return ESP_OK en éxito
   */
  esp_err_t disconnect();

  /**
   * @brief Publica un mensaje en el topic configurado
   * @param payload Datos a publicar
   * @param payloadLen Longitud de los datos
   * @param qos Nivel de calidad de servicio (0, 1 o 2). Si es -1, usa el QoS de la configuración
   * @param retain Si true, el broker retiene el mensaje
   * @return ESP_OK en éxito
   */
  esp_err_t publish(const uint8_t *payload, size_t payloadLen, int qos = -1, bool retain = false);

  /**
   * @brief Publica un mensaje en un topic específico
   * @param topic Topic donde publicar
   * @param payload Datos a publicar
   * @param payloadLen Longitud de los datos
   * @param qos Nivel de calidad de servicio (0, 1 o 2). Si es -1, usa el QoS de la configuración
   * @param retain Si true, el broker retiene el mensaje
   * @return ESP_OK en éxito
   */
  esp_err_t publish(const char *topic, const uint8_t *payload, size_t payloadLen, int qos = -1, bool retain = false);

  /**
   * @brief Suscribe al topic configurado
   * @return ESP_OK en éxito
   */
  esp_err_t subscribe();

  /**
   * @brief Suscribe a un topic específico
   * @param topic Topic al que suscribirse
   * @param qos Nivel de calidad de servicio (0, 1 o 2). Si es -1, usa el QoS de la configuración
   * @return ESP_OK en éxito
   */
  esp_err_t subscribe(const char *topic, int qos = -1);

  /**
   * @brief Desuscribe del topic configurado
   * @return ESP_OK en éxito
   */
  esp_err_t unsubscribe();

  /**
   * @brief Desuscribe de un topic específico
   * @param topic Topic del que desuscribirse
   * @return ESP_OK en éxito
   */
  esp_err_t unsubscribe(const char *topic);

  /**
   * @brief Obtiene el estado actual del cliente
   * @return Estado actual
   */
  State getState() const { return m_state; }

  /**
   * @brief Verifica si está conectado
   * @return true si está conectado
   */
  bool isConnected() const { return m_state == State::CONNECTED; }

  /**
   * @brief Establece el callback para mensajes recibidos
   * @param callback Función a llamar cuando se recibe un mensaje
   */
  void setMessageCallback(MessageCallback callback) { m_messageCallback = callback; }

  /**
   * @brief Establece el callback para eventos de conexión
   * @param callback Función a llamar cuando cambia el estado de conexión
   */
  void setConnectionCallback(ConnectionCallback callback) { m_connectionCallback = callback; }

  /**
   * @brief Habilita o deshabilita la reconexión automática
   * @param enabled true para habilitar reconexión automática
   */
  void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }

  /**
   * @brief Obtiene la configuración MQTT actual desde ConfigManager
   * @return ESP_OK en éxito
   */
  esp_err_t reloadConfig();

private:
  /**
   * @brief Callback interno para eventos MQTT
   * @param handler_args Argumentos del handler
   * @param base Base del evento
   * @param event_id ID del evento
   * @param event_data Datos del evento
   */
  static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  /**
   * @brief Maneja eventos MQTT
   * @param event_id ID del evento
   * @param event_data Datos del evento
   */
  void handleMqttEvent(int32_t event_id, void *event_data);

  /**
   * @brief Intenta reconectar al broker
   */
  void attemptReconnect();

  esp_mqtt_client_handle_t m_client;      ///< Handle del cliente MQTT
  State m_state;                          ///< Estado actual
  bool m_autoReconnect;                    ///< Reconexión automática habilitada
  uint32_t m_reconnectAttempts;           ///< Intentos de reconexión
  uint32_t m_lastReconnectAttempt;        ///< Último intento de reconexión (ms)
  
  // Configuración desde NVS
  char m_host[64];                        ///< Host del broker
  uint16_t m_port;                        ///< Puerto del broker
  uint8_t m_qos;                          ///< Quality of Service (0, 1 o 2)
  bool m_useAuth;                         ///< Usar autenticación
  char m_username[32];                    ///< Usuario (si useAuth)
  char m_password[64];                    ///< Contraseña (si useAuth)
  char m_topicPub[64];                    ///< Topic para publicar
  char m_topicSub[64];                    ///< Topic para suscribirse
  char m_clientId[32];                    ///< ID del cliente
  char m_uri[128];                        ///< URI completa del broker (persistente para MQTT config)

  MessageCallback m_messageCallback;      ///< Callback para mensajes
  ConnectionCallback m_connectionCallback; ///< Callback para conexión
};

