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

const byte WIDGET_PINS[] = {A0, A1, A2, A3, A4, A5};
const byte PIN_COUNT = 6;

// Inicializace instancí periferií
TM1637Display display(DISP_CLK, DISP_DIO);
U8G2_SSD1306_128X32_UNIVISION_1_SW_I2C u8g2(U8G2_R0, 3, 2, U8X8_PIN_NONE);
SoftwareSerial mySoftwareSerial(9, 8);
DFRobotDFPlayerMini myDFPlayer;
MCP_CAN CAN(SPI_CS_PIN);

// ==========================================
// NASTAVENÍ ANALOGOVÝCH TLAČÍTEK
// ==========================================
#define BTN_NONE 0
#define BTN_RIGHT 1
#define BTN_OK 2
#define BTN_LEFT 4

const int targetValues[4] PROGMEM = {0, 515, 702, 847};
const byte buttonStates[4] PROGMEM = {BTN_NONE, BTN_LEFT, BTN_OK, BTN_RIGHT};

// ==========================================
// DEFINICE WIDGETŮ A JEJICH ADC HODNOT
// ==========================================
enum WidgetType
{
    W_UNKNOWN = -1,
    W_NOTHING = -2,
    W_BAT_D = 0,
    W_BAT_AA,
    W_SND,
    W_CLR,
    W_NSA,
    W_IND,
    W_SIG,
    W_CAR,
    W_MSA,
    W_TRN,
    W_BOB,
    W_FRK,
    W_FRQ,
    W_DVI,
    W_PAR,
    W_SER,
    W_PS2,
    W_RJ45,
    W_RCA,
    W_COMBO_A,
    W_COMBO_B,
    W_COMBO_C
};

struct WidgetInfo
{
    int8_t type;
    int16_t adcTarget;
    bool hasLED;
};

const WidgetInfo KNOWN_WIDGETS[] PROGMEM = {
    {(int8_t)W_BAT_D, 22, false}, {(int8_t)W_SND, 33, true}, {(int8_t)W_CLR, 46, true}, {(int8_t)W_IND, 65, true}, {(int8_t)W_SIG, 93, true}, {(int8_t)W_NSA, 134, true}, {(int8_t)W_CAR, 184, true}, {(int8_t)W_MSA, 254, true}, {(int8_t)W_TRN, 291, true}, {(int8_t)W_FRK, 327, true}, {(int8_t)W_BOB, 375, true}, {(int8_t)W_FRQ, 414, true}, {(int8_t)W_DVI, 512, false}, {(int8_t)W_PAR, 682, false}, {(int8_t)W_SER, 844, false}, {(int8_t)W_COMBO_A, 892, false}, {(int8_t)W_COMBO_B, 930, false}, {(int8_t)W_RJ45, 959, false}, {(int8_t)W_BAT_AA, 979, false}, {(int8_t)W_COMBO_C, 990, false}, {(int8_t)W_RCA, 1002, false}, {(int8_t)W_PS2, 1013, false}};
const int TYPE_COUNT = sizeof(KNOWN_WIDGETS) / sizeof(WidgetInfo);

WidgetType foundTypes[PIN_COUNT];
bool gameIndicatorState[PIN_COUNT];

// Globální detekce widgetů a periferií
int batteryCount = 0;
int aaCount = 0;
int dCount = 0;
int totalWidgets = 0;
int portCounts[6] = {0, 0, 0, 0, 0, 0};
uint16_t foundIndMask = 0;
uint16_t litIndMask = 0;

// ==========================================
// KOMUNIKAČNÍ PROTOKOL A STAVY HRY
// ==========================================
#define ID_GAME_STATE 0x001
#define ID_BOMB_INFO 0x020
#define ID_MODULE_START 0x100
#define ID_MODULE_END 0x1FF

enum GameState
{
    STATE_MENU = 0,
    STATE_GAME = 1,
    STATE_WIN = 2,
    STATE_LOSE = 3,
    STATE_ENDED = 4,
    STATE_ARMING = 5
};
GameState currentState = STATE_MENU;

// Globální nastavení parametrů hry
long settingTimeSeconds = 300;
int settingMaxMistakes = 3;
int settingVolume = 4;

