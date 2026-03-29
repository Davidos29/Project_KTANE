#include <SPI.h>
#include <mcp_can.h>
#include <EEPROM.h>

// ==========================================
// LADĚNÍ (0 = Vypnuto, 1 = Zapnuto)
// ==========================================
#define DEBUG_MODE 0
#if DEBUG_MODE == 1
  #define D_PRINT(x) Serial.print(x)
  #define D_PRINTLN(x) Serial.println(x)
#else
  #define D_PRINT(x)
  #define D_PRINTLN(x)
#endif

// ==========================================
// 1. KOMUNIKAČNÍ PROTOKOL A ID
// ==========================================
#define ID_GAME_STATE 0x001
#define ID_BOMB_INFO  0x020 
#define ID_MOD_WIRES  0x101 // Hardcodované ID tohoto konkrétního modulu v CAN síti

// Definice packetů odesílaných zpět do Master modulu
#define STATUS_HELLO  0 // Heartbeat a registrace do sítě
#define STATUS_SOLVED 1 // Povel "Modul byl úspěšně vyřešen"
#define STATUS_STRIKE 2 // Povel "Byla udělána chyba, přičti strike"
#define STATUS_ABORT  3 // Hlášení o kritické chybě hardwaru - zastavit start

// ==========================================
// 2. HARDWAROVÉ MAPOVÁNÍ A NASTAVENÍ
// ==========================================
const int SPI_CS_PIN = 10;
MCP_CAN CAN(SPI_CS_PIN);

// Indikační LED modulu
const int ledRed = 4;
const int ledGreen = 5;
const int jasZelene = 45; // Statická hodnota jasu (z důvodu chybějícího PWM u červené)

// Mapování fyzických barev na ADC hodnoty (Toleranční tabulka)
const char* barvy[6] = {"cerna", "bila", "cervena", "zluta", "modra", "zelena"};
int hodnoty[6] = {937, 849, 699, 512, 340, 176};

// ==========================================
// 3. EEPROM STRUKTURA (Power-Loss Resilience)
// ==========================================
struct GameData {
  byte signature;        // Verifikační bajt (ochrana před čtením náhodného šumu)
  bool active;           // Příznak běžící hry
  byte targetWirePin;    // Který fyzický pin je cílem k přestřižení
  bool wireExists[6];    // Snapshot drátů zapojených na začátku hry
  bool solved;           // Flag vyřešeného stavu
  bool lastDigitOdd;     // Poslední číslice sériového čísla (uloženo z broadcastu)
};
const byte EEPROM_SIG = 0xA5; // Kontrolní znak 10100101

// ==========================================
// 4. BĚHOVÉ PROMĚNNÉ
// ==========================================
bool lastDigitOdd = true; 
bool activeWires[6] = {false}; 
int targetPin = -1;

// Proměnné pro HW debouncing
bool wireIsLow[6] = {false, false, false, false, false, false};
unsigned long wireCutTime[6] = {0, 0, 0, 0, 0, 0};

// Statistiky ze Sériového čísla (získané z CAN sběrnice)
int vowelsCount = 0;
int digitsCount = 0;
int consonantsCount = 0;

bool gameRunning = false;     
bool solved = false;          
bool isPrepared = false; // Flag úspěšné pre-start validace

unsigned long casPosledniChyby = 0; 
unsigned long lastRegistrationTime = 0; 

// Deklarace předem pro použití v setup()
void odeslatZpravu(unsigned long id, byte stav);
void recoverGameState();

// ==========================================
// INICIALIZACE MODULU
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(ledRed, OUTPUT);
  pinMode(ledGreen, OUTPUT);

  // Inicializace CAN sběrnice na 125 kbps - nutné sjednotit na všech uzlech!
  if (CAN.begin(MCP_ANY, CAN_125KBPS, MCP_8MHZ) == CAN_OK) {
    D_PRINTLN(F("CAN Bus OK!"));
  } else {
    D_PRINTLN(F("CAN Init Failed!"));
    while (1) { digitalWrite(ledRed, HIGH); delay(100); digitalWrite(ledRed, LOW); delay(100); }
  }
  CAN.setMode(MCP_NORMAL);

  D_PRINTLN(F("Modul Draty pripraven..."));
  vypniLedky();

  recoverGameState();
}

void vypniLedky() {
  digitalWrite(ledRed, LOW);
  analogWrite(ledGreen, 0);
}

// Obalová funkce pro non-blocking odeslání dat přes MCP2515
void odeslatZpravu(unsigned long id, byte stav) {
  byte data[1] = {stav};
  byte vysledek = CAN.sendMsgBuf(id, 0, 1, data);

  if (vysledek == CAN_OK) {
    if(stav != STATUS_HELLO) { D_PRINT(F("CAN Odeslano: ")); D_PRINTLN(stav); }
  } else {
    if(stav != STATUS_HELLO) { D_PRINTLN(F("Chyba odesilani CAN")); }
  }
}

