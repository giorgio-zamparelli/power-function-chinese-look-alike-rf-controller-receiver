/*
 * lego-rf — transmitter for the cloned LEGO-PF RF receiver (reverse-engineered)
 *
 * Replays the controller's protocol, decoded from sniffing:
 *   scrambled XN297, 1 Mbps, RF channels 5 & 68 (channel-switch position 1)
 *   address (logical) = 55 05 44 34   (over-air after preamble: D7 F5 4E BF)
 *   packet = [4-byte addr][1-byte payload][2-byte CRC]
 *   payload: 0 = stop; small values = speed/direction (we map this empirically here)
 *
 * XN297 emulation from goebish/nrf24_multipro (GPLv3).
 * Wiring (adapter -> Uno): VCC=5V GND=GND CE=D9 CSN=D10 SCK=D13 MOSI=D11 MISO=D12. Serial @115200.
 *
 * Serial commands:
 *   <number>\n   set payload byte to that value (0-255), e.g. "5\n"
 *   + / -        payload +1 / -1
 *   s            stop (payload 0)
 *   .            print current payload
 *   t            toggle transmit on/off (default on)
 */

#include <SPI.h>

#define CE_PIN  9
#define CSN_PIN 10

#define CMD_R_REGISTER   0x00
#define CMD_W_REGISTER   0x20
#define CMD_W_TX_PAYLOAD 0xA0
#define CMD_FLUSH_TX     0xE1
#define CMD_NOP          0xFF

#define REG_CONFIG     0x00
#define REG_EN_AA      0x01
#define REG_EN_RXADDR  0x02
#define REG_SETUP_AW   0x03
#define REG_SETUP_RETR 0x04
#define REG_RF_CH      0x05
#define REG_RF_SETUP   0x06
#define REG_STATUS     0x07
#define REG_TX_ADDR    0x10
#define REG_DYNPD      0x1C
#define REG_FEATURE    0x1D
#define CFG_PWR_UP     0x02
#define ST_TX_DS       0x20

static inline void csnLow(){digitalWrite(CSN_PIN,LOW);}
static inline void csnHigh(){digitalWrite(CSN_PIN,HIGH);}
static inline void ceLow(){digitalWrite(CE_PIN,LOW);}
static inline void ceHigh(){digitalWrite(CE_PIN,HIGH);}

void wreg(uint8_t r,uint8_t v){csnLow();SPI.transfer(CMD_W_REGISTER|(r&0x1F));SPI.transfer(v);csnHigh();}
uint8_t rreg(uint8_t r){csnLow();SPI.transfer(CMD_R_REGISTER|(r&0x1F));uint8_t v=SPI.transfer(CMD_NOP);csnHigh();return v;}
void wregM(uint8_t r,const uint8_t*d,uint8_t n){csnLow();SPI.transfer(CMD_W_REGISTER|(r&0x1F));for(uint8_t i=0;i<n;i++)SPI.transfer(d[i]);csnHigh();}
uint8_t wpl(uint8_t*d,uint8_t n){csnLow();uint8_t s=SPI.transfer(CMD_W_TX_PAYLOAD);for(uint8_t i=0;i<n;i++)SPI.transfer(d[i]);csnHigh();return s;}
void strobe(uint8_t c){csnLow();SPI.transfer(c);csnHigh();}

// ---- XN297 (goebish, GPLv3) ----
static uint8_t xn297_addr_len, xn297_tx_addr[5], xn297_crc=0;
static const uint8_t xn297_scramble[]={0xe3,0xb1,0x4b,0xea,0x85,0xbc,0xe5,0x66,0x0d,0xae,0x8c,0x88,
0x12,0x69,0xee,0x1f,0xc7,0x62,0x97,0xd5,0x0b,0x79,0xca,0xcc,0x1b,0x5d,0x19,0x10,0x24,0xd3,0xdc,0x3f,0x8e,0xc5,0x2f};
static const uint16_t xn297_crc_xorout[]={0x0000,0x3448,0x9BA7,0x8BBB,0x85E1,0x3E8C,0x451E,0x18E6,0x6B24,
0xE7AB,0x3828,0x814B,0xD461,0xF494,0x2503,0x691D,0xFE8B,0x9BA7,0x8B17,0x2920,0x8B5F,0x61B1,0xD391,0x7401,0x2138,0x129F,0xB3A0,0x2988};
uint8_t bit_reverse(uint8_t b){uint8_t o=0;for(uint8_t i=0;i<8;i++){o=(o<<1)|(b&1);b>>=1;}return o;}
uint16_t crc16_update(uint16_t c,uint8_t a){c^=a<<8;for(uint8_t i=0;i<8;i++)c=(c&0x8000)?((c<<1)^0x1021):(c<<1);return c;}
void XN297_SetTXAddr(const uint8_t*addr,uint8_t len){
  if(len>5)len=5;if(len<3)len=3;
  uint8_t buf[]={0x55,0x0F,0x71,0x0C,0x00};
  xn297_addr_len=len;
  if(len<4)for(uint8_t i=0;i<4;i++)buf[i]=buf[i+1];
  wreg(REG_SETUP_AW,len-2);
  wregM(REG_TX_ADDR,buf,5);
  memcpy(xn297_tx_addr,addr,len);
}
uint8_t XN297_WritePayload(uint8_t*msg,uint8_t len){
  uint8_t buf[32],last=0;
  if(xn297_addr_len<4)buf[last++]=0x55;
  for(uint8_t i=0;i<xn297_addr_len;i++)buf[last++]=xn297_tx_addr[xn297_addr_len-i-1]^xn297_scramble[i];
  for(uint8_t i=0;i<len;i++)buf[last++]=bit_reverse(msg[i])^xn297_scramble[xn297_addr_len+i];
  if(xn297_crc){
    uint8_t off=xn297_addr_len<4?1:0; uint16_t crc=0xb5d2;
    for(uint8_t i=off;i<last;i++)crc=crc16_update(crc,buf[i]);
    crc^=xn297_crc_xorout[xn297_addr_len-3+len];
    buf[last++]=crc>>8; buf[last++]=crc&0xff;
  }
  return wpl(buf,last);
}

