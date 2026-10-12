/*
 * ESP32-C5 10 MHz HAMNET station demo @ 5730 MHz.
 *
 * Builds on the earlier result that the ESP32-C5 CAN associate at 10 MHz once
 * modem_syscon_ll_enable_fe_40m_clock(&MODEM_SYSCON, 1) is enabled before
 * esp_wifi_init() (front-end clock needed for TX in the 11p / half-clocked PHY).
 *
 * 5730 MHz is channel 146, which is not a valid 20 MHz ESP channel. The driver
 * retunes itself during connect by calling phy_change_channel(freq,1,0,sec).
 * We therefore tell the driver a nearby valid channel (144 = 5720 MHz) and use
 * a linker --wrap hook (see src/CMakeLists.txt) to redirect that retune to the
 * real 5730 MHz center.
 *
 * Flow: PHY hack -> connect -> DHCP -> ping gateway -> HTTP GET -> steady ping.
 * On link loss it resumes reconnecting.
 *
 * Target AP: SSID OE6DBR, BSSID 08:55:31:be:0f:57, open, 5730 MHz / 10 MHz / 11an.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

#include "hal/modem_syscon_ll.h"

#define TARGET_FREQ_MHZ          5730   /* real off-grid center frequency      */
#define PUBLIC_FREQ_MHZ          5720   /* what the driver thinks (channel 144)*/
#define PUBLIC_CHANNEL           144
#define ANCHOR_CHANNEL           140

#define TARGET_SSID              "HAMNET"
#define TARGET_BSSID_BYTES       0x08, 0x55, 0x31, 0xbe, 0x0f, 0x57

#define HTTP_URL                 "http://news.oe1xqu.hamip.at"
#define HTTP_PRINT_CHARS         1000

#define BEACON_WAIT_MS           3000
#define CONNECT_TIMEOUT_MS       15000
#define RECONNECT_BACKOFF_MS     2000

extern void phy_11p_set(int enable, int param2);
extern void phy_change_channel(int freq, int bw, int param3, int param4);

static const char *TAG = "hamnet10m";
static const uint8_t TARGET_BSSID[6] = { TARGET_BSSID_BYTES };

#define EVT_GOT_IP        BIT0
#define EVT_DISCONNECTED  BIT1

static EventGroupHandle_t s_evt;
static volatile wifi_err_reason_t s_disc_reason = 0;
static volatile esp_ip4_addr_t s_gw;

/* Essential RX diagnostics: tells "no RX" apart from "assoc failed". */
static volatile uint32_t s_beacons, s_auth, s_assoc_resp, s_deauth;

/* ---- --wrap hook: land the internal retune on the real 5730 MHz ---- */

int __real_phy_change_channel(int freq, int bw, int p3, int p4);

int __wrap_phy_change_channel(int freq, int bw, int p3, int p4)
{
    if (freq == PUBLIC_FREQ_MHZ || freq == PUBLIC_CHANNEL) {
        ESP_LOGI(TAG, "wrap: phy_change_channel(%d) -> %d MHz", freq, TARGET_FREQ_MHZ);
        freq = TARGET_FREQ_MHZ;
    }
    return __real_phy_change_channel(freq, bw, p3, p4);
}

/* ---- Hidden clock / PHY setup ---- */

static void enable_fe_40m_clock(const char *why)
{
    modem_syscon_ll_enable_fe_40m_clock(&MODEM_SYSCON, 1);
    ESP_LOGI(TAG, "FE 40M clock (%s): %d", why, (int)MODEM_SYSCON.clk_conf1.clk_fe_40m_en);
}

/* Only valid while STA is idle (not scanning/connecting). */
static bool apply_10mhz_phy(void)
{
    phy_11p_set(1, 0);
    esp_err_t err = esp_wifi_set_channel(ANCHOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel(%d) failed: %s", ANCHOR_CHANNEL, esp_err_to_name(err));
        return false;
    }
    phy_change_channel(TARGET_FREQ_MHZ, 1, 0, 0);
    ESP_LOGI(TAG, "10 MHz PHY hack applied (%d MHz)", TARGET_FREQ_MHZ);
    return true;
}

