// splinter — BLE privacy / anti-tracking decoy for the ESP32 family (ESP-IDF + NimBLE)
//
// Continuously fabricates a churning crowd of plausible-but-fake BLE devices so
// that, in a space you control, a tracking/scanning system sees lots of ordinary
// traffic and your real device(s) do not stand out.
//
// Two build flavours, selected automatically by the target's BLE capability:
//   * Classic ESP32 (BT 4.2): ONE legacy advertiser, rotated at maximum rate —
//     a single MAC on air at a time that churns into a large crowd over time.
//   * BLE-5 chips, e.g. ESP32-C5 / C3 (CONFIG_BT_NIMBLE_EXT_ADV): up to
//     CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES *concurrent* legacy-PDU advertising
//     sets, each with its own MAC, all rotated round-robin — genuinely
//     simultaneous decoys AND maximum churn.
//
// Decoy guardrails (see decoy_vendors.h): advertising is NON-CONNECTABLE and the
// payload is never shaped like Apple Continuity / Microsoft Swift Pair / Google
// Fast Pair, so it creates realistic presence without popping pairing dialogs on
// bystanders' devices.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "decoy_vendors.h"

static const char *TAG = "splinter";

// ---- tunables -------------------------------------------------------------
#define SPLINTER_ROTATE_MS   250   // paced mode: delay between decoys (BENCHMARK=0)
#define SPLINTER_NAME_PROB    40   // % chance a decoy advertises a device name
#define SPLINTER_MFG_PROB     90   // % chance a decoy carries vendor mfg data
#define SPLINTER_ADV_MS      100   // on-air advertising interval per decoy (ms)
#define SPLINTER_EXT_REFRESH_MS 20 // C5 ext-adv: gap between per-instance identity
                                   // refreshes. The instances advertise concurrently
                                   // on their own; this just paces the churn AND
                                   // yields the single core (host/USB/idle/WDT).
#define SPLINTER_REPEAT_MIN    4   // an identity (MAC+payload) stays on an
#define SPLINTER_REPEAT_MAX   12   // instance for this many turns before being
                                   // retired and replaced — makes a decoy look
                                   // like a lingering device, At the default cadence
                                   // (SPLINTER_INSTANCES * SPLINTER_EXT_REFRESH_MS
                                   // per turn) this gives each identity enough
                                   // time on air to transmit several real
                                   // advertising packets (see SPLINTER_ADV_MS).
#define SPLINTER_TX_POWER_MIN -20  // dBm range the radio actually transmits at,
#define SPLINTER_TX_POWER_MAX   4  // randomized per identity. Real RF power, not
                                   // a reported value — a scanner's RSSI genuinely
                                   // comes out weaker for a low-power pick, making
                                   // that decoy look farther away. -20..+4 dBm
                                   // spans what small real peripherals (earbuds,
                                   // trackers, sensors) typically use; the C5 can
                                   // go up to +20 dBm but that reads as unusually
                                   // strong for this class of device.
#define SPLINTER_TX_DRIFT_MAX           3  // dBm: max |Δpower| applied to a held
                                            // identity's real radio power on each
                                            // turn, relative to its own PREVIOUS
                                            // turn's power (a random walk, not a
                                            // fixed step) — simulates someone still
                                            // walking nearer/farther while remaining
                                            // "the same" decoy between mints.
#define SPLINTER_TX_DRIFT_REVERSE_PCT  15  // % chance per held turn the drift
                                            // direction flips instead of continuing
                                            // — strongly favors one direction per
                                            // identity (a "walk"), without ever
                                            // fully forbidding it from doubling back.

// 1 = flood/benchmark (max rate, reports devices/sec). 0 = paced decoy.
#define SPLINTER_BENCHMARK     1

// BLE advertising interval is expressed in 0.625 ms units.
#define ADV_ITVL_UNITS(ms)   ((uint16_t)(((ms) * 1000) / 625))

static volatile bool s_host_synced = false;

// Build a valid random-static address: 6 random bytes with the two most
// significant bits set (out[5] is the most-significant octet). Regenerates the
// astronomically rare all-zero / all-ones random part that NimBLE would reject.
static void make_random_static_addr(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) {
            out[i] = (uint8_t)(esp_random() & 0xff);
        }
        out[5] |= 0xc0;

        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) {
            ones += __builtin_popcount(out[i]);
        }
        if (ones != 0 && ones != 46) {
            return;
        }
    }
}

