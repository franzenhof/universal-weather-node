#pragma once
// Pages of the config portal that the normal web interface serves too
// (implemented in portal.cpp, used by web_api.cpp): one copy of the
// embedded decoder and calibration page in flash, one save path.
// Srv: WebServer (config portal) or HttpServer (web interface) - both
// instantiated in portal.cpp.

// GET: the version-matched payload decoder (chirpstack/decoder.js) as a download.
template <class Srv> void portalSendDecoder(Srv& srv);
// GET: the guided wind-direction calibration page (include/calibrate_page.h).
template <class Srv> void portalSendCalibratePage(Srv& srv);
// GET: {"adc":N}, the live wind-vane ADC reading (0 on a board without vane input).
template <class Srv> void portalSendCalibrateSample(Srv& srv);
// POST {"positions":[{"deg":..,"adc":..},...]}: stores the calibration table
// (NVS and gCfg) and switches the wind-direction sensor type to "calibrated".
// Sends the response; true = saved.
template <class Srv> bool portalHandleCalibrateSave(Srv& srv);
