#include "TfaWeatherSensor.h"

SX1262* TfaWeatherSensor::radio = nullptr;
volatile bool TfaWeatherSensor::received = false;

#if defined(ESP32)
void IRAM_ATTR TfaWeatherSensor::onPacket() { received = true; }
#else
void TfaWeatherSensor::onPacket() { received = true; }
#endif

int16_t TfaWeatherSensor::startReceive(Rate rate)
{
    if (!radio) return RADIOLIB_ERR_NULL_POINTER;
    currentRate = rate;
    ConfigFSK_t c;
    c.frequency          = 868.26f;                               // MHz; sensors measured at 868.22-868.30
    c.bitRate            = rate == IT_PLUS_9579 ? 9.579f : 17.241f; // kbit/s
    c.frequencyDeviation = 50.0f;                                 // kHz (TX only)
    c.receiverBandwidth  = 234.3f;                                // kHz: sensors are often 50-75 kHz low
    c.power              = 10;
    c.preambleLength     = 8;                                     // 8-bit detector: some send a short preamble
    int16_t st = radio->beginFSK(c);
    if (st == RADIOLIB_ERR_NONE) st = radio->fixedPacketLengthMode(ITPLUS_FRAME_LEN);
    if (st == RADIOLIB_ERR_NONE) st = radio->setCRC(0);            // CRC-8 checked in software
    uint8_t sync[] = {0x2D, 0xD4};
    if (st == RADIOLIB_ERR_NONE) st = radio->setSyncWord(sync, 2);
    if (st != RADIOLIB_ERR_NONE) return st;
    received = false;
    radio->setPacketReceivedAction(onPacket);
    return radio->startReceive();
}

bool TfaWeatherSensor::getRaw(uint8_t* buf, float& rssi)
{
    if (!radio || !received) return false;
    received = false;
    int16_t st = radio->readData(buf, ITPLUS_FRAME_LEN);
    rssi = radio->getRSSI();
    radio->startReceive();
    return st == RADIOLIB_ERR_NONE;
}

bool TfaWeatherSensor::getMessage(ItPlusFrame& frame, float& rssi)
{
    uint8_t b[ITPLUS_FRAME_LEN];
    return getRaw(b, rssi) && itPlusDecode(b, frame);
}
