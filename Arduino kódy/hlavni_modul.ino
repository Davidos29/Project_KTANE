#include <SPI.h>
#include <mcp_can.h>
#include <TM1637Display.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <U8g2lib.h>
#include <avr/pgmspace.h> 

// ==========================================
// LADĚNÍ (0 = Vypnuto, 1 = Zapnuto)
// ==========================================
#define DEBUG_MODE 0
#if DEBUG_MODE == 1
  #define D_PRINT(x) Serial.print(x)
  #define D_PRINTLN(x) Serial.println(x)
  #define D_PRINT_HEX(x, y) Serial.print(x, y)
#else
  #define D_PRINT(x)
  #define D_PRINTLN(x)
  #define D_PRINT_HEX(x, y)
#endif

// ==========================================
// DEFINICE PINŮ A HARDWARU
// ==========================================
#define SPI_CS_PIN 10
#define PIN_LED_ERR1 5 
#define PIN_LED_ERR2 6 
#define DISP_CLK 4
#define DISP_DIO 7
#define PIN_BUTTONS A6
#define PIN_BATTERY A7

const byte PINY_WIDGETU[] = { A0, A1, A2, A3, A4, A5 }; 
const byte POCET_PINU = 6;

// Inicializace instancí periferií
TM1637Display display(DISP_CLK, DISP_DIO);
U8G2_SSD1306_128X32_UNIVISION_1_SW_I2C u8g2(U8G2_R0, 3, 2, U8X8_PIN_NONE);
SoftwareSerial mySoftwareSerial(9, 8); 
DFRobotDFPlayerMini myDFPlayer;
MCP_CAN CAN(SPI_CS_PIN);

// ==========================================
// NASTAVENÍ ANALOGOVÝCH TLAČÍTEK
// ==========================================
#define BTN_NONE  0
#define BTN_RIGHT 1  
#define BTN_OK    2  
#define BTN_LEFT  4  

const int ciloveHodnoty[4] PROGMEM = { 0, 515, 702, 847 };
const byte stavyTlacitek[4] PROGMEM = { BTN_NONE, BTN_LEFT, BTN_OK, BTN_RIGHT };

// ==========================================
// DEFINICE WIDGETŮ A JEJICH ADC HODNOT
// ==========================================
enum TypWidgetu {
  W_NEZNAMY = -1, W_NIC = -2,
  W_BAT_D = 0, W_BAT_AA,
  W_SND, W_CLR, W_NSA, W_IND, W_SIG, W_CAR, W_MSA, W_TRN, W_BOB, W_FRK, W_FRQ,
  W_DVI, W_PAR, W_SER, W_PS2, W_RJ45, W_RCA,
  W_COMBO_A, W_COMBO_B, W_COMBO_C
};

struct WidgetInfo {
  int8_t typ;     
  int16_t adcCil;  
  bool maLED;      
};

const WidgetInfo ZNAMY_WIDGETY[] PROGMEM = {
  { (int8_t)W_BAT_D,    22,  false }, { (int8_t)W_SND,      33,  true  },      
  { (int8_t)W_CLR,      46,  true  }, { (int8_t)W_IND,      65,  true  },      
  { (int8_t)W_SIG,      93,  true  }, { (int8_t)W_NSA,     134,  true  },      
  { (int8_t)W_CAR,     184,  true  }, { (int8_t)W_MSA,     254,  true  },      
  { (int8_t)W_TRN,     291,  true  }, { (int8_t)W_FRK,     327,  true  },      
  { (int8_t)W_BOB,     375,  true  }, { (int8_t)W_FRQ,     414,  true  },      
  { (int8_t)W_DVI,     512,  false }, { (int8_t)W_PAR,     682,  false },      
  { (int8_t)W_SER,     844,  false }, { (int8_t)W_COMBO_A, 892,  false },          
  { (int8_t)W_COMBO_B, 930,  false }, { (int8_t)W_RJ45,    959,  false },    
  { (int8_t)W_BAT_AA,  979,  false }, { (int8_t)W_COMBO_C, 990,  false },
  { (int8_t)W_RCA,    1002,  false }, { (int8_t)W_PS2,    1013,  false }
};
const int POCET_TYPU = sizeof(ZNAMY_WIDGETY) / sizeof(WidgetInfo);

TypWidgetu nalezeneTypy[POCET_PINU]; 
bool herniStavIndikatoru[POCET_PINU]; 

