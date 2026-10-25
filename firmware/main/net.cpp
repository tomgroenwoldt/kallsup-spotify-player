#include "net.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "captive_dns.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "settings.h"

namespace net {

static const char *TAG = "net";

constexpr uint64_t AP_FALLBACK_US = 60ULL * 1000 * 1000;
// Reconnect back-off: 5, 10, 20, 40, 60, 60, ... s
constexpr uint64_t RETRY_MIN_US = 5ULL * 1000 * 1000;
constexpr uint64_t RETRY_MAX_US = 60ULL * 1000 * 1000;
// Station retries hop channels and disturb clients on the setup AP, so retry less often while it runs.
constexpr uint64_t RETRY_WITH_AP_US = 30ULL * 1000 * 1000;
// Once connected, the setup AP stays this long, so the setup page can still show the result.
constexpr uint64_t AP_LINGER_US = 60ULL * 1000 * 1000;
// Delay before applying new Wi-Fi credentials, so the HTTP response that saved them still gets out.
constexpr uint64_t RECONNECT_DELAY_US = 1000ULL * 1000;

static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_ap_stop_timer;
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_ap_active{false};
static std::atomic<bool> s_have_creds{false};
static std::atomic<bool> s_time_synced{false};
static std::atomic<WifiError> s_error{WifiError::NONE};  // of the last failed attempt
static uint64_t s_retry_us = RETRY_MIN_US;  // next back-off delay, event task only

static void start_ap() {
    if (s_ap_active) return;
    wifi_config_t ap = {};
    const size_t len = strlen(AP_SSID);
    memcpy(ap.ap.ssid, AP_SSID, len);
    ap.ap.ssid_len = len;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_active = true;
    captive_dns::start();
    ESP_LOGW(TAG, "no Wi-Fi connection, started setup AP \"%s\" (http://192.168.4.1)", AP_SSID);
}

static void stop_ap() {
    if (!s_ap_active) return;
    captive_dns::stop();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_active = false;
    ESP_LOGI(TAG, "setup AP stopped");
}

// Loads the station credentials from settings into the Wi-Fi driver.
static bool load_sta_config() {
    const settings::Config cfg = settings::get();
    const std::string &ssid = cfg.wifi_ssid;
    const std::string &pass = cfg.wifi_pass;
    wifi_config_t sta = {};
    if (ssid.empty() || ssid.size() > sizeof(sta.sta.ssid) || pass.size() >= sizeof(sta.sta.password)) {
        return false;
    }
    memcpy(sta.sta.ssid, ssid.data(), ssid.size());
    memcpy(sta.sta.password, pass.data(), pass.size());
    sta.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_LOGI(TAG, "station SSID \"%s\"", ssid.c_str());
    return true;
}

static void on_fallback_timer(void *) {
    if (!s_connected) start_ap();
}

static void on_retry_timer(void *) {
    if (!s_connected && s_have_creds) esp_wifi_connect();
}

static void on_reconnect_timer(void *) {
    reconnect();
}

static void on_ap_stop_timer(void *) {
    if (s_connected) stop_ap();
}

static WifiError error_from_reason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return WifiError::WRONG_PASSWORD;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return WifiError::NOT_FOUND;
        default:
            return WifiError::OTHER;
    }
}

static void on_settings_changed(const settings::Config &old_cfg, const settings::Config &cfg) {
    if (cfg.time_zone != old_cfg.time_zone) apply_time_zone();
    if (cfg.device_name != old_cfg.device_name) mdns_instance_name_set(cfg.device_name.c_str());
    if (cfg.wifi_ssid != old_cfg.wifi_ssid || cfg.wifi_pass != old_cfg.wifi_pass) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
    }
}

