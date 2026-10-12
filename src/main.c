/*
 * ESP32-C5 10 MHz HAMNET station demo.
 *
 * Goal: associate as a normal STA (open, DHCP) with an AP running at
 * 5745 MHz / 10 MHz / 11an, using undocumented PHY hooks.
 *
 * New compared to the previous RX-only harness:
 *   modem_syscon_ll_enable_fe_40m_clock(&MODEM_SYSCON, 1) is called right
 *   before esp_wifi_init(), exactly as in the ITS-G5 "txenabled" firmware.
 *   Hypothesis: this front-end clock is required for TX in the 11p
 *   (half-clocked, 10 MHz) PHY mode. Without it, RX/scan worked but no
 *   auth frames ever reached the AP.
 *
 * Sequence:
 *   1. FE 40 MHz clock on -> esp_wifi_init/start (STA, 5 GHz, 11a/n, HT20)
 *   2. promiscuous on -> phy_11p_set(1,0) -> set_channel(140)
 *      -> phy_change_channel(5745,1,0,0)          (proven RX order)
 *   3. wait for target beacons -> esp_wifi_connect()
 *   4. on disconnect: re-assert clock + PHY hack while idle, reconnect
 *   5. on GOT_IP: ping the gateway to prove bidirectional traffic
 *
 * Target AP:
 *   SSID  = OE6DBR
 *   BSSID = 08:55:31:be:0f:57
 *   RF    = 5745 MHz (public ch 149) / 10 MHz / 11an, open
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

#include "hal/modem_syscon_ll.h"

#define TARGET_FREQ_MHZ          5745
#define TARGET_PUBLIC_CHANNEL    149
#define TARGET_ANCHOR_CHANNEL    140

#define TARGET_SSID              "OE6DBR"
#define TARGET_BSSID_BYTES       0x08, 0x55, 0x31, 0xbe, 0x0f, 0x57

#define BEACON_WAIT_MS           3000
#define RECONNECT_BACKOFF_MS     2000
#define STATUS_PERIOD_MS         1000
#define PING_COUNT               10

extern void phy_11p_set(int enable, int param2);
extern void phy_change_channel(int freq, int bw, int param3, int param4);

static const char *TAG = "hamnet10m";
static const uint8_t TARGET_BSSID[6] = { TARGET_BSSID_BYTES };

#define EVT_DISCONNECTED  BIT0

static EventGroupHandle_t s_evt;

typedef struct {
    volatile uint32_t target_mgmt;
    volatile uint32_t beacons;
    volatile uint32_t probe_resp;
    volatile uint32_t auth;
    volatile uint32_t assoc_req;
    volatile uint32_t assoc_resp;
    volatile uint32_t disassoc;
    volatile uint32_t deauth;
    volatile int32_t rssi_sum;
    volatile int8_t rssi_min;
    volatile int8_t rssi_max;
} sniff_stats_t;

static sniff_stats_t g_stats;
static volatile bool g_connected = false;
static volatile bool g_got_ip = false;
static volatile uint32_t g_attempts = 0;
static volatile wifi_err_reason_t g_last_disc_reason = 0;
static esp_ping_handle_t g_ping = NULL;

/* ---- Hidden clock / PHY setup ---- */

static void enable_fe_40m_clock(const char *why)
{
    bool was = MODEM_SYSCON.clk_conf1.clk_fe_40m_en;
    modem_syscon_ll_enable_fe_40m_clock(&MODEM_SYSCON, 1);
    ESP_LOGI(TAG, "FE 40M clock (%s): was=%d now=%d",
             why, was, (int)MODEM_SYSCON.clk_conf1.clk_fe_40m_en);
}

/* Must only be called while the STA is idle (not scanning/connecting). */
static bool apply_10mhz_phy(const char *why)
{
    ESP_LOGI(TAG, "Applying 10 MHz PHY hack (%s)", why);
    phy_11p_set(1, 0);

    esp_err_t err = esp_wifi_set_channel(TARGET_ANCHOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_channel(%d) failed: %s",
                 TARGET_ANCHOR_CHANNEL, esp_err_to_name(err));
        return false;
    }

    phy_change_channel(TARGET_FREQ_MHZ, 1, 0, 0);
    return true;
}

