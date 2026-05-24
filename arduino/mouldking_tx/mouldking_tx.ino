/*
 * lego-rf — standalone Mould King transmitter (XN297 emulated on NRF24L01+)
 *
 * Emulates a Mould King 2.4GHz transmitter: binds to a receiver that's in
 * pairing mode, then sends analog Power-Functions packets to drive its outputs.
 * This is BOTH the protocol-confirmation test and the eventual control path:
 * if the receiver replies to our bind on ch 76, it IS Mould King.
 *
 * Credits (GPLv3):
 *   XN297 emulation  — goebish/nrf24_multipro
 *   Mould King proto — pascallanger/DIY-Multiprotocol-TX-Module (MouldKg_nrf24l01.ino)
 *
 * Wiring (adapter -> Uno): VCC=5V GND=GND CE=D9 CSN=D10 SCK=D13 MOSI=D11 MISO=D12. Serial @115200.
 *
 * Serial commands (single chars):
 *   b        (re)start bind
 *   0..5     select output byte index to poke (default 0)
 *   f / r    selected output -> forward (0xC0) / reverse (0x40)
 *   F / R    selected output -> full fwd (0xFF) / full rev (0x00)
 *   x        selected output -> stop (0x80)
 *   a        ALL outputs stop (0x80)
 *   d        toggle: dump raw RX bytes during bind (debug)
 *   ?        print state
 */

#include <SPI.h>
#include <util/atomic.h>

#define CE_PIN  9
#define CSN_PIN 10

// ---------- nRF24 commands / registers ----------
#define CMD_R_REGISTER   0x00
#define CMD_W_REGISTER   0x20
#define CMD_R_RX_PAYLOAD 0x61
#define CMD_W_TX_PAYLOAD 0xA0
#define CMD_FLUSH_TX     0xE1
#define CMD_FLUSH_RX     0xE2
#define CMD_NOP          0xFF

#define REG_CONFIG       0x00
#define REG_EN_AA        0x01
#define REG_EN_RXADDR    0x02
#define REG_SETUP_RETR   0x04
#define REG_RF_CH        0x05
#define REG_RF_SETUP     0x06
#define REG_STATUS       0x07
#define REG_RX_PW_P0     0x11
#define REG_DYNPD        0x1C
#define REG_FEATURE      0x1D

// names used verbatim by the goebish XN297 code below
#define NRF24L01_00_CONFIG     0x00
#define NRF24L01_03_SETUP_AW   0x03
#define NRF24L01_0A_RX_ADDR_P0 0x0A
#define NRF24L01_10_TX_ADDR    0x10
#define NRF24L01_00_EN_CRC     3   // CONFIG bit positions
#define NRF24L01_00_CRCO       2

// CONFIG bits
#define CFG_PWR_UP  0x02
#define CFG_PRIM_RX 0x01

// STATUS bits
#define ST_RX_DR  0x40
#define ST_TX_DS  0x20

// ---------- low-level SPI ----------
static inline void csnLow()  { digitalWrite(CSN_PIN, LOW);  }
static inline void csnHigh() { digitalWrite(CSN_PIN, HIGH); }
static inline void ceLow()   { digitalWrite(CE_PIN,  LOW);  }
static inline void ceHigh()  { digitalWrite(CE_PIN,  HIGH); }

uint8_t NRF24L01_WriteReg(uint8_t reg, uint8_t val) {
  csnLow();
  uint8_t s = SPI.transfer(CMD_W_REGISTER | (reg & 0x1F));
  SPI.transfer(val);
  csnHigh();
  return s;
}
uint8_t NRF24L01_ReadReg(uint8_t reg) {
  csnLow();
  SPI.transfer(CMD_R_REGISTER | (reg & 0x1F));
  uint8_t v = SPI.transfer(CMD_NOP);
  csnHigh();
  return v;
}
void NRF24L01_WriteRegisterMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  csnLow();
  SPI.transfer(CMD_W_REGISTER | (reg & 0x1F));
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  csnHigh();
}
uint8_t NRF24L01_WritePayload(uint8_t* data, uint8_t len) {
  csnLow();
  uint8_t s = SPI.transfer(CMD_W_TX_PAYLOAD);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  csnHigh();
  return s;
}
uint8_t NRF24L01_ReadPayload(uint8_t* data, uint8_t len) {
  csnLow();
  uint8_t s = SPI.transfer(CMD_R_RX_PAYLOAD);
  for (uint8_t i = 0; i < len; i++) data[i] = SPI.transfer(CMD_NOP);
  csnHigh();
  return s;
}
void NRF24L01_Strobe(uint8_t cmd) { csnLow(); SPI.transfer(cmd); csnHigh(); }

