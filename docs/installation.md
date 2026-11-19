# Installation and First Setup

How to put the firmware on a new Heltec board from the browser, set the node up in its config portal and update it later over Wi-Fi.

[← back to README](../README.md)

## Installing the Firmware (browser, no software)

The firmware is installed **once over USB, directly from the web browser** — no PlatformIO, no Python, no drivers on most computers. After that, updates go over Wi-Fi ([OTA](#firmware-update-ota)).

> **Heltec V3 and V4** use the same firmware image. The board is detected at boot by its flash size (V3 8 MB, V4 16 MB). On the V4 the RF front end (V4.2 GC1109 / V4.3 KCT8103L) is switched automatically, and the radio's TX power is reduced by the amplifier gain so the radiated power stays at the LoRaWAN regional limit (EU868: 16 dBm). The **V4 is for the [build without PCB](minimal-build.md) only**; the **V4-R8** (ESP32-S3R8) is not supported. On the V4 the serial log is not available over USB — use the web log; the first flash may need the BOOT/PRG button held while plugging in. *V4 support is not yet tested on real hardware.*

**Two files per release in [`firmware/`](../firmware/) — pick the right one:**

| File | Contains | Use it for | How |
|------|----------|------------|-----|
| `universal-weather-node-<version>-factory.bin` | bootloader + partition table (with `cfgdata`) + firmware, one complete image | **first installation** of a new board, or repairing a board that no longer starts | over **USB**, in the browser (steps below) or esptool at address **`0x0`** |
| `universal-weather-node-<version>.bin` | the firmware (application) only | **updates** of a node that already runs this firmware | over **Wi-Fi** ([OTA](#firmware-update-ota), portal → *Firmwareupdate*), or USB at address `0x10000` |

> **Which file for OTA?** Nodes running **2.0.0-rc.1 or newer** accept **both** files: of a `-factory` file only the firmware part is written (bootloader and partition table can't be changed over OTA; if the file's partition table differs from the node's, the upload is refused — flash it over USB then). Nodes running **2.0.0-beta.3** refuse the `-factory` file, and **2.0.0-beta.2 or older must never get it** (they would write the bootloader into the firmware slot and no longer start) — for those, always use the file **without** `-factory`. When in doubt, take the file without `-factory`; it works everywhere.

**You need:** a computer with **Chrome or Edge** (desktop; Firefox and Safari can't talk to USB devices), a **USB-C data cable** (some cables only charge — if no device shows up, try another one), the Heltec WiFi LoRa 32 V3 with its **868 MHz antenna screwed on** (never power the board without the antenna).

1. **Download the firmware.** In [`firmware/`](../firmware/) click the newest **`universal-weather-node-<version>-factory.bin`** and then *Download* (or *Raw*). Use the **`-factory`** file (see the table above).
2. **Connect the board.** Plug the Heltec into the computer with the USB-C cable. On a v2.0 PCB: disconnect the external power and remove JP8 first.
3. **Open the flasher.** In Chrome or Edge go to **[web.esphome.io](https://web.esphome.io)** (ESPHome's web flasher — it works for any ESP32 firmware, ESPHome itself is not needed).
4. Click **CONNECT** and choose the serial port of the board in the browser's list, e.g. *CP2102 USB to UART*, *USB Serial*, *cu.usbserial-…* (macOS) or *COM3* (Windows), then *Connect*.
   - **No port in the list?** Try another cable/USB port. On Windows, install the [Silicon Labs CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) once and try again.
5. Click **INSTALL**, choose the downloaded **`…-factory.bin`** and confirm with **INSTALL**. Wait until *Installation complete* (about 1 minute). Don't unplug the board meanwhile.
   - **Fails to connect / "failed to enter bootloader"?** Put the board into download mode by hand: hold the **PRG** button, press and release **RST**, release **PRG** — then click *INSTALL* again.
6. Press **RST** on the board (or unplug and plug in again). The display shows **Config Portal** — continue with [Config Portal](#config-portal): connect your phone to the Wi-Fi **`WeatherNode`** and open http://192.168.4.1.

**Good to know**
- Our own partition layout (with the `cfgdata` settings partition) is part of the `-factory` file, so nothing has to be set up by hand. Settings stored by an earlier installation of this firmware are kept.
- **Alternative flasher:** Espressif's [esptool-js](https://espressif.github.io/esptool-js/) (also in Chrome/Edge): *Connect*, then *Flash Address* **`0x0`**, choose the `-factory` file, *Program*. The old desktop *ESPHome-Flasher* program is no longer maintained — use the web flasher instead.
- **Updating later:** use [OTA](#firmware-update-ota) — with the normal `universal-weather-node-<version>.bin` (always safe), or from 2.0.0-rc.1 on also with the `-factory` file. The `-factory` file over USB also works and keeps the settings.

## Config Portal

**First boot:** a node that has never been configured opens the portal automatically.

1. The display shows **"Config Portal"** with the network name and URL.
2. Connect your phone or laptop to the Wi-Fi network **`WeatherNode`** (no password).
3. Open **http://192.168.4.1** (most devices open it automatically).
4. Click **Universal-Weather-Node Parameter**, fill in the settings, click *Save* — the node saves and restarts.

**Opening it again later:**

| How | |
|-----|---|
| **Button** | Hold the user button for **at least 3 seconds** during operation |
| **LoRaWAN downlink** | Send byte `0x03` (takes effect after the current send cycle) |

> Don't hold the button while powering on or resetting — that starts the ESP32 bootloader instead of the firmware.

- **Passwords and keys** (AppKey, web settings admin password, MQTT password) are not shown in the portal: the field stays empty with *(unchanged)*; leave it empty to keep the stored value, or tick *Remove* to clear a password. The portal network itself stays open (no Wi-Fi password) so a node can always be recovered — close the portal when you are done.
- *Save* on the parameter page is enough. Only use **Configure WiFi** if you want to change the Wi-Fi network or a static IP. The network is stored even if it is not in range (pre-provisioning a node for its final site): the portal reports the failed connection, and after closing it the node keeps trying that network. In normal operation the [web settings page](settings.md#web-settings-page) can change the network too.
- If the portal is left untouched for 3 minutes, it closes and the node restarts. (The timer restarts with every page you open.)
- The start page also offers the version-matched **payload decoder** download, the **help page** and the **wind-direction calibration** wizard.
- The normal sampling/transmit loop pauses while the portal is open. Heater/fan outputs are forced off until restart; do not use the portal/update process when continuous output control is required.

<!-- help:skip -->
<p align="center">
  <img src="../pictures/wifi-config-startpage.png" alt="Config portal start page with the main menu" width="50%">
  <br><strong>Config portal: start page</strong>
</p>
<!-- /help:skip -->

## Firmware Update (OTA)

1. Open the config portal (button 3 s or downlink `0x03`) and connect to **`WeatherNode`**.
2. Open **http://192.168.4.1** and click **Firmwareupdate**.
3. Upload **`universal-weather-node-<version>.bin`** from [`firmware/`](../firmware/) and wait for the restart. Keep power stable and do not close the upload page early. Nodes from 2.0.0-rc.1 on also accept the `-factory` file (only its firmware part is written); on 2.0.0-beta.2 or older never upload the `-factory` file — see [which file](#installing-the-firmware-browser-no-software). Files that are not firmware are refused before anything is written (from 2.0.0-beta.3).

With a **web settings admin password** set, the portal's `/update` page asks for it (user `admin`) and ArduinoOTA (`pio run -t upload` over the network) needs it as well (`upload_flags = --auth=<password>`). Without an admin password both stay open while the portal is running.

OTA requires an existing compatible partition table with `cfgdata`; it cannot prepare a new board or repair an older table. Use [USB flashing](development.md#build--flash) in that case. Heater/fan outputs remain off throughout the portal/update and resume according to saved settings after restart.

> If the captive-portal window doesn't allow file uploads, open **http://192.168.4.1/update** in a normal browser.

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