static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "%s (reason %d)", s_connected ? "disconnected" : "connect failed", ev->reason);
        s_connected = false;
        s_error = error_from_reason(ev->reason);
        if (!s_have_creds) return;
        if (!esp_timer_is_active(s_fallback_timer)) esp_timer_start_once(s_fallback_timer, AP_FALLBACK_US);
        esp_timer_stop(s_retry_timer);
        const uint64_t delay = s_ap_active ? std::max(s_retry_us, RETRY_WITH_AP_US) : s_retry_us;
        esp_timer_start_once(s_retry_timer, delay);
        ESP_LOGI(TAG, "retrying in %llu s", delay / 1000000);
        s_retry_us = std::min(s_retry_us * 2, RETRY_MAX_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "connected, IP " IPSTR ", http://%s.local", IP2STR(&ev->ip_info.ip), HOSTNAME);
        s_connected = true;
        s_error = WifiError::NONE;
        s_retry_us = RETRY_MIN_US;
        esp_timer_stop(s_fallback_timer);
        if (s_ap_active) {
            esp_timer_stop(s_ap_stop_timer);
            esp_timer_start_once(s_ap_stop_timer, AP_LINGER_US);
        }
    }
}

static void on_time_sync(struct timeval *) {
    s_time_synced = true;
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[48];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "time synced, local time %s", buf);
}

static esp_err_t start_mdns() {
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(HOSTNAME), TAG, "mdns hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(settings::get().device_name.c_str()), TAG, "mdns instance");
    ESP_RETURN_ON_ERROR(mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0), TAG, "mdns service");
    return ESP_OK;
}

esp_err_t init() {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_sta_netif, HOSTNAME), TAG, "hostname");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    // Credentials live in our settings namespace, not in the driver's own NVS copy.
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr), TAG, "ip ev");

    esp_timer_create_args_t fallback_args = {};
    fallback_args.callback = on_fallback_timer;
    fallback_args.name = "wifi_fallback";
    ESP_RETURN_ON_ERROR(esp_timer_create(&fallback_args, &s_fallback_timer), TAG, "timer");
    esp_timer_create_args_t retry_args = {};
    retry_args.callback = on_retry_timer;
    retry_args.name = "wifi_retry";
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "timer");
    esp_timer_create_args_t reconnect_args = {};
    reconnect_args.callback = on_reconnect_timer;
    reconnect_args.name = "wifi_reconnect";
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &s_reconnect_timer), TAG, "timer");
    esp_timer_create_args_t ap_stop_args = {};
    ap_stop_args.callback = on_ap_stop_timer;
    ap_stop_args.name = "wifi_ap_stop";
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_stop_args, &s_ap_stop_timer), TAG, "timer");
    settings::on_change(on_settings_changed);

    apply_time_zone();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb = on_time_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "sntp");
    ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mdns");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    s_have_creds = load_sta_config();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    // Power save adds 100+ ms latency spikes; streaming needs steady throughput.
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps");
    if (s_have_creds) {
        esp_timer_start_once(s_fallback_timer, AP_FALLBACK_US);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi credentials stored");
        start_ap();
    }
    return ESP_OK;
}

void reconnect() {
    esp_timer_stop(s_retry_timer);
    s_connected = false;
    s_error = WifiError::NONE;
    esp_wifi_disconnect();
    s_have_creds = load_sta_config();
    if (!s_have_creds) return;
    if (!s_ap_active) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, AP_FALLBACK_US);
    }
    esp_wifi_connect();
}

void apply_time_zone() {
    const std::string tz = settings::get().time_zone;
    setenv("TZ", tz.c_str(), 1);
    tzset();
    ESP_LOGI(TAG, "time zone %s", tz.c_str());
}

Status status() {
    Status st;
    st.sta_connected = s_connected;
    st.ap_active = s_ap_active;
    st.error = s_error;
    if (st.sta_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st.ssid = reinterpret_cast<const char *>(ap.ssid);
            st.rssi = ap.rssi;
        }
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            st.ip = buf;
        }
    }
    return st;
}

bool time_synced() {
    return s_time_synced;
}

}  // namespace net
