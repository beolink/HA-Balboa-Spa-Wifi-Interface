# Balboa Spa Wifi Interface

ESP32 firmware that sits on a Balboa spa's RS-485 bus and stands in for the Balboa BWA
Wi-Fi module on the local network. It speaks the module's protocol on TCP port 4257 and
answers UDP discovery on port 30303, so these work without changes:

* [Balboa Spa Easyconf](https://github.com/beolink/HA-Balboa-Spa-Easyconf)
* Home Assistant's built-in `balboa` integration
* pybalboa

The bridge differs from the stock module in four ways:

* **Several clients at once.** The stock module takes one TCP client and refuses a
  second one. The bridge takes three, for example Home Assistant, the app and a script.
* **Local only.** There is no cloud. Wi-Fi setup happens on the bridge's own web page,
  not over the protocol.
* **Careful on the bus.** It starts in listen-only mode and never writes a byte. Later
  it takes its own channel the way a top-side panel does and writes only when the main
  board gives it the turn.
* **Open to inspection.** A web page shows the bus state, the spa's status, the time it
  took to answer each Clear to Send, and a log of bus traffic.

> Status: 0.2.0. The protocol core, the network side and the integration have been
> tested together in a simulator (below). The firmware builds for the boards listed
> below. It has **not yet been on a real spa bus**. Start in listen-only mode.

## Hardware

| Board | Environment | Notes |
| --- | --- | --- |
| M5Stack AtomS3 Lite + Atomic RS485 Base | `atoms3-lite` | Recommended. Enclosed. The base takes the spa's 12-15 V and has an auto-direction transceiver. UART on GPIO 5 (RX) and 6 (TX). LED on 35. |
| M5Stack Atom Lite + Atomic RS485 Base | `atom-lite` | Same base. UART on GPIO 22 (RX) and 19 (TX). LED on 27. |
| Waveshare ESP32-S3-RS485-CAN ("Industrial ESP32-S3 Control Board with RS485 and CAN") | `waveshare-s3-rs485-can` | Everything in one box for rail mounting (91.6 × 23.3 × 58.7 mm). Takes 7-36 V on its power terminal, so it runs straight off the spa. RS-485 side galvanically isolated. The SP3485 has no automatic direction: GPIO 21 is DE (high = send), driven by the UART's RS-485 mode; UART on GPIO 18 (RX) and 17 (TX). 16 MB flash. No status LED; its own RS-485 LEDs show traffic (green send, blue receive). Pins from Waveshare's schematic. Not the 1-channel relay model: that one takes 5 V only. |
| Any ESP32 + 3.3 V RS-485 module (MAX3485/SP3485) + buck converter | `esp32dev` | Set the pins in `platformio.ini`. Set `BUS_DE_PIN` if the module needs a driver-enable line. |

Check that the base or converter can take about 16 V. One owner measured 15.9 V on
the bus supply.

Waveshare's FAQ says not to power the ESP32-S3-RS485-CAN from its terminal and USB-C
at the same time. Flash it over USB-C before it goes into the spa, and update it over
Wi-Fi after that. Leave its 120 Ω termination jumper open as delivered.

### Connecting to the spa

**Switch the spa off at the breaker before opening the pack.** The bus connector is low
voltage, but mains runs next to it.

The main board's client connector is a 4-pin Molex Micro-Fit 3.0 (2 × 2, 3.0 mm
pitch). The cable end is a Molex 43025-0400 / TE 794617-4 receptacle with 43030
crimp terminals. On BP-series boards (BP200G2, BP2100G1, BP6013G1-G3 per Balboa's
tech sheets) the ports are J34 and J35, marked MAIN, shared by the top-side panel
and the Wi-Fi module. With both in use, add a Y-splitter: Balboa 25657, or a
Micro-Fit 3.0 extension with a third lead spliced in.

| Pin | Signal | Atomic RS485 Base | Waveshare ESP32-S3-RS485-CAN |
| --- | --- | --- | --- |
| 1 | +12-15 V | 12 V terminal | power terminal + |
| 2 | RS-485 B | B | RS485 B- |
| 3 | RS-485 A | A | RS485 A+ |
| 4 | Ground | GND | power terminal − (its RS-485 side is isolated and needs no ground) |

The pin order comes from the community wiki and wire colours vary, so measure with a
meter first. Pin 1 to pin 4 should read 12-15 V. A and B each sit at 2-3 V, with B
slightly above A.

If A and B are swapped, the bridge can tell: bytes arrive but no valid frame. It
inverts its receiver to keep listening, blocks all writing, and the web page and LED
say so. Swap the wires, set the receiver polarity to *auto* in the settings, and
restart.

## First start

1. Build and flash over USB, with the environment for your board from the table
   above:

   ```bash
   .venv/bin/pio run -e atoms3-lite -t upload
   ```

2. Join the access point `SpaBridge-XXXX` (password `spabridge`, set at build time with
   `AP_PASSWORD`). Open http://192.168.4.1 and enter your Wi-Fi network and an admin
   password. The bridge restarts and joins the network as `spa-bridge.local`.
