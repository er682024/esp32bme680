#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "http_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define TAG         "HTTP"
#define ADMIN_USER  "admin"
#define ADMIN_PASS_DEFAULT "esp32admin"   // password usata se non impostata in NVS
#define NVS_NS_AUTH      "auth_cfg"
#define NVS_KEY_PASS     "admin_pass"

static httpd_handle_t server = NULL;

// Legge la password admin da NVS; se assente usa il default.
static void load_admin_pass(char *buf, size_t len) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_AUTH, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_KEY_PASS, buf, &len) == ESP_OK) {
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }
    // Fallback al default di fabbrica
    strncpy(buf, ADMIN_PASS_DEFAULT, len - 1);
    buf[len - 1] = '\0';
}

// Salva la nuova password admin in NVS.
static esp_err_t save_admin_pass(const char *new_pass) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_AUTH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, NVS_KEY_PASS, new_pass);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// Dati condivisi
static float   s_temp = 0, s_hum = 0, s_pres = 0;
static uint32_t s_gas = 0;
static char    s_time[32] = "--:--:--";

void http_server_update_data(float temp, float hum, float pres, uint32_t gas, const char *time_str) {
    s_temp = temp;
    s_hum  = hum;
    s_pres = pres;
    s_gas  = gas;
    strncpy(s_time, time_str, sizeof(s_time) - 1);
}

static bool check_auth(httpd_req_t *req) {
    char auth[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Non autorizzato", -1);
        return false;
    }

    // Salta "Basic "
    if (strncmp(auth, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Non autorizzato", -1);
        return false;
    }

    char *encoded = auth + 6;
    unsigned char decoded[64] = {0};
    size_t decoded_len = 0;

    mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                          (unsigned char *)encoded, strlen(encoded));
    decoded[decoded_len] = '\0';

	// Legge la password corrente da NVS (o usa il default)
    char current_pass[64];
    load_admin_pass(current_pass, sizeof(current_pass));

    char expected[64];
    snprintf(expected, sizeof(expected), "%s:%s", ADMIN_USER, ADMIN_PASS);

    if (strcmp((char *)decoded, expected) != 0) {
        ESP_LOGW(TAG, "Auth fallita: ricevuto='%s' atteso='%s'", decoded, expected);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
        httpd_resp_send(req, "Credenziali errate", -1);
        return false;
    }
    return true;
}

/*
static bool check_token(httpd_req_t *req) {
    char token[64] = {0};
    if (httpd_req_get_url_query_str(req, token, sizeof(token)) == ESP_OK) {
        char val[32] = {0};
        if (httpd_query_key_value(token, "token", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, ADMIN_PASS) == 0) return true;
        }
    }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req, "Token mancante o errato", -1);
    return false;
}
*/

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ─── GET /data  (JSON) ────────────────────────────────────────────────────────
static esp_err_t data_handler(httpd_req_t *req) {
	// Legge RSSI del segnale WiFi in modalità STA
    int8_t rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    // if (!check_token(req)) return ESP_OK;
    char json[300];
    snprintf(json, sizeof(json),
        "{\"time\":\"%s\","
        "\"temperature\":%.2f,"
        "\"humidity\":%.2f,"
        "\"pressure\":%.2f,"
        "\"gas\":%u}"
		"\"rssi\":%d}",
        s_time, s_temp, s_hum, s_pres, (unsigned)s_gas, (int)rssi);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}