// ==========================================
// OCHRANA PŘED PODVÁDĚNÍM PŘI VÝPADKU (Zpracování EEPROM)
// ==========================================
void recoverGameState() {
  GameData data;
  EEPROM.get(0, data);

  if (data.signature == EEPROM_SIG && data.active == true) {
    D_PRINTLN(F(">>> OBNOVA STAVU Z EEPROM (HOT-PLUG) <<<"));
    
    targetPin = data.targetWirePin;
    solved = data.solved;
    lastDigitOdd = data.lastDigitOdd;
    
    if (solved) {
        isPrepared = true;
        analogWrite(ledGreen, jasZelene);
        digitalWrite(ledRed, LOW);
        return; 
    }

    int wrongCuts = 0;
    bool correctCut = false;
    bool stavZmenen = false;
    
    // Porovnání aktuálního fyzického stavu drátů s uloženým snapshotem
    for(int i=0; i<6; i++) {
        if (data.wireExists[i]) {
          if (analogRead(i) < 50) { 
              activeWires[i] = false; 
              data.wireExists[i] = false; 
              stavZmenen = true;
              if (i == targetPin) correctCut = true;
              else wrongCuts++;
          } else {
              activeWires[i] = true;
          }
        } else {
            activeWires[i] = false;
        }
    }
    
    if (stavZmenen) {
        EEPROM.put(0, data);
    }
    
    isPrepared = true;

    if (wrongCuts > 0) {
        D_PRINTLN(F("CHYBA: Nalezeny spatne strihy ziskane pri vypadku!"));
        for(int k=0; k<wrongCuts; k++) {
             odeslatZpravu(ID_MOD_WIRES, STATUS_STRIKE);
             delay(600); 
        }
        digitalWrite(ledRed, HIGH); 
        casPosledniChyby = millis();
    }

    if (correctCut) {
        D_PRINTLN(F(">>> CIL BYL PRESTRIZEN VE VYPNUTEM STAVU! <<<"));
        solved = true; 
        analogWrite(ledGreen, jasZelene); 
        digitalWrite(ledRed, LOW);
        
        data.solved = true; 
        EEPROM.put(0, data);
        
        odeslatZpravu(ID_MOD_WIRES, STATUS_SOLVED);
    } else if (!solved) {
        D_PRINT(F("CIL (ze zaznamu): Pin A")); D_PRINTLN(targetPin);
    }
  }
}

// ==========================================
// PRE-START VALIDACE A GENEROVÁNÍ LOGIKY (Arming)
// ==========================================
bool armingSequence() {
  D_PRINTLN(F(">>> ANALYZA NOVE HRY <<<"));
  vypniLedky(); 
  
  GameData newData;
  newData.signature = EEPROM_SIG; 
  for(int i=0; i<6; i++) { activeWires[i] = false; newData.wireExists[i] = false; }
  
  // Nahrazeno z pole Stringů na pole indexů kvůli zamezení fragmentace paměti
  int nacteneBarvyIdx[6];
  int fyzickePiny[6];
  int pocet = 0;

  for (int pin = 0; pin < 6; pin++) {
    int val = analogRead(pin);
    if (val >= 50) { 
      int nejIndex = 0; int minRozdil = abs(val - hodnoty[0]);
      for (int i = 1; i < 6; i++) {
        int diff = abs(val - hodnoty[i]);
        if (diff < minRozdil) { minRozdil = diff; nejIndex = i; }
      }
      activeWires[pin] = true; newData.wireExists[pin] = true;  
      nacteneBarvyIdx[pocet] = nejIndex;
      fyzickePiny[pocet] = pin;
      pocet++;
      D_PRINT(F("A")); D_PRINT(pin); D_PRINT(F(": ")); D_PRINTLN(barvy[nejIndex]);
    }
  }

  if (pocet < 3 || pocet > 6) { 
    D_PRINTLN(F("CHYBA: Nespravny pocet dratu!")); 
    return false; 
  }

  // Indexy: 0=cerna, 1=bila, 2=cervena, 3=zluta, 4=modra, 5=zelena
  auto countColor = [&](int colorIdx) { int n=0; for(int i=0; i<pocet; i++) if(nacteneBarvyIdx[i]==colorIdx)n++; return n; };
  int indexRezu = 1; 

  if (pocet == 3) {
    if (countColor(2) == 0) indexRezu = 2; // cervena=2
    else if (nacteneBarvyIdx[pocet - 1] == 1) indexRezu = pocet; // bila=1
    else if (countColor(4) > 1) { for (int i = pocet - 1; i >= 0; i--) if (nacteneBarvyIdx[i] == 4) { indexRezu = i + 1; break; } } // modra=4
    else indexRezu = pocet;
  } 
  else if (pocet == 4) {
    if (countColor(2) > 1 && lastDigitOdd) { for (int i = pocet - 1; i >= 0; i--) if (nacteneBarvyIdx[i] == 2) { indexRezu = i + 1; break; } }
    else if (nacteneBarvyIdx[pocet - 1] == 3 && countColor(2) == 0) indexRezu = 1; // zluta=3
    else if (countColor(4) == 1) indexRezu = 1;
    else if (countColor(3) > 1) indexRezu = pocet;
    else indexRezu = 2;
  }
  else if (pocet == 5) {
    if (nacteneBarvyIdx[pocet - 1] == 0 && lastDigitOdd) indexRezu = 4; // cerna=0
    else if (countColor(2) == 1 && countColor(3) > 1) indexRezu = 1;
    else if (countColor(0) == 0) indexRezu = 2;
    else indexRezu = 1;
  }
  else if (pocet == 6) {
    if (countColor(3) == 0 && lastDigitOdd) indexRezu = 3;
    else if (countColor(3) == 1 && countColor(1) > 1) indexRezu = 4;
    else if (countColor(2) == 0) indexRezu = pocet;
    else indexRezu = 4;
  }

  targetPin = fyzickePiny[indexRezu - 1]; 
  D_PRINT(F("Vypocitany CIL: Pin A")); D_PRINTLN(targetPin);

  newData.active = true;
  newData.targetWirePin = targetPin;
  newData.solved = false;
  newData.lastDigitOdd = lastDigitOdd;
  EEPROM.put(0, newData);
  
  isPrepared = true;
  return true; 
}