// Shared: populate a non-connectable decoy's advertising fields. `mfg` must point
// to a >=10 byte scratch buffer that outlives the field's use/serialization. The
// manufacturer body is uniform random — intentionally NOT a pop-up-triggering
// Continuity / Swift Pair / Fast Pair shape.
static void build_decoy_fields(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    const vendor_t *v = &VENDORS[esp_random() % VENDOR_COUNT];

    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    bool used_name = false;
    if (v->name != NULL && (esp_random() % 100) < SPLINTER_NAME_PROB) {
        f->name = (uint8_t *)v->name;
        f->name_len = strlen(v->name);
        f->name_is_complete = 1;
        used_name = true;
    }

    if ((esp_random() % 100) < SPLINTER_MFG_PROB) {
        size_t body = used_name ? 3 : (3 + (esp_random() % 5));
        mfg[0] = (uint8_t)(v->company_id & 0xff);
        mfg[1] = (uint8_t)((v->company_id >> 8) & 0xff);
        for (size_t i = 0; i < body; i++) {
            mfg[2 + i] = (uint8_t)(esp_random() & 0xff);
        }
        f->mfg_data = mfg;
        f->mfg_data_len = 2 + body;
    }

    // Self-reported TX Power Level: a fixed, product-calibrated value (never
    // the real randomized radio power — see decoy_vendors.h for why). A row
    // set to -128 omits the field entirely for that row.
    if (v->typical_tx_dbm != -128) {
        f->tx_pwr_lvl = v->typical_tx_dbm;
        f->tx_pwr_lvl_is_present = 1;
    }
}

#ifdef CONFIG_BT_NIMBLE_EXT_ADV
// =========================================================================
// BLE 5 path: several CONCURRENT extended-advertising instances (e.g. C5/C3)
// =========================================================================
#ifndef CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES
#define CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 1
#endif
#define SPLINTER_INSTANCES   CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES

// Per-instance "how many more turns does the current identity get before it's
// retired and replaced" countdown. The identity itself (MAC + payload) isn't
// stored here — once handed to the controller it keeps transmitting on its
// own every advertising interval, so a 1-byte counter per instance is all we
// need. C zero-initializes static arrays, so every instance starts at 0 and
// is due for its first identity as soon as splinter_run() begins.
static uint8_t s_repeats_left[SPLINTER_INSTANCES];

// Per-instance drift state, plus a cache of the exact address/payload bytes
// each instance is currently using. The cache exists so drift_instance() can
// resend an identity's bytes byte-for-byte while only changing tx_power,
// without relying on an unconfirmed assumption that reconfiguring an
// instance's params alone leaves its previously-set address/data untouched.
static int8_t  s_power[SPLINTER_INSTANCES];      // current real radio power (dBm)
static int8_t  s_drift_dir[SPLINTER_INSTANCES];  // +1 or -1: current drift bias
static uint8_t s_addr[SPLINTER_INSTANCES][6];
static uint8_t s_payload[SPLINTER_INSTANCES][BLE_HS_ADV_MAX_SZ];
static uint8_t s_payload_len[SPLINTER_INSTANCES];

// Configure one instance as a legacy-PDU, non-connectable advertiser so it is
// visible to ordinary (legacy) scanners. Done once per instance at startup.
static int configure_instance(uint8_t instance, int8_t tx_power)
{
    struct ble_gap_ext_adv_params p;
    memset(&p, 0, sizeof(p));
    p.legacy_pdu    = 1;                 // legacy PDUs -> seen by all scanners
    p.connectable   = 0;
    p.scannable     = 0;                 // NONCONN legacy = legacy_pdu only
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy   = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid           = instance;
    p.itvl_min      = ADV_ITVL_UNITS(SPLINTER_ADV_MS);
    p.itvl_max      = ADV_ITVL_UNITS(SPLINTER_ADV_MS + 30);
    p.tx_power      = tx_power;
    return ble_gap_ext_adv_configure(instance, &p, NULL, NULL, NULL);
}