// ─── GET /monitor  (HTML dashboard) ──────────────────────────────────────────
static const char *HTML_MONITOR =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 BME680 Monitor</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;min-height:100vh;"
"display:flex;flex-direction:column;align-items:center;padding:2rem}"
"h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.3rem;letter-spacing:2px;text-transform:uppercase}"
"#clock{font-size:2.2rem;font-weight:bold;color:#f0f6fc;margin-bottom:2rem;letter-spacing:3px}"
".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:1.2rem;width:100%;max-width:480px}"
".card{background:#161b22;border:1px solid #30363d;border-radius:14px;padding:1.4rem;"
"display:flex;flex-direction:column;align-items:center;gap:0.4rem;transition:border-color .3s}"
".card:hover{border-color:#58a6ff}"
".icon{font-size:2.2rem}"
".label{font-size:0.75rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px}"
".value{font-size:2rem;font-weight:bold;color:#f0f6fc}"
".unit{font-size:0.85rem;color:#8b949e}"
"#temp  .value{color:#ff7b72}"
"#hum   .value{color:#79c0ff}"
"#pres  .value{color:#a5d6ff}"
"#gas   .value{color:#7ee787}"
/* WiFi card — occupa tutta la larghezza */
"#wifi{grid-column:1/-1;flex-direction:row;justify-content:space-between;padding:1rem 1.4rem;gap:1rem}"
"#wifi .left{display:flex;flex-direction:column;gap:0.3rem}"
"#wifi .icon{font-size:1.6rem}"
"#wifi .value{font-size:1.5rem}"
/* barre segnale */
".bars{display:flex;align-items:flex-end;gap:3px;height:22px}"
".bar{width:6px;border-radius:2px;background:#30363d;transition:background .4s}"
".bar.lit-good{background:#3fb950}"
".bar.lit-ok  {background:#d29922}"
".bar.lit-poor{background:#f85149}"
".rssi-label{font-size:0.72rem;color:#8b949e;margin-top:2px}"
".status{margin-top:1.5rem;font-size:0.75rem;color:#484f58}"
"</style></head><body>"
"<h1>&#x1F4E1; BME680 Monitor</h1>"
"<div id='clock'>--:--:--</div>"
"<div class='grid'>"
"  <div class='card' id='temp'>"
"    <span class='icon'>&#x1F321;</span>"
"    <span class='label'>Temperatura</span>"
"    <span class='value' id='v-temp'>--.-</span>"
"    <span class='unit'>&deg;C</span>"
"  </div>"
"  <div class='card' id='hum'>"
"    <span class='icon'>&#x1F4A7;</span>"
"    <span class='label'>Umidità</span>"
"    <span class='value' id='v-hum'>--.-</span>"
"    <span class='unit'>%</span>"
"  </div>"
"  <div class='card' id='pres'>"
"    <span class='icon'>&#x1F30D;</span>"
"    <span class='label'>Pressione</span>"
"    <span class='value' id='v-pres'>----</span>"
"    <span class='unit'>hPa</span>"
"  </div>"
"  <div class='card' id='gas'>"
"    <span class='icon'>&#x1F32C;</span>"
"    <span class='label'>Gas</span>"
"    <span class='value' id='v-gas'>----</span>"
"    <span class='unit'>&Omega;</span>"
"  </div>"
/* card WiFi a larghezza piena */
"  <div class='card' id='wifi'>"
"    <div class='left'>"
"      <span class='label'>Segnale WiFi</span>"
"      <span class='value' id='v-rssi'>---</span>"
"      <span class='unit'>dBm &nbsp;<span id='v-quality'>--</span></span>"
"    </div>"
"    <div style='display:flex;flex-direction:column;align-items:center;gap:4px'>"
"      <div class='bars' id='bars'>"
"        <div class='bar' id='b1' style='height:5px'></div>"
"        <div class='bar' id='b2' style='height:9px'></div>"
"        <div class='bar' id='b3' style='height:13px'></div>"
"        <div class='bar' id='b4' style='height:18px'></div>"
"        <div class='bar' id='b5' style='height:22px'></div>"
"      </div>"
"      <div class='rssi-label' id='v-rssi-label'>in attesa...</div>"
"    </div>"
"  </div>"
"</div>"
"<div class='status' id='status'>Connessione...</div>"
"<script>"
"function updateWifi(rssi) {"
"  document.getElementById('v-rssi').textContent = rssi;"
/* Qualità testuale */
"  let quality, cls, label;"
"  if      (rssi >= -55) { quality='Ottimo';  cls='lit-good'; label='Ottimo'; }"
"  else if (rssi >= -67) { quality='Buono';   cls='lit-good'; label='Buono'; }"
"  else if (rssi >= -78) { quality='Discreto';cls='lit-ok';   label='Discreto'; }"
"  else                  { quality='Scarso';  cls='lit-poor'; label='Scarso'; }"
"  document.getElementById('v-quality').textContent = quality;"
"  document.getElementById('v-rssi-label').textContent = label;"
/* Numero di barre accese: 5 soglie */
"  const lit = rssi >= -55 ? 5 : rssi >= -67 ? 4 : rssi >= -75 ? 3 : rssi >= -85 ? 2 : 1;"
"  for (let i = 1; i <= 5; i++) {"
"    const b = document.getElementById('b' + i);"
"    b.className = 'bar' + (i <= lit ? ' ' + cls : '');"
"  }"
"}"
"async function fetchData() {"
"  try {"
"    const r = await fetch('/data');"
"    if (!r.ok) { document.getElementById('status').textContent = 'Errore auth'; return; }"
"    const d = await r.json();"
"    document.getElementById('clock').textContent   = d.time;"
"    document.getElementById('v-temp').textContent  = d.temperature.toFixed(1);"
"    document.getElementById('v-hum').textContent   = d.humidity.toFixed(1);"
"    document.getElementById('v-pres').textContent  = d.pressure.toFixed(1);"
"    document.getElementById('v-gas').textContent   = d.gas;"
"    if (d.rssi !== undefined) updateWifi(d.rssi);"
"    document.getElementById('status').textContent  = 'Aggiornato: ' + new Date().toLocaleTimeString();"
"  } catch(e) {"
"    document.getElementById('status').textContent = 'Errore connessione';"
"  }"
"}"
"fetchData();"
"setInterval(fetchData, 2000);"
"</script>"
"</body></html>";

