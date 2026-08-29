#include <SPI.h>
#include <mcp_can.h>
#include <EEPROM.h>

// ==========================================
// LADĚNÍ (0 = Vypnuto, 1 = Zapnuto)
// ==========================================
#define DEBUG_MODE 1
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
#define ID_BOMB_INFO 0x020

// !!! DŮLEŽITÉ: ZMĚNIT PRO KAŽDÝ NOVÝ MODUL (0x102, 0x103 atd.) !!!
#define ID_MOD_TEMPLATE 0x102

// Definice packetů odesílaných zpět do Master modulu
#define STATUS_HELLO 0  // Heartbeat a registrace do sítě
#define STATUS_SOLVED 1 // Povel "Modul byl úspěšně vyřešen"
#define STATUS_STRIKE 2 // Povel "Byla udělána chyba, přičti strike"
#define STATUS_ABORT 3  // Hlášení o kritické chybě hardwaru - zastavit start

// ==========================================
// 2. HARDWAROVÉ MAPOVÁNÍ A NASTAVENÍ
// ==========================================
const int SPI_CS_PIN = 10;
MCP_CAN CAN(SPI_CS_PIN);

// Indikační LED modulu (Kdyžtak změnit podle svého zapojení)
const int ledRed = 4;
const int ledGreen = 5;

// ==========================================
// Piny Simon Screams
// TODO: Zkontrolujte a upravte piny pro svůj hardware.
// POZOR: Na Arduino Nano jsou piny A6 a A7 pouze analogové vstupy (nelze použít jako OUTPUT pro LED).
// Možná bude nutné použít posuvný registr, expandér nebo analogový žebříček pro tlačítka, pokud dojdou piny.
// ==========================================
// 0=Red, 1=Orange, 2=Yellow, 3=Green, 4=Blue, 5=Purple
const int pinLedColors[6] = {A0, A1, A2, A3, A4, A5}; 
const int pinBtnColors[6] = {2, 3, 6, 7, 8, 9};
const int pinLedStages[3] = {A6, A7, 0}; // TODO: Změnit A6/A7 pokud to je Nano

// ==========================================
// 3. EEPROM STRUKTURA (Power-Loss Resilience)
// ==========================================
struct GameData
{
    byte signature; // Verifikační bajt
    bool active;    // Příznak běžící hry
    bool solved;    // Flag vyřešeného stavu

    // DOPLNIT: Zde přidat jakékoliv proměnné, které si modul musí pamatovat
    // v případě výpadku proudu (např. pozice v bludišti, stisknutá tlačítka)
    byte currentStage; // 0, 1, 2 (odpovídá stage 1, 2, 3)
    byte inputIndex;   // Kolik správných odpovědí hráč zatím zadal v aktuální stage
    byte flashSequence[3]; // Vygenerovaná sekvence blikání
};
const byte EEPROM_SIG = 0xA5; // Kontrolní znak 10100101

// ==========================================
// 4. BĚHOVÉ PROMĚNNÉ
// ==========================================
// Statistiky ze Sériového čísla (získané z CAN sběrnice)
bool lastDigitOdd = true;
int vowelsCount = 0;
int digitsCount = 0;
int consonantsCount = 0; // Tohle lze použít jako počet písmen (6 - digitsCount)
int batteryHolders = 0;
int totalPorts = 0;
int totalIndicators = 0;

bool gameRunning = false;
bool solved = false;
bool isPrepared = false; // Flag úspěšné pre-start validace

unsigned long timeOfLastMistake = 0;
unsigned long lastRegistrationTime = 0;

// Deklarace předem pro použití v setup()
void sendMessage(unsigned long id, byte state);
void recoverGameState();

/* 
Simon screams - reimplementation of the Timwi module.
The real module shuffles the colored buttons around, this one has the colors fixed
From the most top one, in clockwise direction: red, orange, yellow, green, blue, purple
*/

enum SubState {
    SUB_WAITING,
    SUB_FLASHING,
    SUB_INPUT
};
SubState currentSubState = SUB_WAITING;

byte currentStage = 0;
byte inputIndex = 0;
byte flashSequence[3] = {0};
byte expectedAnswers[6] = {0}; // Správné odpovědi pro danou stage (max 6 barev)
byte expectedAnswersCount = 0;

unsigned long flashTimer = 0;
int flashIndex = 0;
bool isFlashing = false;

// ==========================================
// INICIALIZACE MODULU
// ==========================================
void setup()
{
    Serial.begin(115200);
    pinMode(ledRed, OUTPUT);
    pinMode(ledGreen, OUTPUT);

    for (int i = 0; i < 6; i++) {
        pinMode(pinLedColors[i], OUTPUT);
        pinMode(pinBtnColors[i], INPUT_PULLUP);
    }
    for (int i = 0; i < 3; i++) {
        pinMode(pinLedStages[i], OUTPUT);
    }

    // Inicializace CAN sběrnice
    if (CAN.begin(MCP_ANY, CAN_125KBPS, MCP_8MHZ) == CAN_OK)
    {
        D_PRINTLN(F("CAN Bus OK!"));
    }
    else
    {
        D_PRINTLN(F("CAN Init Failed!"));
        while (1)
        {
            digitalWrite(ledRed, HIGH);
            delay(100);
            digitalWrite(ledRed, LOW);
            delay(100);
        }
    }
    CAN.setMode(MCP_NORMAL);

    D_PRINTLN(F("Novy Modul pripraven..."));
    turnOffLeds();

    recoverGameState();
}