// ==========================================
// HLAVNÍ SMYČKA FSM A CAN POLLING
// ==========================================
void loop() {
  if (gameRunning && casPosledniChyby > 0) {
     if (millis() - casPosledniChyby > 1000) { 
       digitalWrite(ledRed, LOW); 
       casPosledniChyby = 0; 
     }
  }

  while (CAN_MSGAVAIL == CAN.checkReceive()) {
    long unsigned int rxId; unsigned char len = 0; unsigned char buf[8];
    CAN.readMsgBuf(&rxId, &len, buf);

    if (rxId == ID_BOMB_INFO) { 
      vowelsCount = (buf[1] >> 5) & 0x07;
      digitsCount = buf[2] & 0x07;
      consonantsCount = 6 - (vowelsCount + digitsCount); 
      
      bool isEven = (buf[4] >> 6) & 0x01;
      lastDigitOdd = !isEven; 
    }
    else if (rxId == ID_GAME_STATE) {
      byte stav = buf[0];
      
      if (stav == 5) { 
        if (armingSequence()) {
           D_PRINTLN(F("Validace OK - Cekam na start"));
        } else {
           odeslatZpravu(ID_MOD_WIRES, STATUS_ABORT); 
           for(int i=0; i<12; i++) { 
               digitalWrite(ledRed, HIGH); delay(100); 
               digitalWrite(ledRed, LOW); delay(100); 
           }
        }
      }
      else if (stav == 1) { 
        if (isPrepared) {
           gameRunning = true;
        }
      } 
      else if (stav == 0) { 
         if (millis() - lastRegistrationTime > 500) {
             lastRegistrationTime = millis();
             odeslatZpravu(ID_MOD_WIRES, STATUS_HELLO); 
         }

         if (gameRunning) D_PRINTLN(F("RESET -> MENU"));
         byte invalidSig = 0x00; EEPROM.put(0, invalidSig); 
         
         gameRunning = false; solved = false; isPrepared = false;
         casPosledniChyby = 0; 
         vypniLedky();
      }
      else if (stav == 2) { 
         gameRunning = false; 
         analogWrite(ledGreen, jasZelene); 
         digitalWrite(ledRed, LOW); 
      }
      else if (stav == 3) { 
         gameRunning = false; 
         analogWrite(ledGreen, 0); 
         digitalWrite(ledRed, HIGH); 
      }
      else if (stav == 4) { 
         gameRunning = false; 
         vypniLedky(); 
      }
    }
  }

  // ==========================================
  // JÁDRO HRY - DETEKCE PŘERUŠENÍ OBVODU
  // ==========================================
  if (gameRunning && !solved) {
    for (int pin = 0; pin < 6; pin++) {
      if (activeWires[pin]) {
        if (analogRead(pin) < 50) { 
            if (!wireIsLow[pin]) {
                wireIsLow[pin] = true;
                wireCutTime[pin] = millis();
            } else if (millis() - wireCutTime[pin] > 50) {
                wireIsLow[pin] = false;
                D_PRINT(F("STRIH: A")); D_PRINTLN(pin);
                
                if (pin == targetPin) {
                  D_PRINTLN(F("VYSLEDEK: SPRAVNE!"));
                  solved = true;
                  analogWrite(ledGreen, jasZelene);
                  digitalWrite(ledRed, LOW);
                  
                  GameData d; EEPROM.get(0, d); d.solved = true; EEPROM.put(0, d);
                  
                  odeslatZpravu(ID_MOD_WIRES, STATUS_SOLVED);
                } else {
                  D_PRINTLN(F("VYSLEDEK: CHYBA!"));
                  activeWires[pin] = false; 
                  
                  digitalWrite(ledRed, HIGH);
                  casPosledniChyby = millis(); 
                  
                  odeslatZpravu(ID_MOD_WIRES, STATUS_STRIKE);
                }
            }
        } else {
            wireIsLow[pin] = false; 
        }
      }
    }
  }

  delay(10); 
}