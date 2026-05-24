# Power Functions Chinese look-alike RF controller/receiver

Reverse-engineering and computer control of a **Chinese clone of the LEGO® Power Functions**
remote (clone of `8879`) + receiver (clone of `8884`) that — unlike the genuine IR LEGO PF —
communicates over **2.4 GHz RF**. The end result is a web app that drives the cloned receiver's
motor (forward / reverse / brake, with incremental speed) from a laptop, using an **Arduino Uno +
NRF24L01+PA+LNA** as the radio.

> ⚠️ Not affiliated with or endorsed by the LEGO Group. "LEGO" and "Power Functions" are
> trademarks of the LEGO Group. This is an independent interoperability / reverse-engineering
> project for hardware the author owns.

## What it does

- Sniffs the clone's 2.4 GHz traffic, descrambles it, and decodes the protocol.
- Emulates the controller so a computer can drive the receiver — no original remote needed.
- Serves a small web control panel (Forward / Reverse / Stop + incremental speed).

## The decoded protocol

The clone uses a **scrambled XN297** link (the NRF24L01+ can emulate XN297 in software):

| Property | Value |
|---|---|
| Modulation | XN297, scrambled, **1 Mbps** |
| RF channels (channel-switch position 1) | **5 and 68** |
| Address (fixed, 4 bytes) | logical `55 05 44 34` (over-air after preamble: `D7 F5 4E BF`) |
| Packet | `[4-byte addr][1-byte payload][2-byte CRC]` |
| CRC | 16-bit, poly `0x1021`, init `0xB5D2`, scrambled |
| Bind/pairing | none — fixed address selected by the 1–4 channel switch |

**Payload byte — one byte drives BOTH outputs at once.** The high nibble is Output A, the low
nibble is Output B, using the *same* scheme — so `A_command == B_command << 4` (i.e. ×16):

```
         +------+------+------+------+------+------+------+------+
 bit     |  7   |  6   |  5   |  4   |  3   |  2   |  1   |  0   |
         +------+------+------+------+------+------+------+------+
 use     | spdA | spdA | dirA | dirA | spdB | spdB | dirB | dirB |
         +------+------+------+------+------+------+------+------+
           \________ OUTPUT A ________/  \________ OUTPUT B ________/
            bits 4-7  (= Output B << 4)    bits 0-3

  direction = low 2 bits of a nibble       speed = high 2 bits of a nibble
    01 = CW  / forward                       = the wheel's quadrature encoder.
    10 = CCW / reverse                       A static value does nothing — speed
    11 = brake / stop                        steps only on a phase CHANGE, so you
    00 = float (coast)                       send a sequence (repeat to ramp):
                                               Output B faster:  0x05 -> 0x09
                                               Output A faster:  0x50 -> 0x90

  combine by OR-ing the two nibbles:
    0x11 = 0001 0001  =  A forward + B forward     0x10 =  A forward only (B off)
    0x21 = 0010 0001  =  A reverse + B forward     0x01 =  B forward only (A off)
    0x12 = 0001 0010  =  A forward + B reverse     0x33 =  brake BOTH
```

Perfect for a car: steering on one output, drive on the other — set each nibble, OR them, send one
byte, both act simultaneously. Full capture/decode work is in [`docs/protocol-notes.md`](docs/protocol-notes.md);
the **1–4 channel switch** (frequency + address per position) is documented in [`docs/channels.md`](docs/channels.md).

## Hardware

- **Arduino Uno R3** (the radio driver)
- **NRF24L01+PA+LNA** module on its AMS1117 adapter base (feed the adapter **5 V**)
- the clone controller + receiver + a PF motor

Wiring (adapter → Uno): `VCC→5V, GND→GND, CE→D9, CSN→D10, SCK→D13, MOSI→D11, MISO→D12` (IRQ unused).
For long range / robustness, add a 10–100 µF decoupling cap across the module's VCC/GND (works at
close range without it).

## Usage

1. Flash the transmitter:
   ```
   arduino-cli compile --fqbn arduino:avr:uno arduino/lego_tx
   arduino-cli upload  -p /dev/cu.usbmodemXXXX --fqbn arduino:avr:uno arduino/lego_tx
   ```
2. Run the control server (no dependencies, Python 3 stdlib):
   ```
   python3 webapp/control_server.py /dev/cu.usbmodemXXXX
   ```
3. Open **http://localhost:8080** — control panel; **/grid** — raw 0–255 payload explorer.

## Repo layout

- `arduino/lego_tx/` — the working transmitter (XN297 emulation + decoded protocol)
- `arduino/sniffer/` — XN297 promiscuous sniffer (preamble-match capture)
- `arduino/channel_scanner/` — 2.4 GHz band scanner (RPD)
- `arduino/mouldking_tx/` — early (wrong) Mould King hypothesis, kept for reference
- `webapp/control_server.py` — web control panel + serial bridge
- `docs/` — [`protocol-notes.md`](docs/protocol-notes.md), [`channels.md`](docs/channels.md), capture data, and upstream reference code

## Credits & license

- XN297-on-NRF24 emulation adapted from **[goebish/nrf24_multipro](https://github.com/goebish/nrf24_multipro)** (GPLv3)
- XN297 sniffing/decode technique from **[pascallanger/DIY-Multiprotocol-TX-Module](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module)** (GPLv3)

Because it incorporates GPLv3 code, this project is licensed under **GPL-3.0** — see [`LICENSE`](LICENSE).
