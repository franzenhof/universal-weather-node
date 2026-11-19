#pragma once
// The shared SX1262 (LoRaWAN + wireless sensors), defined in heltec_board.cpp
// instead of heltec_unofficial.h (HELTEC_NO_RADIO_INSTANCE in main.cpp), so
// the Heltec V4's front-end module (heltec_board.h) can be handled for every
// user of the radio:
//  - RX/TX switching of the FEM: RadioLib's RF switch table (set by femInit()),
//    applied by RadioLib itself on every standby/RX/TX - LoRaWAN, Bresser,
//    IT+ and Fine Offset alike;
//  - TX power: setOutputPower() takes the wanted radiated power and sets the
//    SX1262 lower by the PA gain (femChipPowerDbm());
//  - sleep(): FEM shut down, re-enabled by the first RF switch write after
//    it (BoardHal).
// On a V3 all of this is a pass-through.
#include <LoRaWAN_ESP32.h>   // RadioLib on the target; the host fake defines HOST_FAKE_RADIO

#ifndef HOST_FAKE_RADIO
class BoardSX1262 : public SX1262 {
public:
    explicit BoardSX1262(Module* mod) : SX1262(mod) {}
    using SX1262::setOutputPower;
    using SX1262::sleep;
    int16_t setOutputPower(int8_t power) override;
    int16_t sleep() override;
};
#endif

extern BoardSX1262 radio;