// Globální detekce widgetů a periferií
int pocetBaterii = 0;
int aaCount = 0;
int dCount = 0;
int celkemWidgetu = 0;
int portCounts[6] = {0, 0, 0, 0, 0, 0}; 
uint16_t nalezeneIndMask = 0; 
uint16_t sviticiIndMask = 0;  

// ==========================================
// KOMUNIKAČNÍ PROTOKOL A STAVY HRY
// ==========================================
#define ID_GAME_STATE   0x001 
#define ID_BOMB_INFO    0x020 
#define ID_MODULE_START 0x100
#define ID_MODULE_END   0x1FF

enum StavHry { STAV_MENU = 0, STAV_HRA = 1, STAV_VYHRA = 2, STAV_PROHRA = 3, STAV_UKONCENO = 4, STAV_ARMING = 5 };
StavHry aktualniStav = STAV_MENU;

// Globální nastavení parametrů hry
long nastavCasSekundy = 300; 
int nastavMaxChyb = 3;
int nastavHlasitost = 4; 

int jasDisplejeIndex = 4; 
const byte jasMap[5] = {0, 1, 2, 3, 7}; 

// Běhové proměnné
char serialNumber[7]; 
bool snLiche = false;
bool snSamohlaska = false;
int snVowelsCount = 0;
int snDigitsCount = 0;

unsigned long zbyvaMs = 0;
unsigned long lastLoopMs = 0;
long naposledyZobrazeneSekundy = -1; 
int aktualniPocetChyb = 0;
int pocetVyresenychModulu = 0; 

int celkemModulu = 0; 
unsigned long registrovaneModuly[16]; 
unsigned long blokovatTikDo = 0; 

int strankaMenu = 0; 
unsigned long pressTimeLeft=0, pressTimeOk=0, pressTimeRight=0;
bool stateLeft=0, stateOk=0, stateRight=0;     
bool handledLeft=0, handledOk=0, handledRight=0; 
const int LONG_PRESS_MS = 600;

bool oledUIPopupActive = false;
unsigned long oledUIPopupTimer = 0;

const uint8_t SEG_DONE[] = { SEG_B|SEG_C|SEG_D|SEG_E|SEG_G, SEG_C|SEG_D|SEG_E|SEG_G, SEG_C|SEG_E|SEG_G, SEG_A|SEG_D|SEG_E|SEG_F|SEG_G };
const uint8_t SEG_FAIL[] = { SEG_A|SEG_E|SEG_F|SEG_G, SEG_A|SEG_B|SEG_C|SEG_E|SEG_F|SEG_G, SEG_B|SEG_C, SEG_D|SEG_E|SEG_F };
const uint8_t SEG_ERR[]  = { SEG_A|SEG_D|SEG_E|SEG_F|SEG_G, SEG_E|SEG_G, SEG_E|SEG_G, 0 }; 
const uint8_t SEG_END[]  = { SEG_A|SEG_D|SEG_E|SEG_F|SEG_G, SEG_C|SEG_E|SEG_G, SEG_B|SEG_C|SEG_D|SEG_E|SEG_G, 0 };

int getButtonState(); int ctiTlacitko(bool, unsigned long&, bool&, bool&);
void vypisMenu(); void prepniNaArming(); void loopHra(int);
void konecHry(StavHry, const char*); void handleEndGame(int); void checkVyhra();
void upravHodnotu(int); void prehrajZvuk(int); void vykresliOled(); 
int zmerBaterii(); void odeslatZpravu(unsigned long, byte*, byte); 
void skenujPeriferie(); void nastavJedenIndikator(int, bool);
bool maTentoTypLED(TypWidgetu); int ziskejBitIndexIndikatoru(TypWidgetu);
void vypisNazevWidgetu(TypWidgetu); void registrujModul(unsigned long);
void zobrazCas(long sekundy);
void vykresliHodnotu(); 
void odeslatBroadcastInfo(long odesilanyCas);