int displayBrightnessIndex = 4;
const byte brightnessMap[5] = {0, 1, 2, 3, 7};

// Běhové proměnné
char serialNumber[7];
bool snOdd = false;
bool snVowel = false;
int snVowelsCount = 0;
int snDigitsCount = 0;

unsigned long remainingMs = 0;
unsigned long lastLoopMs = 0;
long lastDisplayedSeconds = -1;
int currentMistakeCount = 0;
int solvedModulesCount = 0;

int totalModulesCount = 0;
unsigned long registeredModules[16];
unsigned long blockTickUntil = 0;

int menuPage = 0;
unsigned long pressTimeLeft = 0, pressTimeOk = 0, pressTimeRight = 0;
bool stateLeft = 0, stateOk = 0, stateRight = 0;
bool handledLeft = 0, handledOk = 0, handledRight = 0;
const int LONG_PRESS_MS = 600;

bool oledUIPopupActive = false;
unsigned long oledUIPopupTimer = 0;

const uint8_t SEG_DONE[] = {SEG_B | SEG_C | SEG_D | SEG_E | SEG_G, SEG_C | SEG_D | SEG_E | SEG_G, SEG_C | SEG_E | SEG_G, SEG_A | SEG_D | SEG_E | SEG_F | SEG_G};
const uint8_t SEG_FAIL[] = {SEG_A | SEG_E | SEG_F | SEG_G, SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, SEG_B | SEG_C, SEG_D | SEG_E | SEG_F};
const uint8_t SEG_ERR[] = {SEG_A | SEG_D | SEG_E | SEG_F | SEG_G, SEG_E | SEG_G, SEG_E | SEG_G, 0};
const uint8_t SEG_END[] = {SEG_A | SEG_D | SEG_E | SEG_F | SEG_G, SEG_C | SEG_E | SEG_G, SEG_B | SEG_C | SEG_D | SEG_E | SEG_G, 0};

int getButtonState();
int readButton(bool, unsigned long &, bool &, bool &);
void printMenu();
void switchToArming();
void gameLoop(int);
void endGame(GameState, const char *);
void handleEndGame(int);
void checkWin();
void adjustValue(int);
void playSound(int);
void drawOled();
int measureBattery();
void sendMessage(unsigned long, byte *, byte);
void scanPeripherals();
void setOneIndicator(int, bool);
bool hasThisTypeLED(WidgetType);
int getIndicatorBitIndex(WidgetType);
void printWidgetName(WidgetType);
void registerModule(unsigned long);
void displayTime(long sekundy);
void drawValue();
void sendBroadcastInfo(long odesilanyCas);

// ==========================================
// INICIALIZACE SYSTÉMU
// ==========================================
void setup()
{
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
    if (!myDFPlayer.begin(mySoftwareSerial))
    {
        D_PRINTLN(F("CHYBA: DFPlayer nenalezen!"));
    }
    else
    {
        D_PRINTLN(F("DFPlayer OK"));
        myDFPlayer.setTimeOut(500);
        myDFPlayer.volume(settingVolume * 6);
        delay(500);
    }

    display.setBrightness(brightnessMap[displayBrightnessIndex]);
    displayTime(settingTimeSeconds);

    while (CAN_OK != CAN.begin(MCP_ANY, CAN_125KBPS, MCP_8MHZ))
    {
        D_PRINTLN(F("Chyba CAN modulu..."));
        delay(100);
    }
    CAN.setMode(MCP_NORMAL);
    D_PRINTLN(F("--- SYSTEM START ---"));

    scanPeripherals();

    totalModulesCount = 0;
    for (int i = 0; i < 16; i++)
        registeredModules[i] = 0;

    byte dataReset[1] = {0};
    for (int i = 0; i < 3; i++)
    {
        sendMessage(ID_GAME_STATE, dataReset, 1);
        delay(20);
    }

    printMenu();
}

// ==========================================
// ZPRACOVÁNÍ ANALOGOVÝCH VSTUPŮ
// ==========================================
WidgetType identifyWidget(int val)
{
    if (val > 1015)
        return W_NOTHING;
    int bestIndex = -1;
    int smallestDifference = 9999;
    for (int i = 0; i < TYPE_COUNT; i++)
    {
        int adcTarget = pgm_read_word(&KNOWN_WIDGETS[i].adcTarget);
        int difference = abs(val - adcTarget);
        if (difference < smallestDifference)
        {
            smallestDifference = difference;
            bestIndex = i;
        }
    }
    if (smallestDifference > 12)
        return W_UNKNOWN;
    return (WidgetType)pgm_read_byte(&KNOWN_WIDGETS[bestIndex].type);
}