void turnOffLeds()
{
    digitalWrite(ledRed, LOW);
    digitalWrite(ledGreen, LOW);
}

// Obalová funkce pro odeslání dat
void sendMessage(unsigned long id, byte state)
{
    byte data[1] = {state};
    byte result = CAN.sendMsgBuf(id, 0, 1, data);

    if (result == CAN_OK)
    {
        if (state != STATUS_HELLO)
        {
            D_PRINT(F("CAN Odeslano: "));
            D_PRINTLN(state);
        }
    }
    else
    {
        if (state != STATUS_HELLO)
        {
            D_PRINTLN(F("Chyba odesilani CAN"));
        }
    }
}

// ==========================================
// OCHRANA PŘED PODVÁDĚNÍM PŘI VÝPADKU
// ==========================================
void recoverGameState()
{
    GameData data;
    EEPROM.get(0, data);

    if (data.signature == EEPROM_SIG && data.active == true)
    {
        D_PRINTLN(F(">>> OBNOVA STAVU Z EEPROM <<<"));
        solved = data.solved;

        // DOPLNIT: obnovit proměnné zpět do paměti z EEPROM
        // napriklad: krok = data.gameStep;

        if (solved)
        {
            isPrepared = true;
            digitalWrite(ledGreen, HIGH);
            digitalWrite(ledRed, LOW);
            return;
        }

        isPrepared = true;
    }
}

// ==========================================
// PRE-START VALIDACE A GENEROVÁNÍ LOGIKY
// ==========================================
bool armingSequence()
{
    D_PRINTLN(F(">>> PRIPRAVA NOVE HRY <<<"));
    turnOffLeds();

    GameData newData;
    newData.signature = EEPROM_SIG;
    newData.active = true;
    newData.solved = false;

    // DOPLNIT: Tady se vygeneruje pravidlo modulu na základě sériového čísla.
    // U Simon Screams se zde generuje náhodná sekvence barev.
    randomSeed(millis());
    for (int i = 0; i < 3; i++) {
        flashSequence[i] = random(0, 6);
    }
    currentStage = 0;
    inputIndex = 0;
    currentSubState = SUB_FLASHING;

    EEPROM.put(0, newData);
    isPrepared = true;

    return true; // Vrať false, pokud selhala HW kontrola (např. chybí tlačítko)
}