// ==========================================
// INICIALIZACE SYSTÉMU
// ==========================================
void setup() {
  Serial.begin(115200);
  mySoftwareSerial.begin(9600); 
  
  pinMode(PIN_LED_ERR1, OUTPUT);
  pinMode(PIN_LED_ERR2, OUTPUT);
  digitalWrite(PIN_LED_ERR1, LOW);
  digitalWrite(PIN_LED_ERR2, LOW);

  u8g2.begin();
  u8g2.setContrast(255);
  
  D_PRINTLN(F("Init DFPlayer..."));
  delay(1000); 
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    D_PRINTLN(F("CHYBA: DFPlayer nenalezen!"));
  } else {
    D_PRINTLN(F("DFPlayer OK"));
    myDFPlayer.setTimeOut(500); 
    myDFPlayer.volume(nastavHlasitost * 6); 
    delay(500); 
  }

  display.setBrightness(jasMap[jasDisplejeIndex]); 
  zobrazCas(nastavCasSekundy);
  
  while (CAN_OK != CAN.begin(MCP_ANY, CAN_125KBPS, MCP_8MHZ)) {
    D_PRINTLN(F("Chyba CAN modulu...")); delay(100);
  }
  CAN.setMode(MCP_NORMAL);
  D_PRINTLN(F("--- SYSTEM START ---"));

  skenujPeriferie(); 

  celkemModulu = 0;
  for(int i=0; i<16; i++) registrovaneModuly[i] = 0;

  byte dataReset[1] = { 0 }; 
  for(int i=0; i<3; i++) { odeslatZpravu(ID_GAME_STATE, dataReset, 1); delay(20); }
  
  vypisMenu(); 
}

// ==========================================
// ZPRACOVÁNÍ ANALOGOVÝCH VSTUPŮ
// ==========================================
TypWidgetu identifikujWidget(int val) {
  if (val > 1015) return W_NIC; 
  int nejlepsiIndex = -1; int nejmensiRozdil = 9999;
  for (int i = 0; i < POCET_TYPU; i++) {
    int adcCil = pgm_read_word(&ZNAMY_WIDGETY[i].adcCil);
    int rozdil = abs(val - adcCil);
    if (rozdil < nejmensiRozdil) { nejmensiRozdil = rozdil; nejlepsiIndex = i; }
  }
  if (nejmensiRozdil > 12) return W_NEZNAMY;
  return (TypWidgetu)pgm_read_byte(&ZNAMY_WIDGETY[nejlepsiIndex].typ);
}

bool maTentoTypLED(TypWidgetu typ) {
  for (int i = 0; i < POCET_TYPU; i++) {
    if ((TypWidgetu)pgm_read_byte(&ZNAMY_WIDGETY[i].typ) == typ) {
       return pgm_read_byte(&ZNAMY_WIDGETY[i].maLED);
    }
  }
  return false;
}

void vypisNazevWidgetu(TypWidgetu typ) {
  switch(typ) {
    case W_BAT_D: D_PRINT(F("Bat D")); break;
    case W_BAT_AA: D_PRINT(F("Bat AA")); break;
    case W_SND: D_PRINT(F("IND SND")); break;
    case W_CLR: D_PRINT(F("IND CLR")); break;
    case W_NSA: D_PRINT(F("IND NSA")); break;
    case W_IND: D_PRINT(F("IND IND")); break;
    case W_SIG: D_PRINT(F("IND SIG")); break;
    case W_CAR: D_PRINT(F("IND CAR")); break;
    case W_MSA: D_PRINT(F("IND MSA")); break;
    case W_TRN: D_PRINT(F("IND TRN")); break;
    case W_BOB: D_PRINT(F("IND BOB")); break;
    case W_FRK: D_PRINT(F("IND FRK")); break;
    case W_FRQ: D_PRINT(F("IND FRQ")); break;
    case W_DVI: D_PRINT(F("Port DVI")); break;
    case W_PAR: D_PRINT(F("Port Paralel")); break;
    case W_SER: D_PRINT(F("Port Serial")); break;
    case W_PS2: D_PRINT(F("Port PS/2")); break;
    case W_RJ45: D_PRINT(F("Port RJ-45")); break;
    case W_RCA: D_PRINT(F("Port RCA")); break;
    case W_COMBO_A: D_PRINT(F("COMBO A (DVI+PS/2)")); break;
    case W_COMBO_B: D_PRINT(F("COMBO B (RJ-45+RCA)")); break;
    case W_COMBO_C: D_PRINT(F("COMBO C (RJ-45+Ser)")); break;
    case W_NIC: D_PRINT(F("Nic")); break;
    case W_NEZNAMY: D_PRINT(F("Neznamo")); break;
    default: D_PRINT(F("???")); break;
  }
}

int ziskejBitIndexIndikatoru(TypWidgetu typ) {
  switch(typ) {
    case W_SND: return 0; case W_CLR: return 1; case W_CAR: return 2;
    case W_IND: return 3; case W_FRQ: return 4; case W_SIG: return 5;
    case W_NSA: return 6; case W_MSA: return 7; case W_TRN: return 8;
    case W_BOB: return 9; case W_FRK: return 10; default: return -1;
  }
}

