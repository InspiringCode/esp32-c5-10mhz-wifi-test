# Experimental Evaluation of ESP32-C5 Operation on a 5745 MHz / 10 MHz / 802.11an WLAN

> **Update — working 10 MHz STA demo (current `src/main.c`):** The ESP32-C5 **does** associate at 10 MHz
> once `modem_syscon_ll_enable_fe_40m_clock(&MODEM_SYSCON, 1)` is called immediately before
> `esp_wifi_init()` (found in the TX-enabled ITS-G5 firmware,
> `reference/its-g5-receiver-firmware_txenabled/main/main.c`). This front-end clock is what was missing for
> **TX** in the 11p (10 MHz) PHY mode. With it, the demo connects to the open AP, gets a DHCP lease, and
> pings the gateway successfully.
>
> The current demo targets **5730 MHz** (= channel 146, which is not a valid 20 MHz ESP channel). The driver
> retunes itself during connect via `phy_change_channel()`, so it is told a nearby valid channel (144 /
> 5720 MHz) and a linker `--wrap` hook (`src/CMakeLists.txt`) redirects that retune to the real 5730 MHz
> center. After connecting it pings the gateway, performs an HTTP GET to `http://news.oe1xqu.hamip.at`
> (prints the status and first 1000 chars), then pings the gateway once per second, resuming reconnection if
> the link drops. The conclusions in the report below predate these experiments.

## Abstract

This project documents a structured investigation into whether the ESP32-C5 can be coerced into operating on a non-standard 5 GHz Wi-Fi channel configuration: 5745 MHz center frequency, 10 MHz channel width, and 802.11a/n signaling. The central research question was whether hidden PHY-layer functions could be used not only to passively receive traffic from such a network, but also to allow the ESP32-C5 to associate as a normal Wi-Fi station with an unsecured access point configured for 5745/10/an. The experiments show that passive reception is reproducibly achievable using undocumented PHY calls derived from an 802.11p-oriented sniffing path, but managed station association is not. The evidence strongly suggests that the stock ESP32-C5 non-AP Wi-Fi stack is architecturally constrained to supported client bandwidth modes, documented by Espressif as 20 MHz-only for non-AP operation, and cannot be extended to 10 MHz client operation through application-level sequencing alone.[cite:530][cite:693]

## Overview

The work began from an empirical observation: an ESP32-C5 could be made to receive beacons from a MikroTik SXTsq Lite5 configured as an open AP on 5745 MHz with 10 MHz channel width. That observation raised the more ambitious question of whether the same platform could be made to complete a full 802.11 station join on that network. The resulting investigation combined code-level experimentation, PHY retuning, packet-level observability, AP-side logging attempts, and repeated controlled failures to separate what was physically possible from what was blocked by the managed Wi-Fi stack.[cite:530][cite:655]

This report is written as a research starting point for future work. It records not only what worked, but also dead ends, negative results, and the reasoning used to interpret them. Those negative results are especially important because they sharply narrow the remaining hypothesis space.

## Research Question

The core question was: **Can the ESP32-C5 be hacked into associating as a normal station to an unsecured AP operating on 5745 MHz / 10 MHz / 802.11an?** The investigation separated that broad question into two smaller ones:

1. Can the ESP32-C5 reliably **receive** traffic on 5745/10/an?
2. If receive is possible, can the ESP32-C5 also **transmit and process the managed authentication/association exchange** required for station operation on that same channel configuration?

This decomposition follows first principles of 802.11 operation. Passive sniffing and active association are not the same problem: a receiver only needs enough PHY synchronization to demodulate and parse frames, whereas a station must also transmit valid management frames, obey strict timing constraints, maintain coherent channel state, and process responses using the vendor-managed MAC state machine.[cite:529][cite:530]

## Experimental Platform

### Device Under Test

The device under test was an ESP32-C5 running ESP-IDF-based firmware with access to undocumented PHY functions exposed as linkable symbols in the build environment. The relevant hidden functions were `phy_11p_set(int,int)` and `phy_change_channel(int,int,int,int)`, both already present in a known working sniffer implementation.[cite:568]

### Access Point

The AP was a MikroTik SXTsq Lite5 running RouterOS 6.45.9, configured as an open wireless network with the following effective parameters:

