#include "WebServer.h"
#include "../config/ConfigManager.h"
#include "../mqtt/MqttClient.h"
#include "../mqtt/MqttManager.h"
#include "../pipeline/DataPipeline.h"
#include "../storage/FlashRing.h"
#include "../transport/TransportTypes.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logo_data.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "WebServer";

// Root credentials (hardcoded)
static const char *ROOT_USER = "Lucas";
static const char *ROOT_PASS = "Syncmaster";

namespace WebServer {

// Module state
static INetworkInterface *s_ethInterface = nullptr;
static INetworkInterface *s_wifiInterface = nullptr;
static httpd_handle_t s_serverHandle = nullptr;
static uint16_t s_port = 80;
static bool s_initialized = false;
static bool s_running = false;
static DataLoggerCallbacks s_dataloggerCallbacks = {};

// Handlers
static esp_err_t rootHandler(httpd_req_t *req);
static esp_err_t logoHandler(httpd_req_t *req);
static esp_err_t apiLoginHandler(httpd_req_t *req);
static esp_err_t apiStatusHandler(httpd_req_t *req);
static esp_err_t apiDataLoggerStatsHandler(httpd_req_t *req);
static esp_err_t apiDataLoggerFormatHandler(httpd_req_t *req);
static esp_err_t apiWifiConfigHandler(httpd_req_t *req);
static esp_err_t apiUserConfigHandler(httpd_req_t *req);
static esp_err_t apiSystemRebootHandler(httpd_req_t *req);
static esp_err_t apiGetFullConfigHandler(httpd_req_t *req);
static esp_err_t apiSaveFullConfigHandler(httpd_req_t *req);
static esp_err_t apiTestMqttHandler(httpd_req_t *req);

esp_err_t init(INetworkInterface *ethInterface,
               INetworkInterface *wifiInterface, uint16_t port) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }
  s_ethInterface = ethInterface;
  s_wifiInterface = wifiInterface;
  s_port = port;
  s_initialized = true;
  ESP_LOGI(TAG, "Web server initialized (port: %d)", s_port);
  return ESP_OK;
}

esp_err_t start() {
  if (!s_initialized)
    return ESP_ERR_INVALID_STATE;
  if (s_running)
    return ESP_OK;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = s_port;
  config.max_uri_handlers = 20;
  config.stack_size = 12288; // 12KB - sufficient for MQTT test handler (MqttManager + FullConfig need ~3KB)

  esp_err_t ret = httpd_start(&s_serverHandle, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
    return ret;
  }

  UriHandler handlers[] = {
      {"/", HTTP_GET, rootHandler},
      {"/logo.png", HTTP_GET, logoHandler},
      {"/api/login", HTTP_POST, apiLoginHandler},
      {"/api/status", HTTP_GET, apiStatusHandler},
      {"/api/datalogger/stats", HTTP_GET, apiDataLoggerStatsHandler},
      {"/api/datalogger/format", HTTP_POST, apiDataLoggerFormatHandler},
      {"/api/wifi/config", HTTP_POST, apiWifiConfigHandler},
      {"/api/user/config", HTTP_POST, apiUserConfigHandler},
      {"/api/system/reboot", HTTP_POST, apiSystemRebootHandler},
      {"/api/config", HTTP_GET, apiGetFullConfigHandler},
      {"/api/config", HTTP_POST, apiSaveFullConfigHandler},
      {"/api/mqtt/test", HTTP_POST, apiTestMqttHandler},
  };

  for (const auto &handler : handlers) {
    registerUri(handler);
  }

  s_running = true;
  ESP_LOGI(TAG, "Web server started on port %d", s_port);
  return ESP_OK;
}

esp_err_t stop() {
  if (!s_running)
    return ESP_OK;
  if (s_serverHandle) {
    httpd_stop(s_serverHandle);
    s_serverHandle = nullptr;
  }
  s_running = false;
  return ESP_OK;
}

esp_err_t deinit() {
  stop();
  s_ethInterface = nullptr;
  s_wifiInterface = nullptr;
  s_initialized = false;
  return ESP_OK;
}

esp_err_t registerUri(const UriHandler &handler) {
  if (!s_serverHandle)
    return ESP_ERR_INVALID_STATE;
  httpd_uri_t uri = {.uri = handler.uri,
                     .method = handler.method,
                     .handler = handler.handler,
                     .user_ctx =
                         handler.userCtx ? handler.userCtxData : nullptr};
  esp_err_t ret = httpd_register_uri_handler(s_serverHandle, &uri);
  return ret;
}

httpd_handle_t getHandle() { return s_serverHandle; }
bool isRunning() { return s_running; }
void setDataLoggerCallbacks(const DataLoggerCallbacks *cb) {
  if (cb)
    s_dataloggerCallbacks = *cb;
  else
    memset(&s_dataloggerCallbacks, 0, sizeof(s_dataloggerCallbacks));
}

// ============== HTML SPA ==============
static esp_err_t rootHandler(httpd_req_t *req) {
  const char *html = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DataLogger Pro</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@20..48,100..700,0..1,-50..200" />
<style>
:root{
  --bg:#0d1117; --card:#161b22; --border:#30363d; --text:#c9d1d9; --sub:#8b949e;
  --accent:#58a6ff; --accent-hover:#1f6feb; --success:#238636; --danger:#da3633;
  --font-size:14px;
}
*{box-sizing:border-box;margin:0;padding:0;scrollbar-width:thin;scrollbar-color:var(--border) var(--bg)}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text);font-size:var(--font-size);line-height:1.5;overflow-x:hidden}
.hidden{display:none!important}
.container{max-width:1000px;margin:0 auto;padding:15px;animation:fadeIn 0.3s ease}
@keyframes fadeIn{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:translateY(0)}}

.header{display:flex;align-items:center;gap:15px;padding:10px 0;border-bottom:1px solid var(--border);margin-bottom:20px}
.header img{height:40px;object-fit:contain}