static esp_err_t monitor_handler(httpd_req_t *req) {
    // if (!check_token(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_MONITOR, -1);
    return ESP_OK;
}

static void save_wifi_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid",     ssid);
    nvs_set_str(nvs, "password", password);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Credenziali salvate: SSID='%s'", ssid);
}

// Pagina HTML
static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 WiFi Config</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
    "justify-content:center;align-items:center;height:100vh;margin:0}"
    ".box{background:#16213e;padding:2rem;border-radius:12px;width:300px;"
    "box-shadow:0 4px 20px rgba(0,0,0,0.5)}"
    "h2{text-align:center;color:#0f3460;margin-bottom:1.5rem;color:#e94560}"
    "input{width:100%;padding:10px;margin:8px 0;border:none;border-radius:6px;"
    "background:#0f3460;color:#eee;box-sizing:border-box;font-size:14px}"
    "button{width:100%;padding:12px;background:#e94560;color:#fff;border:none;"
    "border-radius:6px;cursor:pointer;font-size:16px;margin-top:1rem}"
    "button:hover{background:#c73652}"
    ".msg{text-align:center;margin-top:1rem;color:#4ecca3}"
    "</style></head><body>"
    "<div class='box'>"
    "<h2>&#x1F4F6; WiFi Config</h2>"
    "<form method='POST' action='/save'>"
    "<input type='text'     name='ssid'     placeholder='Nome rete (SSID)' required><br>"
    "<input type='password' name='password' placeholder='Password WiFi'   required><br>"
    "<button type='submit'>Salva e Riavvia</button>"
    "</form>"
    "</div></body></html>";

static const char *HTML_OK =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Salvato</title>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
    "display:flex;justify-content:center;align-items:center;height:100vh}"
    ".box{background:#16213e;padding:2rem;border-radius:12px;text-align:center}"
    "h2{color:#4ecca3}p{color:#aaa}</style></head><body>"
    "<div class='box'><h2>&#x2705; Salvato!</h2>"
    "<p>L'ESP32 si riavvierà e tenterà la connessione.</p></div>"
    "</body></html>";

// ─── GET /settings  (cambio password + reset di fabbrica) ────────────────────
static const char *HTML_SETTINGS =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Impostazioni</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;"
"display:flex;flex-direction:column;align-items:center;padding:2rem;min-height:100vh}"
"h1{font-size:1.2rem;color:#58a6ff;letter-spacing:2px;text-transform:uppercase;margin-bottom:2rem}"
".card{background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:1.6rem;width:100%;max-width:420px;margin-bottom:1.2rem}"
".card h2{font-size:0.85rem;color:#8b949e;text-transform:uppercase;letter-spacing:1px;margin-bottom:1.2rem}"
"label{display:block;font-size:0.8rem;color:#8b949e;margin-bottom:0.3rem;margin-top:0.8rem}"
"input[type=password]{"
"width:100%;padding:10px 12px;background:#0d1117;border:1px solid #30363d;"
"border-radius:8px;color:#e6edf3;font-size:0.9rem;outline:none;transition:border-color .2s}"
"input[type=password]:focus{border-color:#58a6ff}"
".btn{display:block;width:100%;padding:11px;border:none;border-radius:8px;"
"font-size:0.9rem;cursor:pointer;margin-top:1rem;transition:opacity .2s}"
".btn:hover{opacity:.85}"
".btn-blue{background:#1f6feb;color:#fff}"
".btn-red{background:#b91c1c;color:#fff}"
"#msg-pw,#msg-rst{font-size:0.8rem;margin-top:0.8rem;min-height:1.2em;text-align:center}"
".ok{color:#3fb950}.err{color:#f85149}"
".divider{border:none;border-top:1px solid #21262d;margin:0.4rem 0}"
".warn-box{background:rgba(248,81,73,.08);border:1px solid rgba(248,81,73,.3);"
"border-radius:8px;padding:0.9rem 1rem;font-size:0.8rem;color:#f85149;margin-bottom:1rem;line-height:1.5}"
".nav{font-size:0.8rem;margin-bottom:1.5rem}"
".nav a{color:#58a6ff;text-decoration:none}"
"</style></head><body>"
"<h1>&#x2699;&#xFE0F; Impostazioni</h1>"
"<div class='nav'><a href='/monitor'>&#x2190; Monitor</a> &nbsp;|&nbsp; <a href='/update'>OTA Update</a></div>"