| Parameter | Value |
|---|---|
| SSID | `TEST` |
| BSSID | `08:55:31:be:0f:57` |
| Center frequency | 5745 MHz |
| Nominal channel | 149 |
| Channel width | 10 MHz |
| PHY family | 802.11a/n |
| Security | Open / unsecured |

The AP configuration itself was treated as fixed during the experiments so that observed changes could be attributed to ESP-side modifications rather than AP-side variability.

### Software References

Two code bases anchored the investigation:

- A prior custom `main.c` used for matrix testing of ESP32-C5 receive behavior under different hidden-PHY call combinations.[cite:469]
- A working `cmd_sniffer.c` implementation whose ordering of undocumented PHY calls proved crucial and became the reference for successful 10 MHz reception.[cite:568]

## 802.11 Background Needed To Interpret the Results

### Why 10 MHz Is Unusual

Mainstream Wi-Fi client operation typically assumes 20 MHz or wider channels. A 10 MHz channel is narrower, which changes PHY timing and bandwidth assumptions. ESP32-C5 documentation describes non-AP mode in terms consistent with standard client operation and does not expose public APIs for 10 MHz STA mode; Espressif documentation also states that non-AP mode is 20 MHz-only, which became a key interpretive anchor for the entire study.[cite:530][cite:693]

### Why Receiving a Beacon Is Easier Than Joining an AP

A beacon is a periodic management frame sent by the AP. If a receiver is sufficiently tuned and synchronized, it can decode the beacon without transmitting anything. By contrast, joining even an open AP requires an 802.11 management exchange: Authentication Request, Authentication Response, Association Request, Association Response. Only after that does data traffic such as ARP, DHCP, and IP begin.[cite:529]

This distinction matters because a successful beacon sniffer only proves that the receive path can be bent into the needed mode. It says nothing yet about whether the vendor-managed station path can also transmit and receive coherent management traffic under the same non-standard PHY conditions.[cite:529][cite:530]

## Experimental Methodology

The investigation proceeded in phases. Each phase tested one narrower hypothesis than the last.

### Phase 1: Receive Matrix Testing

The first phase tested whether the ESP32-C5 could passively decode traffic from the target AP under several hidden-PHY configurations. The receive-only matrix in the custom `main.c` tried combinations of:

- baseline operation,
- `phy_11p_set(1, 0)` only,
- `phy_change_channel(5745, 1, 0, 0)` only,
- both hidden calls together.[cite:469]

The goal was to determine whether any single hidden function was sufficient, or whether the two had to be used together.

### Phase 2: Ordering Analysis

The matrix testing alone did not fully explain why one configuration occasionally succeeded while others failed. Attention then shifted to the working `cmd_sniffer.c`, which used a more specific sequence:

1. Enable Wi-Fi promiscuous mode.
2. Call `phy_11p_set(1, 0)`.
3. Set a public anchor channel with `esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE)`.
4. Retune with `phy_change_channel(channel, 1, 0, 0)`.[cite:568]

This sequencing differed materially from earlier tests and became the reference hypothesis for reliable reception.

### Phase 3: Station Association Attempt

Once passive reception had been stabilized, the next phase asked whether the same hidden PHY setup could support a real station join. A new `main.c` configured the ESP32-C5 as a normal STA, forced 5 GHz operation, pinned the target BSSID, configured the network as open, and then called `esp_wifi_connect()`. Promiscuous mode remained enabled at the same time to provide local visibility into whether any authentication or association frames involving the target AP became visible during the connection attempt.[cite:529]

### Phase 4: Connect-Time Re-Hack Attempt

A later phase tested whether the hidden PHY state was being overwritten by the managed station connect path. In that version, the hidden PHY sequence was re-applied immediately after `esp_wifi_connect()` and repeatedly during the early authentication window. This experiment directly tested whether the STA engine was taking back channel ownership during connect.[cite:529]

## Results

### Result 1: Passive Receive on 5745/10/an Was Confirmed

The strongest positive result of the entire study was the reproducible passive reception of target AP beacons on 5745 MHz with 10 MHz width using the hidden PHY sequence taken from the sniffer implementation. The working order was:

1. Enable promiscuous mode.
2. Call `phy_11p_set(1, 0)`.
3. Set public anchor channel 140.
4. Retune with `phy_change_channel(5745, 1, 0, 0)`.[cite:568]