bool hasThisTypeLED(WidgetType type)
{
    for (int i = 0; i < TYPE_COUNT; i++)
    {
        if ((WidgetType)pgm_read_byte(&KNOWN_WIDGETS[i].type) == type)
        {
            return pgm_read_byte(&KNOWN_WIDGETS[i].hasLED);
        }
    }
    return false;
}

void printWidgetName(WidgetType type)
{
    switch (type)
    {
    case W_BAT_D:
        D_PRINT(F("Bat D"));
        break;
    case W_BAT_AA:
        D_PRINT(F("Bat AA"));
        break;
    case W_SND:
        D_PRINT(F("IND SND"));
        break;
    case W_CLR:
        D_PRINT(F("IND CLR"));
        break;
    case W_NSA:
        D_PRINT(F("IND NSA"));
        break;
    case W_IND:
        D_PRINT(F("IND IND"));
        break;
    case W_SIG:
        D_PRINT(F("IND SIG"));
        break;
    case W_CAR:
        D_PRINT(F("IND CAR"));
        break;
    case W_MSA:
        D_PRINT(F("IND MSA"));
        break;
    case W_TRN:
        D_PRINT(F("IND TRN"));
        break;
    case W_BOB:
        D_PRINT(F("IND BOB"));
        break;
    case W_FRK:
        D_PRINT(F("IND FRK"));
        break;
    case W_FRQ:
        D_PRINT(F("IND FRQ"));
        break;
    case W_DVI:
        D_PRINT(F("Port DVI"));
        break;
    case W_PAR:
        D_PRINT(F("Port Paralel"));
        break;
    case W_SER:
        D_PRINT(F("Port Serial"));
        break;
    case W_PS2:
        D_PRINT(F("Port PS/2"));
        break;
    case W_RJ45:
        D_PRINT(F("Port RJ-45"));
        break;
    case W_RCA:
        D_PRINT(F("Port RCA"));
        break;
    case W_COMBO_A:
        D_PRINT(F("COMBO A (DVI+PS/2)"));
        break;
    case W_COMBO_B:
        D_PRINT(F("COMBO B (RJ-45+RCA)"));
        break;
    case W_COMBO_C:
        D_PRINT(F("COMBO C (RJ-45+Ser)"));
        break;
    case W_NOTHING:
        D_PRINT(F("Nic"));
        break;
    case W_UNKNOWN:
        D_PRINT(F("Neznamo"));
        break;
    default:
        D_PRINT(F("???"));
        break;
    }
}

int getIndicatorBitIndex(WidgetType type)
{
    switch (type)
    {
    case W_SND:
        return 0;
    case W_CLR:
        return 1;
    case W_CAR:
        return 2;
    case W_IND:
        return 3;
    case W_FRQ:
        return 4;
    case W_SIG:
        return 5;
    case W_NSA:
        return 6;
    case W_MSA:
        return 7;
    case W_TRN:
        return 8;
    case W_BOB:
        return 9;
    case W_FRK:
        return 10;
    default:
        return -1;
    }
}

void setOneIndicator(int pinIdx, bool toLight)
{
    WidgetType type = foundTypes[pinIdx];
    int pin = WIDGET_PINS[pinIdx];
    if (hasThisTypeLED(type))
    {
        if (toLight)
        {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, HIGH);
        }
        else
        {
            pinMode(pin, INPUT);
        }
    }
    else
    {
        pinMode(pin, INPUT);
    }
}