/* ── Card cambio password ── */
"<div class='card'>"
"<h2>&#x1F512; Cambia Password Admin</h2>"
"<label>Password attuale</label>"
"<input type='password' id='cur-pw' autocomplete='current-password'>"
"<label>Nuova password</label>"
"<input type='password' id='new-pw' autocomplete='new-password'>"
"<label>Conferma nuova password</label>"
"<input type='password' id='cfm-pw' autocomplete='new-password'>"
"<button class='btn btn-blue' onclick='changePw()'>&#x1F4BE; Salva password</button>"
"<div id='msg-pw'></div>"
"</div>"

/* ── Card reset di fabbrica ── */
"<div class='card'>"
"<h2>&#x26A0;&#xFE0F; Reset di Fabbrica</h2>"
"<div class='warn-box'>Questa operazione cancella le credenziali WiFi e la password admin.<br>"
"Il dispositivo si riavvierà in modalità AP con le impostazioni predefinite.</div>"
"<button class='btn btn-red' onclick='confirmReset()'>&#x1F504; Esegui Reset di Fabbrica</button>"
"<div id='msg-rst'></div>"
"</div>"

"<script>"
/* cambio password */
"async function changePw() {"
"  const cur = document.getElementById('cur-pw').value;"
"  const np  = document.getElementById('new-pw').value;"
"  const cf  = document.getElementById('cfm-pw').value;"
"  const msg = document.getElementById('msg-pw');"
"  if (!cur || !np) { msg.className='err'; msg.textContent='Compila tutti i campi.'; return; }"
"  if (np !== cf)   { msg.className='err'; msg.textContent='Le password non coincidono.'; return; }"
"  if (np.length < 8) { msg.className='err'; msg.textContent='Minimo 8 caratteri.'; return; }"
"  const body = 'cur=' + encodeURIComponent(cur) + '&new=' + encodeURIComponent(np);"
"  try {"
"    const r = await fetch('/set-password', {"
"      method:'POST',"
"      headers:{'Content-Type':'application/x-www-form-urlencoded',"
"               'Authorization':'Basic ' + btoa('admin:' + cur)},"
"      body: body"
"    });"
"    if (r.ok) {"
"      msg.className='ok'; msg.textContent='\\u2705 Password aggiornata. Riautenticati.';"
"      document.getElementById('cur-pw').value='';"
"      document.getElementById('new-pw').value='';"
"      document.getElementById('cfm-pw').value='';"
"    } else {"
"      msg.className='err'; msg.textContent='\\u274C Credenziali errate o errore server.';"
"    }"
"  } catch(e) { msg.className='err'; msg.textContent='\\u274C Errore connessione.'; }"
"}"
/* reset di fabbrica */
"function confirmReset() {"
"  if (!confirm('Confermi il reset di fabbrica?\\nWiFi e password verranno cancellati.')) return;"
"  const msg = document.getElementById('msg-rst');"
"  fetch('/factory-reset', {method:'POST'})"
"    .then(r => {"
"      if (r.ok) { msg.className='ok'; msg.textContent='\\u2705 Reset eseguito. Riavvio in corso...'; }"
"      else      { msg.className='err'; msg.textContent='\\u274C Errore durante il reset.'; }"
"    })"
"    .catch(() => { msg.className='ok'; msg.textContent='Riavvio in corso...'; });"
"}"
"</script>"
"</body></html>";