Under this setup, the ESP32-C5 repeatedly decoded beacons from the target AP at strong RSSI values in the low -20 dBm range, and sustained reception for the duration of the test. This was the central positive finding: the ESP32-C5 PHY can be coerced into receiving traffic from a 5745/10/an network even though such operation is not exposed through public STA APIs.[cite:530][cite:568]

### Result 2: The Sniffer Reference Was More Informative Than the Early Matrix

The initial receive matrix was valuable as exploration, but it became clear that call ordering mattered more than simple presence or absence of hidden functions. The decisive clue came from `cmd_sniffer.c`, whose sequence was not “call both hidden functions at any time,” but rather “enable promiscuous first, then apply 11p mode, then set an anchor channel, then retune.”[cite:568][cite:469]

This was an important methodological lesson: undocumented PHY behavior was stateful and order-dependent. Early “dead” variants were not necessarily proving impossibility; some were simply initializing the baseband in the wrong mode or at the wrong time.[cite:568]

### Result 3: `rx_ctrl.channel` Was Not a Reliable Ground Truth After Retune

During successful receive runs, the ESP often reported a public channel value corresponding to the anchor channel rather than the actual AP channel context implied by the decoded beacon contents. This meant `rx_ctrl.channel` reflected the managed/public driver context, not necessarily the real RF state after hidden retuning.[cite:568]

This became an important interpretive constraint. It explained why packet metadata could appear inconsistent even when the beacon payload clearly matched the target AP and the AP’s own HT Operation information still pointed to channel 149.[cite:568]

### Result 4: Station Connect Entered `auth`, Then Failed Back to `init`

In the first full association attempt, the ESP32-C5 performed the following observable transition sequence:

- target beacons were received successfully after the hidden PHY setup,
- `esp_wifi_connect()` was called,
- the Wi-Fi driver switched public station context to channel 149,
- the station state transitioned `init -> auth`,
- shortly afterward it fell back `auth -> init`,
- the disconnect reason reported by the driver was 2.[cite:529]

Crucially, during and after this sequence, promiscuous statistics continued to show only beacons from the target AP. No target authentication, association request, or association response frames were observed by the ESP-side sniffer logic.[cite:529]

### Result 5: AP-Side Logging Showed No Evidence of an Auth Attempt

Because a second 10 MHz-capable monitor receiver was unavailable, MikroTik wireless debug logging was used as the AP-side observability mechanism. Logging was configured with `wireless,debug`, but no new AP-side entries corresponding to the ESP’s connection attempt were produced.[cite:655]

This negative result was highly informative. If the AP had received even a recognizable authentication request from the ESP, wireless debug logging would normally be expected to emit client-side connection activity. The absence of any AP-side debug evidence strongly suggested that no usable authentication frame was reaching the AP at all.[cite:655]

### Result 6: Retuning During Connect Was Explicitly Blocked by the STA Engine

The connect-time re-hack experiment produced perhaps the clearest architectural result in the study. Immediately after `esp_wifi_connect()`, attempts to call `esp_wifi_set_channel(TARGET_ANCHOR_CHANNEL, ...)` failed with the runtime warning:

> `STA is scanning or connecting, or AP has connected with external STAs, cannot set channel`

This failure was consistent and repeatable.[cite:529][cite:688]

This result showed that once the managed station engine entered scan/connect/auth handling, it took ownership of channel management and explicitly rejected application-level channel retuning. The failure was not accidental misuse of the API; it was the driver enforcing a design invariant during connect.[cite:529][cite:688]

## Dead Ends and Negative Results

### Dead End 1: Naive Hidden-PHY Combination Testing

The earliest receive matrix tested hidden functions more or less combinatorially. This was useful as reconnaissance, but it did not by itself isolate the working mechanism because it ignored state ordering and driver lifecycle interactions. Several early variants therefore looked like failures of capability when they were really failures of sequence.[cite:469][cite:568]

### Dead End 2: Treating Beacon Reception as Evidence That Association Might Be Near

At several points in the investigation, it was tempting to interpret strong beacon reception as evidence that station association was “close.” The later experiments disproved that intuition. Beacon decode success and full station operation turned out to be separated by a major architectural boundary: the closed STA engine. Passive receive and active client operation were not adjacent steps on one continuum; they were qualitatively different operating modes.[cite:529][cite:530]