// ---- protocol params (reverse-engineered) ----
uint8_t ADDR[4] = {0x55,0x05,0x44,0x34};   // logical addr; bytes 1,2 = the two RF channels
uint8_t CHANS[2] = {5, 68};                 // RF channels for the current switch position
uint8_t curChannel = 1;
uint8_t payload = 0;
bool txOn = true;

// Channel switch 1-4: first RF = 3+2*n, second = first+63, address embeds both channel numbers.
void setChannel(uint8_t n){
  if(n<1 || n>4) return;
  uint8_t f = 3 + 2*n;          // 1->5, 2->7, 3->9, 4->11
  ADDR[1]=f;  ADDR[2]=f+63;     // e.g. ch1: 0x05/0x44, ch2: 0x07/0x46
  CHANS[0]=f; CHANS[1]=f+63;    // RF channels, e.g. 5&68, 7&70 ...
  curChannel=n;
  Serial.print(F("channel=")); Serial.print(n);
  Serial.print(F(" rf=")); Serial.print(CHANS[0]); Serial.print(','); Serial.println(CHANS[1]);
}

void radioInit(){
  pinMode(CE_PIN,OUTPUT);pinMode(CSN_PIN,OUTPUT);ceLow();csnHigh();
  SPI.begin();SPI.setBitOrder(MSBFIRST);SPI.setDataMode(SPI_MODE0);SPI.setClockDivider(SPI_CLOCK_DIV2);
  delay(100);
  wreg(REG_CONFIG,CFG_PWR_UP);delay(5);
  wreg(REG_EN_AA,0x00);
  wreg(REG_EN_RXADDR,0x01);
  wreg(REG_SETUP_RETR,0x00);
  wreg(REG_RF_SETUP,0x06);          // 1 Mbps, max power (full PA)
  wreg(REG_STATUS,0x70);
  wreg(REG_DYNPD,0x00);
  wreg(REG_FEATURE,0x00);
  strobe(CMD_FLUSH_TX);
  xn297_crc=1;
}

void sendOn(uint8_t ch){
  ceLow();
  wreg(REG_CONFIG,CFG_PWR_UP);
  wreg(REG_RF_CH,ch);
  strobe(CMD_FLUSH_TX);
  wreg(REG_STATUS,0x70);
  uint8_t msg=payload;
  XN297_SetTXAddr(ADDR,4);
  XN297_WritePayload(&msg,1);
  ceHigh();delayMicroseconds(15);ceLow();
  uint32_t t=micros();
  while(!(rreg(REG_STATUS)&ST_TX_DS)) if(micros()-t>3000)break;
  wreg(REG_STATUS,0x70);
}

void handleSerial(){
  static long num=-1; static bool chMode=false;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='c'||c=='C'){ chMode=true; num=0; }            // "c<n>\n" sets channel 1-4
    else if(c>='0'&&c<='9'){ if(num<0)num=0; num=num*10+(c-'0'); }
    else if(c=='\n'||c=='\r'){
      if(chMode){ setChannel(num); chMode=false; }
      else if(num>=0&&num<=255){ payload=num; Serial.print(F("payload="));Serial.println(payload); }
      num=-1; }
    else if(c=='+'){ payload++; Serial.print(F("payload="));Serial.println(payload); }
    else if(c=='-'){ payload--; Serial.print(F("payload="));Serial.println(payload); }
    else if(c=='s'){ payload=0; Serial.println(F("STOP (payload=0)")); }
    else if(c=='.'){ Serial.print(F("payload="));Serial.println(payload); }
    else if(c=='t'){ txOn=!txOn; Serial.print(F("txOn="));Serial.println(txOn); }
  }
}

uint32_t last=0;
uint8_t chIdx=0;
void setup(){
  Serial.begin(115200);
  radioInit();
  setChannel(1);
  Serial.println(F("LEGO-RF TX ready. Send number(0-255)=payload, 'c<1-4>'=channel, 's'=stop."));
}
void loop(){
  handleSerial();
  if(txOn && micros()-last>=15000){   // every 15 ms, both channels (max delivery)
    last=micros();
    for(uint8_t i=0;i<sizeof(CHANS);i++) sendOn(CHANS[i]);
  }
}
