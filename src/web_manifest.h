#pragma once
// Web app manifest (/manifest.webmanifest) for the home screen / installed
// app: name = device name, icons, colours of the dashboard palette. URLs are
// relative to the manifest, so it also works behind a reverse proxy.
// Host-tested (test/firmware_manifest.cpp).
#include <ArduinoJson.h>
#include "config.h"

void buildManifest(JsonDocument& doc, const Config& c);
