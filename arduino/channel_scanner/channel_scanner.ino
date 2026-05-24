/*
 * lego-rf — Stage 0 diagnostic: 2.4 GHz channel scanner
 *
 * Purpose: confirm the Chinese LEGO-PF receiver/remote is in the NRF24/XN297 family
 * (controllable by this module) vs. Bluetooth LE (not).
 *
 * A "poor man's spectrum analyzer": sweeps all 126 NRF24 channels, uses the Received
 * Power Detector (RPD) to sense carriers > -64 dBm, and prints a per-channel histogram.
 *
 * HOW TO USE
 *   1. Wire per README (mind the PA+LNA power note — use the AMS1117 base / external 3V3 + cap).
 *   2. Library: install "RF24" by TMRh20 (Library Manager).
 *   3. Open Serial Monitor @ 115200.
 *   4. First read the BASELINE with everything off (note channels with WiFi noise).
 *   5. Now press buttons / power-cycle the ORIGINAL remote + receiver next to the antenna.
 *      - Spikes appearing around ch 11 and 76 (bind), and ch ~12–60 (data) => Mould King/XN297
 *        family => GO. The original Mould King hops, so expect activity to move around.
 *      - No change vs. baseline anywhere => likely BLE (uses ch 37/38/39 ≈ NRF24 ch 2/26/80,
 *        but BLE packets are short/scrambled and often won't trip RPD reliably) => consider the
 *        Pi + Bluetooth path instead.
 *
 * Channel→frequency: f = 2400 + ch  (MHz).  ch 0 = 2400 MHz ... ch 125 = 2525 MHz.
 */

#include <SPI.h>
#include "RF24.h"

#define CE_PIN  9
#define CSN_PIN 10

RF24 radio(CE_PIN, CSN_PIN);

const uint8_t NUM_CHANNELS = 126;
const uint8_t REPS = 100;          // samples per channel per pass
uint8_t values[NUM_CHANNELS];

void setup() {
  Serial.begin(115200);
  while (!Serial) { /* wait for USB serial on some boards */ }

  if (!radio.begin()) {
    Serial.println(F("NRF24 not responding! Check wiring AND power (PA+LNA needs a stable 3.3V + cap)."));
    while (1) {}
  }
  radio.setAutoAck(false);
  radio.setDataRate(RF24_1MBPS);   // match XN297/Mould King data rate
  radio.stopListening();
  radio.startListening();
  radio.stopListening();

  // Header: channel index (read vertically as two hex digits, high then low)
  Serial.println(F("NRF24 channel scanner ready. f(MHz) = 2400 + channel."));
  Serial.print(F("\n        "));
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) Serial.print(i >> 4, HEX);
  Serial.print(F("\n        "));
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) Serial.print(i & 0x0F, HEX);
  Serial.println();
}

void loop() {
  memset(values, 0, sizeof(values));

  // Accumulate carrier hits across REPS passes
  for (uint8_t rep = 0; rep < REPS; rep++) {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      radio.setChannel(ch);
      radio.startListening();
      delayMicroseconds(130);        // let the receiver settle
      radio.stopListening();
      if (radio.testRPD()) {
        if (values[ch] < 0x0F) values[ch]++;   // clamp to one hex digit
      }
    }
  }

  // Print one histogram line (0-9,a-f per channel; '-' = silent)
  Serial.print(F("scan:   "));
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (values[ch] == 0) Serial.print('-');
    else                 Serial.print(values[ch], HEX);
  }
  Serial.println();
}