### Dead End 3: MikroTik Packet Sniffer on `wlan1`

The built-in MikroTik packet sniffer did not produce useful results when restricted to `wlan1`. This turned out not to be evidence that no wireless traffic existed, but rather a limitation of the observation point. RouterOS packet capture often sees CPU-visible traffic rather than raw on-air 802.11 frames, making it poorly suited to proving whether over-the-air auth management frames existed on a live AP interface in this setup.[cite:610]

### Dead End 4: Re-Applying the PHY Hack During Connect

The last high-value experiment tried to re-impose the hidden 10 MHz state immediately after `esp_wifi_connect()` and throughout the auth window. This failed because the STA engine refused channel changes while connecting. This was a dead end in the narrow sense—no successful join resulted—but it was one of the most important scientific results because it revealed that the managed connect path actively defends its own channel state.[cite:529][cite:688]

## Interpretation

### Working Hypothesis Supported by the Evidence

The evidence supports the following model:

1. The ESP32-C5 baseband/PHY can be coerced into a receive-capable 10 MHz state using undocumented functions and careful ordering.[cite:568]
2. The stock managed station path is a separate layer that assumes supported non-AP operation and takes control of channel state during scan/connect/auth.[cite:529]
3. When station connect begins, that managed path overrides or invalidates the hidden receive-oriented PHY setup and prevents further retuning.[cite:688][cite:529]
4. As a result, the chip continues to be demonstrably capable of passive reception, but does not successfully emit or complete a usable authentication/association exchange on the 10 MHz AP.[cite:655][cite:529]

### Why This Looks Fundamental Rather Than Merely Incomplete

Several lines of evidence point to an architectural limitation rather than a missing sequencing trick:

- Passive receive was achieved repeatedly, so the absence of association was not due to total incompatibility with the RF environment.[cite:530]
- The managed STA engine explicitly blocked channel changes during connect, indicating that user-space sequencing was not free to preserve the hidden state through auth.[cite:529][cite:688]
- No target auth or assoc frames were observed locally or AP-side, indicating failure at the earliest meaningful active station step.[cite:655][cite:529]
- Espressif documentation states that non-AP mode is 20 MHz-only, which is directly consistent with the observed boundary between a hacked receive path and an unhackable stock station path.[cite:530][cite:693]

Taken together, these observations make the most plausible conclusion that stock ESP32-C5 station/client operation on 5745/10/an is **not achievable from application space** using hidden PHY calls alone.[cite:530][cite:693]

## Final Conclusion

The investigation answers the central question with a qualified but strong negative result.

**Yes**, the ESP32-C5 can be coerced into passively receiving beacons and other traffic from an AP operating at 5745 MHz / 10 MHz / 802.11an, using the hidden PHY sequence derived from the sniffing implementation.[cite:568][cite:530]

**No, probably not**, the ESP32-C5 cannot be coerced into associating as a standard Wi-Fi station to that AP using stock ESP-IDF station mode plus those hidden PHY calls from application code. The decisive barriers were the absence of any observable authentication/association exchange and the station engine’s explicit refusal to allow retuning during connect, both of which align with Espressif’s documentation that non-AP mode is 20 MHz-only.[cite:529][cite:688][cite:693]

## Implications for Future Research

The next phase of research should assume that the easy path has been exhausted. Additional progress is unlikely to come from more application-level permutations of `esp_wifi_connect()`, `esp_wifi_set_channel()`, `phy_11p_set()`, and `phy_change_channel()` alone. Future work should instead focus on one of three directions.

### Direction 1: Lower-Level Reverse Engineering

If 10 MHz client operation is still a goal, the problem likely sits below the public STA API layer. Future work would need to determine whether the managed MAC state machine, TX rate control, timing, and channel-width assumptions can be patched or replaced at a lower level than ordinary ESP-IDF application code.[cite:529][cite:693]

### Direction 2: Broader PHY Characterization

The hidden PHY functions could still be studied further in receive-only mode. A useful follow-up would be systematic characterization of what kinds of frames can be received reliably at 10 MHz, under what timing and rate conditions, and how stable the mode is across reboots, firmware versions, and chip revisions.[cite:568]

### Direction 3: Comparative Platform Study

