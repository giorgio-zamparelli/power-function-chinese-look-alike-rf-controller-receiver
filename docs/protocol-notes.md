# ✅ ACTUAL DECODED PROTOCOL (this device — clone of LEGO 8879/8884)

Reverse-engineered 2026-05-23/24 by sniffing + live testing. This device is **NOT Mould King**
(the Mould King notes below were an early wrong hypothesis, kept for reference).

**Radio:** scrambled XN297, 1 Mbps. **RF channels (switch pos. 1): 5 and 68.**
**Address (4 bytes):** logical `55 05 44 34` (over-air after preamble `D7 F5 4E BF`).
**Packet:** `[4-byte addr][1-byte payload][2-byte CRC]`. CRC init `0xB5D2`, poly `0x1021`, scrambled
(goebish `xn297_crc_xorout`, index = total_len-3). No bind/pairing — fixed address per channel switch.

**Payload byte = LEGO "Combo Direct"-style command, low 2 bits:**
| payload | bits | action |
|---|---|---|
| `0x01` | `01` | clockwise / forward |
| `0x02` | `10` | counter-clockwise / reverse |
| `0x03` | `11` | brake / stop |
| `0x00` | `00` | float (coast) |

- **bit2 (`0x04`) set → command ignored.**
- **bit3 (`0x08`) and bit7 (`0x80`)** = alternate channel/output bits; `1/2/3` drive the connected
  output, but `9/10/11` and `129/130/131` produce the same cw/ccw/stop on it too.
- No proportional speed in this mode — just forward / reverse / brake (full speed).

To control: emulate the TX (XN297 on NRF24, see `arduino/lego_tx`), send `1`/`2`/`3` on ch 5 & 68
to addr `55 05 44 34`. Reliable at close range without an extra decoupling cap.

---

# Mould King (XN297) protocol notes — EARLY HYPOTHESIS, turned out WRONG

Decoded from `MouldKg_nrf24l01_reference.ino` (DIY-Multiprotocol-TX-Module, GPLv3).
Protocol id `PROTO_MOULDKG` = 90.

## Radio layer
- **Chip:** XN297 — **emulated on the NRF24L01+** via data scrambling + fixed preamble.
  (Plain `RF24` library calls are *not* enough; you need the XN297 emulation layer.)
- **Config:** `XN297_Configure(CRC enabled, SCRAMBLED, 1 Mbps)`.
- **Bind TX address:** ASCII `"KDH"` = `4B 44 48` (3 bytes).
- **Bind channels:** TX bind = **11**, RX bind = **76**.
- **Data channels:** 4 hopping channels derived from the receiver's ID (roughly 0x0C–0x3C).

## Bind handshake
1. TX transmits on ch 11, address `KDH`, 7-byte payload: `C0 <txid0> <txid1> <txid2> 00 00 00`.
2. Receiver (in bind mode) replies on ch 76 with: `5A <txid…> <rxid0> <rxid1> <rxid2>`.
   The last 3 bytes are the **receiver's ID** — used as the address + to compute hop channels afterwards.
3. Normal packets then go to address = RX ID, hopping channels:
   ```
   rf = 0x0C;
   if (packet_count & 0x04) { n++; rf += 0x20; }
   if (packet_count & 0x02) rf += 0x10 + (RX_id[n] >> 4);
   else                     rf += RX_id[n] & 0x0F;
   ```
   Captured example: RX reply `… 03 0D 8E` → hop channels `0F 1C 39 3C`.

> Practical upshot: we don't need the original remote. Put the receiver in **bind/pairing mode**
> (usually power-on + bind button) and bind our emulated TX to it.

## DIGITAL packet (5 bytes) — button-style control (matches "Output A/B/C/D")
Two packet types are alternated by `packet_count & 1`:

```
byte0 = 0x30 or 0x31   (header / packet type)
byte1..3 = address (TX id during bind, RX id in normal mode)
byte4 = value (button bits, see below)
```

`byte4` button bits:
| Output | Channel | Forward | Reverse | In packet |
|--------|---------|---------|---------|-----------|
| A      | CH1     | `0x01`  | `0x02`  | `0x30` (base value `0x60`) |
| D      | CH4     | `0x04`  | `0x08`  | `0x30` |
| B      | CH2     | `0x40`  | `0x80`  | `0x31` |
| C      | CH3     | `0x10`  | `0x20`  | `0x31` |

So **Output A forward** = `0x30` packet with `byte4 = 0x60 | 0x01 = 0x61`;
**Output A reverse** = `0x60 | 0x02 = 0x62`; **stop** = `0x60`.

## ANALOG packet (10 bytes) — proportional speed control
```
byte0 = 0x36
byte1..3 = address
byte4..9 = 6 channel values, 0x80 = centre/stop, channel order index = {1,0,2,3,5,4}
```
Use this if you want variable speed (`-100%..+100%`) rather than on/off buttons.
`convert_channel_8b()` maps stick position to a byte; `0x80` is neutral.

## "Channel 1 / Output A" mapping — confirm empirically
- "Output A" → CH1 → the `0x30` packet, bit `0x01`/`0x02` on `byte4`.
- "Channel 1" most likely = receiver/box number 1. The protocol addresses up to 4 boxes via
  `num_ch`/`RX_num` and separate bound RX IDs. With a single receiver bound, there's one box.
- During Stage 1 bring-up, try each output and watch which physical motor port reacts to lock the map.

## Sub-protocols
- `MOULDKG_ANALOG4` / `MOULDKG_ANALOG6` (4 or 6 analog channels), header `0x36`.
- `MOULDKG_DIGIT` (digital buttons), headers `0x30`/`0x31`.
- v4.0 = 4-channel boxes, v6.0 = 6-channel boxes.