// Give one instance a fresh identity (MAC + payload) and (re)start it. The other
// instances keep advertising throughout, so ~SPLINTER_INSTANCES are always live.
static int refresh_instance(uint8_t instance)
{
    int rc;
    static int diag_budget = 8;   // only print details for the first 8 failures,
                                   // so we don't flood the UART at 50 calls/sec

    ble_gap_ext_adv_stop(instance);      // ok if it wasn't running

    int8_t power = (int8_t)(SPLINTER_TX_POWER_MIN +
        (esp_random() % (SPLINTER_TX_POWER_MAX - SPLINTER_TX_POWER_MIN + 1)));
    rc = configure_instance(instance, power);
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "configure inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    uint8_t raw[6];
    make_random_static_addr(raw);
    ble_addr_t addr;
    addr.type = BLE_ADDR_RANDOM;
    memcpy(addr.val, raw, sizeof(raw));
    rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "set_addr inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    struct ble_hs_adv_fields fields;
    uint8_t mfg[10];
    build_decoy_fields(&fields, mfg);

    uint8_t buf[BLE_HS_ADV_MAX_SZ];
    uint8_t buf_len = 0;
    rc = ble_hs_adv_set_fields(&fields, buf, &buf_len, sizeof(buf));
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "adv_set_fields inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, buf_len);
    if (om == NULL) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "mbuf_from_flat inst=%u ENOMEM", instance); diag_budget--; }
        return BLE_HS_ENOMEM;
    }
    rc = ble_gap_ext_adv_set_data(instance, om);   // consumes om
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "ext_adv_set_data inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    rc = ble_gap_ext_adv_start(instance, 0, 0);  // duration 0 = forever
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "ext_adv_start inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    // Cache this identity so later drift turns can resend the identical
    // address+payload while only changing tx_power (see drift_instance()).
    memcpy(s_addr[instance], raw, sizeof(raw));
    memcpy(s_payload[instance], buf, buf_len);
    s_payload_len[instance] = buf_len;
    s_power[instance] = power;
    return 0;
}

// Nudge one instance's real transmit power by a small random step, biased to
// continue the direction it was already drifting (but not locked to it) —
// simulates someone continuing to walk nearer/farther while still reading as
// "the same" decoy. Resends the cached address+payload byte-for-byte; only
// tx_power actually changes.
static int drift_instance(uint8_t instance)
{
    int rc;
    static int diag_budget = 8;

    if ((esp_random() % 100) < SPLINTER_TX_DRIFT_REVERSE_PCT) {
        s_drift_dir[instance] = (int8_t)(-s_drift_dir[instance]);
    }
    int8_t step = (int8_t)(1 + (esp_random() % SPLINTER_TX_DRIFT_MAX));
    int16_t next = (int16_t)s_power[instance] + (int16_t)(step * s_drift_dir[instance]);
    if (next <= SPLINTER_TX_POWER_MIN) {
        next = SPLINTER_TX_POWER_MIN;
        s_drift_dir[instance] = 1;    // bounce back inward
    } else if (next >= SPLINTER_TX_POWER_MAX) {
        next = SPLINTER_TX_POWER_MAX;
        s_drift_dir[instance] = -1;   // bounce back inward
    }
    s_power[instance] = (int8_t)next;

    ble_gap_ext_adv_stop(instance);
    rc = configure_instance(instance, s_power[instance]);
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "drift configure inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    ble_addr_t addr;
    addr.type = BLE_ADDR_RANDOM;
    memcpy(addr.val, s_addr[instance], sizeof(addr.val));
    rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "drift set_addr inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_payload[instance], s_payload_len[instance]);
    if (om == NULL) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "drift mbuf_from_flat inst=%u ENOMEM", instance); diag_budget--; }
        return BLE_HS_ENOMEM;
    }
    rc = ble_gap_ext_adv_set_data(instance, om);   // consumes om
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "drift set_data inst=%u rc=%d", instance, rc); diag_budget--; }
        return rc;
    }

    rc = ble_gap_ext_adv_start(instance, 0, 0);
    if (rc != 0) {
        if (diag_budget > 0) { ESP_LOGE(TAG, "drift start inst=%u rc=%d", instance, rc); diag_budget--; }
    }
    return rc;
}