static esp_err_t settings_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_SETTINGS, -1);
    return ESP_OK;
}

// ─── POST /set-password ───────────────────────────────────────────────────────
static esp_err_t set_password_handler(httpd_req_t *req) {
    // check_auth verifica la password CORRENTE (già nel body come header Basic)
    if (!check_auth(req)) return ESP_OK;

    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';

    // Parsing "new=<valore>"
    char new_pass[64] = {0};
    char *p = strstr(body, "new=");
    if (p) {
        p += 4;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(new_pass, p, sizeof(new_pass) - 1);
    }

    if (strlen(new_pass) < 8) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Password troppo corta (minimo 8 caratteri)", -1);
        return ESP_OK;
    }

    esp_err_t err = save_admin_pass(new_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Errore salvataggio password: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Password admin aggiornata");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ─── POST /factory-reset ──────────────────────────────────────────────────────
static esp_err_t factory_reset_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Reset di fabbrica richiesto — cancello WiFi e password admin");

    // Cancella credenziali WiFi
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, "ssid");
        nvs_erase_key(nvs, "password");
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    // Cancella password admin (tornerà al default ADMIN_PASS_DEFAULT)
    if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_PASS);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// GET /
static esp_err_t get_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, -1);
    return ESP_OK;
}

// POST /save
static esp_err_t post_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[len] = '\0';

    // Parsing ssid= e password=
    char ssid[64] = {0}, password[64] = {0};
    char *p;

    p = strstr(body, "ssid=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(ssid, p, sizeof(ssid) - 1);
        if (end) *end = '&';
    }

    p = strstr(body, "password=");
    if (p) {
        p += 9;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(password, p, sizeof(password) - 1);
    }

    // URL decode base (sostituisce + con spazio)
    for (char *c = ssid;     *c; c++) if (*c == '+') *c = ' ';
    for (char *c = password; *c; c++) if (*c == '+') *c = ' ';

    ESP_LOGI(TAG, "Ricevuto SSID='%s'", ssid);
    save_wifi_credentials(ssid, password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OK, -1);

    // Riavvia dopo 2 secondi
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// ─── GET /update  (pagina upload OTA) ────────────────────────────────────────
static const char *HTML_OTA =
"<!DOCTYPE html><html lang='it'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 OTA Update</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;"
"display:flex;justify-content:center;align-items:center;min-height:100vh}"
".box{background:#161b22;border:1px solid #30363d;border-radius:14px;"
"padding:2rem;width:100%;max-width:420px}"
"h2{color:#58a6ff;margin-bottom:1.5rem;text-align:center}"
".drop{border:2px dashed #30363d;border-radius:10px;padding:2rem;"
"text-align:center;cursor:pointer;transition:border-color .3s;margin-bottom:1rem}"
".drop:hover,.drop.over{border-color:#58a6ff}"
".drop input{display:none}"
".drop p{color:#8b949e;font-size:0.9rem;margin-top:0.5rem}"
"#filename{color:#58a6ff;font-size:0.85rem;margin-top:0.5rem}"
"button{width:100%;padding:12px;background:#238636;color:#fff;border:none;"
"border-radius:6px;cursor:pointer;font-size:1rem;margin-top:0.5rem}"
"button:disabled{background:#2d333b;color:#484f58;cursor:not-allowed}"
"button:hover:not(:disabled){background:#2ea043}"
"#progress-wrap{background:#21262d;border-radius:6px;height:8px;margin-top:1rem;display:none}"
"#progress-bar{height:8px;background:#58a6ff;border-radius:6px;width:0;transition:width .3s}"
"#status{text-align:center;margin-top:1rem;font-size:0.85rem;color:#8b949e}"
"</style></head><body>"
"<div class='box'>"
"<h2>&#x1F504; OTA Firmware Update</h2>"
"<div class='drop' id='drop' onclick='document.getElementById(\"file\").click()'>"
"  <span style='font-size:2rem'>&#x1F4BE;</span>"
"  <p>Clicca o trascina il file <strong>.bin</strong></p>"
"  <div id='filename'>Nessun file selezionato</div>"
"  <input type='file' id='file' accept='.bin'>"
"</div>"
"<button id='btn' disabled onclick='startUpload()'>Carica Firmware</button>"
"<div id='progress-wrap'><div id='progress-bar'></div></div>"
"<div id='status'></div>"
"</div>"
"<script>"
"const drop = document.getElementById('drop');"
"const fileInput = document.getElementById('file');"
"const btn = document.getElementById('btn');"
"let selectedFile = null;"