// ==========================================
// HLAVNÍ SMYČKA FSM A CAN POLLING
// ==========================================
void loop()
{
    if (gameRunning && timeOfLastMistake > 0)
    {
        if (millis() - timeOfLastMistake > 1000)
        {
            digitalWrite(ledRed, LOW);
            timeOfLastMistake = 0;
        }
    }

    // Polling CAN sběrnice
    while (CAN_MSGAVAIL == CAN.checkReceive())
    {
        long unsigned int rxId;
        unsigned char len = 0;
        unsigned char buf[8];
        CAN.readMsgBuf(&rxId, &len, buf);

        if (rxId == ID_BOMB_INFO)
        {
            // TODO: Varování ohledně CAN sběrnice a Simon Screams:
            // Hlavní modul momentálně používá bitové omezení min(aaCount, 3) a podobně pro dCount a portCounts.
            // Pokud bude mít bomba 4 stejné baterie (AA), hodnoty budou oříznuty na 3.
            // Znamená to, že podmínka "≤ 3 bat. holders" by zde pro 4 baterie nesprávně vyšla TRUE!
            // Bude-li to problém, je potřeba upravit hlavní modul, aby posílal absolutní counts nebo použil přesnější CAN strukturu.
            
            vowelsCount = (buf[1] >> 5) & 0x07;
            digitsCount = buf[2] & 0x07;
            consonantsCount = 6 - (vowelsCount + digitsCount);

            // Výpočet holders a portů z limitovaných dat z hlavního modulu
            int aaCount = (buf[2] >> 6) & 0x03;
            int dCount = buf[3] & 0x03;
            batteryHolders = aaCount + dCount; // TODO: Pozor, capping
            
            totalPorts = ((buf[3] >> 2) & 0x03) + ((buf[3] >> 4) & 0x03) + ((buf[3] >> 6) & 0x03) + 
                         (buf[4] & 0x03) + ((buf[4] >> 2) & 0x03) + ((buf[4] >> 4) & 0x03);

            // Počet indikátorů z počtu jedniček ve foundIndMask
            int indMask = buf[5] | ((buf[6] & 0x07) << 8);
            totalIndicators = 0;
            for(int i = 0; i < 11; i++) {
                if(indMask & (1 << i)) totalIndicators++;
            }

            bool isEven = (buf[4] >> 6) & 0x01;
            lastDigitOdd = !isEven;
        }
        else if (rxId == ID_GAME_STATE)
        {
            byte state = buf[0];

            if (state == 5)
            {
                if (armingSequence())
                {
                    D_PRINTLN(F("Validace OK - Cekam na start"));
                }
                else
                {
                    sendMessage(ID_MOD_TEMPLATE, STATUS_ABORT);
                    for (int i = 0; i < 12; i++)
                    {
                        digitalWrite(ledRed, HIGH);
                        delay(100);
                        digitalWrite(ledRed, LOW);
                        delay(100);
                    }
                }
            }
            else if (state == 1)
            {
                if (isPrepared)
                    gameRunning = true;
            }
            else if (state == 0)
            {
                if (millis() - lastRegistrationTime > 500)
                {
                    lastRegistrationTime = millis();
                    sendMessage(ID_MOD_TEMPLATE, STATUS_HELLO);
                }

                if (gameRunning)
                    D_PRINTLN(F("RESET -> MENU"));
                byte invalidSig = 0x00;
                EEPROM.put(0, invalidSig);

                gameRunning = false;
                solved = false;
                isPrepared = false;
                timeOfLastMistake = 0;
                turnOffLeds();
            }
            else if (state == 2)
            {
                gameRunning = false;
                digitalWrite(ledGreen, HIGH);
                digitalWrite(ledRed, LOW);
            }
            else if (state == 3)
            {
                gameRunning = false;
                digitalWrite(ledGreen, LOW);
                digitalWrite(ledRed, HIGH);
            }
            else if (state == 4)
            {
                gameRunning = false;
                turnOffLeds();
            }
        }
    }

    // ==========================================
    // JÁDRO HRY - DOPLNIT VLASTNÍ LOGIKU
    // ==========================================
    if (gameRunning && !solved)
    {
        // 1. Řízení Stage LED
        for (int i = 0; i < 3; i++) {
            digitalWrite(pinLedStages[i], (i < currentStage) ? HIGH : LOW);
        }

        // 2. Stavový automat hry
        if (currentSubState == SUB_FLASHING) {
            // Logika přehrávání záblesků
            if (millis() - flashTimer > 500) {
                flashTimer = millis();
                
                // Zhasnutí všeho
                for (int i = 0; i < 6; i++) { digitalWrite(pinLedColors[i], LOW); }

                if (isFlashing) {
                    isFlashing = false;
                    flashIndex++;
                    if (flashIndex > currentStage) {
                        currentSubState = SUB_INPUT; // Konec blikání pro tuto stage
                        
                        // TODO: Zde zavolat funkci evaluateSimonScreams() která 
                        // vyplní pole expectedAnswers a nastaví expectedAnswersCount
                        // na základě flashSequence, tabulek a ID_BOMB_INFO dat.
                        
                        inputIndex = 0;
                    }
                } else {
                    isFlashing = true;
                    digitalWrite(pinLedColors[flashSequence[flashIndex]], HIGH);
                }
            }
        } 
        else if (currentSubState == SUB_INPUT) {
            // Logika čekání na stisk tlačítek
            for (int i = 0; i < 6; i++) {
                if (digitalRead(pinBtnColors[i]) == LOW) { // Předpokládám INPUT_PULLUP (spíná k zemi)
                    // Jednoduchý debounce
                    delay(50);
                    while(digitalRead(pinBtnColors[i]) == LOW);
                    delay(50);

                    // Kontrola správnosti
                    // if (expectedAnswersCount > 0 && i == expectedAnswers[inputIndex]) { ...
                    // Pro účely draftu jen "něco jsme stiskli":
                    bool spravne = true; // TODO: Nahradit porovnáním s expectedAnswers[inputIndex]
                    
                    if (spravne) {
                        inputIndex++;
                        if (inputIndex >= expectedAnswersCount) {
                            // Stage hotova
                            currentStage++;
                            if (currentStage >= 3) {
                                solved = true;
                                digitalWrite(ledGreen, HIGH);
                                GameData d; EEPROM.get(0, d); d.solved = true; EEPROM.put(0, d);
                                sendMessage(ID_MOD_TEMPLATE, STATUS_SOLVED);
                            } else {
                                // Jdeme na další stage (delší blikání)
                                currentSubState = SUB_FLASHING;
                                flashIndex = 0;
                                flashTimer = millis();
                            }
                        }
                    } else {
                        // Chyba
                        digitalWrite(ledRed, HIGH);
                        timeOfLastMistake = millis();
                        sendMessage(ID_MOD_TEMPLATE, STATUS_STRIKE);
                        
                        // Restart aktuální stage blikáním
                        currentSubState = SUB_FLASHING;
                        flashIndex = 0;
                        flashTimer = millis();
                    }
                }
            }
        }
    }

    delay(10);
}