static void splinter_run(void)
{
    for (uint8_t i = 0; i < SPLINTER_INSTANCES; i++) {
        int rc = configure_instance(i, 127);
        if (rc != 0) {
            ESP_LOGW(TAG, "configure instance %u failed rc=%d", i, rc);
        }
    }
    ESP_LOGW(TAG, "BLE5 flood: %d concurrent ext-adv instances, round-robin refresh",
             SPLINTER_INSTANCES);

    uint8_t inst = 0;
    uint32_t t0 = esp_log_timestamp(), minted = 0, held = 0, fail = 0;
    for (;;) {
        if (s_repeats_left[inst] == 0) {
            // This instance's identity (if any) has served its repeats — mint
            // a fresh one, same as the old unconditional-refresh behaviour.
            if (refresh_instance(inst) == 0) {
                minted++;
                uint8_t span = SPLINTER_REPEAT_MIN +
                    (uint8_t)(esp_random() % (SPLINTER_REPEAT_MAX - SPLINTER_REPEAT_MIN + 1));
                s_repeats_left[inst] = span - 1;  // this turn was appearance #1
                s_drift_dir[inst] = (esp_random() % 2) ? 1 : -1;
            } else {
                fail++;
                // leave s_repeats_left[inst] at 0 so we retry on its next turn
            }
        } else {
            // Same MAC/payload, but its real radio power takes one more step
            // of its random walk — see drift_instance().
            if (drift_instance(inst) != 0) {
                fail++;
            }
            s_repeats_left[inst]--;
            held++;
        }
        inst = (inst + 1) % SPLINTER_INSTANCES;

        uint32_t now = esp_log_timestamp();
        if (now - t0 >= 1000) {
            ESP_LOGW(TAG, "rate: %lu new/sec, %lu held/sec, %d live instances (fail=%lu)",
                     (unsigned long)minted, (unsigned long)held, SPLINTER_INSTANCES,
                     (unsigned long)fail);
            minted = 0;
            held = 0;
            fail = 0;
            t0 = now;
        }

        // Always yield: on the single-core C5 a tight loop starves the BLE host,
        // the USB-Serial-JTAG console, and the idle/watchdog tasks. The 4
        // instances keep advertising concurrently between turns regardless.
        vTaskDelay(pdMS_TO_TICKS(SPLINTER_EXT_REFRESH_MS));
    }
}

#else
// =========================================================================
// Classic ESP32 (BT 4.2) path: one legacy advertiser, rotated fast
// =========================================================================

// Quiet (no per-cycle logging) so it can be driven at the controller's maximum
// identity-switch rate without the UART becoming the bottleneck.
static int start_one_decoy(void)
{
    int rc;

    uint8_t addr[6];
    make_random_static_addr(addr);
    rc = ble_hs_id_set_rnd(addr);
    if (rc != 0) return rc;

    struct ble_hs_adv_fields fields;
    uint8_t mfg[10];
    build_decoy_fields(&fields, mfg);

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_NON;   // non-connectable: pure presence
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advp.itvl_min  = ADV_ITVL_UNITS(SPLINTER_ADV_MS);
    advp.itvl_max  = ADV_ITVL_UNITS(SPLINTER_ADV_MS + 30);

    return ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                             &advp, NULL, NULL);
}

static void splinter_run(void)
{
#if SPLINTER_BENCHMARK
    ESP_LOGW(TAG, "BENCHMARK/flood: rotating one legacy advertiser at max rate");
    uint32_t t0 = esp_log_timestamp(), ok = 0, fail = 0;
    for (;;) {
        ble_gap_adv_stop();
        if (start_one_decoy() == 0) {
            ok++;
        } else {
            fail++;
        }
        uint32_t now = esp_log_timestamp();
        if (now - t0 >= 1000) {
            ESP_LOGW(TAG, "rate: %lu devices/sec (fail=%lu)",
                     (unsigned long)ok, (unsigned long)fail);
            ok = 0;
            fail = 0;
            t0 = now;
        }
    }
#else
    ESP_LOGI(TAG, "splinter active — rotating a decoy every %d ms", SPLINTER_ROTATE_MS);
    for (;;) {
        ble_gap_adv_stop();
        if (start_one_decoy() != 0) {
            ESP_LOGW(TAG, "decoy cycle failed");
        }
        vTaskDelay(pdMS_TO_TICKS(SPLINTER_ROTATE_MS));
    }
#endif
}
#endif // CONFIG_BT_NIMBLE_EXT_ADV

static void splinter_task(void *arg)
{
    while (!s_host_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    splinter_run();
}

static void on_sync(void)
{
    s_host_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void on_reset(int reason)
{
    s_host_synced = false;
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // NimBLE logs every GAP procedure at INFO; at high rotation rates that UART
    // spam would dominate and throttle the loop. Keep only warnings+.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();

    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(splinter_task, "splinter", 4096, NULL, 5, NULL);
}