.nav{display:flex;gap:6px;margin-left:auto}
.nav button{background:transparent;border:1px solid transparent;color:var(--sub);padding:10px 18px;border-radius:6px;cursor:pointer;font-size:13px;display:flex;align-items:center;gap:8px;transition:0.2s}
.nav button:hover{background:var(--card);border-color:var(--border)}
.nav button.active{background:var(--accent-hover);border-color:rgba(255,255,255,0.1);color:#fff}

.card{background:var(--card);border-radius:8px;padding:18px;margin-bottom:18px;border:1px solid var(--border);box-shadow:0 1px 4px rgba(0,0,0,0.3)}
.card h2{font-size:16px;color:var(--accent);margin-bottom:15px;display:flex;align-items:center;gap:10px;font-weight:600;text-transform:uppercase;letter-spacing:0.4px}
.card h2 .material-symbols-outlined{font-size:22px}

.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.stat-item{background:var(--bg);padding:12px;border-radius:6px;border:1px solid #21262d}
.stat-label{font-size:12px;color:var(--sub);margin-bottom:4px;font-weight:500}
.stat-value{font-size:18px;font-weight:600;color:#f0f6fc;font-family:monospace}

.progress-container{margin:12px 0}
.progress-meta{display:flex;justify-content:space-between;font-size:12px;color:var(--sub);margin-bottom:6px}
.progress{height:10px;background:var(--border);border-radius:5px;overflow:hidden}
.progress-bar{height:100%;background:linear-gradient(90deg, #238636, #3fb950);width:0%;transition:width 0.5s ease}

.form-row{display:grid;grid-template-columns:1fr 1fr;gap:15px;margin-bottom:15px}
.form-group{margin-bottom:15px}
.form-group label{display:block;font-size:13px;color:var(--text);margin-bottom:6px;font-weight:500}
.form-group input, .form-group select{width:100%;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:6px;color:#f0f6fc;font-size:14px;outline:none;transition:border-color 0.2s}
.form-group input:focus{border-color:var(--accent)}
.form-group input:disabled{color:var(--sub);background:rgba(255,255,255,0.02)}

.btn{background:var(--success);color:#fff;border:1px solid rgba(240,246,252,0.1);padding:10px 22px;border-radius:6px;cursor:pointer;font-size:13px;font-weight:600;display:inline-flex;align-items:center;gap:10px;transition:0.2s;justify-content:center}
.btn:hover{filter:brightness(1.1)}
.btn:active{transform:scale(0.98)}
.btn-danger{background:var(--danger)}
.btn-secondary{background:var(--border);color:var(--text)}
.btn-accent{background:var(--accent-hover)}
.btn-success{background:var(--success)}
.btn-error{background:var(--danger)}
.btn .material-symbols-outlined{font-size:20px}

.status-row{display:flex;gap:15px;margin-bottom:20px;flex-wrap:wrap}
.status-badge{padding:8px 16px;border-radius:20px;font-size:13px;font-weight:600;display:inline-flex;align-items:center;gap:8px}
.status-badge.ok{background:#23863620;color:#3fb950;border:1px solid #238636}
.status-badge.err{background:#da363320;color:#f85149;border:1px solid #da3633}

/* UI Elements */
.switch-group{display:flex;align-items:center;justify-content:space-between;padding:12px;background:rgba(255,255,255,0.02);border-radius:8px;border:1px solid var(--border);margin-bottom:15px}
.config-group{border:1px solid var(--border);border-radius:8px;margin-bottom:15px;overflow:hidden;background:rgba(255,255,255,0.01)}
.group-header{display:flex;align-items:center;justify-content:space-between;padding:12px;background:rgba(255,255,255,0.02);border-bottom:1px solid transparent;transition:0.2s}
.group-header.open{border-bottom-color:var(--border);background:rgba(255,255,255,0.03)}
.group-content{padding:15px;animation:slideDown 0.2s ease}
@keyframes slideDown{from{opacity:0;transform:translateY(-5px)}to{opacity:1;transform:translateY(0)}}

.switch{position:relative;display:inline-block;width:44px;height:22px}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;transition:.4s;border-radius:22px}
.slider:before{position:absolute;content:"";height:16px;width:16px;left:3px;bottom:3px;background-color:white;transition:.4s;border-radius:50%}
input:checked + .slider{background-color:var(--success)}
input:focus + .slider{box-shadow:0 0 1px var(--success)}
input:checked + .slider:before{transform:translateX(22px)}

.sub-card{border-top:1px solid var(--border);margin-top:15px;padding-top:15px}
.sub-card h3{font-size:14px;color:var(--sub);margin-bottom:12px;display:flex;align-items:center;gap:8px;text-transform:uppercase;letter-spacing:1px}

/* Login */
.login-page{display:flex;height:100vh;align-items:center;justify-content:center;background:radial-gradient(circle at center, #161b22 0%, #0d1117 100%)}
.login-card{width:400px;text-align:center;padding:40px;overflow:hidden}
.login-card img{max-width:100%;height:auto;max-height:75px;width:auto;object-fit:contain;margin-bottom:35px;display:block;margin-left:auto;margin-right:auto}

.msg{margin-top:15px;font-size:13px;padding:10px;border-radius:6px;text-align:center}
.msg.ok{background:#23863620;color:#3fb950}
.msg.err{background:#da363320;color:#f85149}

.mac-label{font-family:monospace;background:var(--bg);padding:6px 12px;border-radius:4px;border:1px solid var(--border);color:var(--accent)}

.material-symbols-outlined{font-variation-settings:'FILL' 0,'wght' 400,'GRAD' 0,'opsz' 24;vertical-align:middle}
</style>
</head>
<body>

<!-- LOGIN VIEW -->
<div id="v-login" class="login-page">
<div class="card login-card">
  <img src="/logo.png" alt="Logo">
  <div class="form-group"><label>Usuario</label><input type="text" id="lUser"></div>
  <div class="form-group"><label>Contraseña</label><input type="password" id="lPass"></div>
  <button class="btn" style="width:100%;justify-content:center;margin-top:15px" onclick="doLogin()">
    <span class="material-symbols-outlined">key</span> Ingresar
  </button>
  <div id="lMsg" class="msg hidden"></div>
</div>
</div>

<!-- MAIN DASHBOARD -->
<div id="v-dash" class="container hidden">
<div class="header">
  <img src="/logo.png" alt="Logo">
  <div class="nav">
    <button class="active" onclick="showView('dash')"><span class="material-symbols-outlined">monitoring</span> Estado</button>
    <button onclick="showView('config')"><span class="material-symbols-outlined">settings</span> Configuración</button>
    <button onclick="logout()"><span class="material-symbols-outlined">logout</span></button>
  </div>
</div>

<div class="status-row">
  <div id="ethStat" class="status-badge err">Ethernet: Offline</div>
  <div id="wifiStat" class="status-badge err">WiFi: Offline</div>
</div>

<div class="card">
  <h2><span class="material-symbols-outlined">storage</span> Memoria Datalog</h2>
  <div class="progress-container">
    <div class="progress-meta"><span id="flashLabels">- / -</span><span id="flashPct">0%</span></div>
    <div class="progress"><div id="flashBar" class="progress-bar"></div></div>
  </div>
  <div class="stats-grid">
    <div class="stat-item"><div class="stat-label">Usado</div><div id="sUsed" class="stat-value">-</div></div>
    <div class="stat-item"><div class="stat-label">Libre</div><div id="sFree" class="stat-value">-</div></div>
    <div class="stat-item"><div class="stat-label">Vueltas</div><div id="sWrap" class="stat-value">-</div></div>
    <div class="stat-item"><div class="stat-label">Total Escrito</div><div id="sTot" class="stat-value">-</div></div>
  </div>
</div>

<div style="display:grid;grid-template-columns:1fr 1fr;gap:15px">
  <div class="card">
    <h2><span class="material-symbols-outlined">cable</span> Transporte</h2>
    <div style="margin-bottom:10px"><div class="stat-label">Modo</div><div id="tType" class="stat-value">-</div></div>
    <div class="grid" style="grid-template-columns:1fr 1fr">
      <div class="stat-item"><div class="stat-label">Total MiB</div><div id="tBytes" class="stat-value">-</div></div>
      <div class="stat-item"><div class="stat-label">Ráfagas</div><div id="tBurst" class="stat-value">-</div></div>
    </div>
  </div>
  <div class="card">
    <h2><span class="material-symbols-outlined">schema</span> Pipeline</h2>
    <div style="margin-bottom:10px"><div class="stat-label">Estado</div><div id="pStat" class="stat-value">-</div></div>
    <div class="grid" style="grid-template-columns:1fr 1fr">
      <div class="stat-item"><div class="stat-label">Escrito</div><div id="pWr" class="stat-value">-</div></div>
      <div class="stat-item"><div class="stat-label">Descartado</div><div id="pDr" class="stat-value">-</div></div>
    </div>
  </div>
</div>
</div>

<!-- CONFIGURATION VIEW -->
<div id="v-config" class="container hidden">
<div class="header">
  <img src="/logo.png" alt="Logo">
  <div class="nav">
    <button onclick="showView('dash')"><span class="material-symbols-outlined">monitoring</span> Estado</button>
    <button class="active" onclick="showView('config')"><span class="material-symbols-outlined">settings</span> Configuración</button>
    <button onclick="logout()"><span class="material-symbols-outlined">logout</span></button>
  </div>
</div>

<!-- 1. BLOQUE DISPOSITIVO -->
<div class="card">
  <h2><span class="material-symbols-outlined">router</span> Dispositivo</h2>
  <div class="form-row">
    <div class="form-group"><label>Nombre del Dispositivo</label><input type="text" id="devName" placeholder="Ej: Planta 1"></div>
    <div class="form-group"><label>Tipo de Dispositivo</label>
      <select id="devType" onchange="uiUpdateBlocks()">
        <option value="COORDINADOR">COORDINADOR</option>
        <option value="ENDPOINT">ENDPOINT</option>
      </select>
    </div>
  </div>
  <div class="form-group">
    <label>ID del Dispositivo</label>
    <span id="devMac" class="mac-label">C0:4E:30:XX:XX:XX</span>
  </div>
</div>

<!-- 2. BLOQUE COMUNICACIONES -->
<div class="card">
  <h2><span class="material-symbols-outlined">hub</span> Comunicaciones</h2>
  
  <!-- A. LAN -->
  <div class="config-group">
    <div id="lanHead" class="group-header">
      <div style="display:flex;align-items:center;gap:10px"><span class="material-symbols-outlined">lan</span> <strong>LAN (Ethernet W5500)</strong></div>
      <label class="switch"><input type="checkbox" id="lanEn" onchange="uiToggleGroup('lan', this.checked)"><span class="slider"></span></label>
    </div>
    <div id="lanSet" class="group-content hidden">
      <div class="form-row">
        <div class="form-group"><label>DHCP</label>
          <select id="lanDhcp" onchange="uiToggleSection('lanIpSet', this.value=='static')">
            <option value="dhcp">Activado (Auto)</option>
            <option value="static">Desactivado (Manual)</option>
          </select>
        </div>
        <div></div>
      </div>
      <div id="lanIpSet" class="hidden">
        <div class="form-row">
          <div class="form-group"><label>Dirección IP</label><input type="text" id="lanIp"></div>
          <div class="form-group"><label>Máscara de Subred</label><input type="text" id="lanMask"></div>
        </div>
        <div class="form-group"><label>Puerta de Enlace</label><input type="text" id="lanGw"></div>
      </div>
    </div>
  </div>

  <!-- B. WLAN-OP (STA) -->
  <div class="config-group">
    <div id="staHead" class="group-header">
      <div style="display:flex;align-items:center;gap:10px"><span class="material-symbols-outlined">wifi</span> <strong>WLAN-OP (Modo STA)</strong></div>
      <label class="switch"><input type="checkbox" id="staEn" onchange="uiToggleGroup('sta', this.checked)"><span class="slider"></span></label>
    </div>
    <div id="staSet" class="group-content hidden">
      <div class="form-row">
        <div class="form-group"><label>SSID</label><input type="text" id="staSsid"></div>
        <div class="form-group"><label>Contraseña</label><input type="password" id="staPass"></div>
      </div>
      <div class="form-row">
        <div class="form-group"><label>Modo IP</label>
          <select id="staDhcp" onchange="uiToggleSection('staIpSet', this.value=='static')">
            <option value="dhcp">DHCP</option>
            <option value="static">Estática</option>
          </select>
        </div>
        <div></div>
      </div>
      <div id="staIpSet" class="hidden">
        <div class="form-row">
          <div class="form-group"><label>Dirección IP</label><input type="text" id="staIp"></div>
          <div class="form-group"><label>Máscara de Subred</label><input type="text" id="staMask"></div>
        </div>
        <div class="form-group"><label>Puerta de Enlace</label><input type="text" id="staGw"></div>
      </div>
    </div>
  </div>

  <!-- C. WLAN-SAFE (AP) -->
  <div class="config-group">
    <div class="group-header open">
      <div style="display:flex;align-items:center;gap:10px"><span class="material-symbols-outlined">security</span> <strong>WLAN-SAFE (Modo AP)</strong></div>
      <div></div>
    </div>
    <div class="group-content">
      <div class="form-row">
        <div class="form-group"><label>SSID del AP</label><input type="text" id="apSsid"></div>
        <div class="form-group"><label>Contraseña</label><input type="password" id="apPass"></div>
      </div>
      <div class="form-row">
        <div class="form-group"><label>Canal</label>
          <select id="apChan">
            <option value="1">Canal 1</option><option value="2">Canal 2</option><option value="3">Canal 3</option>
            <option value="4">Canal 4</option><option value="5">Canal 5</option><option value="6">Canal 6</option>
            <option value="7">Canal 7</option><option value="8">Canal 8</option><option value="9">Canal 9</option>
            <option value="10">Canal 10</option><option value="11">Canal 11</option>
          </select>
        </div>
        <div class="form-group"><label>Visibilidad</label>
          <select id="apHid"><option value="0">Visible</option><option value="1">Oculto</option></select>
        </div>
      </div>
      <div class="form-group"><label>IP Local del AP</label><input type="text" id="apIp" value="192.168.4.1"></div>
    </div>
  </div>
</div>

<!-- 3. BLOQUES CONDICIONALES -->
<!-- COORDINADOR -->
<div id="blkCoord" class="card hidden">
  <h2><span class="material-symbols-outlined">hub</span> Configuración de Coordinador</h2>
  <div style="padding:20px;text-align:center;color:var(--sub)">Sin parámetros adicionales por el momento.</div>
</div>

<!-- ENDPOINT -->
<div id="blkEnd" class="card hidden">
  <h2><span class="material-symbols-outlined">data_saver_on</span> Configuracion del END POINT</h2>
  <div class="form-group"><label>Nombre del Huesped</label><input type="text" id="hostName"></div>
  <div class="form-group"><label>Origen de Datos</label>
    <select id="srcType" onchange="uiUpdateDataSource()">
      <option value="SERIE">SERIE</option>
      <option value="PARALELO">PARALELO</option>
      <option value="DESHABILITADO">DESHABILITADO</option>
    </select>
  </div>

  <div id="srcSerie" class="sub-card hidden">
    <h3><span class="material-symbols-outlined">settings_input_component</span> Configuración Serie</h3>
    <div class="form-row">
      <div class="form-group"><label>Interfaz Física</label>
        <select id="serIf"><option value="RS232">RS232</option><option value="RS485">RS485</option></select>
      </div>
      <div class="form-group"><label>Baudios</label>
        <select id="serBaud">
          <option value="9600">9600</option><option value="19200">19200</option><option value="38400">38400</option>
          <option value="57600">57600</option><option value="115200" selected>115200</option><option value="230400">230400</option>
          <option value="460800">460800</option><option value="921600">921600</option>
        </select>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group"><label>Bits de Datos</label>
        <select id="serBits"><option value="5">5</option><option value="6">6</option><option value="7">7</option><option value="8" selected>8</option></select>
      </div>
      <div class="form-group"><label>Paridad</label>
        <select id="serPari"><option value="none">Ninguna</option><option value="even">Par</option><option value="odd">Impar</option></select>
      </div>
    </div>
    <div class="form-group"><label>Bits de Parada</label>
      <select id="serStop"><option value="1">1</option><option value="1.5">1.5</option><option value="2">2</option></select>
    </div>
  </div>
</div>

<!-- MQTT BROKER (Sólo ENDPOINT) -->
<div id="blkMqtt" class="card hidden">
  <h2><span class="material-symbols-outlined">cloud_queue</span> MQTT Broker</h2>
  <div class="form-row">
    <div class="form-group" style="display:flex;align-items:flex-end;gap:8px">
      <div style="flex:1"><label>Host / IP</label><input type="text" id="mqHost" placeholder="iot.eclipse.org"></div>
      <button class="btn btn-secondary" onclick="testMqttConnection()" id="mqTestBtn" style="white-space:nowrap;padding:10px 16px">
        <span class="material-symbols-outlined">network_check</span> Test
      </button>
    </div>
    <div class="form-group"><label>Puerto</label><input type="number" id="mqPort" value="1883"></div>
    <div class="form-group"><label>QoS</label><input type="number" id="mqQos" value="1" min="0" max="2"></div>
  </div>
  <div class="switch-group" style="margin-bottom:12px">
    <div style="display:flex;align-items:center;gap:10px"><span class="material-symbols-outlined">security</span> <strong>Usar Autenticación</strong></div>
    <label class="switch"><input type="checkbox" id="mqAuth" onchange="uiMqttAuthToggle(this.checked)"><span class="slider"></span></label>
  </div>
  <div class="form-row">
    <div class="form-group"><label>Usuario</label><input type="text" id="mqUser" disabled></div>
    <div class="form-group"><label>Contraseña</label><input type="password" id="mqPass" disabled></div>
  </div>
  <div class="form-row">
    <div class="form-group"><label>Topic Publicación (pub)</label><input type="text" id="mqPub"></div>
    <div class="form-group"><label>Topic Suscripción (sub)</label><input type="text" id="mqSub"></div>
  </div>
</div>

<!-- 4. BLOQUE SISTEMA -->
<div class="card">
  <h2><span class="material-symbols-outlined">settings</span> Sistema</h2>
  
  <div class="form-row">
    <!-- Memoria -->
    <div class="sub-card" style="margin-top:0;padding-top:0">
      <h3><span class="material-symbols-outlined">memory</span> Memoria</h3>
      <div style="display:flex;flex-direction:column;gap:12px">
        <button class="btn btn-secondary" onclick="uiFlashDownload()"><span class="material-symbols-outlined">download_for_offline</span> Descargar Flash</button>
        <button class="btn btn-danger" onclick="formatFlash()"><span class="material-symbols-outlined">delete_forever</span> Formatear Flash</button>
      </div>
    </div>
    <!-- Backup -->
    <div class="sub-card" style="margin-top:0;padding-top:0">
      <h3><span class="material-symbols-outlined">cloud_download</span> Backup</h3>
      <div style="display:flex;flex-direction:column;gap:12px">
        <button class="btn btn-secondary" onclick="uiBackupDownload()"><span class="material-symbols-outlined">download</span> Descargar JSON</button>
        <div style="border:1px dashed var(--border);padding:10px;border-radius:6px">
          <input type="file" id="bkFile" style="width:100%;font-size:12px" accept=".json">
          <button class="btn btn-secondary" style="width:100%;margin-top:8px" onclick="uiBackupUpload()"><span class="material-symbols-outlined">upload</span> Cargar Backup</button>
        </div>
      </div>
    </div>
  </div>

  <div class="sub-card">
    <h3><span class="material-symbols-outlined">lock</span> Acceso WEB</h3>
    <div class="form-row">
      <div class="form-group"><label>Usuario Operador</label><input type="text" id="nuName"></div>
      <div class="form-group"><label>Nueva Contraseña</label><input type="password" id="nuPass"></div>
    </div>
  </div>

  <div class="sub-card" style="display:flex;gap:12px;justify-content:flex-end">
    <button class="btn btn-accent" onclick="saveAll()"><span class="material-symbols-outlined">save</span> GUARDAR</button>
    <button class="btn btn-secondary" onclick="loadConfig()"><span class="material-symbols-outlined">history</span> RESTAURAR</button>
    <button class="btn btn-danger" onclick="reboot()"><span class="material-symbols-outlined">restart_alt</span> REINICIAR</button>
  </div>
</div>

<div id="cfgMsg" class="msg hidden"></div>
</div>

<script>
let token=sessionStorage.getItem('auth')||'';
let pollInt;

function showView(v){
  const views=['v-login','v-dash','v-config'];
  views.forEach(id=>document.getElementById(id).classList.add('hidden'));
  document.getElementById('v-'+v).classList.remove('hidden');
  document.querySelectorAll('.nav button').forEach(b=>b.classList.remove('active'));
  const btns=document.querySelectorAll('.nav button');
  if(v==='dash'&&btns[0])btns[0].classList.add('active');
  if(v==='config'&&btns[1])btns[1].classList.add('active');
  if(v==='config'){ loadConfig(); }
}

/* UI Dynamics */
function uiUpdateBlocks(){
  const type=document.getElementById('devType').value;
  document.getElementById('blkCoord').classList.toggle('hidden', type!=='COORDINADOR');
  document.getElementById('blkEnd').classList.toggle('hidden', type!=='ENDPOINT');
  document.getElementById('blkMqtt').classList.toggle('hidden', type!=='ENDPOINT');
  if(type==='ENDPOINT') uiUpdateDataSource();
}

function uiUpdateDataSource(){
  const src=document.getElementById('srcType').value;
  document.getElementById('srcSerie').classList.toggle('hidden', src!=='SERIE');
}

function uiToggleSection(id, show){
  document.getElementById(id).classList.toggle('hidden', !show);
}

function uiToggleGroup(prefix, show){
  const head = document.getElementById(prefix + 'Head');
  const body = document.getElementById(prefix + 'Set');
  if(head) head.classList.toggle('open', show);
  if(body) body.classList.toggle('hidden', !show);
}

function uiMqttAuthToggle(show){
  document.getElementById('mqUser').disabled = !show;
  document.getElementById('mqPass').disabled = !show;
}

function testMqttConnection(){
  const btn=document.getElementById('mqTestBtn');
  const host=document.getElementById('mqHost').value;
  const port=parseInt(document.getElementById('mqPort').value)||1883;
  const qos=parseInt(document.getElementById('mqQos').value)||1;
  const useAuth=document.getElementById('mqAuth').checked;
  const username=document.getElementById('mqUser').value;
  const password=document.getElementById('mqPass').value;
  
  if(!host){
    const m=document.getElementById('cfgMsg');
    if(m){m.className='msg err';m.textContent='Por favor ingrese un Host/IP';m.classList.remove('hidden');setTimeout(()=>m.classList.add('hidden'),3000);}
    return;
  }
  
  if(btn){
    btn.disabled=true;
    // Guardar estado original
    const originalText=btn.innerHTML;
    const originalClasses=btn.className;
    // Estado de prueba
    btn.className='btn btn-secondary';
    btn.innerHTML='<span class="material-symbols-outlined">hourglass_empty</span> Probando...';
    
    const testCfg={
      host:host,
      port:port,
      qos:qos,
      useAuth:useAuth,
      username:useAuth?username:'',
      password:useAuth?password:''
    };
    
    fetch('/api/mqtt/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(testCfg)})
      .then(r=>r.json())
      .then(d=>{
        const m=document.getElementById('cfgMsg');
        if(m){
          m.className='msg '+(d.success?'ok':'err');
          m.textContent=d.success?(d.message||'Conexión exitosa'):(d.error||'Error de conexión');
          m.classList.remove('hidden');
          setTimeout(()=>m.classList.add('hidden'),5000);
        }
        // Cambiar color del botón según resultado
        if(d.success){
          btn.className='btn btn-success';
          btn.innerHTML='<span class="material-symbols-outlined">check_circle</span> OK';
        }else{
          btn.className='btn btn-error';
          btn.innerHTML='<span class="material-symbols-outlined">error</span> Error';
        }
        btn.disabled=false;
        // Restaurar estado original después de 3 segundos
        setTimeout(()=>{
          btn.className=originalClasses;
          btn.innerHTML=originalText;
        },3000);
      })
      .catch(e=>{
        const m=document.getElementById('cfgMsg');
        if(m){m.className='msg err';m.textContent='Error de conexión: '+e.message;m.classList.remove('hidden');setTimeout(()=>m.classList.add('hidden'),5000);}
        // Botón en rojo por error
        btn.className='btn btn-error';
        btn.innerHTML='<span class="material-symbols-outlined">error</span> Error';
        btn.disabled=false;
        // Restaurar estado original después de 3 segundos
        setTimeout(()=>{
          btn.className=originalClasses;
          btn.innerHTML=originalText;
        },3000);
      });
  }
}

/* Save Configuration */
function saveAll(){
  const cfg={
    device:{
      type:document.getElementById('devType').value==='COORDINADOR'?0:1,
      name:document.getElementById('devName').value||'DataLogger',
      id:document.getElementById('devMac')?document.getElementById('devMac').textContent:''
    },
    network:{
      lan:{
        enabled:document.getElementById('lanEn')?document.getElementById('lanEn').checked:true,
        useDhcp:document.getElementById('lanDhcp')?document.getElementById('lanDhcp').value==='dhcp':false,
        staticIp:document.getElementById('lanIp')?document.getElementById('lanIp').value:'192.168.29.10',
        netmask:document.getElementById('lanMask')?document.getElementById('lanMask').value:'255.255.255.0',
        gateway:document.getElementById('lanGw')?document.getElementById('lanGw').value:'192.168.29.1'
      },
      wlanOp:{
        enabled:document.getElementById('staEn')?document.getElementById('staEn').checked:false,
        ssid:document.getElementById('staSsid')?document.getElementById('staSsid').value:'',
        password:document.getElementById('staPass')?document.getElementById('staPass').value:'',
        useDhcp:document.getElementById('staDhcp')?document.getElementById('staDhcp').value==='dhcp':true,
        staticIp:document.getElementById('staIp')?document.getElementById('staIp').value:'192.168.1.50',
        netmask:document.getElementById('staMask')?document.getElementById('staMask').value:'255.255.255.0',
        gateway:document.getElementById('staGw')?document.getElementById('staGw').value:'192.168.1.1'
      },
      wlanSafe:{
        ssid:document.getElementById('apSsid')?document.getElementById('apSsid').value:'DataLogger-AP',
        password:document.getElementById('apPass')?document.getElementById('apPass').value:'12345678',
        channel:parseInt(document.getElementById('apCh')?document.getElementById('apCh').value:6),
        hidden:document.getElementById('apHidden')?document.getElementById('apHidden').checked:false,
        apIp:document.getElementById('apIp')?document.getElementById('apIp').value:'192.168.4.1'
      },
      webServerPort:80
    },
    endpoint:{
      hostName:document.getElementById('hostName')?document.getElementById('hostName').value:'Device01',
      source:document.getElementById('srcType')&&document.getElementById('srcType').value==='SERIE'?1:0,
      serial:{
        interface:document.getElementById('serInt')&&document.getElementById('serInt').value==='RS485'?1:0,
        baudRate:parseInt(document.getElementById('serBaud')?document.getElementById('serBaud').value:115200),
        dataBits:8,
        parity:0,
        stopBits:1
      }
    },
    mqtt:{
      host:document.getElementById('mqHost')?document.getElementById('mqHost').value:'mqtt.example.com',
      port:parseInt(document.getElementById('mqPort')?document.getElementById('mqPort').value:1883),
      qos:parseInt(document.getElementById('mqQos')?document.getElementById('mqQos').value:1),
      useAuth:document.getElementById('mqAuth')?document.getElementById('mqAuth').checked:false,
      username:document.getElementById('mqUser')?document.getElementById('mqUser').value:'',
      password:document.getElementById('mqPass')?document.getElementById('mqPass').value:'',
      topicPub:document.getElementById('mqPub')?document.getElementById('mqPub').value:'datalogger/telemetry',
      topicSub:document.getElementById('mqSub')?document.getElementById('mqSub').value:'datalogger/commands'
    },
    webUser:{
      username:document.getElementById('nuName')?document.getElementById('nuName').value:'admin',
      password:document.getElementById('nuPass')?document.getElementById('nuPass').value:'admin'
    }
  };
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)}).then(r=>r.json()).then(d=>{
    const m=document.getElementById('cfgMsg');
    if(m){m.className='msg '+(d.success?'ok':'err');m.textContent=d.success?'Configuración guardada correctamente':(d.error||'Error al guardar');m.classList.remove('hidden');setTimeout(()=>m.classList.add('hidden'),5000);}
  }).catch(e=>{
    const m=document.getElementById('cfgMsg');
    if(m){m.className='msg err';m.textContent='Error de conexión';m.classList.remove('hidden');setTimeout(()=>m.classList.add('hidden'),5000);}
  });
}
function uiBackupDownload(){ alert('Descargando backup.json...'); }
function uiBackupUpload(){ alert('Cargando backup.json...'); }
function uiFlashDownload(){ alert('Descargando volcado de flash (bin)...'); }

/* Standard Actions */
function doLogin(){
  const u=document.getElementById('lUser').value;
  const p=document.getElementById('lPass').value;
  const msg=document.getElementById('lMsg');
  fetch('/api/login',{method:'POST',body:JSON.stringify({user:u,pass:p})}).then(r=>r.json()).then(d=>{
    if(d.success){token=d.token;sessionStorage.setItem('auth',token);showView('dash');startPolling();}
    else{msg.className='msg err';msg.textContent=d.error;msg.classList.remove('hidden');}
  });
}
function logout(){token='';sessionStorage.removeItem('auth');stopPolling();showView('login');}
function fmtB(b){if(b===0)return'0 B';const k=1024,s=['B','KiB','MiB','GiB'];const i=Math.floor(Math.log(b)/Math.log(k));return(b/Math.pow(k,i)).toFixed(2)+' '+s[i];}

function refresh(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    const e=document.getElementById('ethStat');
    if(d.ethernet&&d.ethernet.connected){ e.className='status-badge ok'; e.innerHTML='<span class="material-symbols-outlined">lan</span> Ethernet: '+d.ethernet.ip; } 
    else { e.className='status-badge err'; e.innerHTML='<span class="material-symbols-outlined">link_off</span> Ethernet: Offline'; }
    const w=document.getElementById('wifiStat');
    if(d.wifi&&d.wifi.connected){ w.className='status-badge ok'; w.innerHTML='<span class="material-symbols-outlined">wifi</span> WiFi: '+d.wifi.ip; } 
    else { w.className='status-badge err'; w.innerHTML='<span class="material-symbols-outlined">wifi_off</span> WiFi: Offline'; }
  });
  fetch('/api/datalogger/stats').then(r=>r.json()).then(d=>{
    if(d.flash){
      document.getElementById('sUsed').textContent=fmtB(d.flash.usedBytes);
      document.getElementById('sFree').textContent=fmtB(d.flash.freeBytes);
      document.getElementById('sTot').textContent=fmtB(d.flash.totalWritten);
      document.getElementById('sWrap').textContent=d.flash.wrapCount;
      document.getElementById('flashBar').style.width=d.flash.usedPercent+'%';
      document.getElementById('flashPct').textContent=Math.round(d.flash.usedPercent)+'%';
      document.getElementById('flashLabels').textContent=fmtB(d.flash.usedBytes)+' / '+fmtB(d.flash.partitionSize);
    }
  });
}

function loadConfig(){
  fetch('/api/config').then(r=>r.json()).then(d=>{
    if(d.device){
      const devTypeEl=document.getElementById('devType');
      if(devTypeEl){
        devTypeEl.value=d.device.type===0?'COORDINADOR':'ENDPOINT';
        // Actualizar bloques después de establecer el tipo de dispositivo
        uiUpdateBlocks();
      }
      document.getElementById('devName').value=d.device.name||'';
      const macEl=document.getElementById('devMac');
      if(macEl)macEl.textContent=d.device.id||'';
    }
    if(d.network&&d.network.lan){
      const lanEn=document.getElementById('lanEn');
      if(lanEn){lanEn.checked=d.network.lan.enabled;uiToggleGroup('lan',d.network.lan.enabled);}
      const lanDhcp=document.getElementById('lanDhcp');
      if(lanDhcp){
        lanDhcp.value=d.network.lan.useDhcp?'dhcp':'static';
        // Actualizar visibilidad de campos IP según el modo DHCP
        uiToggleSection('lanIpSet', lanDhcp.value==='static');
      }
      const lanIp=document.getElementById('lanIp');
      if(lanIp)lanIp.value=d.network.lan.staticIp||'';
      const lanMask=document.getElementById('lanMask');
      if(lanMask)lanMask.value=d.network.lan.netmask||'';
      const lanGw=document.getElementById('lanGw');
      if(lanGw)lanGw.value=d.network.lan.gateway||'';
    }
    if(d.network&&d.network.wlanOp){
      const staEn=document.getElementById('staEn');
      if(staEn){staEn.checked=d.network.wlanOp.enabled;uiToggleGroup('sta',d.network.wlanOp.enabled);}
      const staSsid=document.getElementById('staSsid');
      if(staSsid)staSsid.value=d.network.wlanOp.ssid||'';
      const staPass=document.getElementById('staPass');
      if(staPass)staPass.value=d.network.wlanOp.password||'';
      const staDhcp=document.getElementById('staDhcp');
      if(staDhcp){
        staDhcp.value=d.network.wlanOp.useDhcp?'dhcp':'static';
        // Actualizar visibilidad de campos IP según el modo DHCP
        uiToggleSection('staIpSet', staDhcp.value==='static');
      }
      const staIp=document.getElementById('staIp');
      if(staIp)staIp.value=d.network.wlanOp.staticIp||'';
      const staMask=document.getElementById('staMask');
      if(staMask)staMask.value=d.network.wlanOp.netmask||'';
      const staGw=document.getElementById('staGw');
      if(staGw)staGw.value=d.network.wlanOp.gateway||'';
    }
    if(d.network&&d.network.wlanSafe){
      const apSsid=document.getElementById('apSsid');
      if(apSsid)apSsid.value=d.network.wlanSafe.ssid||'';
      const apPass=document.getElementById('apPass');
      if(apPass)apPass.value=d.network.wlanSafe.password||'';
      const apCh=document.getElementById('apCh');
      if(apCh)apCh.value=d.network.wlanSafe.channel||6;
    }
    if(d.endpoint){
      const epHost=document.getElementById('hostName');
      if(epHost)epHost.value=d.endpoint.hostName||'';
      const srcType=document.getElementById('srcType');
      if(srcType){
        if(d.endpoint.source===1)srcType.value='SERIE';
        else if(d.endpoint.source===2)srcType.value='PARALELO';
        else srcType.value='DESHABILITADO';
        // Actualizar visibilidad del bloque de fuente de datos
        uiUpdateDataSource();
      }
      if(d.endpoint.serial){
        const serBaud=document.getElementById('serBaud');
        if(serBaud)serBaud.value=d.endpoint.serial.baudRate||115200;
        const serIf=document.getElementById('serIf');
        if(serIf)serIf.value=d.endpoint.serial.interface===1?'RS485':'RS232';
        const serBits=document.getElementById('serBits');
        if(serBits)serBits.value=d.endpoint.serial.dataBits||8;
        const serPari=document.getElementById('serPari');
        if(serPari){
          if(d.endpoint.serial.parity===1)serPari.value='even';
          else if(d.endpoint.serial.parity===2)serPari.value='odd';
          else serPari.value='none';
        }
        const serStop=document.getElementById('serStop');
        if(serStop)serStop.value=d.endpoint.serial.stopBits===2?2:1;
      }
    }
    if(d.mqtt){
      const mqHost=document.getElementById('mqHost');
      if(mqHost)mqHost.value=d.mqtt.host||'';
      const mqPort=document.getElementById('mqPort');
      if(mqPort)mqPort.value=d.mqtt.port||1883;
      const mqQos=document.getElementById('mqQos');
      if(mqQos)mqQos.value=d.mqtt.qos!==undefined?d.mqtt.qos:1;
      const mqAuth=document.getElementById('mqAuth');
      if(mqAuth){mqAuth.checked=d.mqtt.useAuth;uiMqttAuthToggle(d.mqtt.useAuth);}
      const mqUser=document.getElementById('mqUser');
      if(mqUser)mqUser.value=d.mqtt.username||'';
      const mqPass=document.getElementById('mqPass');
      if(mqPass)mqPass.value=d.mqtt.password||'';
      const mqPub=document.getElementById('mqPub');
      if(mqPub)mqPub.value=d.mqtt.topicPub||'';
      const mqSub=document.getElementById('mqSub');
      if(mqSub)mqSub.value=d.mqtt.topicSub||'';
    }
    if(d.webUser){
      const nuName=document.getElementById('nuName');
      if(nuName)nuName.value=d.webUser.username||'';
      const nuPass=document.getElementById('nuPass');
      if(nuPass)nuPass.value=d.webUser.password||'';
    }
  });
}
function saveUser(){
  const u=document.getElementById('nuName').value, p=document.getElementById('nuPass').value;
  fetch('/api/user/config',{method:'POST',body:JSON.stringify({user:u,pass:p})}).then(r=>r.json()).then(showMsg);
}
function showMsg(d){
  const m=document.getElementById('cfgMsg');
  m.className='msg '+(d.success?'ok':'err');
  m.textContent=d.success?(d.message||'Realizado'):(d.error||'Error');
  m.classList.remove('hidden');
  setTimeout(()=>m.classList.add('hidden'),5000);
}
function formatFlash(){if(confirm('¿Borrar todos los datos?'))fetch('/api/datalogger/format',{method:'POST'}).then(r=>r.json()).then(showMsg);}
function reboot(){if(confirm('¿Reiniciar sistema?'))fetch('/api/system/reboot',{method:'POST'});}
function startPolling(){refresh();pollInt=setInterval(refresh,3000);}
function stopPolling(){clearInterval(pollInt);}

if(token){showView('dash');startPolling();}else showView('login');
</script>
</body>
</html>
)HTML";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html, strlen(html));
  return ESP_OK;
}

static esp_err_t logoHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/png");
  httpd_resp_send(req, (const char *)LOGO_PNG_DATA, LOGO_PNG_SIZE);
  return ESP_OK;
}

static esp_err_t apiLoginHandler(httpd_req_t *req) {
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  buf[ret] = '\0';
  char user[32] = {0}, pass[32] = {0};
  char *uStart = strstr(buf, "\"user\"");
  if (uStart) {
    uStart = strchr(uStart, ':');
    if (uStart)
      uStart = strchr(uStart, '"');
    if (uStart) {
      uStart++;
      char *uEnd = strchr(uStart, '"');
      if (uEnd) {
        size_t len = uEnd - uStart;
        if (len > 31)
          len = 31;
        strncpy(user, uStart, len);
      }
    }
  }
  char *pStart = strstr(buf, "\"pass\"");
  if (pStart) {
    pStart = strchr(pStart, ':');
    if (pStart)
      pStart = strchr(pStart, '"');
    if (pStart) {
      pStart++;
      char *pEnd = strchr(pStart, '"');
      if (pEnd) {
        size_t len = pEnd - pStart;
        if (len > 31)
          len = 31;
        strncpy(pass, pStart, len);
      }
    }
  }
  bool valid = (strcmp(user, ROOT_USER) == 0 && strcmp(pass, ROOT_PASS) == 0);
  if (!valid) {
    ConfigManager::FullConfig cfg;
    if (ConfigManager::getConfig(&cfg) == ESP_OK) {
      valid = (strcmp(user, cfg.webUser.username) == 0 &&
               strcmp(pass, cfg.webUser.password) == 0);
    }
  }
  httpd_resp_set_type(req, "application/json");
  if (valid)
    httpd_resp_send(req, "{\"success\":true,\"token\":\"ok\"}", -1);
  else
    httpd_resp_send(
        req, "{\"success\":false,\"error\":\"Credenciales invalidas\"}", -1);
  return ESP_OK;
}

static esp_err_t apiStatusHandler(httpd_req_t *req) {
  char resp[256] = "{";
  auto getIfStat = [](INetworkInterface *iface, char *buf, size_t sz) {
    if (!iface) {
      snprintf(buf, sz, "{\"connected\":false}");
      return;
    }
    Network::IpAddress ip;
    bool conn = iface->isConnected();
    if (conn && iface->getIpAddress(&ip) == ESP_OK)
      snprintf(buf, sz, "{\"connected\":true,\"ip\":\"%d.%d.%d.%d\"}",
               ip.addr[0], ip.addr[1], ip.addr[2], ip.addr[3]);
    else
      snprintf(buf, sz, "{\"connected\":false}");
  };
  char ethBuf[80], wifiBuf[80];
  getIfStat(s_ethInterface, ethBuf, 80);
  getIfStat(s_wifiInterface, wifiBuf, 80);
  snprintf(resp, sizeof(resp), "{\"ethernet\":%s,\"wifi\":%s}", ethBuf,
           wifiBuf);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t apiDataLoggerStatsHandler(httpd_req_t *req) {
  if (!s_dataloggerCallbacks.getFlashStats)
    return ESP_FAIL;
  struct FlashStats {
    size_t partitionSize, usedBytes, freeBytes;
    uint32_t wrapCount, totalWritten;
  } fs = {};
  s_dataloggerCallbacks.getFlashStats(&fs);
  struct TransportStats {
    uint64_t totalBytesReceived;
    uint32_t burstCount, overflowCount;
  } ts = {};
  bool hasTr = s_dataloggerCallbacks.getTransportStats &&
               s_dataloggerCallbacks.getTransportStats(&ts) == ESP_OK;
  struct PipelineStats {
    size_t bytesWrittenToFlash, bytesDropped;
    uint32_t writeOperations, flushOperations;
    bool running;
  } ps = {};
  (void)s_dataloggerCallbacks.getPipelineStats; // Silence unused warning
  if (s_dataloggerCallbacks.getPipelineStats)
    s_dataloggerCallbacks.getPipelineStats(&ps);
  const char *trType = (hasTr && s_dataloggerCallbacks.getTransportTypeName)
                           ? s_dataloggerCallbacks.getTransportTypeName()
                           : "unknown";
  char resp[512];
  snprintf(resp, sizeof(resp),
           "{\"flash\":{\"partitionSize\":%u,\"usedBytes\":%u,\"freeBytes\":%u,"
           "\"usedPercent\":%.1f,\"wrapCount\":%lu,\"totalWritten\":%lu},"
           "\"transport\":{\"totalBytes\":%llu,\"bursts\":%lu,\"overflows\":%"
           "lu,\"type\":\"%s\"},"
           "\"pipeline\":{\"bytesWritten\":%u,\"bytesDropped\":%u,\"writeOps\":"
           "%lu,\"running\":%s}}",
           fs.partitionSize, fs.usedBytes, fs.freeBytes,
           fs.partitionSize > 0 ? (100.0f * fs.usedBytes / fs.partitionSize)
                                : 0.0f,
           fs.wrapCount, fs.totalWritten, ts.totalBytesReceived, ts.burstCount,
           ts.overflowCount, trType, ps.bytesWrittenToFlash, ps.bytesDropped,
           ps.writeOperations, ps.running ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t apiGetFullConfigHandler(httpd_req_t *req) {
  ConfigManager::FullConfig cfg;
  if (ConfigManager::getConfig(&cfg) != ESP_OK)
    return ESP_FAIL;

  // Build complete JSON response with all configuration fields
  char resp[2048];
  int len = snprintf(
      resp, sizeof(resp),
      "{"
      "\"device\":{"
      "\"type\":%d,"
      "\"name\":\"%s\","
      "\"id\":\"%s\""
      "},"
      "\"network\":{"
      "\"lan\":{"
      "\"enabled\":%s,"
      "\"useDhcp\":%s,"
      "\"staticIp\":\"%d.%d.%d.%d\","
      "\"netmask\":\"%d.%d.%d.%d\","
      "\"gateway\":\"%d.%d.%d.%d\""
      "},"
      "\"wlanOp\":{"
      "\"enabled\":%s,"
      "\"ssid\":\"%s\","
      "\"password\":\"%s\","
      "\"useDhcp\":%s,"
      "\"staticIp\":\"%d.%d.%d.%d\","
      "\"netmask\":\"%d.%d.%d.%d\","
      "\"gateway\":\"%d.%d.%d.%d\""
      "},"
      "\"wlanSafe\":{"
      "\"ssid\":\"%s\","
      "\"password\":\"%s\","
      "\"channel\":%d,"
      "\"hidden\":%s,"
      "\"apIp\":\"%d.%d.%d.%d\""
      "},"
      "\"webServerPort\":%d"
      "},"
      "\"endpoint\":{"
      "\"hostName\":\"%s\","
      "\"source\":%d,"
      "\"serial\":{"
      "\"interface\":%d,"
      "\"baudRate\":%lu,"
      "\"dataBits\":%d,"
      "\"parity\":%d,"
      "\"stopBits\":%d"
      "}"
      "},"
      "\"mqtt\":{"
      "\"host\":\"%s\","
      "\"port\":%d,"
      "\"qos\":%d,"
      "\"useAuth\":%s,"
      "\"username\":\"%s\","
      "\"password\":\"%s\","
      "\"topicPub\":\"%s\","
      "\"topicSub\":\"%s\""
      "},"
      "\"webUser\":{"
      "\"username\":\"%s\","
      "\"password\":\"%s\""
      "}"
      "}",
      // Device
      (int)cfg.device.type, cfg.device.name, cfg.device.id,
      // Network - LAN
      cfg.network.lan.enabled ? "true" : "false",
      cfg.network.lan.useDhcp ? "true" : "false",
      cfg.network.lan.staticIp.addr[0], cfg.network.lan.staticIp.addr[1],
      cfg.network.lan.staticIp.addr[2], cfg.network.lan.staticIp.addr[3],
      cfg.network.lan.netmask.addr[0], cfg.network.lan.netmask.addr[1],
      cfg.network.lan.netmask.addr[2], cfg.network.lan.netmask.addr[3],
      cfg.network.lan.gateway.addr[0], cfg.network.lan.gateway.addr[1],
      cfg.network.lan.gateway.addr[2], cfg.network.lan.gateway.addr[3],
      // Network - WLAN-OP
      cfg.network.wlanOp.enabled ? "true" : "false", cfg.network.wlanOp.ssid,
      cfg.network.wlanOp.password,
      cfg.network.wlanOp.useDhcp ? "true" : "false",
      cfg.network.wlanOp.staticIp.addr[0], cfg.network.wlanOp.staticIp.addr[1],
      cfg.network.wlanOp.staticIp.addr[2], cfg.network.wlanOp.staticIp.addr[3],
      cfg.network.wlanOp.netmask.addr[0], cfg.network.wlanOp.netmask.addr[1],
      cfg.network.wlanOp.netmask.addr[2], cfg.network.wlanOp.netmask.addr[3],
      cfg.network.wlanOp.gateway.addr[0], cfg.network.wlanOp.gateway.addr[1],
      cfg.network.wlanOp.gateway.addr[2], cfg.network.wlanOp.gateway.addr[3],
      // Network - WLAN-SAFE
      cfg.network.wlanSafe.ssid, cfg.network.wlanSafe.password,
      cfg.network.wlanSafe.channel,
      cfg.network.wlanSafe.hidden ? "true" : "false",
      cfg.network.wlanSafe.apIp.addr[0], cfg.network.wlanSafe.apIp.addr[1],
      cfg.network.wlanSafe.apIp.addr[2], cfg.network.wlanSafe.apIp.addr[3],
      cfg.network.webServerPort,
      // Endpoint
      cfg.endpoint.hostName, (int)cfg.endpoint.source,
      (int)cfg.endpoint.serial.interface, cfg.endpoint.serial.baudRate,
      cfg.endpoint.serial.dataBits, (int)cfg.endpoint.serial.parity,
      (int)cfg.endpoint.serial.stopBits,
      // MQTT
      cfg.mqtt.host, cfg.mqtt.port, cfg.mqtt.qos,
      cfg.mqtt.useAuth ? "true" : "false",
      cfg.mqtt.username, cfg.mqtt.password, cfg.mqtt.topicPub,
      cfg.mqtt.topicSub,
      // Web User
      cfg.webUser.username, cfg.webUser.password);

  if (len >= sizeof(resp)) {
    ESP_LOGW("WebServer", "JSON response truncated");
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

static esp_err_t apiSaveFullConfigHandler(httpd_req_t *req) {
  char buf[2048];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  buf[ret] = '\0';

  ConfigManager::FullConfig cfg;
  if (ConfigManager::getConfig(&cfg) != ESP_OK)
    return ESP_FAIL;

  // Helper functions for JSON parsing
  auto findValue = [](const char *json, const char *key) -> const char * {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    return strstr(json, search);
  };

  auto parseString = [](const char *pos, char *out, size_t maxLen) {
    if (!pos || !out || maxLen == 0)
      return;
    // Clear output buffer
    memset(out, 0, maxLen);
    // pos already points to the key (e.g., "topicPub":)
    // Find the colon - it should be right after the key
    const char *colon = strchr(pos, ':');
    if (!colon)
      return;
    // Skip colon
    colon++;
    // Skip whitespace after colon
    while (*colon == ' ' || *colon == '\t')
      colon++;
    // Find opening quote
    const char *quoteStart = strchr(colon, '"');
    if (!quoteStart)
      return;
    // Skip opening quote
    quoteStart++;
    // Find closing quote
    const char *quoteEnd = strchr(quoteStart, '"');
    if (!quoteEnd)
      return;
    // Copy the string between quotes
    size_t len = quoteEnd - quoteStart;
    if (len > maxLen - 1)
      len = maxLen - 1;
    memcpy(out, quoteStart, len);
    out[len] = '\0';
  };

  auto parseInt = [](const char *pos) -> int {
    if (!pos)
      return 0;
    pos = strchr(pos, ':');
    return pos ? atoi(pos + 1) : 0;
  };

  auto parseBool = [](const char *pos) -> bool {
    if (!pos)
      return false;
    pos = strchr(pos, ':');
    if (!pos)
      return false;
    while (*pos && (*pos == ':' || *pos == ' '))
      pos++;
    return (*pos == 't' || *pos == 'T');
  };

  auto parseIp = [](const char *pos, Network::IpAddress &ip) {
    if (!pos)
      return;
    pos = strchr(pos, ':');
    if (!pos)
      return;
    pos = strchr(pos, '"');
    if (!pos)
      return;
    int a, b, c, d;
    if (sscanf(pos + 1, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
      ip.addr[0] = a;
      ip.addr[1] = b;
      ip.addr[2] = c;
      ip.addr[3] = d;
    }
  };

  // Parse Device
  parseString(findValue(buf, "name"), cfg.device.name, sizeof(cfg.device.name));
  cfg.device.type = (ConfigManager::DeviceType)parseInt(findValue(buf, "type"));

  // Parse Network - LAN
  cfg.network.lan.enabled = parseBool(findValue(buf, "enabled"));
  cfg.network.lan.useDhcp = parseBool(findValue(buf, "useDhcp"));
  parseIp(findValue(buf, "staticIp"), cfg.network.lan.staticIp);
  parseIp(findValue(buf, "netmask"), cfg.network.lan.netmask);
  parseIp(findValue(buf, "gateway"), cfg.network.lan.gateway);

  // Parse Network - WLAN-OP
  const char *wlanOp = strstr(buf, "\"wlanOp\"");
  if (wlanOp) {
    cfg.network.wlanOp.enabled = parseBool(findValue(wlanOp, "enabled"));
    parseString(findValue(wlanOp, "ssid"), cfg.network.wlanOp.ssid,
                sizeof(cfg.network.wlanOp.ssid));
    parseString(findValue(wlanOp, "password"), cfg.network.wlanOp.password,
                sizeof(cfg.network.wlanOp.password));
    cfg.network.wlanOp.useDhcp = parseBool(findValue(wlanOp, "useDhcp"));
  }

  // Parse Network - WLAN-SAFE
  const char *wlanSafe = strstr(buf, "\"wlanSafe\"");
  if (wlanSafe) {
    parseString(findValue(wlanSafe, "ssid"), cfg.network.wlanSafe.ssid,
                sizeof(cfg.network.wlanSafe.ssid));
    parseString(findValue(wlanSafe, "password"), cfg.network.wlanSafe.password,
                sizeof(cfg.network.wlanSafe.password));
    cfg.network.wlanSafe.channel = parseInt(findValue(wlanSafe, "channel"));
    cfg.network.wlanSafe.hidden = parseBool(findValue(wlanSafe, "hidden"));
  }

  // Parse Endpoint
  const char *endpoint = strstr(buf, "\"endpoint\"");
  if (endpoint) {
    parseString(findValue(endpoint, "hostName"), cfg.endpoint.hostName,
                sizeof(cfg.endpoint.hostName));
    cfg.endpoint.source =
        (ConfigManager::DataSource)parseInt(findValue(endpoint, "source"));
    const char *serial = strstr(endpoint, "\"serial\"");
    if (serial) {
      cfg.endpoint.serial.baudRate = parseInt(findValue(serial, "baudRate"));
      cfg.endpoint.serial.interface =
          (ConfigManager::PhysicalInterface)parseInt(
              findValue(serial, "interface"));
    }
  }

  // Parse MQTT
  const char *mqtt = strstr(buf, "\"mqtt\"");
  if (mqtt) {
    // Find the opening brace of mqtt object
    mqtt = strchr(mqtt, '{');
    if (mqtt) {
      // Find the closing brace of mqtt object
      const char *mqttEnd = strchr(mqtt, '}');
      if (mqttEnd) {
        // Create a helper that searches only within mqtt section
        auto findValueInSection = [mqtt, mqttEnd](const char *key) -> const char * {
          char search[64];
          snprintf(search, sizeof(search), "\"%s\":", key);
          const char *pos = strstr(mqtt, search);
          if (pos && pos < mqttEnd) {
            return pos;
          }
          return nullptr;
        };
        
        parseString(findValueInSection("host"), cfg.mqtt.host, sizeof(cfg.mqtt.host));
        cfg.mqtt.port = parseInt(findValueInSection("port"));
        cfg.mqtt.qos = parseInt(findValueInSection("qos"));
        if (cfg.mqtt.qos > 2) cfg.mqtt.qos = 1; // Validar QoS (0, 1 o 2)
        cfg.mqtt.useAuth = parseBool(findValueInSection("useAuth"));
        parseString(findValueInSection("username"), cfg.mqtt.username,
                    sizeof(cfg.mqtt.username));
        parseString(findValueInSection("password"), cfg.mqtt.password,
                    sizeof(cfg.mqtt.password));
        const char *topicPubPos = findValueInSection("topicPub");
        if (topicPubPos) {
          parseString(topicPubPos, cfg.mqtt.topicPub, sizeof(cfg.mqtt.topicPub));
          ESP_LOGI(TAG, "Parsed topicPub: [%s] (len=%zu)", cfg.mqtt.topicPub, strlen(cfg.mqtt.topicPub));
        }
        const char *topicSubPos = findValueInSection("topicSub");
        if (topicSubPos) {
          parseString(topicSubPos, cfg.mqtt.topicSub, sizeof(cfg.mqtt.topicSub));
          ESP_LOGI(TAG, "Parsed topicSub: [%s] (len=%zu)", cfg.mqtt.topicSub, strlen(cfg.mqtt.topicSub));
        }
      }
    }
  }

  // Parse WebUser
  const char *webUser = strstr(buf, "\"webUser\"");
  if (webUser) {
    parseString(findValue(webUser, "username"), cfg.webUser.username,
                sizeof(cfg.webUser.username));
    parseString(findValue(webUser, "password"), cfg.webUser.password,
                sizeof(cfg.webUser.password));
  }

  ESP_LOGI("WebServer", "Saving configuration...");
  if (ConfigManager::saveConfig(&cfg) == ESP_OK) {
    httpd_resp_send(req,
                    "{\"success\":true,\"message\":\"Configuration saved. "
                    "Rebooting in 3 seconds...\"}",
                    -1);
    ESP_LOGI("WebServer",
             "Configuration saved successfully. Rebooting in 3 seconds...");

    // Schedule reboot after 3 seconds to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
  } else {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to save\"}",
                    -1);
    ESP_LOGE("WebServer", "Failed to save configuration");
  }
  return ESP_OK;
}

static esp_err_t apiWifiConfigHandler(httpd_req_t *req) {
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  buf[ret] = '\0';

  char ssid[33] = {0}, pass[65] = {0};
  char *s = strstr(buf, "\"ssid\"");
  if (s) {
    s = strchr(s, ':');
    if (s)
      s = strchr(s, '"');
    if (s)
      sscanf(s + 1, "%[^\"]", ssid);
  }
  char *p = strstr(buf, "\"password\"");
  if (p) {
    p = strchr(p, ':');
    if (p)
      p = strchr(p, '"');
    if (p)
      sscanf(p + 1, "%[^\"]", pass);
  }

  ConfigManager::FullConfig cfg;
  if (ConfigManager::getConfig(&cfg) == ESP_OK) {
    cfg.network.wlanOp.enabled = true;
    strncpy(cfg.network.wlanOp.ssid, ssid, 32);
    strncpy(cfg.network.wlanOp.password, pass, 64);
    ConfigManager::saveConfig(&cfg);
    httpd_resp_send(req, "{\"success\":true}", -1);
  } else {
    httpd_resp_send(req, "{\"success\":false}", -1);
  }
  return ESP_OK;
}

static esp_err_t apiUserConfigHandler(httpd_req_t *req) {
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  buf[ret] = '\0';
  char user[33] = {0}, pass[33] = {0};
  char *u = strstr(buf, "\"user\"");
  if (u) {
    u = strchr(u, ':');
    if (u)
      u = strchr(u, '"');
    if (u)
      sscanf(u + 1, "%[^\"]", user);
  }
  char *p = strstr(buf, "\"pass\"");
  if (p) {
    p = strchr(p, ':');
    if (p)
      p = strchr(p, '"');
    if (p)
      sscanf(p + 1, "%[^\"]", pass);
  }
  ConfigManager::FullConfig cfg;
  if (ConfigManager::getConfig(&cfg) == ESP_OK) {
    strncpy(cfg.webUser.username, user, 31);
    strncpy(cfg.webUser.password, pass, 31);
    ConfigManager::saveConfig(&cfg);
    httpd_resp_send(req, "{\"success\":true}", -1);
  } else
    httpd_resp_send(req, "{\"success\":false}", -1);
  return ESP_OK;
}

static esp_err_t apiDataLoggerFormatHandler(httpd_req_t *req) {
  if (s_dataloggerCallbacks.formatFlash &&
      s_dataloggerCallbacks.formatFlash() == ESP_OK)
    httpd_resp_send(req, "{\"success\":true}", -1);
  else
    httpd_resp_send(req, "{\"success\":false}", -1);
  return ESP_OK;
}

static esp_err_t apiSystemRebootHandler(httpd_req_t *req) {
  httpd_resp_send(req, "{\"success\":true}", -1);
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;
}

static esp_err_t apiTestMqttHandler(httpd_req_t *req) {
  char buf[512];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to receive "
                         "request\"}",
                    -1);
    return ESP_FAIL;
  }
  buf[ret] = '\0';

  // Parse JSON (simple parsing)
  auto findValue = [](const char *json, const char *key) -> const char * {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);
    const char *keyPos = strstr(json, searchKey);
    if (!keyPos)
      return nullptr;
    const char *colon = strchr(keyPos, ':');
    if (!colon)
      return nullptr;
    const char *valueStart = colon + 1;
    while (*valueStart == ' ' || *valueStart == '\t')
      valueStart++;
    if (*valueStart == '"') {
      valueStart++;
      return valueStart;
    }
    return valueStart;
  };

  auto parseString = [](const char *start, char *out, size_t maxLen) {
    if (!start)
      return;
    const char *end = strchr(start, '"');
    if (end) {
      size_t len = end - start;
      if (len > maxLen - 1)
        len = maxLen - 1;
      strncpy(out, start, len);
      out[len] = '\0';
    } else {
      // Try to parse as number or boolean
      strncpy(out, start, maxLen - 1);
      out[maxLen - 1] = '\0';
    }
  };

  auto parseInt = [](const char *str) -> int {
    if (!str)
      return 0;
    return atoi(str);
  };

  auto parseBool = [](const char *str) -> bool {
    if (!str)
      return false;
    return (strncmp(str, "true", 4) == 0 || strncmp(str, "1", 1) == 0);
  };

  // Parse MQTT test parameters
  char host[64] = "";
  uint16_t port = 1883;
  uint8_t qos = 1;
  bool useAuth = false;
  char username[32] = "";
  char password[64] = "";

  parseString(findValue(buf, "host"), host, sizeof(host));
  port = (uint16_t)parseInt(findValue(buf, "port"));
  if (port == 0)
    port = 1883;
  qos = (uint8_t)parseInt(findValue(buf, "qos"));
  if (qos > 2)
    qos = 1;
  useAuth = parseBool(findValue(buf, "useAuth"));
  if (useAuth) {
    parseString(findValue(buf, "username"), username, sizeof(username));
    parseString(findValue(buf, "password"), password, sizeof(password));
  }

  if (strlen(host) == 0) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Host is required\"}",
                    -1);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Testing MQTT connection to %s:%d", host, port);

  // Load current config
  ConfigManager::FullConfig tempConfig;
  if (ConfigManager::getConfig(&tempConfig) != ESP_OK) {
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to load base "
                         "configuration\"}",
                    -1);
    return ESP_OK;
  }

  // Save only MQTT config to restore later (optimize memory usage)
  struct {
    char host[64];
    uint16_t port;
    uint8_t qos;
    bool useAuth;
    char username[32];
    char password[64];
  } originalMqtt;
  memcpy(&originalMqtt, &tempConfig.mqtt, sizeof(originalMqtt));

  // Override MQTT config with test parameters (preserve topics from original config)
  strncpy(tempConfig.mqtt.host, host, sizeof(tempConfig.mqtt.host) - 1);
  tempConfig.mqtt.host[sizeof(tempConfig.mqtt.host) - 1] = '\0';
  tempConfig.mqtt.port = port;
  tempConfig.mqtt.qos = qos;
  tempConfig.mqtt.useAuth = useAuth;
  if (useAuth) {
    strncpy(tempConfig.mqtt.username, username, sizeof(tempConfig.mqtt.username) - 1);
    tempConfig.mqtt.username[sizeof(tempConfig.mqtt.username) - 1] = '\0';
    strncpy(tempConfig.mqtt.password, password, sizeof(tempConfig.mqtt.password) - 1);
    tempConfig.mqtt.password[sizeof(tempConfig.mqtt.password) - 1] = '\0';
  }
  // Topics are preserved (not overridden in test)

  // Temporarily save config for test
  ConfigManager::saveConfig(&tempConfig);

  // Create temporary MQTT manager for testing (after config is saved)
  MqttManager testManager;

  // Initialize and connect
  bool connected = false;
  bool published = false;
  esp_err_t initRet = testManager.init();
  
  // Reload config to ensure MqttManager uses the temporary test configuration
  if (initRet == ESP_OK) {
    testManager.reloadConfig();
  }
  if (initRet == ESP_OK) {
    esp_err_t connectRet = testManager.connect();

    if (connectRet == ESP_OK) {
      // Wait up to 5 seconds for connection
      for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (testManager.isConnected()) {
          connected = true;
          break;
        }
      }
      
      // If connected, wait a bit for subscription to complete, then publish test message
      if (connected) {
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for subscription to complete
        
        // Publish a test status message using MqttManager API
        // This will automatically include deviceId, deviceName, status, and timestamp
        esp_err_t pubRet = testManager.sendStatus("test_connection");
        if (pubRet == ESP_OK) {
          published = true;
          ESP_LOGI(TAG, "Mensaje de prueba (status) publicado usando MqttManager");
          vTaskDelay(pdMS_TO_TICKS(500)); // Give time for message to be sent
        }
        
        // Keep connected for a bit longer so MQTT Explorer can see it
        vTaskDelay(pdMS_TO_TICKS(2000)); // Stay connected for 2 more seconds
      }
    }

    // Disconnect
    testManager.disconnect();
    vTaskDelay(pdMS_TO_TICKS(500)); // Give time to disconnect
  }

  // Restore original MQTT config (only the MQTT part, not the entire config)
  memcpy(&tempConfig.mqtt, &originalMqtt, sizeof(originalMqtt));
  ConfigManager::saveConfig(&tempConfig);

  // Send response
  if (connected) {
    char resp[320];
    if (published) {
      snprintf(resp, sizeof(resp),
               "{\"success\":true,\"message\":\"Conexión exitosa a %s:%d. "
               "Mensaje de prueba publicado en %s. Verifique en MQTT Explorer.\"}",
               host, port, tempConfig.mqtt.topicPub);
    } else {
      snprintf(resp, sizeof(resp),
               "{\"success\":true,\"message\":\"Conexión exitosa a %s:%d. "
               "Suscripción realizada. Verifique en MQTT Explorer.\"}",
               host, port);
    }
    httpd_resp_send(req, resp, -1);
  } else {
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"success\":false,\"error\":\"No se pudo conectar a %s:%d. "
             "Verifique la configuración y la conectividad de red.\"}",
             host, port);
    httpd_resp_send(req, resp, -1);
  }

  return ESP_OK;
}

} // namespace WebServer