/* ---- Promiscuous diagnostics (minimal) ---- */

static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = buf;
    const uint8_t *d = pkt->payload;
    if (pkt->rx_ctrl.rx_state != 0 || pkt->rx_ctrl.sig_len < 24) return;
    if (memcmp(&d[10], TARGET_BSSID, 6) != 0 && memcmp(&d[16], TARGET_BSSID, 6) != 0) return;

    switch ((d[0] & 0xf0) >> 4) {
    case 1:  s_assoc_resp++; break;
    case 8:  s_beacons++;    break;
    case 11: s_auth++;       break;
    case 12: s_deauth++;     break;
    default: break;
    }
}

/* ---- Events ---- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = data;
        ESP_LOGI(TAG, "CONNECTED channel=%u aid=%u", e->channel, e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        s_disc_reason = e->reason;
        ESP_LOGW(TAG, "DISCONNECTED reason=%u rssi=%d", (unsigned)e->reason, e->rssi);
        xEventGroupSetBits(s_evt, EVT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_gw = e->ip_info.gw;
        ESP_LOGI(TAG, "GOT_IP ip=" IPSTR " gw=" IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_evt, EVT_GOT_IP);
    }
}

/* ---- Ping helpers ---- */

static void ping_success(esp_ping_handle_t h, void *a)
{
    uint16_t seq; uint32_t rtt;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    ESP_LOGI(TAG, "ping seq=%u rtt=%" PRIu32 " ms", seq, rtt);
}

static void ping_timeout(esp_ping_handle_t h, void *a)
{
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    ESP_LOGW(TAG, "ping timeout seq=%u", seq);
}

/* Ping gateway `count` times, block until done, return replies received. */
static uint32_t ping_gateway_blocking(uint32_t count)
{
    esp_ping_handle_t h = NULL;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = count;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = s_gw.addr;
    esp_ping_callbacks_t cbs = { .on_ping_success = ping_success, .on_ping_timeout = ping_timeout };
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) return 0;
    esp_ping_start(h);
    /* ESP_PING_DEFAULT timeout 1s + interval 1s per packet; wait a bit longer. */
    vTaskDelay(pdMS_TO_TICKS(count * 1200 + 1500));
    uint32_t rx = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &rx, sizeof(rx));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return rx;
}

static esp_ping_handle_t s_steady_ping;

static void start_steady_ping(void)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = s_gw.addr;
    esp_ping_callbacks_t cbs = { .on_ping_success = ping_success, .on_ping_timeout = ping_timeout };
    if (esp_ping_new_session(&cfg, &cbs, &s_steady_ping) == ESP_OK) {
        esp_ping_start(s_steady_ping);
        ESP_LOGI(TAG, "steady ping started (1 s interval)");
    }
}

static void stop_steady_ping(void)
{
    if (s_steady_ping) {
        esp_ping_stop(s_steady_ping);
        esp_ping_delete_session(s_steady_ping);
        s_steady_ping = NULL;
    }
}

/* ---- HTTP GET ---- */

static void http_get_demo(void)
{
    char *body = malloc(HTTP_PRINT_CHARS + 1);
    if (!body) { ESP_LOGE(TAG, "http: no mem"); return; }
    size_t used = 0;

    esp_http_client_config_t cfg = {
        .url = HTTP_URL,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "http: init failed"); free(body); return; }

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        free(body);
        return;
    }

    int clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "HTTP GET %s -> status=%d content_length=%d", HTTP_URL, status, clen);

    while (used < HTTP_PRINT_CHARS) {
        int r = esp_http_client_read(c, body + used, HTTP_PRINT_CHARS - used);
        if (r <= 0) break;
        used += r;
    }
    body[used] = '\0';
    ESP_LOGI(TAG, "HTTP first %u chars:\n%s", (unsigned)used, body);

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
}