void scanPeripherals()
{
    D_PRINTLN(F("\n=== DETEKCE PERIFERII ==="));
    batteryCount = 0;
    aaCount = 0;
    dCount = 0;
    totalWidgets = 0;
    foundIndMask = 0;
    for (int i = 0; i < 6; i++)
        portCounts[i] = 0;

    for (int i = 0; i < PIN_COUNT; i++)
    {
        int pin = WIDGET_PINS[i];
        pinMode(pin, INPUT);
        delay(40);
        int val = analogRead(pin);
        WidgetType type = identifyWidget(val);
        foundTypes[i] = type;

        if (type != W_NOTHING && type != W_UNKNOWN)
        {
            totalWidgets++;
        }

        if (type == W_BAT_AA)
        {
            aaCount++;
            batteryCount += 2;
        }
        if (type == W_BAT_D)
        {
            dCount++;
            batteryCount += 1;
        }

        switch (type)
        {
        case W_DVI:
            portCounts[0]++;
            break;
        case W_PAR:
            portCounts[1]++;
            break;
        case W_PS2:
            portCounts[2]++;
            break;
        case W_RJ45:
            portCounts[3]++;
            break;
        case W_SER:
            portCounts[4]++;
            break;
        case W_RCA:
            portCounts[5]++;
            break;
        case W_COMBO_A:
            portCounts[0]++;
            portCounts[2]++;
            break;
        case W_COMBO_B:
            portCounts[3]++;
            portCounts[5]++;
            break;
        case W_COMBO_C:
            portCounts[3]++;
            portCounts[4]++;
            break;
        }

        int bitIdx = getIndicatorBitIndex(type);
        if (bitIdx != -1)
            foundIndMask |= (1 << bitIdx);

        if (type != W_NOTHING)
        {
            D_PRINT(F("Pin A"));
            D_PRINT(i);
            D_PRINT(F(" [ADC:"));
            D_PRINT(val);
            D_PRINT(F("] -> "));
            printWidgetName(type);
            D_PRINTLN("");
        }
    }
    D_PRINTLN(F("========================="));
}

// ==========================================
// FUNKCE KOMUNIKACE (CAN BUS)
// ==========================================
void sendMessage(unsigned long id, byte *data, byte len)
{
    if (CAN.sendMsgBuf(id, 0, len, data) != CAN_OK)
    {
        D_PRINTLN(F("Chyba odesilani CAN"));
    }
}

void sendBroadcastInfo(long odesilanyCas)
{
    byte buf[8] = {0};
    uint16_t t = (uint16_t)constrain(odesilanyCas, 0, 8191);

    buf[0] = t & 0xFF;
    buf[1] = (t >> 8) & 0x1F;
    buf[1] |= (min(snVowelsCount, 7) & 0x07) << 5;
    buf[2] = (min(snDigitsCount, 7) & 0x07) | ((currentState & 0x07) << 3) | ((min(aaCount, 3) & 0x03) << 6);
    buf[3] = (min(dCount, 3) & 0x03) | ((min(portCounts[0], 3) & 0x03) << 2) | ((min(portCounts[1], 3) & 0x03) << 4) | ((min(portCounts[2], 3) & 0x03) << 6);
    buf[4] = (min(portCounts[3], 3) & 0x03) | ((min(portCounts[4], 3) & 0x03) << 2) | ((min(portCounts[5], 3) & 0x03) << 4) | ((!snOdd & 0x01) << 6) | ((snVowel & 0x01) << 7);
    buf[5] = (foundIndMask & 0xFF);
    buf[6] = ((foundIndMask >> 8) & 0x07) | ((litIndMask & 0x1F) << 3);
    buf[7] = (litIndMask >> 5) & 0x3F;

    sendMessage(ID_BOMB_INFO, buf, 8);
}

void registerModule(unsigned long id)
{
    for (int i = 0; i < totalModulesCount; i++)
    {
        if (registeredModules[i] == id)
            return;
    }
    if (totalModulesCount < 16)
    {
        registeredModules[totalModulesCount] = id;
        totalModulesCount++;
        D_PRINT(F("NOVY MODUL: 0x"));
        D_PRINT_HEX(id, HEX);
        D_PRINTLN("");
        printMenu();
    }
}

