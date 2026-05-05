/*
 * ESP32-C5 final evidence harness:
 * 5745 MHz / 10 MHz / 11an receive works, but STA association does not.
 *
 * Purpose:
 * - Confirm passive RX on the target AP using the proven hidden PHY sequence.
 * - Attempt a normal open-AP connection once.
 * - Record the managed STA transition and the refusal to retune during connect.
 * - Avoid crash-prone behavior; collect evidence cleanly.
 *
 * Target AP:
 *   SSID  = OE6DBR
 *   BSSID = 08:55:31:be:0f:57
 *   RF    = 5745 MHz / 10 MHz / 11an
 *
 * Key interpretation:
 * - If beacons are continuously decoded after the hidden setup, RX hack is proven.
 * - If esp_wifi_connect() enters auth but no auth/assoc frames are observed and
 *   retune is rejected during connect, managed STA is not working in hacked 10 MHz mode.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define TARGET_FREQ_MHZ          5745
#define TARGET_PUBLIC_CHANNEL    149
#define TARGET_ANCHOR_CHANNEL    140

#define TARGET_SSID              "OE6DBR"
#define TARGET_BSSID_BYTES       0x08, 0x55, 0x31, 0xbe, 0x0f, 0x57

#define PRE_CONNECT_WAIT_MS      1000
#define STATUS_PERIOD_MS         1000
#define CONNECT_OBSERVE_MS       20000

extern void phy_11p_set(int enable, int param2);
extern void phy_change_channel(int freq, int bw, int param3, int param4);

static const char *TAG = "assoc10m";
static const uint8_t TARGET_BSSID[6] = { TARGET_BSSID_BYTES };

typedef struct {
    uint32_t any_mgmt;
    uint32_t target_mgmt;
    uint32_t beacons;
    uint32_t probe_resp;
    uint32_t auth;
    uint32_t assoc_req;
    uint32_t assoc_resp;
    uint32_t disassoc;
    uint32_t deauth;
    int32_t rssi_sum;
    int8_t rssi_min;
    int8_t rssi_max;
    bool first_any_logged;
    bool first_target_logged;
    bool initial_rx_confirmed;
} sniff_stats_t;

static sniff_stats_t g_stats;
static bool g_sta_started = false;
static bool g_connected = false;
static bool g_got_ip = false;
static bool g_connect_attempted = false;
static bool g_connect_retune_rejected = false;
static wifi_err_reason_t g_last_disc_reason = 0;

static void stats_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.rssi_min = 127;
    g_stats.rssi_max = -127;
    g_sta_started = false;
    g_connected = false;
    g_got_ip = false;
    g_connect_attempted = false;
    g_connect_retune_rejected = false;
    g_last_disc_reason = 0;
}

static const char *mgmt_subtype_name(uint8_t subtype)
{
    switch (subtype) {
    case 0:  return "Assoc Req";
    case 1:  return "Assoc Resp";
    case 4:  return "Probe Req";
    case 5:  return "Probe Resp";
    case 8:  return "Beacon";
    case 10: return "Disassoc";
    case 11: return "Auth";
    case 12: return "Deauth";
    case 13: return "Action";
    default: return "Other";
    }
}

static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type == WIFI_PKT_MISC) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *d = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (pkt->rx_ctrl.rx_state != 0) return;
    if (len < 24) return;

    uint8_t frame_type = (d[0] & 0x0c) >> 2;
    uint8_t frame_subtype = (d[0] & 0xf0) >> 4;
    if (frame_type != 0) return;

    g_stats.any_mgmt++;

    if (!g_stats.first_any_logged) {
        g_stats.first_any_logged = true;
        ESP_LOGI(TAG,
                 "FIRST_ANY_MGMT: subtype=%s len=%u rssi=%d public_channel=%u",
                 mgmt_subtype_name(frame_subtype),
                 len,
                 (int)(int8_t)pkt->rx_ctrl.rssi,
                 (unsigned)pkt->rx_ctrl.channel);
    }

    bool related_to_target =
        memcmp(&d[10], TARGET_BSSID, 6) == 0 ||
        memcmp(&d[16], TARGET_BSSID, 6) == 0;

    if (!related_to_target) return;

    g_stats.target_mgmt++;

    if (!g_stats.first_target_logged) {
        g_stats.first_target_logged = true;
        g_stats.initial_rx_confirmed = true;
        ESP_LOGI(TAG,
                 "FIRST_TARGET_MGMT: subtype=%s len=%u rssi=%d public_channel=%u",
                 mgmt_subtype_name(frame_subtype),
                 len,
                 (int)(int8_t)pkt->rx_ctrl.rssi,
                 (unsigned)pkt->rx_ctrl.channel);
    }

    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
    if (rssi < g_stats.rssi_min) g_stats.rssi_min = rssi;
    if (rssi > g_stats.rssi_max) g_stats.rssi_max = rssi;
    g_stats.rssi_sum += rssi;

    switch (frame_subtype) {
    case 5:  g_stats.probe_resp++; break;
    case 8:  g_stats.beacons++; break;
    case 10: g_stats.disassoc++; break;
    case 11: g_stats.auth++; break;
    case 12: g_stats.deauth++; break;
    case 0:  g_stats.assoc_req++; break;
    case 1:  g_stats.assoc_resp++; break;
    default: break;
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            g_sta_started = true;
            ESP_LOGI(TAG, "EVENT: WIFI_EVENT_STA_START");
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)event_data;
            g_connected = true;
            ESP_LOGI(TAG,
                     "EVENT: WIFI_EVENT_STA_CONNECTED ssid=%.*s channel=%u authmode=%u",
                     e->ssid_len, e->ssid, e->channel, e->authmode);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)event_data;
            g_connected = false;
            g_last_disc_reason = e->reason;
            ESP_LOGW(TAG, "EVENT: WIFI_EVENT_STA_DISCONNECTED reason=%u", (unsigned)e->reason);
            break;
        }

        default:
            ESP_LOGI(TAG, "EVENT: WIFI_EVENT id=%" PRId32, event_id);
            break;
        }
    }

    if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            g_got_ip = true;
            ESP_LOGI(TAG, "EVENT: IP_EVENT_STA_GOT_IP");
            break;

        default:
            ESP_LOGI(TAG, "EVENT: IP_EVENT id=%" PRId32, event_id);
            break;
        }
    }
}

static bool apply_hidden_10mhz_phy(const char *why)
{
    ESP_LOGI(TAG, "Applying hidden PHY hack: %s", why);
    phy_11p_set(1, 0);

    esp_err_t err = esp_wifi_set_channel(TARGET_ANCHOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "esp_wifi_set_channel(%d) failed during '%s': %s",
                 TARGET_ANCHOR_CHANNEL, why, esp_err_to_name(err));
        return false;
    }

    phy_change_channel(TARGET_FREQ_MHZ, 1, 0, 0);
    return true;
}

static void log_status(uint32_t elapsed_ms)
{
    uint32_t matched = g_stats.beacons + g_stats.probe_resp + g_stats.auth +
                       g_stats.assoc_req + g_stats.assoc_resp +
                       g_stats.disassoc + g_stats.deauth;
    int avg_rssi = matched ? (g_stats.rssi_sum / (int32_t)matched) : 0;

    ESP_LOGI(TAG,
             "[%5" PRIu32 " ms] rx_confirmed=%d sta_started=%d connect_attempted=%d "
             "retune_rejected=%d connected=%d got_ip=%d disc_reason=%u "
             "beacons=%" PRIu32 " auth=%" PRIu32 " assoc_req=%" PRIu32
             " assoc_resp=%" PRIu32 " deauth=%" PRIu32 " avg_rssi=%d min=%d max=%d",
             elapsed_ms,
             g_stats.initial_rx_confirmed,
             g_sta_started,
             g_connect_attempted,
             g_connect_retune_rejected,
             g_connected,
             g_got_ip,
             (unsigned)g_last_disc_reason,
             g_stats.beacons,
             g_stats.auth,
             g_stats.assoc_req,
             g_stats.assoc_resp,
             g_stats.deauth,
             avg_rssi,
             g_stats.rssi_min,
             g_stats.rssi_max);
}

static void wifi_init_and_prepare(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_country_t country = {
        .cc = "AT",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    ESP_ERROR_CHECK(esp_wifi_set_protocols(WIFI_IF_STA, &protocols));

    wifi_bandwidths_t bw = {
        .ghz_2g = WIFI_BW_HT20,
        .ghz_5g = WIFI_BW_HT20,
    };
    ESP_ERROR_CHECK(esp_wifi_set_bandwidths(WIFI_IF_STA, &bw));

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL & ~WIFI_PROMIS_FILTER_MASK_FCSFAIL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_LOGI(TAG, "Promiscuous mode enabled");
    bool ok = apply_hidden_10mhz_phy("initial RX setup");
    ESP_LOGI(TAG, "Initial hidden PHY setup %s", ok ? "succeeded" : "failed");
}

static void configure_sta_profile(void)
{
    wifi_config_t sta_cfg = {0};

    memcpy(sta_cfg.sta.ssid, TARGET_SSID, strlen(TARGET_SSID));
    memcpy(sta_cfg.sta.bssid, TARGET_BSSID, 6);
    sta_cfg.sta.bssid_set = true;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.pmf_cfg.capable = false;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.channel = TARGET_PUBLIC_CHANNEL;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    ESP_LOGI(TAG,
             "Configured STA profile: SSID=%s BSSID=%02x:%02x:%02x:%02x:%02x:%02x public_channel=%d",
             TARGET_SSID,
             TARGET_BSSID[0], TARGET_BSSID[1], TARGET_BSSID[2],
             TARGET_BSSID[3], TARGET_BSSID[4], TARGET_BSSID[5],
             TARGET_PUBLIC_CHANNEL);
}

static void attempt_connect_once(void)
{
    ESP_LOGI(TAG, "Calling esp_wifi_connect()");
    g_connect_attempted = true;

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed immediately: %s", esp_err_to_name(err));
        return;
    }

    if (!apply_hidden_10mhz_phy("connect-time observation")) {
        g_connect_retune_rejected = true;
    }
}

static void emit_final_verdict(void)
{
    if (g_got_ip) {
        ESP_LOGI(TAG, "VERDICT: full station connection succeeded");
        return;
    }

    if (g_connected) {
        ESP_LOGW(TAG, "VERDICT: associated, but no IP");
        return;
    }

    if (g_stats.initial_rx_confirmed &&
        g_connect_attempted &&
        g_connect_retune_rejected &&
        g_stats.auth == 0 &&
        g_stats.assoc_req == 0 &&
        g_stats.assoc_resp == 0) {
        ESP_LOGW(TAG, "VERDICT: passive 10 MHz RX works, managed STA 10 MHz connect not achieved");
        ESP_LOGW(TAG, "VERDICT: stock STA path takes channel ownership during connect and blocks retune");
        return;
    }

    if (g_stats.initial_rx_confirmed) {
        ESP_LOGW(TAG, "VERDICT: RX confirmed, connection failed without management progress");
        return;
    }

    ESP_LOGW(TAG, "VERDICT: no useful target RX observed");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    stats_reset();

    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "ESP32-C5 final 10 MHz STA evidence harness");
    ESP_LOGI(TAG, "Target AP: SSID=%s BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
             TARGET_SSID,
             TARGET_BSSID[0], TARGET_BSSID[1], TARGET_BSSID[2],
             TARGET_BSSID[3], TARGET_BSSID[4], TARGET_BSSID[5]);
    ESP_LOGI(TAG, "Target RF: %d MHz / 10 MHz / 11an", TARGET_FREQ_MHZ);
    ESP_LOGI(TAG, "Public STA channel: %d, anchor channel: %d",
             TARGET_PUBLIC_CHANNEL, TARGET_ANCHOR_CHANNEL);
    ESP_LOGI(TAG, "======================================================");

    wifi_init_and_prepare();
    configure_sta_profile();

    ESP_LOGI(TAG, "Waiting %d ms for target beacons", PRE_CONNECT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(PRE_CONNECT_WAIT_MS));

    attempt_connect_once();

    uint32_t elapsed = 0;
    while (elapsed < CONNECT_OBSERVE_MS) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
        elapsed += STATUS_PERIOD_MS;
        log_status(elapsed);

        if (g_got_ip) {
            break;
        }
    }

    emit_final_verdict();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}