#pragma once
// Pages of the config portal that the normal web interface serves too
// (implemented in portal.cpp, used by web_api.cpp): one copy of the
// embedded decoder and calibration page in flash, one save path.
#include <WebServer.h>

// GET: the version-matched payload decoder (chirpstack/decoder.js) as a download.
void portalSendDecoder(WebServer& srv);
// GET: the guided wind-direction calibration page (include/calibrate_page.h).
void portalSendCalibratePage(WebServer& srv);
// GET: {"adc":N}, the live wind-vane ADC reading (0 on a board without vane input).
void portalSendCalibrateSample(WebServer& srv);
// POST {"positions":[{"deg":..,"adc":..},...]}: stores the calibration table
// (NVS and gCfg) and switches the wind-direction sensor type to "calibrated".
// Sends the response; true = saved.
bool portalHandleCalibrateSave(WebServer& srv);