/* ---- Promiscuous diagnostics ---- */

static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *d = pkt->payload;

    if (pkt->rx_ctrl.rx_state != 0 || pkt->rx_ctrl.sig_len < 24) return;
    if (memcmp(&d[10], TARGET_BSSID, 6) != 0 && memcmp(&d[16], TARGET_BSSID, 6) != 0) return;

    g_stats.target_mgmt++;

    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
    if (rssi < g_stats.rssi_min) g_stats.rssi_min = rssi;
    if (rssi > g_stats.rssi_max) g_stats.rssi_max = rssi;
    g_stats.rssi_sum += rssi;

    switch ((d[0] & 0xf0) >> 4) {
    case 0:  g_stats.assoc_req++; break;
    case 1:  g_stats.assoc_resp++; break;
    case 5:  g_stats.probe_resp++; break;
    case 8:  g_stats.beacons++; break;
    case 10: g_stats.disassoc++; break;
    case 11: g_stats.auth++; break;
    case 12: g_stats.deauth++; break;
    default: break;
    }
}

static void log_status(uint32_t elapsed_ms)
{
    uint32_t n = g_stats.target_mgmt;
    int avg = n ? (int)(g_stats.rssi_sum / (int32_t)n) : 0;

    ESP_LOGI(TAG,
             "[%6" PRIu32 " ms] attempt=%" PRIu32 " connected=%d ip=%d disc=%u fe40m=%d | "
             "bcn=%" PRIu32 " prb=%" PRIu32 " auth=%" PRIu32 " areq=%" PRIu32
             " aresp=%" PRIu32 " deauth=%" PRIu32 " disassoc=%" PRIu32
             " rssi avg/min/max=%d/%d/%d",
             elapsed_ms, g_attempts, g_connected, g_got_ip,
             (unsigned)g_last_disc_reason, (int)MODEM_SYSCON.clk_conf1.clk_fe_40m_en,
             g_stats.beacons, g_stats.probe_resp, g_stats.auth, g_stats.assoc_req,
             g_stats.assoc_resp, g_stats.deauth, g_stats.disassoc,
             avg, n ? g_stats.rssi_min : 0, n ? g_stats.rssi_max : 0);
}

/* ---- Ping ---- */

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq;
    uint32_t rtt;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    ESP_LOGI(TAG, "PING reply seq=%u rtt=%" PRIu32 " ms", seq, rtt);
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    ESP_LOGW(TAG, "PING timeout seq=%u", seq);
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t tx, rx, dur;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &rx, sizeof(rx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &dur, sizeof(dur));
    ESP_LOGI(TAG, "PING done: %" PRIu32 "/%" PRIu32 " replies in %" PRIu32 " ms", rx, tx, dur);
    if (rx > 0) {
        ESP_LOGI(TAG, "VERDICT: bidirectional IP traffic over 10 MHz works");
    }
}

static void start_ping(esp_ip4_addr_t gw)
{
    if (g_ping) {
        esp_ping_stop(g_ping);
        esp_ping_delete_session(g_ping);
        g_ping = NULL;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = PING_COUNT;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = gw.addr;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };

    if (esp_ping_new_session(&cfg, &cbs, &g_ping) == ESP_OK) {
        ESP_LOGI(TAG, "Pinging gateway " IPSTR, IP2STR(&gw));
        esp_ping_start(g_ping);
    } else {
        ESP_LOGE(TAG, "esp_ping_new_session failed");
        g_ping = NULL;
    }
}

