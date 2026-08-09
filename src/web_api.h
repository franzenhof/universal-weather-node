#pragma once

// Registers routes and spawns the dedicated web-server FreeRTOS task (core
// 0, see web_api.cpp's webApiTask) — call once from setup() regardless of
// webApiEn. That task starts/stops the listening socket itself based on
// wifiEn/webApiEn and current WiFi state, and calls server.handleClient() in
// a loop; nothing needs to be called from the main loop() for this anymore.
void webApiSetup();

// Stops the listening socket immediately, freeing port 80. Call before
// entering the WiFiManager config portal — its own captive-portal web
// server also binds port 80, and having both listening at once is at best
// undefined (whichever gets the incoming connection) and at worst leaves
// the portal's server unable to bind at all, since our own socket is
// already sitting on that port. The web-server task will happily re-open it
// after a restart if wifiEn/webApiEn are still on; no need to call this
// outside of that one case. Thread-safe against the web-server task (see
// serverMutex in web_api.cpp).
void webApiStop();