/* ---- Wi-Fi setup ---- */

static void wifi_init_and_prepare(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    enable_fe_40m_clock("before esp_wifi_init");   /* must precede esp_wifi_init() */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_country_t country = {
        .cc = "AT", .schan = 1, .nchan = 13,
        .max_tx_power = 20, .policy = WIFI_COUNTRY_POLICY_MANUAL,
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

    wifi_bandwidths_t bw = { .ghz_2g = WIFI_BW_HT20, .ghz_5g = WIFI_BW_HT20 };
    ESP_ERROR_CHECK(esp_wifi_set_bandwidths(WIFI_IF_STA, &bw));

    enable_fe_40m_clock("after esp_wifi_start");

    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, TARGET_SSID, strlen(TARGET_SSID));
    memcpy(sta.sta.bssid, TARGET_BSSID, 6);
    sta.sta.bssid_set = true;
    sta.sta.channel = PUBLIC_CHANNEL;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_FAST_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

/* ---- Connect cycle ---- */

/* Returns true once fully connected with an IP. */
static bool connect_once(void)
{
    enable_fe_40m_clock("before connect");
    if (!apply_10mhz_phy()) return false;

    /* Wait for a target beacon so we know RX works before attempting assoc. */
    uint32_t start = s_beacons;
    for (int i = 0; i < BEACON_WAIT_MS / 100 && s_beacons == start; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "beacons=%" PRIu32 " auth=%" PRIu32 " assoc_resp=%" PRIu32 " deauth=%" PRIu32,
             s_beacons, s_auth, s_assoc_resp, s_deauth);
    if (s_beacons == start) ESP_LOGW(TAG, "no target beacon seen before connect");

    xEventGroupClearBits(s_evt, EVT_GOT_IP | EVT_DISCONNECTED);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_GOT_IP | EVT_DISCONNECTED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & EVT_GOT_IP) return true;
    if (bits & EVT_DISCONNECTED) ESP_LOGW(TAG, "connect failed (reason=%u)", (unsigned)s_disc_reason);
    else ESP_LOGW(TAG, "connect timed out, auth=%" PRIu32 " assoc_resp=%" PRIu32, s_auth, s_assoc_resp);
    esp_wifi_disconnect();
    return false;
}

static void app_task(void *arg)
{
    for (;;) {
        /* 1-2: get fully connected (retry until success). */
        while (!connect_once()) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_BACKOFF_MS));
        }

        /* 3: gateway ping check. */
        uint32_t rx = ping_gateway_blocking(3);
        ESP_LOGI(TAG, "gateway ping check: %" PRIu32 "/3 replies", rx);
        if (rx == 0) {
            ESP_LOGW(TAG, "no ping replies, reconnecting");
            esp_wifi_disconnect();
            xEventGroupWaitBits(s_evt, EVT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_BACKOFF_MS));
            continue;
        }

        /* 4: HTTP GET. */
        http_get_demo();

        /* 5: steady-state ping until the link drops, then reconnect. */
        ESP_LOGI(TAG, "SUCCESS: entering steady-state ping");
        start_steady_ping();
        xEventGroupWaitBits(s_evt, EVT_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        stop_steady_ping();
        ESP_LOGW(TAG, "link dropped (reason=%u), resuming reconnect", (unsigned)s_disc_reason);
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

    ESP_LOGI(TAG, "ESP32-C5 10 MHz HAMNET demo: SSID=%s %d MHz (driver ch %d) / 10 MHz / 11an",
             TARGET_SSID, TARGET_FREQ_MHZ, PUBLIC_CHANNEL);

    wifi_init_and_prepare();
    xTaskCreate(app_task, "app", 8192, NULL, 5, NULL);
}