void nastavJedenIndikator(int pinIdx, bool svitit) {
  TypWidgetu typ = nalezeneTypy[pinIdx];
  int pin = PINY_WIDGETU[pinIdx];
  if (maTentoTypLED(typ)) {
    if (svitit) { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); } 
    else { pinMode(pin, INPUT); }
  } else { pinMode(pin, INPUT); }
}

void skenujPeriferie() {
  D_PRINTLN(F("\n=== DETEKCE PERIFERII ==="));
  pocetBaterii = 0; aaCount = 0; dCount = 0; celkemWidgetu = 0; nalezeneIndMask = 0;
  for(int i=0; i<6; i++) portCounts[i] = 0;

  for (int i = 0; i < POCET_PINU; i++) {
    int pin = PINY_WIDGETU[i];
    pinMode(pin, INPUT); delay(40);
    int val = analogRead(pin);
    TypWidgetu typ = identifikujWidget(val);
    nalezeneTypy[i] = typ;

    if (typ != W_NIC && typ != W_NEZNAMY) {
       celkemWidgetu++;
    }

    if (typ == W_BAT_AA) { aaCount++; pocetBaterii += 2; }
    if (typ == W_BAT_D) { dCount++; pocetBaterii += 1; }

    switch(typ) {
      case W_DVI:  portCounts[0]++; break; case W_PAR:  portCounts[1]++; break;
      case W_PS2:  portCounts[2]++; break; case W_RJ45: portCounts[3]++; break;
      case W_SER:  portCounts[4]++; break; case W_RCA:  portCounts[5]++; break;
      case W_COMBO_A: portCounts[0]++; portCounts[2]++; break; 
      case W_COMBO_B: portCounts[3]++; portCounts[5]++; break; 
      case W_COMBO_C: portCounts[3]++; portCounts[4]++; break; 
    }

    int bitIdx = ziskejBitIndexIndikatoru(typ);
    if (bitIdx != -1) nalezeneIndMask |= (1 << bitIdx);

    if (typ != W_NIC) {
        D_PRINT(F("Pin A")); D_PRINT(i);
        D_PRINT(F(" [ADC:")); D_PRINT(val); D_PRINT(F("] -> "));
        vypisNazevWidgetu(typ); D_PRINTLN("");
    }
  }
  D_PRINTLN(F("========================="));
}

// ==========================================
// FUNKCE KOMUNIKACE (CAN BUS)
// ==========================================
void odeslatZpravu(unsigned long id, byte* data, byte len) {
  if (CAN.sendMsgBuf(id, 0, len, data) != CAN_OK) { 
    D_PRINTLN(F("Chyba odesilani CAN"));
  }
}

void odeslatBroadcastInfo(long odesilanyCas) {
  byte buf[8] = {0};
  uint16_t t = (uint16_t)constrain(odesilanyCas, 0, 8191);
  
  buf[0] = t & 0xFF;
  buf[1] = (t >> 8) & 0x1F;
  buf[1] |= (min(snVowelsCount, 7) & 0x07) << 5;
  buf[2] = (min(snDigitsCount, 7) & 0x07) | ((aktualniStav & 0x07) << 3) | ((min(aaCount, 3) & 0x03) << 6);
  buf[3] = (min(dCount, 3) & 0x03) | ((min(portCounts[0], 3) & 0x03) << 2) | ((min(portCounts[1], 3) & 0x03) << 4) | ((min(portCounts[2], 3) & 0x03) << 6);
  buf[4] = (min(portCounts[3], 3) & 0x03) | ((min(portCounts[4], 3) & 0x03) << 2) | ((min(portCounts[5], 3) & 0x03) << 4) | ((!snLiche & 0x01) << 6) | ((snSamohlaska & 0x01) << 7);
  buf[5] = (nalezeneIndMask & 0xFF);
  buf[6] = ((nalezeneIndMask >> 8) & 0x07) | ((sviticiIndMask & 0x1F) << 3);
  buf[7] = (sviticiIndMask >> 5) & 0x3F;

  odeslatZpravu(ID_BOMB_INFO, buf, 8);
}

void registrujModul(unsigned long id) {
  for(int i=0; i < celkemModulu; i++) {
    if(registrovaneModuly[i] == id) return; 
  }
  if(celkemModulu < 16) {
    registrovaneModuly[celkemModulu] = id;
    celkemModulu++;
    D_PRINT(F("NOVY MODUL: 0x")); D_PRINT_HEX(id, HEX); D_PRINTLN("");
    vypisMenu(); 
  }
}