"fileInput.addEventListener('change', () => {"
"  selectedFile = fileInput.files[0];"
"  document.getElementById('filename').textContent = selectedFile ? selectedFile.name : 'Nessun file';"
"  btn.disabled = !selectedFile;"
"});"

"drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });"
"drop.addEventListener('dragleave', () => drop.classList.remove('over'));"
"drop.addEventListener('drop', e => {"
"  e.preventDefault(); drop.classList.remove('over');"
"  selectedFile = e.dataTransfer.files[0];"
"  document.getElementById('filename').textContent = selectedFile.name;"
"  btn.disabled = false;"
"});"

"function startUpload() {"
"  if (!selectedFile) return;"
"  btn.disabled = true;"
"  document.getElementById('progress-wrap').style.display = 'block';"
"  const status = document.getElementById('status');"
"  const bar    = document.getElementById('progress-bar');"
"  status.textContent = 'Upload in corso...';"
"  const xhr = new XMLHttpRequest();"
"  xhr.open('POST', '/ota');"
"  xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
"  xhr.upload.onprogress = e => {"
"    if (e.lengthComputable) {"
"      const pct = Math.round(e.loaded / e.total * 100);"
"      bar.style.width = pct + '%';"
"      status.textContent = 'Upload: ' + pct + '%';"
"    }"
"  };"
"  xhr.onload = () => {"
"    if (xhr.status === 200) {"
"      bar.style.width = '100%';"
"      bar.style.background = '#3fb950';"
"      status.textContent = '✅ Completato! Riavvio in corso...';"
"    } else {"
"      bar.style.background = '#f85149';"
"      status.textContent = '❌ Errore: ' + xhr.responseText;"
"      btn.disabled = false;"
"    }"
"  };"
"  xhr.onerror = () => {"
"    status.textContent = '❌ Errore di connessione';"
"    btn.disabled = false;"
"  };"
"  xhr.send(selectedFile);"
"}"
"</script>"
"</body></html>";

// ─── GET /update ──────────────────────────────────────────────────────────────
static esp_err_t ota_page_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OTA, -1);
    return ESP_OK;
}

// ─── POST /ota  (riceve il .bin e flasha) ─────────────────────────────────────
static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partizione OTA non trovata");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: scrittura su partizione '%s'", update_partition->label);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin fallito");
        return ESP_FAIL;
    }

    char buf[1024];
    int received = 0;
    int remaining = req->content_len;

    while (remaining > 0) {
        int len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (len < 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Errore ricezione");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Errore scrittura flash");
            return ESP_FAIL;
        }
        remaining -= len;
        received  += len;
        ESP_LOGI(TAG, "OTA: ricevuto %d / %d bytes", received, req->content_len);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end fallito");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition fallito");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA completato! Riavvio...");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 11;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    // config.max_req_hdr_len   = 1024;   // ← default è 512, raddoppia

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Errore avvio server"); return;
    }

    httpd_uri_t uris[] = {
		{ .uri="/",            .method=HTTP_GET,  .handler=get_handler      },
	    { .uri="/save",        .method=HTTP_POST, .handler=post_handler     },
	    { .uri="/monitor",     .method=HTTP_GET,  .handler=monitor_handler  },
	    { .uri="/data",        .method=HTTP_GET,  .handler=data_handler     },
	    { .uri="/favicon.ico", .method=HTTP_GET,  .handler=favicon_handler  },
	    { .uri="/update",      .method=HTTP_GET,  .handler=ota_page_handler },
	    { .uri="/ota",         .method=HTTP_POST, .handler=ota_upload_handler},
	    { .uri="/settings",      .method=HTTP_GET,  .handler=settings_handler     },
        { .uri="/set-password",  .method=HTTP_POST, .handler=set_password_handler },
        { .uri="/factory-reset", .method=HTTP_POST, .handler=factory_reset_handler},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
	    httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "Server HTTP avviato su http://<IP>/monitor / /data /update /settings");
}

void http_server_stop(void) {
    if (server) httpd_stop(server);
}