// ==========================================
// HLAVNÍ SMYČKA FSM
// ==========================================
void loop()
{
    int rawButtons = getButtonState();
    bool isLeftPressed = (rawButtons & BTN_LEFT);
    bool isOkPressed = (rawButtons & BTN_OK);
    bool isRightPressed = (rawButtons & BTN_RIGHT);

    int btnLeft = readButton(isLeftPressed, pressTimeLeft, stateLeft, handledLeft);
    int btnOk = readButton(isOkPressed, pressTimeOk, stateOk, handledOk);
    int btnRight = readButton(isRightPressed, pressTimeRight, stateRight, handledRight);

    switch (currentState)
    {
    case STATE_MENU:
        if (oledUIPopupActive && (millis() - oledUIPopupTimer > 2000))
        {
            oledUIPopupActive = false;
            printMenu();
        }

        static unsigned long lastMenuPing = 0;
        if (millis() - lastMenuPing > 1000)
        {
            lastMenuPing = millis();
            byte dataRst[1] = {0};
            sendMessage(ID_GAME_STATE, dataRst, 1);
        }

        if (CAN_MSGAVAIL == CAN.checkReceive())
        {
            long unsigned int rxId;
            unsigned char len = 0;
            unsigned char buf[8];
            CAN.readMsgBuf(&rxId, &len, buf);
            if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END && buf[0] == 0)
                registerModule(rxId);
        }

        if (btnOk == 2)
        {
            switchToArming();
        }
        else if (btnRight == 2)
        {
            menuPage = (menuPage + 1) % 4;
            oledUIPopupActive = true;
            oledUIPopupTimer = millis();
            printMenu();
        }
        else if (btnLeft == 2)
        {
            menuPage = (menuPage == 0) ? 3 : menuPage - 1;
            oledUIPopupActive = true;
            oledUIPopupTimer = millis();
            printMenu();
        }
        else if (btnLeft == 1)
            adjustValue(-1);
        else if (btnRight == 1)
            adjustValue(1);
        break;

    case STATE_ARMING:
        break;
    case STATE_GAME:
        gameLoop(btnOk);
        break;
    case STATE_WIN:
    case STATE_LOSE:
    case STATE_ENDED:
        handleEndGame(btnOk);
        break;
    }
}

// ==========================================
// AUDIO A ZOBRAZENÍ
// ==========================================
void playSound(int id)
{
    if (settingVolume > 0)
        myDFPlayer.playMp3Folder(id);
}

void drawOled()
{
    u8g2.firstPage();
    do
    {
        u8g2.setFont(u8g2_font_profont29_tr);
        u8g2.drawStr(10, 30, serialNumber);
    } while (u8g2.nextPage());
}

void displayTime(long sekundy)
{
    if (sekundy >= 3600)
    {
        int h = sekundy / 3600;
        int m = (sekundy % 3600) / 60;
        uint8_t data[] = {
            display.encodeDigit(h),
            0b01110100, // Znak 'h'
            display.encodeDigit(m / 10),
            display.encodeDigit(m % 10)};
        display.setSegments(data);
    }
    else
    {
        int m = sekundy / 60;
        int s = sekundy % 60;
        display.showNumberDecEx(m * 100 + s, 0b01000000, true);
    }
}

int getButtonState()
{
    int val = analogRead(PIN_BUTTONS);
    if (val < 50)
        return BTN_NONE;
    int nej = BTN_NONE;
    int minDiff = 1024;
    for (int i = 0; i < 4; i++)
    {
        int cil = pgm_read_word(&targetValues[i]);
        int diff = abs(val - cil);
        if (diff < minDiff)
        {
            minDiff = diff;
            nej = pgm_read_byte(&buttonStates[i]);
        }
    }
    return nej;
}

int readButton(bool isPressed, unsigned long &timer, bool &lastState, bool &handled)
{
    int result = 0;
    if (isPressed && !lastState)
    {
        timer = millis();
        handled = false;
    }
    else if (!isPressed && lastState)
    {
        unsigned long duration = millis() - timer;
        if (duration > 50 && duration < LONG_PRESS_MS && !handled)
            result = 1;
    }
    else if (isPressed && lastState)
    {
        if ((millis() - timer) > LONG_PRESS_MS && !handled)
        {
            result = 2;
            handled = true;
        }
    }
    lastState = isPressed;
    return result;
}

