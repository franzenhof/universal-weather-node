#include "FineOffsetSensor.h"

SX1262* FineOffsetSensor::radio = nullptr;
volatile bool FineOffsetSensor::received = false;
volatile uint32_t FineOffsetSensor::irqMs = 0;
uint32_t FineOffsetSensor::readDelayMs = 0;

#if defined(ESP32)
void IRAM_ATTR FineOffsetSensor::onPacket() { irqMs = millis(); received = true; }
#else
void FineOffsetSensor::onPacket() { irqMs = millis(); received = true; }
#endif

int16_t FineOffsetSensor::startReceive(const RxConfig& cfg)
{
    if (!radio) return RADIOLIB_ERR_NULL_POINTER;
    frameLen = cfg.frameLen ? cfg.frameLen : FO_MAX_FRAME_LEN;
    ConfigFSK_t c;
    c.frequency          = cfg.frequencyMhz;
    c.bitRate            = 17.241f;     // kbit/s: 58 us per bit
    c.frequencyDeviation = 50.0f;       // kHz (TX only)
    c.receiverBandwidth  = cfg.bandwidthKhz;
    c.power              = 10;
    c.preambleLength     = 8;           // detector length; the sensors send 32-48 bits
    int16_t st = radio->beginFSK(c);
    if (st == RADIOLIB_ERR_NONE) st = radio->fixedPacketLengthMode(frameLen);
    if (st == RADIOLIB_ERR_NONE) st = radio->setCRC(0);             // CRC-8 + sum checked in software
    uint8_t sync[] = {0x2D, 0xD4};
    if (st == RADIOLIB_ERR_NONE) st = radio->setSyncWord(sync, 2);
    if (st != RADIOLIB_ERR_NONE) return st;
    received = false;
    radio->setPacketReceivedAction(onPacket);
    return radio->startReceive();
}

bool FineOffsetSensor::getRaw(uint8_t* buf, size_t cap, size_t& len, float& rssi)
{
    if (!radio || !received) return false;
    received = false;
    readDelayMs = millis() - irqMs;
    len = frameLen <= cap ? frameLen : cap;
    uint8_t b[256];
    int16_t st = radio->readData(b, frameLen);
    rssi = radio->getRSSI();
    radio->startReceive();
    if (st != RADIOLIB_ERR_NONE) return false;
    for (size_t i = 0; i < len; i++) buf[i] = b[i];
    return true;
}

bool FineOffsetSensor::getMessage(FoFrame& frame, float& rssi)
{
    uint8_t b[FO_MAX_FRAME_LEN];
    size_t len;
    return getRaw(b, sizeof(b), len, rssi) && foDecode(b, len, frame, wh24Units);
}
