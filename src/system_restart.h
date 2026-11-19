#pragma once
// Every planned restart goes through here (OTA success, restart command,
// settings that need a restart, factory reset, portal exit): first saves the
// rain statistics to flash (rainStatsFlush(), only with gCfg.rainPersist and
// only if changed since the last copy), then ESP.restart(). reason: log text.
// Implemented in wx_upload.cpp (the rain record lives there).
void systemRestart(const char* reason);

// Writes the rain record (daily + week/month/year + pending) to NVS now if it
// changed since the last copy - at most once per change. No-op with
// gCfg.rainPersist off or before wxUploadSetup(). Safe from any task.
void rainStatsFlush();