void generateSerialNumber()
{
    const char letters[] = "ABCDEFGHIJKLMNPQRSTUVWXZ";
    const char numbers[] = "0123456789";
    const char everything[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXZ";

    serialNumber[0] = everything[random(sizeof(everything) - 1)];
    serialNumber[1] = everything[random(sizeof(everything) - 1)];
    serialNumber[2] = numbers[random(sizeof(numbers) - 1)];
    serialNumber[3] = letters[random(sizeof(letters) - 1)];
    serialNumber[4] = letters[random(sizeof(letters) - 1)];
    int lastNumber = random(sizeof(numbers) - 1);
    serialNumber[5] = numbers[lastNumber];
    serialNumber[6] = '\0';

    snOdd = (lastNumber % 2 != 0);
    snVowel = false;
    snVowelsCount = 0;
    snDigitsCount = 0;

    for (int i = 0; i < 6; i++)
    {
        char c = serialNumber[i];
        if (c >= '0' && c <= '9')
            snDigitsCount++;
        else if (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U')
        {
            snVowel = true;
            snVowelsCount++;
        }
    }
    D_PRINT(F("SN: "));
    D_PRINTLN(serialNumber);
}

// ==========================================
// START HRY (Arming & Hardware Validace)
// ==========================================
void switchToArming()
{
    currentState = STATE_ARMING;

    if (totalModulesCount == 0)
        D_PRINTLN(F("VAROVANI: Zadne moduly!"));
    else
    {
        D_PRINT(F("HRA STARTUJE. Modulu: "));
        D_PRINTLN(totalModulesCount);
    }

    for (int i = 0; i < PIN_COUNT; i++)
        setOneIndicator(i, false);
    digitalWrite(PIN_LED_ERR1, LOW);
    digitalWrite(PIN_LED_ERR2, LOW);

    randomSeed(millis());
    generateSerialNumber();

    litIndMask = 0;
    for (int i = 0; i < PIN_COUNT; i++)
    {
        WidgetType type = foundTypes[i];
        if (hasThisTypeLED(type))
        {
            bool toLight = random(0, 2);
            gameIndicatorState[i] = toLight;
            if (toLight)
            {
                int bitIdx = getIndicatorBitIndex(type);
                if (bitIdx != -1)
                    litIndMask |= (1 << bitIdx);
            }
        }
        else
        {
            gameIndicatorState[i] = false;
        }
    }

    u8g2.firstPage();
    do
    {
    } while (u8g2.nextPage());

    D_PRINTLN(F("Odesilam CAN CONFIG..."));
    for (int i = 0; i < 3; i++)
    {
        sendBroadcastInfo(settingTimeSeconds);
        delay(20);
    }

    display.clear();
    display.setBrightness(brightnessMap[displayBrightnessIndex]);
    uint8_t seg[] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++)
    {
        seg[i] = SEG_G;
        display.setSegments(seg);
        delay(150);
        seg[i] = 0;
    }
    display.clear();

    D_PRINTLN(F("Odesilam zadost o validaci modulu (STAV 5)..."));
    byte dataPrep[1] = {5};
    sendMessage(ID_GAME_STATE, dataPrep, 1);

    bool validationFailed = false;

    for (int i = 3; i > 0; i--)
    {
        display.showNumberDec(i);
        for (int d = 0; d < 100; d++)
        {
            if (CAN_MSGAVAIL == CAN.checkReceive())
            {
                long unsigned int rxId;
                unsigned char len = 0;
                unsigned char buf[8];
                CAN.readMsgBuf(&rxId, &len, buf);
                if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END && buf[0] == 3)
                {
                    validationFailed = true;
                    break;
                }
            }
            delay(10);
        }
        if (validationFailed)
            break;
    }

    if (validationFailed)
    {
        D_PRINTLN(F("START PRERUSEN: Modul nahlasil chybu!"));
        display.setSegments(SEG_ERR);
        playSound(2);
        delay(2500);
        currentState = STATE_MENU;
        byte dataReset[1] = {0};
        sendMessage(ID_GAME_STATE, dataReset, 1);
        printMenu();
        return;
    }

    display.clear();
    byte dataState[1] = {1};
    for (int i = 0; i < 3; i++)
    {
        sendMessage(ID_GAME_STATE, dataState, 1);
        delay(20);
    }

    playSound(1);
    delay(150);
    playSound(1);
    drawOled();

    for (int i = 0; i < PIN_COUNT; i++)
        setOneIndicator(i, gameIndicatorState[i]);

    currentMistakeCount = 0;
    solvedModulesCount = 0;

    remainingMs = settingTimeSeconds * 1000UL;
    lastLoopMs = millis();

    lastDisplayedSeconds = -1;
    currentState = STATE_GAME;
    blockTickUntil = 0;
}

