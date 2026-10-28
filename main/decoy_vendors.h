// decoy_vendors.h — vendor palette for splinter decoys.
//
// Each entry pairs a Bluetooth SIG "Company Identifier" with a short, plausible
// product-style name, and a "typical" TX Power Level (dBm) for that specific
// product/hardware class. The company ID is what a scanner reads from the
// manufacturer-specific data field to attribute a device to a vendor — this is
// the legitimate, spec-defined vendor signal, and it is NOT what triggers
// pairing pop-ups.
//
// typical_tx_dbm is a FIXED, hardware-calibrated value — like a real product's
// radio, it never varies between sightings of "the same model." It is
// deliberately independent of the random per-identity radio power splinter
// actually transmits at (see SPLINTER_TX_POWER_MIN/MAX in splinter_main.c),
// which is what drives the real RSSI a scanner measures. Real devices work the
// same way: a fixed self-reported calibration value, paired with RSSI that
// genuinely varies with distance. Picking a value per *row* rather than per
// company lets different product classes from the same vendor look right —
// e.g. an earbud/speaker (stronger, prioritizes link reliability) vs. a tiny
// sensor/ring/strap (weaker, prioritizes battery life).
//
// Deliberately EXCLUDED from the default palette:
//   * Apple      (0x004C) — its "Continuity" payload format triggers AirPods/
//                           AirTag/"Find My" pop-ups on nearby iPhones.
//   * Microsoft  (0x0006) — "Swift Pair" beacons trigger Windows pop-ups.
//   * Google Fast Pair (service data 0xFE2C) — triggers Android pairing sheets.
// A privacy decoy needs realistic *presence*, not pop-up prompts aimed at
// bystanders, so those formats are simply never emitted. See splinter_main.c.
//
// Extend this list from the official assigned-numbers registry if you want a
// denser, more varied crowd:
//   https://www.bluetooth.com/specifications/assigned-numbers/

#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t    company_id;      // Bluetooth SIG company identifier (little-endian on air)
    const char *name;            // <= 12 chars to stay within the 31-byte adv budget; NULL = no name
    int8_t      typical_tx_dbm;  // fixed "calibrated" TX Power Level this product class
                                  // self-reports (-127..+20 dBm per the BLE spec)
} vendor_t;

static const vendor_t VENDORS[] = {
    { 0x0075, "Galaxy Buds", -4  },  // Samsung Electronics (earbuds)
    { 0x00E0, "Pixel Buds",  -6  },  // Google (earbuds)
    { 0x009E, "Bose QC",     -2  },  // Bose Corporation (ANC earbuds)
    { 0x0087, "Garmin",      -8  },  // Garmin International (GPS wearable)
    { 0x012D, "WF-1000XM5",  -4  },  // Sony Corporation (earbuds)
    { 0x0059, NULL,         -20  },  // Nordic Semiconductor (nameless sensor)
    { 0x0075, NULL,         -16  },  // Samsung (nameless wearable)
    { 0x0087, "vivosmart",  -12  },  // Garmin (fitness band)
    { 0x00E0, NULL,         -18  },  // Google (nameless)
    { 0x012D, NULL,         -14  },  // Sony (nameless)
    { 0x0055, "Poly Voyager", 0  },  // Plantronics (Poly) office headset
    { 0x0057, "JBL Flip",     4 },  // Harman International (JBL) speaker
    { 0x006B, "Polar H10",  -16 },  // Polar Electro OY (HR chest strap)
    { 0x00CC, "Powerbeats",  -6 },  // Beats Electronics (earbuds)
    { 0x0171, "Echo Buds",   -4 },  // Amazon.com Services (earbuds)
    { 0x01DA, "MX Anywhere", -8 },  // Logitech International (mouse)
    { 0x027D, "FreeBuds",    -6 },  // Huawei Technologies (earbuds)
    { 0x038F, "Mi Band",    -14 },  // Xiaomi (fitness band)
    { 0x03FF, "ScanWatch",  -12 },  // Withings (hybrid watch)
    { 0x05A7, "Sonos Roam",   4 },  // Sonos (portable speaker)
    { 0x0055, NULL,         -18 },  // Plantronics (nameless sensor)
    { 0x038F, NULL,         -18 },  // Xiaomi (nameless)
    { 0x05A7, NULL,           0 },  // Sonos (nameless)
    { 0x003A, "Panasonic",   -8 },  // Panasonic Holdings Corporation (earbuds)
    { 0x01DD, "Philips",     -6 },  // Koninklijke Philips N.V. (earbuds)
    { 0x00C4, "LG Tone",     -4 },  // LG Electronics (neckband earbuds)
    { 0x0103, "B&O Beoplay",  2 },  // Bang & Olufsen A/S (premium headphones/speaker)
    { 0x02C5, "Lenovo",      -8 },  // Lenovo (Singapore) Pte Ltd. (mouse/peripheral)
    { 0x02B2, "Oura Ring",  -18 },  // Oura Health Oy (smart ring — tiny battery)
    { 0x0078, "Nike Adapt", -10 },  // Nike, Inc. (self-lacing shoe BLE module)
    { 0x0494, "Momentum",    -4 },  // Sennheiser electronic GmbH & Co. KG (headphones)
    { 0x0008, "Moto Buds",   -6 },  // Motorola (earbuds)
    { 0x003A, NULL,         -18 },  // Panasonic (nameless sensor)
    { 0x02C5, NULL,         -16 },  // Lenovo (nameless)
    { 0x0078, NULL,         -18 },  // Nike (nameless sensor)
};

#define VENDOR_COUNT (sizeof(VENDORS) / sizeof(VENDORS[0]))
