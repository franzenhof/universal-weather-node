# Installation and First Setup

How to put the firmware on a new Heltec board from the browser, set the node up in its config portal and update it later over Wi-Fi.

[← back to README](../README.md)

## Installing the Firmware (browser, no software)

The firmware is installed **once over USB, directly from the web browser** — no PlatformIO, no Python, no drivers on most computers. After that, updates go over Wi-Fi ([OTA](#firmware-update-ota)).

> **Heltec WiFi LoRa 32 V4 only** (ESP32-S3R2, 16 MB flash, 2 MB PSRAM). On a V3 or a V4-R8 this firmware stops with a message on the display — the **V3** is legacy and runs firmware **3.x** (branch `3.x`, bug fixes only, no new functions) — for the full feature set use a V4. The RF front end (V4.2 GC1109 / V4.3 KCT8103L) is switched automatically, and the radio's TX power is reduced by the amplifier gain so the radiated power stays at the LoRaWAN regional limit (EU868: 16 dBm). The V4 runs [without PCB](minimal-build.md) or on the custom PCB [V4 Full or V4 Light](pcb.md#which-board) (detected by the revision ID on GPIO15). The serial log goes over the native USB. **Coming from 2.x or 3.x:** the partition table changed (16 MB layout) — see [Coming from 2.x or 3.x](#coming-from-2x-or-3x): make a settings backup, flash the `-factory` image over USB once, then restore the backup.

**Two files per release in [`firmware/`](../firmware/) — pick the right one:**

| File | Contains | Use it for | How |
|------|----------|------------|-----|
| `universal-weather-node-<version>-factory.bin` | bootloader + partition table (with `cfgdata`) + firmware, one complete image | **first installation** of a new board, or repairing a board that no longer starts | over **USB**, in the browser (steps below) or esptool at address **`0x0`** |
| `universal-weather-node-<version>.bin` | the firmware (application) only | **updates** of a node that already runs this firmware | over **Wi-Fi** ([OTA](#firmware-update-ota), portal → *Firmware update*), or USB at address `0x10000` |

> **Which file for OTA?** A node running 4.x accepts **both** files: of a `-factory` file only the firmware part is written (bootloader and partition table can't be changed over OTA; if the file's partition table differs from the node's, the upload is refused — flash it over USB then). When in doubt, take the file without `-factory`.

**You need:** a computer with **Chrome or Edge** (desktop; Firefox and Safari can't talk to USB devices), a **USB-C data cable** (some cables only charge — if no device shows up, try another one), the Heltec WiFi LoRa 32 V4 with its **868 MHz antenna screwed on** (never power the board without the antenna).

1. **Download the firmware.** In [`firmware/`](../firmware/) click the newest **`universal-weather-node-<version>-factory.bin`** and then the **download button** (*Download raw file*), or take the file from the release's *Assets*. Use the **`-factory`** file (see the table above).
   - **Check the file:** a `-factory.bin` is about 2–3 MB. A file of a few hundred KB, or one that opens as text, is the GitHub **web page** that some browsers (often Firefox with "Save link as …") save instead of the firmware — download it again with *Download raw file* or from the release assets.
2. **Connect the board.** Plug the Heltec into the computer with the USB-C cable. On a V4 Full PCB: disconnect the external power and remove JP8 first.
3. **Open the flasher.** In Chrome or Edge go to **[web.esphome.io](https://web.esphome.io)** (ESPHome's web flasher — it works for any ESP32 firmware, ESPHome itself is not needed).
4. Click **CONNECT** and choose the serial port of the board in the browser's list — the V4 has no USB-serial chip, it shows up with the ESP32-S3's own USB, e.g. *USB JTAG/serial debug unit*, *cu.usbmodem…* (macOS) or *COM3* (Windows) — then *Connect*.
   - **No port in the list?** Try another cable (a data cable, not a charge-only one) or USB port; no driver is needed.
5. Click **INSTALL**, choose the downloaded **`…-factory.bin`** and confirm with **INSTALL**. Wait until *Installation complete* (about 1 minute). Don't unplug the board meanwhile.
   - **Another firmware ran on the board before** (e.g. Meshtastic)? Tick **Erase device** when asked, so no data of the old firmware is left in the flash.
   - **Fails to connect / "failed to enter bootloader"?** Put the board into download mode by hand: hold the **PRG** button, press and release **RST**, release **PRG** — then click *INSTALL* again.
6. Press **RST** on the board (or unplug and plug in again). The display shows **Config Portal** — continue with [Config Portal](#config-portal): connect your phone to the Wi-Fi **`WeatherNode`** and open http://192.168.4.1.

**Good to know**
- **Chrome or Edge on a desktop computer** is needed for flashing in the browser (Web Serial); Firefox, Safari and phones cannot do it.
- **The first Wi-Fi connection can take up to a minute** after the portal saved (the router's DHCP has to hand out an address); the display shows the IP address once it is connected.
- **Reopen the portal at any time:** hold the **PRG** button for **3 s** (the display shows *Config Portal*), then connect to the Wi-Fi `WeatherNode` again.
- Our own partition layout (with the `cfgdata` settings partition) is part of the `-factory` file, so nothing has to be set up by hand. Settings stored by an earlier installation of 4.x are kept — except the **Wi-Fi network**: the `-factory` image also overwrites the area where the Wi-Fi driver stores it, so open the config portal with the **PRG** button (3 s; it does not open by itself, the settings are still there) and choose it again. (*Erase device* in the flasher erases everything.)
- **Alternative flasher:** Espressif's [esptool-js](https://espressif.github.io/esptool-js/) (also in Chrome/Edge): *Connect*, then *Flash Address* **`0x0`**, choose the `-factory` file, *Program*. The old desktop *ESPHome-Flasher* program is no longer maintained — use the web flasher instead.
- **Updating later:** use [OTA](#firmware-update-ota) — with the normal `universal-weather-node-<version>.bin` (always safe) or with the `-factory` file. The `-factory` file over USB also works and keeps the settings, except the Wi-Fi network (choose it again in the config portal, PRG 3 s).
- **A browser/USB flasher always needs the `-factory` file.** web.esphome.io and esptool-js write the chosen file as a complete image at address `0x0`, so an update over USB must use `-factory` too. The normal `.bin` (without `-factory`) is an **application-only** image for the node's own [OTA update page](#firmware-update-ota) — flashing it at `0x0` with a browser flasher lands at the wrong offset and the board no longer starts (blinking display). The fix is simply to re-flash the `-factory` file; to update *without* a cable, use the OTA page instead.

No Wi-Fi where the node stands (e.g. a model airfield)? After the first setup it can open a Wi-Fi network of its own for the dashboard: [At the Airfield](airfield.md).

## Coming from 2.x or 3.x

A node that runs firmware **2.x or 3.x** (Heltec V4 only — a V3 stays on 2.x / 3.x) changes to **4.x** with **one USB installation**. There is no update over Wi-Fi for this step (see below). Plan about 15 minutes; the node is off the air meanwhile.

1. **Make a settings backup in the old firmware.** *Settings → System → Backup and restore → Download settings* ([details](settings.md#backup-and-restore)). Without **include passwords and keys** the file holds no secrets: after the restore you enter the Wi-Fi, admin, MQTT, weather-network and alarm secrets again (LoRaWAN AppKey too). With the box ticked the file contains them in plain text — keep it private, and note that the restore then also brings back the old admin password. Keep the file safe: the next step erases the node.
2. **Install the 4.x `-factory.bin` over USB**, in the browser with the web flasher, exactly as in [Installing the Firmware](#installing-the-firmware-browser-no-software). This writes the new partition table and **erases the node's settings and the stored Wi-Fi** — that is why the backup comes first.
3. **Wi-Fi in the config portal.** The node opens the portal `WeatherNode` ([Config Portal](#config-portal)); choose your network, save, then go through the [setup wizard](#setup-wizard) (admin password).
4. **Restore the backup.** *Settings → System → Backup and restore → Restore settings from a file*. Invalid values are listed and keep their old value; the next settings page shows what changed.
5. **Wireless build with a BME280 (no PCB):** 4.x expects it on **GPIO47/48** ([wiring](minimal-build.md)). Left on the old pins 41/42 it keeps working while no GNSS module is switched on — the status page then says *on the old pins 41/42*; move it before you add a GNSS module.
6. **Mark your wireless sensors ★ again and check the value sources** (*Settings → Sensors*; give each a custom ID and name if you like — see [Custom ID, name, re-bind](wireless-sensors.md#mine-received-hidden)) and the status page: are all sensors received, do the values look right?

**What is not carried over**
- Settings that 4.x does not know are dropped silently by the restore: `board` and `pcbver` (4.x is V4 only and detects the PCB itself) and the I/O fields (heater, fan, AUX, UV, VE.Direct …) when the node has no V4 Full PCB.
- The **LoRaWAN session** is not in the backup: the node joins again. The AppKey is only in the file if you ticked *include passwords and keys*.
- The **chart history** starts empty (the 4.x history lives in its own partition), and the rain statistics and records are not in the backup either.
- The **★ list** and everything that referred to a wireless sensor by its radio ID — value sources, wireless alarm rules, custom series — start empty: 4.x keeps them on [logical sensors](wireless-sensors.md#mine-received-hidden) (internal ID + custom ID) and does not convert the 2.x / 3.x entries. Placeholders `{rf<ID>_…}` / `{rfN_…}` keep working.
- **Outputs name wireless sensors by their custom ID** (a sensor that is only received: `rf<radio ID>`, e.g. `rf17000023`): MQTT `<prefix>/rf/<custom ID>`, **Loxone `<custom ID>_temp` instead of `rfN_temp`** (rename the inputs once), InfluxDB tag / `/metrics` label `sensor` instead of `id`. **Home Assistant** gets new devices for the ★ sensors; the old ones (by radio ID) are removed at the first MQTT connection.
- Wind-direction calibration table: not in the backup — run the [wizard](settings.md#wind-direction-calibration) again if you use it.

**Why not over the air?** The 2.x / 3.x partition table has two 3.2 MB application slots; 4.x needs two 6 MB slots and a `history` partition. Over the air only the application can be written, never the partition table. So:
- **3.0.0 and later** (the successor of 2.0.5): the update check never offers 4.x, and the upload page refuses a 4.x file (it recognises the ESP-IDF 5 descriptor).
- **Older 2.x:** it accepts the plain `.bin`. 4.x then starts, finds the old partition table, shows a message that this node has the 2.x partition table and needs the `-factory.bin` over USB, and **goes back to the 2.x firmware by itself after 10 s** — nothing is lost, the old firmware and its settings are still there. Then do the steps above.

## Config Portal

**First boot:** a node that has never been configured opens the portal automatically.

1. The display shows **"Config Portal"** with the network name and URL.
2. Connect your phone or laptop to the Wi-Fi network **`WeatherNode`** (no password).
3. Open **http://192.168.4.1** (most devices open it automatically).
4. Choose your network under *Configure WiFi* (that page shows only the network), click *Save & restart* — the node saves, restarts and joins your Wi-Fi (also when the network is not in range yet: it keeps trying; hold PRG 3 s to open the portal again). *Back* returns to the start page without saving. The node's other fields are on their own page, **Setup** on the portal's start page; everything there can also be set later in the web settings or the setup wizard.
5. Open the address shown on the display (`http://<ip>/` or `http://<device-name>.local/`) from a device in the same network: the [setup wizard](#setup-wizard) continues there.

**Language:** the portal's own texts (parameter labels, the start-page buttons, the firmware-update and wind-calibration pages) follow the *Language* setting (field `uilang`, e.g. `2` = Deutsch; takes effect the next time the portal opens). With *Auto* the portal is English — it cannot ask the browser before showing the form. WiFiManager's built-in pages (*Configure WiFi*, *Info*, *Exit*, the Wi-Fi list) are always English.

**Opening it again later:**

| How | |
|-----|---|
| **Button** | Hold the user button for **at least 3 seconds** during operation |
| **LoRaWAN downlink** | Send byte `0x03` (takes effect after the current send cycle) |

> Don't hold the button while powering on or resetting — that starts the ESP32 bootloader instead of the firmware.

- **Passwords and keys** (AppKey, web settings admin password, MQTT password) are not shown in the portal: the field stays empty with *(unchanged)*; leave it empty to keep the stored value, or tick *Remove* to clear a password. The portal network itself stays open (no Wi-Fi password) so a node can always be recovered — close the portal when you are done.
- *Save* on the parameter page is enough. Only use **Configure WiFi** if you want to change the Wi-Fi network or a static IP. The network is stored even if it is not in range (pre-provisioning a node for its final site): the portal reports the failed connection, and after closing it the node keeps trying that network. In normal operation the [web settings page](settings.md#web-settings-page) can change the network too.
- If the portal is left untouched for 3 minutes, it closes and the node restarts. (The timer restarts with every page you open.)
- The start page also offers the version-matched **payload decoder** download, the **help page** and the **wind-direction calibration** wizard. In normal operation the [web settings page](settings.md#web-settings-page) offers the decoder, the [calibration wizard](settings.md#wind-direction-calibration) and [switching Wi-Fi / the web interface off](settings.md#switching-wi-fi-or-the-web-interface-off) too.
- The portal stays necessary for the **first setup** and for a node **without Wi-Fi or web interface** (LoRaWAN-only): it is then the only way to change its settings or switch Wi-Fi back on.
- The normal sampling/transmit loop pauses while the portal is open. Heater/fan outputs are forced off until restart; do not use the portal/update process when continuous output control is required.

<!-- help:skip -->
<p align="center">
  <img src="../pictures/wifi-config-startpage.png" alt="Config portal start page with the main menu" width="50%">
  <br><strong>Config portal: start page</strong>
</p>
<!-- /help:skip -->

## Setup Wizard

After the first start (and after a [reset](settings.md#reset)) the first visit of the dashboard opens the **setup wizard** (`/setup`) — in the browser's language. It opens once per browser session and start: after any restart (a reset, *Erase everything*) it comes again until it is finished or closed with *Later*. The browser remembers it in the session cookie `wnSetup` (an ID of the start, nothing else). The web pages use only such technically needed cookies (`wnSetup`, `wnDev` / `wnLang` for the language); they stay between your browser and the node, no tracking, so there is no cookie banner. It never locks you out: every step has **Skip**, **Back** and **Later** (closes the wizard for good), and nothing in it is required.

| Step | What you do |
|------|-------------|
| 1 Language | Language of the web pages and the portal (prefilled from the browser) |
| 2 Your weather station | The node lists the wireless sensors it hears (refreshes every 10 s; while *Auto* still searches it says so). Mark yours with **★** (fixed numbers, a Home Assistant device each), hide neighbours' sensors with **✕** — or *I have no wireless sensors* switches reception off |
| 3 Location | Station name (also its network name `<name>.local`), **altitude** (needed for the sea-level pressure), latitude / longitude (optional, *Use my position* only where the browser allows it — on plain `http://` usually not, then type it in) and the time zone (suggested from the browser for the common zones) |
| 4 Units | Metric / imperial, wind in m/s or km/h |
| 5 Admin password | Strongly recommended — see below |
| 6 Outputs (optional) | MQTT / Home Assistant, weather networks, Loxone — each in its own collapsible section |
| 7 Summary | What was set, links to dashboard, status and settings, **Finish** |

Every step is saved through the normal settings handler (same checks as the settings page; invalid values are shown on the same step). Settings that need a restart (station name, MQTT …) are saved at once but applied by **one restart at *Finish***.

**Security:** while **no admin password is set and the wizard is not finished**, the wizard — and the settings it posts — works **without a login**, so a new node can be set up from any device in your network (as the open config portal before it). Every page of the wizard warns about this until step 5. Once a password is set the browser asks for it (user `admin`); once the wizard is finished or closed, a node without a password keeps the settings page locked as before. Start the wizard again any time under **Settings → System → Setup wizard** (parameter 58 = 0 offers it again on the next dashboard visit).

## Firmware Update (OTA)

### In the web interface

With a **web settings admin password** set: **Settings → System → Firmware update**, choose **`universal-weather-node-<version>.bin`** from [`firmware/`](../firmware/) (the `-factory.bin` works too; only its firmware part is written) and click **Upload and install**. A progress bar shows the upload; the node checks the file while it streams it into the inactive OTA partition, verifies it and restarts (about 20 s; rain statistics, records and all settings are kept). The page then opens the status page — check the firmware version there. Files that are not firmware, a factory file with a different partition table or too little free memory are refused before anything is written; the running firmware stays active. While the upload runs, HTTPS requests (weather networks, notifications, update check), custom uploads and LoRaWAN uplinks wait. Without JavaScript the form is posted normally and the node answers with a result page.

The **status page** links to this section when the update check finds a newer version.

Without an admin password the web interface refuses the upload (set one under *Settings → System*, or use the config portal below). The request is protected like the other settings changes (Digest login, same-origin check). API: `POST /api/ota` ([OpenAPI](api/openapi.yaml)).

> **Behind a reverse proxy** (e.g. for HTTPS) the upload goes through the proxy like any other request — the page uses relative URLs. Allow request bodies of at least **~3.5 MB** (a firmware file is about 2.9 MB, a `-factory` file a little more; nginx' default `client_max_body_size` is 1 MB, e.g. `client_max_body_size 4m;`) and a generous timeout, or upload directly over the node's local address.

### In the config portal (fallback)

Use the portal when no admin password is set, the node is not in your network, or the web interface is not reachable.

1. Open the config portal (button 3 s or downlink `0x03`) and connect to **`WeatherNode`**.
2. Open **http://192.168.4.1** and click **Firmware update**.
3. Upload **`universal-weather-node-<version>.bin`** from [`firmware/`](../firmware/) and wait for the restart. Keep power stable and do not close the upload page early. The `-factory` file is accepted too (only its firmware part is written) — see [which file](#installing-the-firmware-browser-no-software). Files that are not firmware are refused before anything is written.

With a **web settings admin password** set, the portal's `/update` page asks for it (user `admin`) and ArduinoOTA (`pio run -t upload` over the network) needs it as well (`upload_flags = --auth=<password>`). Without an admin password both stay open while the portal is running.

OTA requires an existing compatible partition table with `cfgdata`; it cannot prepare a new board or repair an older table. Use [USB flashing](development.md#build--flash) in that case. Heater/fan outputs remain off throughout the portal/update and resume according to saved settings after restart.

> **Use a normal browser, not the captive-portal window**, for the firmware update. The small sign-in window that phones open automatically (Android: *Sign in to WeatherNode*, iOS: the portal sheet) often can't upload files, and it can't show the password prompt: with an admin password set, Android only shows *Webpage not available* / `ERR_HTTP_RESPONSE_CODE_FAILURE`. Instead:
> 1. Connect to `WeatherNode`; in the sign-in window choose ⋮ → *Use this network as is* (or just close it) and keep the connection when asked about missing internet.
> 2. Turn mobile data off for a moment (otherwise the phone may route 192.168.4.1 over it).
> 3. Open **http://192.168.4.1/update** in Chrome / Safari, log in as `admin` with the admin password and upload the file.
>
> On a laptop it works the same way: connect to `WeatherNode` and open http://192.168.4.1/update in the browser.

<!-- help:skip -->
<table>
  <tr>
    <td align="center" width="50%">
      <img src="../pictures/wifi-config-firmwareupdate.jpeg" alt="Config portal firmware update page" width="100%">
      <br><strong>Firmware update</strong>
    </td>
    <td align="center" width="50%">
      <img src="../pictures/wifi-config-firmwareupdate-success.jpeg" alt="Config portal firmware update successful" width="100%">
      <br><strong>Update successful</strong>
    </td>
  </tr>
</table>
<!-- /help:skip -->

## Building from Source

To build and flash with PlatformIO instead (bootloader, partition table and app over USB, or esptool by hand), see [Build & Flash](development.md#build--flash). Where settings live and what each one does: [Configuration](settings.md).