// ==========================================
// BĚH HRY A HODNOCENÍ VÝSLEDKŮ
// ==========================================
void gameLoop(int btnOkkState)
{
    unsigned long currentMs = millis();
    unsigned long deltaMs = currentMs - lastLoopMs;

    if (deltaMs >= 40)
    {
        lastLoopMs = currentMs;

        int multiplier = 100 + (min(currentMistakeCount, 2) * 25);
        unsigned long deduction = (deltaMs * multiplier) / 100;

        if (remainingMs <= deduction)
        {
            remainingMs = 0;
            displayTime(0);
            endGame(STATE_LOSE, "CAS VYPRSEL!");
            return;
        }
        else
        {
            remainingMs -= deduction;
        }

        long remainingSec = remainingMs / 1000;

        if (remainingSec != lastDisplayedSeconds)
        {
            lastDisplayedSeconds = remainingSec;

            displayTime(remainingSec);

            if (millis() > blockTickUntil)
            {
                playSound(1);
            }

            sendBroadcastInfo(remainingSec);

            byte dataState[1] = {1};
            sendMessage(ID_GAME_STATE, dataState, 1);
        }
    }

    for (int i = 0; i < PIN_COUNT; i++)
        setOneIndicator(i, gameIndicatorState[i]);

    bool lastLife = (currentMistakeCount == settingMaxMistakes - 1);
    if (lastLife && currentMistakeCount > 0)
    {
        bool toLight = (millis() / 250) % 2;
        digitalWrite(PIN_LED_ERR1, toLight);
        if (currentMistakeCount >= 2)
            digitalWrite(PIN_LED_ERR2, toLight);
    }
    else
    {
        digitalWrite(PIN_LED_ERR1, (currentMistakeCount >= 1));
        digitalWrite(PIN_LED_ERR2, (currentMistakeCount >= 2));
    }

    if (CAN_MSGAVAIL == CAN.checkReceive())
    {
        long unsigned int rxId;
        unsigned char len = 0;
        unsigned char buf[8];
        CAN.readMsgBuf(&rxId, &len, buf);

        if (rxId >= ID_MODULE_START && rxId <= ID_MODULE_END)
        {
            byte status = buf[0];
            if (status == 1)
            {
                solvedModulesCount++;
                checkWin();
            }
            else if (status == 2)
            {
                currentMistakeCount++;

                if (currentMistakeCount >= settingMaxMistakes)
                {
                    endGame(STATE_LOSE, "MOC CHYB!");
                }
                else
                {
                    playSound(2);
                    blockTickUntil = millis() + 1500;
                }
            }
        }
    }
    if (btnOkkState == 2)
        endGame(STATE_ENDED, "UKONCENO UZIVATELEM");
}

void endGame(GameState targetState, const char *reason)
{
    currentState = targetState;
    if (targetState == STATE_WIN)
    {
        display.setSegments(SEG_DONE);
        playSound(4);
    }
    else if (targetState == STATE_ENDED)
    {
        display.setSegments(SEG_END);
    }
    else
    {
        display.setSegments(SEG_FAIL);
        playSound(3);
    }

    digitalWrite(PIN_LED_ERR1, (currentMistakeCount >= 1));
    digitalWrite(PIN_LED_ERR2, (currentMistakeCount >= 2));

    byte statusCode = (byte)targetState;
    sendMessage(ID_GAME_STATE, &statusCode, 1);
}

void handleEndGame(int btnOk)
{
    if (btnOk == 2)
    {
        currentState = STATE_MENU;
        for (int i = 0; i < PIN_COUNT; i++)
            setOneIndicator(i, false);

        totalModulesCount = 0;
        for (int i = 0; i < 16; i++)
            registeredModules[i] = 0;

        byte dataReset[1] = {0};
        for (int i = 0; i < 3; i++)
        {
            sendMessage(ID_GAME_STATE, dataReset, 1);
            delay(20);
        }
        printMenu();
    }
}

