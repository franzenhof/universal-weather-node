#pragma once
#include <stddef.h>
// Davis Vantage Pro2 / Vue (EU 868 MHz) - exclusive receiver mode.
//
// With *Wireless sensor type* = Davis (gCfg.rfProto == RF_PROTO_DAVIS) a
// FreeRTOS task owns the SX1262 and follows the transmitter's hop sequence
// (lib/DavisSensorReceiver: DavisHopper + DavisRadio). LoRaWAN is paused and
// no other wireless protocol is received while it runs. Virtual sensors: the
// task sends their waiting frames in the gap right after a packet that
// confirmed the hop prediction (vs_run.h vsDavisSendInGap()). The main loop
// (davisRxLoop(), from rfSensorsLoop()) applies the settings live, takes the
// decoded packets into the wireless-sensor table (ID DA0000ii) and writes the
// diagnostics (gDavisDiag, davis_diag.h) and the sniffer log.

// Call every loop() while Davis mode is selected: starts the receive task,
// applies changed settings, processes received packets.
void davisRxLoop();
// Davis mode left: stops the task (waits until it released the radio, up to
// 10 s; a task stuck in a radio call restarts the node rather than being
// deleted while it may hold the SPI bus).
void davisRxStop();
// The task owns the radio (from start until davisRxStop() returned).
bool davisRadioOwned();
// LoRaWAN must not use the radio: Davis mode selected or still running.
bool davisLoraPaused();
// The sniffer capture (every packet line and event since Davis mode started)
// is the radio trace (rf_trace.h): Davis mode starts an open-ended one unless
// a trace is already running; /api/davis/log serves it.
