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
| **3** | 17, 65 | 2417 & 2465 MHz | `55 11 41 34` |
| **4** | 12, 75 | 2412 & 2475 MHz | `55 0C 4B 34` |

All four are **sniff-confirmed**.

## The pattern

The RF channels are **not** a simple arithmetic progression — they're **specific values per switch
position** (a fixed lookup table, presumably chosen for frequency separation). But one neat rule
holds on every channel: the **address literally embeds the two RF channel numbers**:

```
   address (logical):   55   [ low RF ]   [ high RF ]   34
                        └fixed┘  byte 1      byte 2     └fixed┘

   e.g. channel 3: low RF = 17 = 0x11, high RF = 65 = 0x41  ->  55 11 41 34
        channel 4: low RF = 12 = 0x0C, high RF = 75 = 0x4B  ->  55 0C 4B 34
```

> An earlier hypothesis assumed a tidy `+2` progression (which fit channels 1→2); hardware testing
> proved it wrong for 3 and 4, so the firmware uses the sniffed table above.

Only the frequency + address change between channels — the **command encoding is identical on every
channel** (see [`protocol-notes.md`](protocol-notes.md): direction + quadrature speed, both outputs
in one byte).

## Using it

- **Web app:** the `1–4` selector on both pages (control + grid). Set it to match the receiver's
  physical switch. (The channel is Arduino-side state, shared by both pages, held until changed.)
- **Arduino serial:** send `c<n>\n` (e.g. `c2`) to switch; the transmitter reprograms its address
  + RF channels on the fly. Payload commands (`0`–`255`) then go to that channel.