// ==================== XN297 emulation (goebish/nrf24_multipro, GPLv3) ====================
static uint8_t xn297_addr_len;
static uint8_t xn297_tx_addr[5];
static uint8_t xn297_rx_addr[5];
static uint8_t xn297_crc = 0;

static const uint8_t xn297_scramble[] = {
  0xe3,0xb1,0x4b,0xea,0x85,0xbc,0xe5,0x66,0x0d,0xae,0x8c,0x88,
  0x12,0x69,0xee,0x1f,0xc7,0x62,0x97,0xd5,0x0b,0x79,0xca,0xcc,
  0x1b,0x5d,0x19,0x10,0x24,0xd3,0xdc,0x3f,0x8e,0xc5,0x2f };

static const uint16_t xn297_crc_xorout[] = {
  0x0000,0x3448,0x9BA7,0x8BBB,0x85E1,0x3E8C,0x451E,0x18E6,0x6B24,
  0xE7AB,0x3828,0x814B,0xD461,0xF494,0x2503,0x691D,0xFE8B,0x9BA7,
  0x8B17,0x2920,0x8B5F,0x61B1,0xD391,0x7401,0x2138,0x129F,0xB3A0,0x2988 };

uint8_t bit_reverse(uint8_t b_in) {
  uint8_t b_out = 0;
  for (uint8_t i = 0; i < 8; ++i) { b_out = (b_out << 1) | (b_in & 1); b_in >>= 1; }
  return b_out;
}
static const uint16_t polynomial = 0x1021;
static const uint16_t initial    = 0xb5d2;
uint16_t crc16_update(uint16_t crc, unsigned char a) {
  crc ^= a << 8;
  for (uint8_t i = 0; i < 8; ++i)
    crc = (crc & 0x8000) ? ((crc << 1) ^ polynomial) : (crc << 1);
  return crc;
}
void XN297_SetTXAddr(const uint8_t* addr, uint8_t len) {
  if (len > 5) len = 5; if (len < 3) len = 3;
  uint8_t buf[] = { 0x55, 0x0F, 0x71, 0x0C, 0x00 };
  xn297_addr_len = len;
  if (xn297_addr_len < 4)
    for (uint8_t i = 0; i < 4; ++i) buf[i] = buf[i+1];
  NRF24L01_WriteReg(NRF24L01_03_SETUP_AW, len - 2);
  NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, buf, 5);
  memcpy(xn297_tx_addr, addr, len);
}
void XN297_SetRXAddr(const uint8_t* addr, uint8_t len) {
  if (len > 5) len = 5; if (len < 3) len = 3;
  uint8_t buf[] = { 0,0,0,0,0 };
  memcpy(buf, addr, len);
  memcpy(xn297_rx_addr, addr, len);
  for (uint8_t i = 0; i < xn297_addr_len; ++i)
    buf[i] = xn297_rx_addr[i] ^ xn297_scramble[xn297_addr_len - i - 1];
  NRF24L01_WriteReg(NRF24L01_03_SETUP_AW, len - 2);
  NRF24L01_WriteRegisterMulti(NRF24L01_0A_RX_ADDR_P0, buf, 5);
}
uint8_t XN297_WritePayload(uint8_t* msg, uint8_t len) {
  uint8_t buf[32];
  uint8_t last = 0;
  if (xn297_addr_len < 4) buf[last++] = 0x55;
  for (uint8_t i = 0; i < xn297_addr_len; ++i)
    buf[last++] = xn297_tx_addr[xn297_addr_len - i - 1] ^ xn297_scramble[i];
  for (uint8_t i = 0; i < len; ++i)
    buf[last++] = bit_reverse(msg[i]) ^ xn297_scramble[xn297_addr_len + i];
  if (xn297_crc) {
    uint8_t offset = xn297_addr_len < 4 ? 1 : 0;
    uint16_t crc = initial;
    for (uint8_t i = offset; i < last; ++i) crc = crc16_update(crc, buf[i]);
    crc ^= xn297_crc_xorout[xn297_addr_len - 3 + len];
    buf[last++] = crc >> 8;
    buf[last++] = crc & 0xff;
  }
  return NRF24L01_WritePayload(buf, last);
}
uint8_t XN297_ReadPayload(uint8_t* msg, uint8_t len) {
  uint8_t res = NRF24L01_ReadPayload(msg, len);
  for (uint8_t i = 0; i < len; i++)
    msg[i] = bit_reverse(msg[i]) ^ bit_reverse(xn297_scramble[i + xn297_addr_len]);
  return res;
}
// ========================================================================================