/* ---- Events ---- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "EVENT: STA_START");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = data;
            g_connected = true;
            ESP_LOGI(TAG, "EVENT: STA_CONNECTED ssid=%.*s channel=%u authmode=%u aid=%u",
                     e->ssid_len, e->ssid, e->channel, e->authmode, e->aid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            g_connected = false;
            g_got_ip = false;
            g_last_disc_reason = e->reason;
            ESP_LOGW(TAG, "EVENT: STA_DISCONNECTED reason=%u rssi=%d",
                     (unsigned)e->reason, e->rssi);
            xEventGroupSetBits(s_evt, EVT_DISCONNECTED);
            break;
        }
        default:
            ESP_LOGI(TAG, "EVENT: WIFI_EVENT id=%" PRId32, id);
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        g_got_ip = true;
        ESP_LOGI(TAG, "EVENT: GOT_IP ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
        start_ping(e->ip_info.gw);
    }
}

/* ---- Wi-Fi setup ---- */

static void wifi_init_and_prepare(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Key step from the TX-enabled ITS-G5 firmware: must precede esp_wifi_init(). */
    enable_fe_40m_clock("before esp_wifi_init");
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

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

    /* esp_wifi_start() manages modem clocks; make sure the bit survived. */
    enable_fe_40m_clock("after esp_wifi_start");

    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_config_t sta_cfg = {0};
    memcpy(sta_cfg.sta.ssid, TARGET_SSID, strlen(TARGET_SSID));
    memcpy(sta_cfg.sta.bssid, TARGET_BSSID, 6);
    sta_cfg.sta.bssid_set = true;
    sta_cfg.sta.channel = TARGET_PUBLIC_CHANNEL;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.pmf_cfg.capable = false;
    sta_cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
}

/* ---- Connect manager ---- */

static void wait_for_beacons(uint32_t timeout_ms)
{
    uint32_t start = g_stats.beacons;
    for (uint32_t t = 0; t < timeout_ms; t += 100) {
        if (g_stats.beacons > start) {
            ESP_LOGI(TAG, "Target beacons visible (%" PRIu32 " total)", g_stats.beacons);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "No target beacon within %" PRIu32 " ms, connecting anyway", timeout_ms);
}

static void connect_task(void *arg)
{
    for (;;) {
        enable_fe_40m_clock("before connect");

        bool phy_ok = false;
        for (int i = 0; i < 5 && !phy_ok; i++) {
            phy_ok = apply_10mhz_phy(g_attempts == 0 ? "initial" : "reconnect");
            if (!phy_ok) vTaskDelay(pdMS_TO_TICKS(500));
        }

        wait_for_beacons(BEACON_WAIT_MS);

        xEventGroupClearBits(s_evt, EVT_DISCONNECTED);
        g_attempts++;
        ESP_LOGI(TAG, "Connect attempt #%" PRIu32, g_attempts);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_BACKOFF_MS));
            continue;
        }

        xEventGroupWaitBits(s_evt, EVT_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_BACKOFF_MS));
    }
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

    s_evt = xEventGroupCreate();
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.rssi_min = 127;
    g_stats.rssi_max = -127;

    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "ESP32-C5 10 MHz HAMNET STA demo (FE 40M clock + 11p PHY)");
    ESP_LOGI(TAG, "Target: SSID=%s BSSID=%02x:%02x:%02x:%02x:%02x:%02x open",
             TARGET_SSID, TARGET_BSSID[0], TARGET_BSSID[1], TARGET_BSSID[2],
             TARGET_BSSID[3], TARGET_BSSID[4], TARGET_BSSID[5]);
    ESP_LOGI(TAG, "RF: %d MHz / 10 MHz / 11an, public ch %d, anchor ch %d",
             TARGET_FREQ_MHZ, TARGET_PUBLIC_CHANNEL, TARGET_ANCHOR_CHANNEL);
    ESP_LOGI(TAG, "======================================================");

    wifi_init_and_prepare();

    xTaskCreate(connect_task, "connect", 4096, NULL, 5, NULL);

    uint32_t elapsed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
        elapsed += STATUS_PERIOD_MS;
        log_status(elapsed);
    }
}
