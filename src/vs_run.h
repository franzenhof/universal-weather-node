#pragma once
// Virtual sensors, device side (vsensor.h for the pure part and the red
// line): value lookup, scheduling in the loop task, the FSK transmit.
//
// Radio sharing: vsLoop() runs in the loop task right after rfSensorsLoop(),
// the same task that runs the LoRaWAN join and uplinks (sendReceive() blocks
// through the RX windows) - so a frame can never be sent during an uplink or
// its RX windows. Before sending, the FSK reception is stopped through the
// LoRaWAN hand-over hook (loraRadioTaken()); rfSensorsLoop() restarts its
// phase on the next loop. A firmware upload skips the frame ("radio busy").
// In Davis mode (own task, owns the radio) the frame waits for the gap after
// the next Davis packet: the loop task queues it (one per slot), the Davis
// task sends it (vsDavisSendInGap(), vsensor.h VsDavisGap), the loop task
// collects the result on its next turn. A send is held back (at
// most VS_MAX_DEFER_MS) while a received sensor's packet is due
// (rf_schedule.h), so the receiver does not miss it.
#include <stdint.h>
#include "vsensor.h"

bool vsActive();                   // switched on and at least one slot enabled
void vsLoop();                     // loop task, after rfSensorsLoop()
void vsNoteLoraAirtime(uint32_t ms);   // a LoRaWAN uplink's airtime (same 1 % band)
void vsNodeMac(uint8_t mac[6]);    // the MAC the IDs are derived from
// A received sensor has the same ID as virtual sensor slot (WH31 IDs have only
// 8 bits, IT+ only 6 - vsIdCollides()): shown as a warning; the ID stays
// stable (a gateway / station is paired to it).
bool vsIdReceived(const VsSlotCfg& c, int slot);
uint64_t vsNowMs();                // the scheduling / airtime clock (64-bit ms)

// Davis receive task, right after an accepted packet that confirmed the hop
// prediction: sends waiting frames while vsDavisGapAllows() (oldest first, at
// most VS_DAVIS_MAX_FRAMES). Returns the frames put on air; > 0 = the radio
// left the Davis setup, the caller sets it up again (DavisRadio::begin()).
int  vsDavisSendInGap(const VsDavisGap& g);
void vsDavisNoteGapUse(uint32_t us);   // gap use measured by the Davis task -> gVsDavisGapMaxUs