// ---------- Mould King protocol ----------
#define MK_TX_BIND_CH   11
#define MK_RX_BIND_CH   76
#define MK_BIND_LEN     7
#define MK_ANALOG_LEN   10

uint8_t txid[3] = { 0x57, 0x1B, 0xF8 };   // our (transmitter) id; receiver echoes it during bind
uint8_t rxid[3] = { 0, 0, 0 };            // receiver id, learned during bind
bool    bound   = false;
uint8_t packet_count = 0;

uint8_t outputs[6] = { 0x80,0x80,0x80,0x80,0x80,0x80 }; // 0x80 = stop
uint8_t sel = 0;          // selected output index for serial poking
bool    dumpRx = false;

uint8_t packet[32];

void nrfInit() {
  pinMode(CE_PIN, OUTPUT); pinMode(CSN_PIN, OUTPUT);
  ceLow(); csnHigh();
  SPI.begin();
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  SPI.setClockDivider(SPI_CLOCK_DIV2);   // 8 MHz
  delay(100);
  NRF24L01_WriteReg(REG_CONFIG, CFG_PWR_UP);   // power up, TX, HW-CRC off (we emulate it)
  delay(5);
  NRF24L01_WriteReg(REG_EN_AA, 0x00);          // no auto-ack
  NRF24L01_WriteReg(REG_EN_RXADDR, 0x01);      // pipe 0 only
  NRF24L01_WriteReg(REG_SETUP_RETR, 0x00);     // no auto-retransmit
  NRF24L01_WriteReg(REG_RF_SETUP, 0x06);       // 1 Mbps, 0 dBm
  NRF24L01_WriteReg(REG_STATUS, 0x70);         // clear IRQ flags
  NRF24L01_WriteReg(REG_DYNPD, 0x00);
  NRF24L01_WriteReg(REG_FEATURE, 0x00);
  NRF24L01_Strobe(CMD_FLUSH_TX);
  NRF24L01_Strobe(CMD_FLUSH_RX);
  xn297_crc = 1;                               // Mould King uses CRC
}

void setChannel(uint8_t ch) { NRF24L01_WriteReg(REG_RF_CH, ch); }

void txOne(uint8_t ch, uint8_t* msg, uint8_t len) {
  ceLow();
  NRF24L01_WriteReg(REG_CONFIG, CFG_PWR_UP);   // TX mode
  setChannel(ch);
  NRF24L01_Strobe(CMD_FLUSH_TX);
  NRF24L01_WriteReg(REG_STATUS, 0x70);
  XN297_WritePayload(msg, len);
  ceHigh(); delayMicroseconds(15); ceLow();    // fire
  uint32_t t = micros();
  while (!(NRF24L01_ReadReg(REG_STATUS) & ST_TX_DS))
    if (micros() - t > 3000) break;
  NRF24L01_WriteReg(REG_STATUS, 0x70);
}

// listen on ch for up to timeout_ms; on packet, descramble paylen bytes into out
bool rxOne(uint8_t ch, uint8_t* out, uint8_t paylen, uint16_t timeout_ms) {
  ceLow();
  NRF24L01_WriteReg(REG_RX_PW_P0, paylen);
  NRF24L01_Strobe(CMD_FLUSH_RX);
  NRF24L01_WriteReg(REG_STATUS, 0x70);
  setChannel(ch);
  NRF24L01_WriteReg(REG_CONFIG, CFG_PWR_UP | CFG_PRIM_RX);
  ceHigh();
  uint32_t t = millis();
  bool got = false;
  while (millis() - t < timeout_ms) {
    if (NRF24L01_ReadReg(REG_STATUS) & ST_RX_DR) {
      XN297_ReadPayload(out, paylen);
      NRF24L01_WriteReg(REG_STATUS, 0x70);
      got = true; break;
    }
  }
  ceLow();
  NRF24L01_WriteReg(REG_CONFIG, CFG_PWR_UP);   // back to TX
  return got;
}

