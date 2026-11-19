#include "heltec_board.h"
#include "radio_board.h"
#include "serial_log.h"
#include <Arduino.h>
#include <SPI.h>
#include <esp_flash.h>
#include <driver/gpio.h>
#include <soc/efuse_reg.h>

BoardInfo gBoard;

namespace {
constexpr int PIN_VFEM = 7, PIN_FEM_CSD = 2, PIN_GC1109_CPS = 46, PIN_KCT8103L_CTX = 5;
constexpr unsigned VFEM_SETTLE_MS = 5;   // Meshtastic: "arbitrary 5 ms for FEM rail power-up"

int  femCtrlPin = -1;      // GPIO46 (GC1109) or GPIO5 (KCT8103L)
bool femAsleep  = false;   // shut down by BoardSX1262::sleep(), pads held
bool vfemOn     = false;

void femHold(bool on)
{
    for (int p : { PIN_VFEM, PIN_FEM_CSD, femCtrlPin }) {
        if (p < 0) continue;
        if (on) gpio_hold_en((gpio_num_t)p);
        else    gpio_hold_dis((gpio_num_t)p);
    }
}

// Before the first RF switch write after sleep: release the pad holds and
// power the FEM rail again (KCT8103L; the GC1109 keeps it on).
void femWake()
{
    femAsleep = false;
    femHold(false);
    if (!vfemOn) {
        digitalWrite(PIN_VFEM, HIGH);
        vfemOn = true;
        delay(VFEM_SETTLE_MS);
    }
}

void femSleep()
{
    if (gBoard.fem == FEM_NONE) return;
    digitalWrite(PIN_FEM_CSD, LOW);
    digitalWrite(femCtrlPin, LOW);
    if (gBoard.fem == FEM_KCT8103L) { digitalWrite(PIN_VFEM, LOW); vfemOn = false; }
    // keep the levels through light sleep (low_power.cpp)
    femHold(true);
    femAsleep = true;
}

// RadioLib writes the RF switch pins through the HAL: CSD (first column of
// the table) going HIGH while the FEM sleeps wakes it first.
class BoardHal : public ArduinoHal {
public:
    explicit BoardHal(SPIClass& spi) : ArduinoHal(spi) {}
    void digitalWrite(uint32_t pin, uint32_t value) override {
        if (pin == RADIOLIB_NC) return;
        if (femAsleep && pin == (uint32_t)PIN_FEM_CSD && value == HIGH) femWake();
        ::digitalWrite(pin, value);
    }
};

// RF switch table: columns CSD, CPS (GC1109) / CTX (KCT8103L). DIO2 does the
// TX/RX select inside the FEM by itself (RadioLib enables DIO2 as RF switch
// in begin()/beginFSK()).
uint32_t femPins[Module::RFSWITCH_MAX_PINS] = { RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC };
const Module::RfSwitchMode_t femTable[] = {
    { Module::MODE_IDLE, { HIGH, LOW } },
    { Module::MODE_RX,   { HIGH, LOW } },    // LNA
    { Module::MODE_TX,   { HIGH, HIGH } },   // PA
    END_OF_MODE_TABLE,
};
}

// The radio instance (heltec_unofficial.h's is disabled in main.cpp): same
// pins and HSPI bus as there (heltec_setup() calls hspi->begin()).
SPIClass* hspi = new SPIClass(HSPI);
static BoardHal boardHal(*hspi);
BoardSX1262 radio(new Module(&boardHal, 8 /*NSS*/, 14 /*DIO1*/, 12 /*RST*/, 13 /*BUSY*/));

int16_t BoardSX1262::setOutputPower(int8_t power)
{
    return SX1262::setOutputPower(femChipPowerDbm(power, gBoard.fem), true);
}

int16_t BoardSX1262::sleep()
{
    int16_t state = SX1262::sleep();
    femSleep();
    return state;
}

void boardDetectHardware()
{
    uint32_t size = 0;
    if (esp_flash_get_physical_size(esp_flash_default_chip, &size) != ESP_OK) size = 0;
    if (!size) size = ESP.getFlashChipSize();   // JEDEC ID read by the ROM
    gBoard.flashBytes = size;
    gBoard.psramCap   = (int)((REG_READ(EFUSE_RD_MAC_SPI_SYS_4_REG) >> 3) & 3);   // BLK1 bit 131
    gBoard.detected   = boardDetect(size);
}

void boardApply(int boardSel)
{
    gBoard.autoSel = boardSel == BOARD_SEL_AUTO;
    gBoard.kind    = boardResolve(boardSel, gBoard.detected);
    dbgSerial.printf("Board: Heltec %s (%s; flash %lu MB, PSRAM cap %d)%s\n", boardName(gBoard.kind),
                     gBoard.autoSel ? "auto" : "set manually", (unsigned long)(gBoard.flashBytes >> 20), gBoard.psramCap,
                     boardIsR8() ? " - V4-R8 (ESP32-S3R8) is NOT supported" : "");
}

void femInit()
{
    if (!boardIsV4()) return;
    gpio_hold_dis((gpio_num_t)PIN_VFEM);
    gpio_hold_dis((gpio_num_t)PIN_FEM_CSD);
    pinMode(PIN_VFEM, OUTPUT);
    digitalWrite(PIN_VFEM, HIGH);
    vfemOn = true;
    delay(1);
    pinMode(PIN_FEM_CSD, INPUT);
    delay(1);
    gBoard.fem = digitalRead(PIN_FEM_CSD) == HIGH ? FEM_KCT8103L : FEM_GC1109;
    femCtrlPin = gBoard.fem == FEM_KCT8103L ? PIN_KCT8103L_CTX : PIN_GC1109_CPS;
    gpio_hold_dis((gpio_num_t)femCtrlPin);
    pinMode(PIN_FEM_CSD, OUTPUT);
    digitalWrite(PIN_FEM_CSD, HIGH);
    pinMode(femCtrlPin, OUTPUT);
    digitalWrite(femCtrlPin, LOW);
    if (gBoard.fem == FEM_KCT8103L) delay(VFEM_SETTLE_MS);
    femPins[0] = PIN_FEM_CSD;
    femPins[1] = (uint32_t)femCtrlPin;
    radio.setRfSwitchTable(femPins, femTable);
    dbgSerial.printf("Heltec V4 RF front end: %s (V4.%d), VFEM GPIO7, CSD GPIO2, %s GPIO%d\n", femName(gBoard.fem),
                     gBoard.fem == FEM_KCT8103L ? 3 : 2, gBoard.fem == FEM_KCT8103L ? "CTX" : "CPS", femCtrlPin);
}