// ==========================================
// HLAVNÍ SMYČKA FSM
// ==========================================
void loop() {
  int rawButtons = getButtonState();
  bool isLeftPressed  = (rawButtons & BTN_LEFT);
  bool isOkPressed    = (rawButtons & BTN_OK);
  bool isRightPressed = (rawButtons & BTN_RIGHT);

  int btnL = ctiTlacitko(isLeftPressed,  pressTimeLeft,  stateLeft,  handledLeft);
  int btnO = ctiTlacitko(isOkPressed,    pressTimeOk,    stateOk,    handledOk);
  int btnR = ctiTlacitko(isRightPressed, pressTimeRight, stateRight, handledRight);

  switch (aktualniStav) {
    case STAV_MENU:
      if (oledUIPopupActive && (millis() - oledUIPopupTimer > 2000)) {
         oledUIPopupActive = false; vypisMenu();
      }
      
      static unsigned long lastMenuPing = 0;
      if (millis() - lastMenuPing > 1000) {
         lastMenuPing = millis();
         byte dataRst[1] = { 0 }; odeslatZpravu(ID_GAME_STATE, dataRst, 1);
      }
      
      if (CAN_MSGAVAIL == CAN.checkReceive()) {
        long unsigned int rxId; unsigned char len=0; unsigned char buf[8];
        CAN.readMsgBuf(&rxId, &len, buf);
        if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END && buf[0] == 0) registrujModul(rxId);
      }
      
      if (btnO == 2) { prepniNaArming(); } 
      else if (btnR == 2) { strankaMenu = (strankaMenu + 1) % 4; oledUIPopupActive = true; oledUIPopupTimer = millis(); vypisMenu(); } 
      else if (btnL == 2) { strankaMenu = (strankaMenu == 0) ? 3 : strankaMenu - 1; oledUIPopupActive = true; oledUIPopupTimer = millis(); vypisMenu(); }
      else if (btnL == 1) upravHodnotu(-1); 
      else if (btnR == 1) upravHodnotu(1);  
      break;

    case STAV_ARMING: break;
    case STAV_HRA: loopHra(btnO); break;
    case STAV_VYHRA:
    case STAV_PROHRA:
    case STAV_UKONCENO: handleEndGame(btnO); break;
  }
}

// ==========================================
// AUDIO A ZOBRAZENÍ
// ==========================================
void prehrajZvuk(int id) { 
  if (nastavHlasitost > 0) myDFPlayer.playMp3Folder(id); 
}

void vykresliOled() {
  u8g2.firstPage(); do {
    u8g2.setFont(u8g2_font_profont29_tr); u8g2.drawStr(10, 30, serialNumber);
  } while ( u8g2.nextPage() );
}

void zobrazCas(long sekundy) {
  if (sekundy >= 3600) {
    int h = sekundy / 3600;
    int m = (sekundy % 3600) / 60;
    uint8_t data[] = {
      display.encodeDigit(h),
      0b01110100, // Znak 'h'
      display.encodeDigit(m / 10),
      display.encodeDigit(m % 10)
    };
    display.setSegments(data);
  } else {
    int m = sekundy / 60;
    int s = sekundy % 60;
    display.showNumberDecEx(m * 100 + s, 0b01000000, true);
  }
}

int getButtonState() {
  int val = analogRead(PIN_BUTTONS);
  if (val < 50) return BTN_NONE; 
  int nej = BTN_NONE; int minRozdil = 1024;
  for (int i = 0; i < 4; i++) {
    int cil = pgm_read_word(&ciloveHodnoty[i]);
    int diff = abs(val - cil);
    if (diff < minRozdil) { minRozdil = diff; nej = pgm_read_byte(&stavyTlacitek[i]); }
  }
  return nej;
}

int ctiTlacitko(bool isPressed, unsigned long &timer, bool &lastState, bool &handled) {
  int result = 0;
  if (isPressed && !lastState) { timer = millis(); handled = false; } 
  else if (!isPressed && lastState) { 
    unsigned long duration = millis() - timer;
    if (duration > 50 && duration < LONG_PRESS_MS && !handled) result = 1; 
  }
  else if (isPressed && lastState) {
    if ((millis() - timer) > LONG_PRESS_MS && !handled) { result = 2; handled = true; }
  }
  lastState = isPressed;
  return result;
}

