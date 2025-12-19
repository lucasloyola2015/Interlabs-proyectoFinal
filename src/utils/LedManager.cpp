#include "LedManager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <atomic>

namespace LedManager {

static const char *TAG = "LedManager";
static const gpio_num_t LED_GPIO = GPIO_NUM_2; // Default onboard LED

struct LedParams {
  uint32_t onTimeMs;
  uint32_t offTimeMs;
};

static LedParams s_stateParams[] = {
    {0, 1000},  // IDLE: Off (0ms on)
    {1000, 0},  // STARTUP: Continuous ON
    {50, 50},   // DATA_ACTIVITY: 50ms ON / 50ms OFF (100ms period)
    {300, 300}, // HOLD_3S: 300ms ON / 300ms OFF
    {100, 100}, // HOLD_8S: 100ms ON / 100ms OFF (Faster than 3s hold)
    {1000, 0}   // FACTORY_READY: Continuous ON
};

static std::atomic<State> s_currentState(State::IDLE);
static esp_timer_handle_t s_ledTimer = nullptr;
static bool s_ledOn = false;
static bool s_dataActive = false;

static void led_timer_callback(void *arg) {
  State state = s_currentState.load();

  // Override IDLE if data is active
  if (state == State::IDLE && s_dataActive) {
    state = State::DATA_ACTIVITY;
  }

  uint32_t index = static_cast<uint32_t>(state);
  LedParams params = s_stateParams[index];

  if (params.onTimeMs == 0) {
    gpio_set_level(LED_GPIO, 0);
    s_ledOn = false;
    // Reschedule for a default time to check again
    esp_timer_start_once(s_ledTimer, 100000); // 100ms
    return;
  }

  if (params.offTimeMs == 0) {
    gpio_set_level(LED_GPIO, 1);
    s_ledOn = true;
    // Reschedule for a default time to check again
    esp_timer_start_once(s_ledTimer, 100000); // 100ms
    return;
  }

  // Toggling logic
  s_ledOn = !s_ledOn;
  gpio_set_level(LED_GPIO, s_ledOn ? 1 : 0);

  uint32_t nextIntervalMs = s_ledOn ? params.onTimeMs : params.offTimeMs;
  esp_timer_start_once(s_ledTimer, nextIntervalMs * 1000);
}

esp_err_t init() {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << LED_GPIO);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  const esp_timer_create_args_t timer_args = {.callback = &led_timer_callback,
                                              .name = "led_timer"};

  esp_err_t ret = esp_timer_create(&timer_args, &s_ledTimer);
  if (ret == ESP_OK) {
    s_currentState = State::STARTUP;
    esp_timer_start_once(s_ledTimer, 10000); // Start in 10ms
    ESP_LOGI(TAG, "Initialized with GPIO %d", LED_GPIO);
  }

  return ret;
}

void setState(State state) { s_currentState.store(state); }

State getState() { return s_currentState.load(); }

void setDataActivity(bool active) { s_dataActive = active; }

} // namespace LedManager
