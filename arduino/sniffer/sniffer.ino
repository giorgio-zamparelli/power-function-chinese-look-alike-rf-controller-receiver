/*
 * lego-rf — XN297 promiscuous sniffer
 *
 * Sets the NRF24 receive address to the fixed XN297 preamble (55 0F 71), so it
 * captures XN297 packets from ANY device regardless of that device's own address.
 * Sweeps RF channels and dumps every raw packet as  C<ch> <hex...>  for offline
 * descramble + CRC validation in Python. Technique from pascallanger XN297Dump (GPLv3).
 *
 * If we get repeating, structured packets while the dial moves -> it's XN297 and we
 * can read the address + payload. If we get nothing across all channels -> it's not
 * XN297 (try plain-nRF24 promiscuous next).
 *
 * Wiring (adapter -> Uno): VCC=5V GND=GND CE=D9 CSN=D10 SCK=D13 MOSI=D11 MISO=D12. Serial @115200.
 * Serial: a=scan 0..83 (default), or send a number to park on one channel.
 */

#include <SPI.h>

#define CE_PIN  9
#define CSN_PIN 10
#define MAXLEN  32

#define CMD_R_REGISTER 0x00
#define CMD_W_REGISTER 0x20
#define CMD_R_RX_PL    0x61
#define CMD_FLUSH_RX   0xE2
#define CMD_NOP        0xFF

#define R_CONFIG     0x00
#define R_EN_AA      0x01
#define R_EN_RXADDR  0x02
#define R_SETUP_AW   0x03
#define R_SETUP_RETR 0x04
#define R_RF_CH      0x05
#define R_RF_SETUP   0x06
#define R_STATUS     0x07
#define R_RX_ADDR_P0 0x0A
#define R_RX_PW_P0   0x11
#define R_DYNPD      0x1C
#define R_FEATURE    0x1D

#define CFG_PWR_UP  0x02
#define CFG_PRIM_RX 0x01
#define ST_RX_DR    0x40

static inline void csnLow(){ digitalWrite(CSN_PIN,LOW); }
static inline void csnHigh(){ digitalWrite(CSN_PIN,HIGH); }
static inline void ceLow(){ digitalWrite(CE_PIN,LOW); }
static inline void ceHigh(){ digitalWrite(CE_PIN,HIGH); }

void wreg(uint8_t r,uint8_t v){ csnLow(); SPI.transfer(CMD_W_REGISTER|(r&0x1F)); SPI.transfer(v); csnHigh(); }
uint8_t rreg(uint8_t r){ csnLow(); SPI.transfer(CMD_R_REGISTER|(r&0x1F)); uint8_t v=SPI.transfer(CMD_NOP); csnHigh(); return v; }
void wregM(uint8_t r,const uint8_t*d,uint8_t n){ csnLow(); SPI.transfer(CMD_W_REGISTER|(r&0x1F)); for(uint8_t i=0;i<n;i++) SPI.transfer(d[i]); csnHigh(); }
void rpl(uint8_t*d,uint8_t n){ csnLow(); SPI.transfer(CMD_R_RX_PL); for(uint8_t i=0;i<n;i++) d[i]=SPI.transfer(CMD_NOP); csnHigh(); }
void strobe(uint8_t c){ csnLow(); SPI.transfer(c); csnHigh(); }

uint8_t chMin=0, chMax=83;       // sweep range
uint16_t dwell=15;               // ms per channel
uint8_t buf[MAXLEN];

void radioInit(){
  pinMode(CE_PIN,OUTPUT); pinMode(CSN_PIN,OUTPUT);
  ceLow(); csnHigh();
  SPI.begin(); SPI.setBitOrder(MSBFIRST); SPI.setDataMode(SPI_MODE0); SPI.setClockDivider(SPI_CLOCK_DIV2);
  delay(100);
  wreg(R_CONFIG, CFG_PWR_UP|CFG_PRIM_RX);    // RX, HW-CRC off (capture raw)
  delay(5);
  wreg(R_EN_AA, 0x00);
  wreg(R_EN_RXADDR, 0x01);
  wreg(R_SETUP_RETR, 0x00);
  wreg(R_SETUP_AW, 0x01);                    // 3-byte address
  wreg(R_RF_SETUP, 0x06);                    // 1 Mbps, 0 dBm
  uint8_t pre[3] = {0x55,0x0F,0x71};         // XN297 preamble as RX address
  wregM(R_RX_ADDR_P0, pre, 3);
  wreg(R_RX_PW_P0, MAXLEN);
  wreg(R_STATUS, 0x70);
  wreg(R_DYNPD, 0x00);
  wreg(R_FEATURE, 0x00);
  strobe(CMD_FLUSH_RX);
}

void dumpIfAny(uint8_t ch){
  if (rreg(R_STATUS) & ST_RX_DR){
    rpl(buf, MAXLEN);
    wreg(R_STATUS, 0x70);
    Serial.print('C'); Serial.print(ch); Serial.print(' ');
    for (uint8_t i=0;i<MAXLEN;i++){ if(buf[i]<16) Serial.print('0'); Serial.print(buf[i],HEX); }
    Serial.println();
  }
}

void handleSerial(){
  static int num=-1;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='a'){ chMin=0; chMax=83; Serial.println(F("scan 0..83")); num=-1; }
    else if(c>='0'&&c<='9'){ if(num<0)num=0; num=num*10+(c-'0'); }
    else if(c=='\n'||c=='\r'){ if(num>=0&&num<=125){ chMin=chMax=num; Serial.print(F("park ch ")); Serial.println(num);} num=-1; }
  }
}

void setup(){
  Serial.begin(115200);
  radioInit();
  Serial.println(F("XN297 sniffer ready. Format: C<ch> <32 hex bytes>. ('a'=scan, number+enter=park)"));
}

void loop(){
  handleSerial();
  for(uint16_t ch=chMin; ch<=chMax; ch++){
    ceLow();
    wreg(R_RF_CH, ch);
    strobe(CMD_FLUSH_RX);
    wreg(R_STATUS, 0x70);
    ceHigh();
    uint32_t t=millis();
    while(millis()-t < dwell){
      dumpIfAny(ch);
    }
  }
}
