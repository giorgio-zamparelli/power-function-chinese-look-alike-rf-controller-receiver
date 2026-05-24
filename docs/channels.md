# Channels (the 1–4 switch)

Both the controller and the receiver have a **1–4 channel switch**; they only talk when set to
the **same** position. This lets several models run near each other without interfering.

## How a channel is distinguished — frequency *and* address

A channel is keyed on **two independent radio layers at once**:

### 1. Frequency (the "RF channel")
The 2.4 GHz band is divided into 1 MHz-wide channels: NRF24/XN297 channel `N` = **2400 + N MHz**.
The radio tunes its carrier to that frequency; two radios must share a frequency to hear each
other. This device sends each command on **two** frequencies (63 channels apart) for
diversity/redundancy — if WiFi is clobbering one, the other still gets through.

### 2. Address (a packet "name tag")
Even on the same frequency, the radio only reacts to packets whose leading **address bytes** match
its own. This is a hardware filter: the chip compares each incoming packet's address field and
silently drops anything that doesn't match. Each switch position uses a different address.

> **Analogy:** frequency = which *station* you tune to; address = the *name* a message is addressed
> to. Even on the same station you only react when you hear your name. These clones change **both**
> per switch position, so two models on different channels never interfere.

At the chip level this is just two registers: `RF_CH` (frequency) and `TX_ADDR` / `RX_ADDR`
(address) — exactly what `setChannel()` in `arduino/lego_tx` reprograms.

## The four channels

| Switch | RF channels | Frequencies | Address (logical) |
|:------:|:-----------:|:-----------:|:------------------|
| **1** | 5, 68  | 2405 & 2468 MHz | `55 05 44 34` |
| **2** | 7, 70  | 2407 & 2470 MHz | `55 07 46 34` |
| **3** | 9, 72  | 2409 & 2472 MHz | `55 09 48 34` |
| **4** | 11, 74 | 2411 & 2474 MHz | `55 0B 4A 34` |

Channels **1 and 2 are sniff-confirmed**; **3 and 4 are derived** from the pattern below —
switch the receiver to channel 3 or 4 (and match the web app) to verify.

## The pattern (it's beautifully regular)

The **address literally embeds the two RF channel numbers**:

```
   address (logical):   55   [ low RF ]   [ high RF ]   34
                        └fixed┘  byte 1      byte 2     └fixed┘

   per switch position N (1..4):
       low RF  = 3 + 2*N          ->  5, 7, 9, 11
       high RF = low RF + 63      ->  68, 70, 72, 74
       address = 0x55, low RF, high RF, 0x34
```

So each switch position simply bumps everything by **+2**. Only the frequency + address change —
the **command encoding is identical on every channel** (see
[`protocol-notes.md`](protocol-notes.md): direction + quadrature speed, both outputs in one byte).

## Using it

- **Web app:** the `1–4` selector on both pages (control + grid). Set it to match the receiver's
  physical switch. (The channel is Arduino-side state, shared by both pages, held until changed.)
- **Arduino serial:** send `c<n>\n` (e.g. `c2`) to switch; the transmitter reprograms its address
  + RF channels on the fly. Payload commands (`0`–`255`) then go to that channel.