// One bind cycle: announce on ch 11, then listen on ch 76 for the receiver's reply.
bool bindCycle() {
  // bind payload: C0 <txid0..2> 00 00 00, address "KDH"
  XN297_SetTXAddr((const uint8_t*)"KDH", 3);
  packet[0] = 0xC0;
  memcpy(&packet[1], txid, 3);
  packet[4] = packet[5] = packet[6] = 0x00;
  txOne(MK_TX_BIND_CH, packet, MK_BIND_LEN);

  // listen for reply on ch 76, same "KDH" address
  XN297_SetRXAddr((const uint8_t*)"KDH", 3);
  uint8_t in[MK_BIND_LEN];
  bool got = rxOne(MK_RX_BIND_CH, in, MK_BIND_LEN, 12);
  if (got) {
    if (dumpRx) {
      Serial.print(F("RX: "));
      for (uint8_t i = 0; i < MK_BIND_LEN; i++) { if (in[i]<16) Serial.print('0'); Serial.print(in[i],HEX); Serial.print(' '); }
      Serial.println();
    }
    // expected reply: 5A <txid match> <rxid0..2>
    if (memcmp(&in[1], txid, 3) == 0) {
      memcpy(rxid, &in[4], 3);
      return true;
    }
  }
  return false;
}

// compute hopping channel for box 0 from rxid + packet_count (Mould King formula)
uint8_t dataChannel() {
  uint8_t n = 0;
  uint8_t rf = 0x0C;
  if (packet_count & 0x04) { n = 1; rf += 0x20; }
  if (packet_count & 0x02) rf += 0x10 + (rxid[n] >> 4);
  else                     rf += rxid[n] & 0x0F;
  return rf;
}

void sendData() {
  XN297_SetTXAddr(rxid, 3);
  packet[0] = 0x36;                 // analog header
  memcpy(&packet[1], rxid, 3);
  for (uint8_t i = 0; i < 6; i++) packet[4 + i] = outputs[i];
  txOne(dataChannel(), packet, MK_ANALOG_LEN);
  packet_count++;
}

void printState() {
  Serial.print(F("bound=")); Serial.print(bound);
  Serial.print(F(" rxid=")); for (uint8_t i=0;i<3;i++){ if(rxid[i]<16)Serial.print('0'); Serial.print(rxid[i],HEX);}
  Serial.print(F(" sel=")); Serial.print(sel);
  Serial.print(F(" outputs="));
  for (uint8_t i=0;i<6;i++){ if(outputs[i]<16)Serial.print('0'); Serial.print(outputs[i],HEX); Serial.print(' '); }
  Serial.println();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'b': bound = false; packet_count = 0; Serial.println(F("rebinding...")); break;
      case '0': case '1': case '2': case '3': case '4': case '5': sel = c - '0'; break;
      case 'f': outputs[sel] = 0xC0; break;
      case 'r': outputs[sel] = 0x40; break;
      case 'F': outputs[sel] = 0xFF; break;
      case 'R': outputs[sel] = 0x00; break;
      case 'x': outputs[sel] = 0x80; break;
      case 'a': for (uint8_t i=0;i<6;i++) outputs[i] = 0x80; break;
      case 'd': dumpRx = !dumpRx; Serial.print(F("dumpRx=")); Serial.println(dumpRx); break;
      case '?': printState(); break;
      default: break;
    }
  }
}

uint32_t lastData = 0;
uint16_t bindTries = 0;

void setup() {
  Serial.begin(115200);
  nrfInit();
  Serial.println(F("Mould King TX ready. Put the RECEIVER in pairing mode, then send 'b'."));
  Serial.println(F("Status reg=0x")); Serial.println(NRF24L01_ReadReg(REG_STATUS), HEX);
}

void loop() {
  handleSerial();

  if (!bound) {
    if (bindCycle()) {
      bound = true; packet_count = 0;
      Serial.print(F("*** BOUND! rxid="));
      for (uint8_t i=0;i<3;i++){ if(rxid[i]<16)Serial.print('0'); Serial.print(rxid[i],HEX); }
      Serial.println(F(" — now driving. Try: 0 then f / r / x to find Output A."));
    } else {
      bindTries++;
      if ((bindTries % 50) == 0) { Serial.print(F("binding... tries=")); Serial.println(bindTries); }
    }
    return;
  }

  // bound: stream analog packets ~ every 5 ms
  if (micros() - lastData >= 5000) {
    lastData = micros();
    sendData();
  }
}