void vygenerujSeriovku() {
  const char pismena[] = "ABCDEFGHIJKLMNPQRSTUVWXZ"; 
  const char cisla[] = "0123456789";
  const char vsehno[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXZ"; 
  
  serialNumber[0] = vsehno[random(sizeof(vsehno) - 1)]; 
  serialNumber[1] = vsehno[random(sizeof(vsehno) - 1)];
  serialNumber[2] = cisla[random(sizeof(cisla) - 1)];
  serialNumber[3] = pismena[random(sizeof(pismena) - 1)]; 
  serialNumber[4] = pismena[random(sizeof(pismena) - 1)];
  int posledniCislo = random(sizeof(cisla) - 1);
  serialNumber[5] = cisla[posledniCislo]; 
  serialNumber[6] = '\0'; 
  
  snLiche = (posledniCislo % 2 != 0);
  snSamohlaska = false;
  snVowelsCount = 0;
  snDigitsCount = 0;

  for (int i = 0; i < 6; i++) {
    char z = serialNumber[i];
    if (z >= '0' && z <= '9') snDigitsCount++;
    else if (z == 'A' || z == 'E' || z == 'I' || z == 'O' || z == 'U') {
      snSamohlaska = true;
      snVowelsCount++;
    }
  }
  D_PRINT(F("SN: ")); D_PRINTLN(serialNumber);
}

// ==========================================
// START HRY (Arming & Hardware Validace)
// ==========================================
void prepniNaArming() {
  aktualniStav = STAV_ARMING;
  
  if (celkemModulu == 0) D_PRINTLN(F("VAROVANI: Zadne moduly!"));
  else { D_PRINT(F("HRA STARTUJE. Modulu: ")); D_PRINTLN(celkemModulu); }

  for(int i=0; i<POCET_PINU; i++) nastavJedenIndikator(i, false);
  digitalWrite(PIN_LED_ERR1, LOW); digitalWrite(PIN_LED_ERR2, LOW);
  
  randomSeed(millis());
  vygenerujSeriovku();

  sviticiIndMask = 0;
  for(int i=0; i<POCET_PINU; i++) {
    TypWidgetu typ = nalezeneTypy[i];
    if (maTentoTypLED(typ)) {
       bool sviti = random(0, 2);
       herniStavIndikatoru[i] = sviti;
       if (sviti) {
         int bitIdx = ziskejBitIndexIndikatoru(typ);
         if (bitIdx != -1) sviticiIndMask |= (1 << bitIdx);
       }
    } else { herniStavIndikatoru[i] = false; }
  }

  u8g2.firstPage(); do { } while(u8g2.nextPage());

  D_PRINTLN(F("Odesilam CAN CONFIG..."));
  for(int i=0; i<3; i++) { odeslatBroadcastInfo(nastavCasSekundy); delay(20); }

  display.clear(); display.setBrightness(jasMap[jasDisplejeIndex]);
  uint8_t seg[] = { 0, 0, 0, 0 };
  for(int i=0; i<4; i++) { seg[i] = SEG_G; display.setSegments(seg); delay(150); seg[i] = 0; }
  display.clear();

  D_PRINTLN(F("Odesilam zadost o validaci modulu (STAV 5)..."));
  byte dataPrep[1] = { 5 };
  odeslatZpravu(ID_GAME_STATE, dataPrep, 1);

  bool validaceSelhala = false;

  for(int i = 3; i > 0; i--) { 
    display.showNumberDec(i); 
    for(int d = 0; d < 100; d++) {
       if (CAN_MSGAVAIL == CAN.checkReceive()) {
          long unsigned int rxId; unsigned char len=0; unsigned char buf[8];
          CAN.readMsgBuf(&rxId, &len, buf);
          if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END && buf[0] == 3) {
             validaceSelhala = true; break; 
          }
       }
       delay(10);
    }
    if (validaceSelhala) break; 
  }

  if (validaceSelhala) {
     D_PRINTLN(F("START PRERUSEN: Modul nahlasil chybu!"));
     display.setSegments(SEG_ERR); prehrajZvuk(2); delay(2500); 
     aktualniStav = STAV_MENU; byte dataReset[1] = { 0 }; odeslatZpravu(ID_GAME_STATE, dataReset, 1); vypisMenu();
     return; 
  }

  display.clear();
  byte dataState[1] = { 1 }; 
  for(int i=0; i<3; i++) { odeslatZpravu(ID_GAME_STATE, dataState, 1); delay(20); }
  
  prehrajZvuk(1); delay(150); prehrajZvuk(1);
  vykresliOled(); 

  for(int i=0; i<POCET_PINU; i++) nastavJedenIndikator(i, herniStavIndikatoru[i]);

  aktualniPocetChyb = 0; pocetVyresenychModulu = 0;
  
  zbyvaMs = nastavCasSekundy * 1000UL;
  lastLoopMs = millis();
  
  naposledyZobrazeneSekundy = -1; aktualniStav = STAV_HRA;
  blokovatTikDo = 0;
}