void checkWin()
{
    if (totalModulesCount > 0 && solvedModulesCount >= totalModulesCount)
    {
        endGame(STATE_WIN, "VSE HOTOVO");
    }
}

// ==========================================
// FUNKCE MENU (Kreslení a nastavování hodnot)
// ==========================================
void adjustValue(int direction)
{
    if (menuPage == 0)
    {
        if (direction > 0)
        {
            if (settingTimeSeconds < 60)
                settingTimeSeconds += 15;
            else if (settingTimeSeconds < 180)
                settingTimeSeconds += 30;
            else if (settingTimeSeconds < 900)
                settingTimeSeconds += 60;
            else
                settingTimeSeconds += 300;
        }
        else
        {
            if (settingTimeSeconds <= 60)
                settingTimeSeconds -= 15;
            else if (settingTimeSeconds <= 180)
                settingTimeSeconds -= 30;
            else if (settingTimeSeconds <= 900)
                settingTimeSeconds -= 60;
            else
                settingTimeSeconds -= 300;
        }
        settingTimeSeconds = constrain(settingTimeSeconds, 15, 7200);
    }
    else if (menuPage == 1)
    {
        settingMaxMistakes += direction;
        settingMaxMistakes = constrain(settingMaxMistakes, 1, 10);
    }
    else if (menuPage == 2)
    {
        settingVolume += direction;
        settingVolume = constrain(settingVolume, 0, 5);
        myDFPlayer.volume(settingVolume * 6);
    }
    else if (menuPage == 3)
    {
        displayBrightnessIndex += direction;
        displayBrightnessIndex = constrain(displayBrightnessIndex, 0, 4);
        display.setBrightness(brightnessMap[displayBrightnessIndex]);
    }

    if (!oledUIPopupActive)
    {
        oledUIPopupActive = true;
        printMenu();
    }
    else
    {
        drawValue();
    }
    oledUIPopupTimer = millis();
}

void drawValue()
{
    if (menuPage == 0)
    {
        displayTime(settingTimeSeconds);
    }
    else if (menuPage == 1)
    {
        display.showNumberDec(settingMaxMistakes);
    }
    else if (menuPage == 2)
    {
        display.showNumberDec(settingVolume);
    }
    else if (menuPage == 3)
    {
        display.showNumberDec(displayBrightnessIndex + 1);
    }
}

void printMenu()
{
    u8g2.firstPage();
    do
    {
        u8g2.setFont(u8g2_font_profont12_tr);

        if (oledUIPopupActive)
        {
            u8g2.setCursor(10, 20);
            if (menuPage == 0)
                u8g2.print(F("Cas Hry"));
            else if (menuPage == 1)
                u8g2.print(F("Limit Chyb"));
            else if (menuPage == 2)
                u8g2.print(F("Hlasitost (0-5)"));
            else if (menuPage == 3)
                u8g2.print(F("Jas Displeje (1-5)"));
        }
        else
        {
            u8g2.setCursor(5, 12);
            u8g2.print(F("Mods:"));
            u8g2.print(totalModulesCount);
            u8g2.setCursor(65, 12);
            u8g2.print(F("Wdg:"));
            u8g2.print(totalWidgets);
            u8g2.setCursor(5, 28);
            u8g2.print(F("Baterie: "));
            u8g2.print(measureBattery());
            u8g2.print(F("%"));
        }
    } while (u8g2.nextPage());

    digitalWrite(PIN_LED_ERR1, LOW);
    digitalWrite(PIN_LED_ERR2, LOW);
    display.clear();
    drawValue();
}

// Výpočet z přesného schématu s 10k/10k děličem s 5V referencí Arduino
int measureBattery()
{
    long sum = 0;
    for (int i = 0; i < 10; i++)
    {
        sum += analogRead(PIN_BATTERY);
        delay(1);
    }
    long avgADC = sum / 10;
    long mv = (avgADC * 10000L) / 1023L;

    if (mv >= 4150)
        return 100;
    if (mv >= 3900)
        return map(mv, 3900, 4150, 75, 100);
    if (mv >= 3700)
        return map(mv, 3700, 3900, 40, 75);
    if (mv >= 3500)
        return map(mv, 3500, 3700, 15, 40);
    if (mv >= 3200)
        return map(mv, 3200, 3500, 0, 15);
    return 0;
}