A broader study comparing ESP32-C5 with platforms that explicitly support 10 MHz client operation, or with more open wireless chipsets, would help separate “ESP32-C5-specific closed-stack limitation” from “general difficulty of client operation on narrow 5 GHz channels.”[cite:530][cite:693]

## Practical Starting Point for Later Work

A later researcher starting from this work should begin with the following retained facts:

| Topic | Current best finding |
|---|---|
| Hidden functions of interest | `phy_11p_set()` and `phy_change_channel()` from the sniffer path.[cite:568] |
| Only proven receive sequence | promiscuous on -> `phy_11p_set(1,0)` -> anchor channel 140 -> `phy_change_channel(5745,1,0,0)`.[cite:568] |
| Reliable evidence of success | sustained decoding of target AP beacons at strong RSSI.[cite:530] |
| Reliable evidence of association failure | `init -> auth -> init`, disconnect reason 2, no auth/assoc frames observed, AP-side debug silent.[cite:529][cite:655] |
| Most important architectural clue | `esp_wifi_set_channel()` refused during connect because STA owns channel state while scanning/connecting.[cite:529][cite:688] |
| Current best overall conclusion | passive RX yes; stock STA association on 10 MHz likely no.[cite:530][cite:693] |


## Distilled Core Code

The following distilled code captures the core technical result of the investigation: the only reproducibly successful 10 MHz operation was passive receive using the hidden PHY sequence derived from the sniffer path, while the connect-time retune attempt demonstrates the station engine taking channel ownership and rejecting further retuning during association.[cite:568][cite:529][cite:688]

### Minimal Receive-Proven Sequence

This snippet is the smallest code pattern that captures the receive-capable setup established during the experiments.[cite:568]

```c
#define TARGET_FREQ_MHZ       5745
#define TARGET_ANCHOR_CHANNEL 140

extern void phy_11p_set(int enable, int param2);
extern void phy_change_channel(int freq, int bw, int param3, int param4);

static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *d = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    uint8_t frame_type = (d[0] & 0x0c) >> 2;
    uint8_t frame_subtype = (d[0] & 0xf0) >> 4;
    if (frame_type != 0) return;

    bool related_to_target =
        memcmp(&d[10], TARGET_BSSID, 6) == 0 ||
        memcmp(&d[16], TARGET_BSSID, 6) == 0;

    if (related_to_target && frame_subtype == 8) {
        ESP_LOGI(TAG, "TARGET BEACON: rssi=%d public_channel=%u",
                 (int)(int8_t)pkt->rx_ctrl.rssi,
                 (unsigned)pkt->rx_ctrl.channel);
    }
}

static void setup_10mhz_receive(void)
{
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL & ~WIFI_PROMIS_FILTER_MASK_FCSFAIL,
    };

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    phy_11p_set(1, 0);
    ESP_ERROR_CHECK(esp_wifi_set_channel(TARGET_ANCHOR_CHANNEL, WIFI_SECOND_CHAN_NONE));
    phy_change_channel(TARGET_FREQ_MHZ, 1, 0, 0);
}
```

### Minimal Evidence Harness for Failed STA Association

This snippet captures the smallest useful pattern for demonstrating the managed STA limitation: passive 10 MHz receive can be established first, but a subsequent `esp_wifi_connect()` causes the driver to reclaim channel control, and connect-time retuning is rejected.[cite:529][cite:688]

```c
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
```

### Why These Snippets Matter

The first snippet is the distilled positive result: it is the smallest code form of the reproducible receive path. The second snippet is the distilled negative result: it shows that association attempts transition into the stock station engine, where retuning is blocked and the 10 MHz hack can no longer be maintained from application space.[cite:568][cite:529][cite:688]

## Summary for Further Research

This study established a clear and useful boundary. The ESP32-C5 is not a dead end for 10 MHz 5 GHz receive; on the contrary, passive reception is a real, repeatable capability when undocumented PHY hooks are used with the correct ordering.[cite:568][cite:530] The dead end is narrower and more important: the stock closed station/client path does not follow that hack into full 10 MHz association, and current evidence indicates that it actively resists the required retuning once connect begins.[cite:529][cite:688]

That boundary is the main contribution of this work. It converts an open-ended “maybe there is another init sequence” problem into a much sharper future research question: **can the closed ESP32-C5 station stack itself be modified or bypassed, or must 10 MHz client operation be sought on a different platform?**[cite:693][cite:530]