// ==========================================
// BĚH HRY A HODNOCENÍ VÝSLEDKŮ
// ==========================================
void loopHra(int btnOkState) {
  unsigned long currentMs = millis();
  unsigned long deltaMs = currentMs - lastLoopMs;

  if (deltaMs >= 40) {
      lastLoopMs = currentMs;

      int nasobic = 100 + (min(aktualniPocetChyb, 2) * 25);
      unsigned long odecet = (deltaMs * nasobic) / 100;

      if (zbyvaMs <= odecet) {
          zbyvaMs = 0;
          zobrazCas(0); 
          konecHry(STAV_PROHRA, "CAS VYPRSEL!");
          return;
      } else {
          zbyvaMs -= odecet;
      }
      
      long zbyvaSec = zbyvaMs / 1000;
      
      if (zbyvaSec != naposledyZobrazeneSekundy) {
        naposledyZobrazeneSekundy = zbyvaSec; 
        
        zobrazCas(zbyvaSec);
        
        if (millis() > blokovatTikDo) {
            prehrajZvuk(1); 
        }
        
        odeslatBroadcastInfo(zbyvaSec);
        
        byte dataState[1] = { 1 }; 
        odeslatZpravu(ID_GAME_STATE, dataState, 1);
      }
  }

  for(int i=0; i<POCET_PINU; i++) nastavJedenIndikator(i, herniStavIndikatoru[i]);

  bool lastLife = (aktualniPocetChyb == nastavMaxChyb - 1);
  if (lastLife && aktualniPocetChyb > 0) {
      bool sviti = (millis() / 250) % 2; 
      digitalWrite(PIN_LED_ERR1, sviti);
      if (aktualniPocetChyb >= 2) digitalWrite(PIN_LED_ERR2, sviti);
  } else {
      digitalWrite(PIN_LED_ERR1, (aktualniPocetChyb >= 1));
      digitalWrite(PIN_LED_ERR2, (aktualniPocetChyb >= 2));
  }
  
  if (CAN_MSGAVAIL == CAN.checkReceive()) {
    long unsigned int rxId; unsigned char len=0; unsigned char buf[8];
    CAN.readMsgBuf(&rxId, &len, buf);
    
    if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END) {
      byte status = buf[0];
      if (status == 1) { 
        pocetVyresenychModulu++; checkVyhra(); 
      } 
      else if (status == 2) { 
        aktualniPocetChyb++; 
        
        if (aktualniPocetChyb >= nastavMaxChyb) {
            konecHry(STAV_PROHRA, "MOC CHYB!");
        } else {
            prehrajZvuk(2); 
            blokovatTikDo = millis() + 1500; 
        }
      }
    }
  }
  if (btnOkState == 2) konecHry(STAV_UKONCENO, "UKONCENO UZIVATELEM");
}

void konecHry(StavHry cilovyStav, const char* duvod) {
  aktualniStav = cilovyStav;
  if (cilovyStav == STAV_VYHRA) { display.setSegments(SEG_DONE); prehrajZvuk(4); }
  else if (cilovyStav == STAV_UKONCENO) { display.setSegments(SEG_END); }
  else { display.setSegments(SEG_FAIL); prehrajZvuk(3); }

  digitalWrite(PIN_LED_ERR1, (aktualniPocetChyb >= 1));
  digitalWrite(PIN_LED_ERR2, (aktualniPocetChyb >= 2));
  
  byte stavKod = (byte)cilovyStav; 
  odeslatZpravu(ID_GAME_STATE, &stavKod, 1);
}

void handleEndGame(int btnO) {
  if (btnO == 2) { 
    aktualniStav = STAV_MENU;
    for(int i=0; i<POCET_PINU; i++) nastavJedenIndikator(i, false);
    
    celkemModulu = 0;
    for(int i=0; i<16; i++) registrovaneModuly[i] = 0;

    byte dataReset[1] = { 0 }; 
    for(int i=0; i<3; i++) { odeslatZpravu(ID_GAME_STATE, dataReset, 1); delay(20); }
    vypisMenu();
  }
}

