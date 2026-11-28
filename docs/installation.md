# Installation and First Setup

How to put the firmware on a new Heltec board from the browser, set the node up in its config portal and update it later over Wi-Fi.

[← back to README](../README.md)

## Installing the Firmware (browser, no software)

The firmware is installed **once over USB, directly from the web browser** — no PlatformIO, no Python, no drivers on most computers. After that, updates go over Wi-Fi ([OTA](#firmware-update-ota)).

> **Heltec V3 and V4** use the same firmware image. The board is detected at boot by its flash size (V3 8 MB, V4 16 MB). On the V4 the RF front end (V4.2 GC1109 / V4.3 KCT8103L) is switched automatically, and the radio's TX power is reduced by the amplifier gain so the radiated power stays at the LoRaWAN regional limit (EU868: 16 dBm). The **V4 runs [without PCB](minimal-build.md) or on [PCB v2.0](pcb.md)** (its V4 sockets; detected by the revision ID on GPIO15 — a V4 is never used on Rev 1.2 / 1.3); the **V4-R8** (ESP32-S3R8) is not supported. On the V4 the serial log goes over the native USB port (USB CDC, 115200 baud; UART0 stays off because GPIO43/44 carry 1-Wire and wind speed on the PCB) — the web log works too; the first flash may need the BOOT/PRG button held while plugging in. *Confirmed on real V4.2 and V4.3 boards; the V4 sockets of PCB v2.0 are not yet tested on real hardware.*

**Two files per release in [`firmware/`](../firmware/) — pick the right one:**

| File | Contains | Use it for | How |
|------|----------|------------|-----|
| `universal-weather-node-<version>-factory.bin` | bootloader + partition table (with `cfgdata`) + firmware, one complete image | **first installation** of a new board, or repairing a board that no longer starts | over **USB**, in the browser (steps below) or esptool at address **`0x0`** |
| `universal-weather-node-<version>.bin` | the firmware (application) only | **updates** of a node that already runs this firmware | over **Wi-Fi** ([OTA](#firmware-update-ota), portal → *Firmwareupdate*), or USB at address `0x10000` |

> **Which file for OTA?** Nodes running **2.0.0-rc.1 or newer** accept **both** files: of a `-factory` file only the firmware part is written (bootloader and partition table can't be changed over OTA; if the file's partition table differs from the node's, the upload is refused — flash it over USB then). Nodes running **2.0.0-beta.3** refuse the `-factory` file, and **2.0.0-beta.2 or older must never get it** (they would write the bootloader into the firmware slot and no longer start) — for those, always use the file **without** `-factory`. When in doubt, take the file without `-factory`; it works everywhere.

**You need:** a computer with **Chrome or Edge** (desktop; Firefox and Safari can't talk to USB devices), a **USB-C data cable** (some cables only charge — if no device shows up, try another one), the Heltec WiFi LoRa 32 V3 or V4 with its **868 MHz antenna screwed on** (never power the board without the antenna).

1. **Download the firmware.** In [`firmware/`](../firmware/) click the newest **`universal-weather-node-<version>-factory.bin`** and then the **download button** (*Download raw file*), or take the file from the release's *Assets*. Use the **`-factory`** file (see the table above).
   - **Check the file:** a `-factory.bin` is about 2–3 MB. A file of a few hundred KB, or one that opens as text, is the GitHub **web page** that some browsers (often Firefox with "Save link as …") save instead of the firmware — download it again with *Download raw file* or from the release assets.
2. **Connect the board.** Plug the Heltec into the computer with the USB-C cable. On a v2.0 PCB: disconnect the external power and remove JP8 first.
3. **Open the flasher.** In Chrome or Edge go to **[web.esphome.io](https://web.esphome.io)** (ESPHome's web flasher — it works for any ESP32 firmware, ESPHome itself is not needed).
4. Click **CONNECT** and choose the serial port of the board in the browser's list, e.g. *CP2102 USB to UART* / *cu.usbserial-…* (V3), *USB JTAG/serial debug unit* / *cu.usbmodem…* (V4, native USB), or *COM3* (Windows), then *Connect*.
   - **No port in the list?** Try another cable/USB port. On Windows with a V3, install the [Silicon Labs CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) once and try again.
5. Click **INSTALL**, choose the downloaded **`…-factory.bin`** and confirm with **INSTALL**. Wait until *Installation complete* (about 1 minute). Don't unplug the board meanwhile.
   - **Another firmware ran on the board before** (e.g. Meshtastic)? Tick **Erase device** when asked, so no data of the old firmware is left in the flash (from 2.0.0-rc.7 the node would also clear its settings area by itself).
   - **Fails to connect / "failed to enter bootloader"?** Put the board into download mode by hand: hold the **PRG** button, press and release **RST**, release **PRG** — then click *INSTALL* again.
6. Press **RST** on the board (or unplug and plug in again). The display shows **Config Portal** — continue with [Config Portal](#config-portal): connect your phone to the Wi-Fi **`WeatherNode`** and open http://192.168.4.1.

**Good to know**
- **Chrome or Edge on a desktop computer** is needed for flashing in the browser (Web Serial); Firefox, Safari and phones cannot do it.
- **The first Wi-Fi connection can take up to a minute** after the portal saved (the router's DHCP has to hand out an address); the display shows the IP address once it is connected.
- **Reopen the portal at any time:** hold the **PRG** button for **3 s** (the display shows *Config Portal*), then connect to the Wi-Fi `WeatherNode` again.
- Our own partition layout (with the `cfgdata` settings partition) is part of the `-factory` file, so nothing has to be set up by hand. Settings stored by an earlier installation of this firmware are kept.
- **Alternative flasher:** Espressif's [esptool-js](https://espressif.github.io/esptool-js/) (also in Chrome/Edge): *Connect*, then *Flash Address* **`0x0`**, choose the `-factory` file, *Program*. The old desktop *ESPHome-Flasher* program is no longer maintained — use the web flasher instead.
- **Updating later:** use [OTA](#firmware-update-ota) — with the normal `universal-weather-node-<version>.bin` (always safe), or from 2.0.0-rc.1 on also with the `-factory` file. The `-factory` file over USB also works and keeps the settings.
- **A browser/USB flasher always needs the `-factory` file.** web.esphome.io and esptool-js write the chosen file as a complete image at address `0x0`, so an update over USB must use `-factory` too. The normal `.bin` (without `-factory`) is an **application-only** image for the node's own [OTA update page](#firmware-update-ota) — flashing it at `0x0` with a browser flasher lands at the wrong offset and the board no longer starts (blinking display). The fix is simply to re-flash the `-factory` file; to update *without* a cable, use the OTA page instead.

## Config Portal

**First boot:** a node that has never been configured opens the portal automatically.

1. The display shows **"Config Portal"** with the network name and URL.
2. Connect your phone or laptop to the Wi-Fi network **`WeatherNode`** (no password).
3. Open **http://192.168.4.1** (most devices open it automatically).
4. Choose your network under *Configure WiFi* (and, if you like, a few fields under **Universal-Weather-Node Parameter**), click *Save* — the node saves, restarts and joins your Wi-Fi.
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

After the first start (and after a factory reset) the first visit of the dashboard opens the **setup wizard** (`/setup`) — in the browser's language. It opens once per browser session and start: after any restart (a reset, *Erase everything*) it comes again until it is finished or closed with *Later*. The browser remembers it in the session cookie `wnSetup` (an ID of the start, nothing else). The web pages use only such technically needed cookies (`wnSetup`, `wnDev` / `wnLang` for the language); they stay between your browser and the node, no tracking, so there is no cookie banner. It never locks you out: every step has **Skip**, **Back** and **Later** (closes the wizard for good), and nothing in it is required.

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

**Security:** while **no admin password is set and the wizard is not finished**, the wizard — and the settings it posts — works **without a login**, so a new node can be set up from any device in your network (as the open setup portal before it). Every page of the wizard warns about this until step 5. Once a password is set the browser asks for it (user `admin`); once the wizard is finished or closed, a node without a password keeps the settings page locked as before. Start the wizard again any time under **Settings → System → Setup wizard** (parameter 58 = 0 offers it again on the next dashboard visit).

## Firmware Update (OTA)

### In the web interface

With a **web settings admin password** set: **Settings → System → Firmware update**, choose **`universal-weather-node-<version>.bin`** from [`firmware/`](../firmware/) (from 2.0.0-rc.1 on also the `-factory.bin`; only its firmware part is written) and click **Upload and install**. A progress bar shows the upload; the node checks the file while it streams it into the inactive OTA partition, verifies it and restarts (about 20 s; rain statistics, records and all settings are kept). The page then opens the status page — check the firmware version there. Files that are not firmware, a factory file with a different partition table or too little free memory are refused before anything is written; the running firmware stays active. While the upload runs, HTTPS requests (weather networks, notifications, update check), custom uploads and LoRaWAN uplinks wait. Without JavaScript the form is posted normally and the node answers with a result page.

The **status page** links to this section when the update check finds a newer version.

Without an admin password the web interface refuses the upload (set one under *Settings → System*, or use the config portal below). The request is protected like the other settings changes (Digest login, same-origin check). API: `POST /api/ota` ([OpenAPI](api/openapi.yaml)).

> **Behind a reverse proxy** (e.g. for HTTPS) the upload goes through the proxy like any other request — the page uses relative URLs. Allow request bodies of at least **~2.5 MB** (a firmware file is about 2.3 MB; nginx' default `client_max_body_size` is 1 MB, e.g. `client_max_body_size 4m;`) and a generous timeout, or upload directly over the node's local address.

### In the config portal (fallback)

Use the portal when no admin password is set, the node is not in your network, or the web interface is not reachable.

1. Open the config portal (button 3 s or downlink `0x03`) and connect to **`WeatherNode`**.
2. Open **http://192.168.4.1** and click **Firmwareupdate**.
3. Upload **`universal-weather-node-<version>.bin`** from [`firmware/`](../firmware/) and wait for the restart. Keep power stable and do not close the upload page early. Nodes from 2.0.0-rc.1 on also accept the `-factory` file (only its firmware part is written); on 2.0.0-beta.2 or older never upload the `-factory` file — see [which file](#installing-the-firmware-browser-no-software). Files that are not firmware are refused before anything is written (from 2.0.0-beta.3).

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
