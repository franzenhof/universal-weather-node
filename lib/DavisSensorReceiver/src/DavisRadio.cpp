#include "DavisRadio.h"
#include <esp_timer.h>

SX1262* DavisRadio::radio = nullptr;
volatile bool DavisRadio::received = false;
volatile int64_t DavisRadio::rxTimeUs = 0;
void (*DavisRadio::packetHook)() = nullptr;

void IRAM_ATTR DavisRadio::onPacket()
{
    rxTimeUs = esp_timer_get_time();
    received = true;
    if (packetHook) packetHook();
}

int16_t DavisRadio::begin(float freqMHz, float rxBwKhz)
{
    if (!radio) return RADIOLIB_ERR_NULL_POINTER;
    ConfigFSK_t c;
    c.frequency          = freqMHz;
    c.bitRate            = 19.2f;       // kbit/s (measured 19.23)
    c.frequencyDeviation = 9.9f;        // kHz; measured 9.2-10 kHz
    c.receiverBandwidth  = rxBwKhz;
    c.power              = 10;          // never transmits
    c.preambleLength     = 16;          // detector length: 16 of the 32 preamble bits
    int16_t st = radio->beginFSK(c);
    if (st == RADIOLIB_ERR_NONE) st = radio->fixedPacketLengthMode(DAVIS_PACKET_LEN);
    if (st == RADIOLIB_ERR_NONE) st = radio->setCRC(0);            // CRC-16 checked in software
    if (st == RADIOLIB_ERR_NONE) st = radio->setWhitening(false);
    if (st == RADIOLIB_ERR_NONE) st = radio->setDataShaping(RADIOLIB_SHAPING_0_5);
    uint8_t sync[] = { 0xCB, 0x89 };
    if (st == RADIOLIB_ERR_NONE) st = radio->setSyncWord(sync, 2);
    if (st != RADIOLIB_ERR_NONE) return st;
    received = false;
    radio->setPacketReceivedAction(onPacket);
    return radio->startReceive();
}

int16_t DavisRadio::tune(float freqMHz)
{
    if (!radio) return RADIOLIB_ERR_NULL_POINTER;
    radio->standby();
    int16_t st = radio->setFrequency(freqMHz);
    if (st != RADIOLIB_ERR_NONE) return st;
    return radio->startReceive();
}

int16_t DavisRadio::restart()
{
    if (!radio) return RADIOLIB_ERR_NULL_POINTER;
    return radio->startReceive();
}

void DavisRadio::standby()
{
    if (radio) radio->standby();
    received = false;
}

bool DavisRadio::read(uint8_t buf[DAVIS_PACKET_LEN], float& rssi, int64_t& rxUs)
{
    if (!radio || !received) return false;
    received = false;
    rxUs = rxTimeUs;
    int16_t st = radio->readData(buf, DAVIS_PACKET_LEN);
    rssi = radio->getRSSI();
    return st == RADIOLIB_ERR_NONE;
}