void checkVyhra() { 
  if (celkemModulu > 0 && pocetVyresenychModulu >= celkemModulu) { konecHry(STAV_VYHRA, "VSE HOTOVO"); }
}

// ==========================================
// FUNKCE MENU (Kreslení a nastavování hodnot)
// ==========================================
void upravHodnotu(int smer) {
  if (strankaMenu == 0) { 
     if (smer > 0) {
        if (nastavCasSekundy < 60) nastavCasSekundy += 15;
        else if (nastavCasSekundy < 180) nastavCasSekundy += 30;
        else if (nastavCasSekundy < 900) nastavCasSekundy += 60;
        else nastavCasSekundy += 300;
     } else {
        if (nastavCasSekundy <= 60) nastavCasSekundy -= 15;
        else if (nastavCasSekundy <= 180) nastavCasSekundy -= 30;
        else if (nastavCasSekundy <= 900) nastavCasSekundy -= 60;
        else nastavCasSekundy -= 300;
     }
     nastavCasSekundy = constrain(nastavCasSekundy, 15, 7200); 
  }
  else if (strankaMenu == 1) { nastavMaxChyb += smer; nastavMaxChyb = constrain(nastavMaxChyb, 1, 10); }
  else if (strankaMenu == 2) { 
     nastavHlasitost += smer; nastavHlasitost = constrain(nastavHlasitost, 0, 5); 
     myDFPlayer.volume(nastavHlasitost * 6); 
  }
  else if (strankaMenu == 3) { 
     jasDisplejeIndex += smer; jasDisplejeIndex = constrain(jasDisplejeIndex, 0, 4); 
     display.setBrightness(jasMap[jasDisplejeIndex]); 
  }

  if (!oledUIPopupActive) {
     oledUIPopupActive = true;
     vypisMenu(); 
  } else {
     vykresliHodnotu(); 
  }
  oledUIPopupTimer = millis();
}

void vykresliHodnotu() {
  if (strankaMenu == 0) { 
      zobrazCas(nastavCasSekundy);
  } 
  else if (strankaMenu == 1) { display.showNumberDec(nastavMaxChyb); }
  else if (strankaMenu == 2) { display.showNumberDec(nastavHlasitost); }
  else if (strankaMenu == 3) { display.showNumberDec(jasDisplejeIndex + 1); }
}

void vypisMenu() {
  u8g2.firstPage(); do {
    u8g2.setFont(u8g2_font_profont12_tr); 
    
    if (oledUIPopupActive) {
       u8g2.setCursor(10, 20);
       if (strankaMenu == 0) u8g2.print(F("Cas Hry"));
       else if (strankaMenu == 1) u8g2.print(F("Limit Chyb"));
       else if (strankaMenu == 2) u8g2.print(F("Hlasitost (0-5)"));
       else if (strankaMenu == 3) u8g2.print(F("Jas Displeje (1-5)"));
    } else {
       u8g2.setCursor(5, 12); u8g2.print(F("Mods:")); u8g2.print(celkemModulu);
       u8g2.setCursor(65, 12); u8g2.print(F("Wdg:")); u8g2.print(celkemWidgetu);
       u8g2.setCursor(5, 28); u8g2.print(F("Baterie: ")); u8g2.print(zmerBaterii()); u8g2.print(F("%"));
    }
  } while(u8g2.nextPage());
  
  digitalWrite(PIN_LED_ERR1, LOW); digitalWrite(PIN_LED_ERR2, LOW);
  display.clear(); 
  vykresliHodnotu(); 
}

// Výpočet z přesného schématu s 10k/10k děličem s 5V referencí Arduino
int zmerBaterii() {
  long soucet = 0; 
  for(int i=0; i<10; i++) { 
    soucet += analogRead(PIN_BATTERY); 
    delay(1); 
  }
  long prumerADC = soucet / 10;
  long mv = (prumerADC * 10000L) / 1023L; 
  
  if (mv >= 4150) return 100;
  if (mv >= 3900) return map(mv, 3900, 4150, 75, 100);
  if (mv >= 3700) return map(mv, 3700, 3900, 40, 75);
  if (mv >= 3500) return map(mv, 3500, 3700, 15, 40);
  if (mv >= 3200) return map(mv, 3200, 3500, 0, 15);
  return 0; 
}