3. Connect the bridge to the spa. It is in **listen-only** mode. On the web page check:
   * the bus state is `listening`, and a valid frame was seen just now
   * the model and water temperature match the spa
   * the bus log shows traffic. If the stock module is on the bus, its `0a bf 22 ...`
     requests and the board's answers show up there.
4. In listen-only mode a client can already connect to `spa-bridge.local:4257`. It sees
   status, and settings requests are answered from answers heard on the bus, so they
   need a stock module or panel that asked. Toggles go nowhere.

## Taking a channel

Set the mode to **Own channel** (`assigned`). The bridge waits for the board's New
Client CTS, asks for a channel and acknowledges it. From then on it answers every CTS
on that channel. The web page should show:

* state `online` on channel `0x10` or above
* *CTS answered* rising about 30-60 a second
* *answer* in microseconds: the time from reading a CTS to writing the answer, which
  should be well under 1000

After that, Home Assistant can point at the bridge.

This works beside the stock module, which keeps channel 0x0A.

The mode **Wi-Fi module channel 0x0A** (`fixed_wifi`) takes the stock module's own
channel. Use it only with the stock module unplugged. The bridge watches 0x0A first
and backs off (`gave_up`) if anything else answers there.

### Replacing the stock module in Home Assistant

The integrations identify a spa by the MAC that the module reports in its
identification message. Set *MAC reported to clients* to the stock module's MAC, for
example `00:15:27:12:34:56`. Then point the existing entry at the bridge's address.
The entry, its entities and their history stay as they are.

## Updating over Wi-Fi

After the first flash over USB the bridge never needs a visit to be updated:

* **Web page:** *Firmware*, upload `.pio/build/<environment>/firmware.bin`. This
  works wherever port 80 of the bridge can be reached: the home network, or a VPN
  into it. It needs the admin password, if one is set.
* **PlatformIO:** `.venv/bin/pio run -e <environment> -t upload --upload-port spa-bridge.local`.
  The bridge connects back to the computer for this, so use it on the same network.
  Over a VPN the web page is the one that always works.

Settings survive an update, and so does the bus channel: the bridge takes its old
channel back after the restart.

A new update starts **on trial**. It stays once it has been on the network for a
minute without a break and has been running for 90 seconds. The bridge's own access
point counts as the network while someone is connected to it. Until then the
bootloader keeps the previous firmware ready. If the new one crashes, hangs or
restarts before that, the bootloader starts the previous one again. It does the same
if the new one is still not online after ten minutes. The web page then says that the
last update was rolled back. Saving settings, restarting from the web page or
starting another update keeps the running firmware at once. Rolling back needs an
older firmware in the other slot. Right after the first flash over USB there may be
none, and then the bridge keeps the new one.

## Status LED

On the M5Stack boards. The Waveshare board has none; use the web page.

| Colour | Meaning |
| --- | --- |
| blinking red | no valid bus frame for 5 s |
| red | receiver inverted (A/B swapped), or no usable channel (`gave_up`) |
| blue | listening |
| yellow | getting a channel |
| green | online on its channel |

## Safety rules

These are in the code and covered by the tests:

* It never writes outside the moment right after a CTS or Existing Client Request for
  its own channel, or a New Client CTS while it asks for a channel. The simulated
  board counts every such write as a collision, and the tests require zero.
* Listen-only mode writes nothing.
* An inverted receiver means it writes nothing.
* It asks for a channel at most 6 times per configuration, and refuses channels the
  board does not poll (above 0x2F). After a software restart (OTA, watchdog, the
  restart button) it takes its old channel back instead of using up a new one. The
  channel is kept in RTC memory.
* Its own writes echoed back by the transceiver are not passed on.
* Over TCP only toggles, temperature, time, settings requests, filter cycles and
  preferences reach the bus. Installer messages are dropped: Change Setup, locks and
  test settings. So is the module's own Wi-Fi setup.

The protocol notes behind this are in [docs/protocol.md](docs/protocol.md).

## Development

The protocol core (`lib/bwa_core`) and the socket server (`lib/bwa_net`) are plain C++17
with no Arduino code. The ESP32 build and the desktop simulator use the same files.

```bash
make test                     # unit tests of the core
make sim                      # build/host/bridge_sim: the bridge against a simulated BP2100G1
make pytest PYTHON=<venv with pybalboa and pytest>   # the simulator against pybalboa
```

`tools/ha_smoke_against_sim.py` runs the Balboa Spa Easyconf integration under a real
Home Assistant against the simulator. It covers the config flow, the entry keyed by
the reported MAC, the light and the thermostat.

`tools/watch_bridge.py spa-bridge.local --save capture.log` follows a bridge on the
real spa and saves its bus log. It is read-only.

Firmware:

```bash
python3 -m venv .venv && .venv/bin/pip install platformio
.venv/bin/pio run                                    # AtomS3 Lite
.venv/bin/pio run -e atom-lite
.venv/bin/pio run -e atoms3-lite -t upload --upload-port spa-bridge.local   # OTA
```

A firmware file can also be uploaded on the web page.

## Licence

[Apache License 2.0](LICENSE).
