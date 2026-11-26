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

> Status: 0.1.0. The protocol core, the network side and the integration have been
> tested together in a simulator (below). The firmware builds for the boards listed
> below. It has **not yet been on a real spa bus**. Start in listen-only mode.

## Hardware

| Board | Environment | Notes |
| --- | --- | --- |
| M5Stack AtomS3 Lite + Atomic RS485 Base | `atoms3-lite` | Recommended. Enclosed. The base takes the spa's 12-15 V and has an auto-direction transceiver. UART on GPIO 5 (RX) and 6 (TX). LED on 35. |
| M5Stack Atom Lite + Atomic RS485 Base | `atom-lite` | Same base. UART on GPIO 22 (RX) and 19 (TX). LED on 27. |
| Any ESP32 + 3.3 V RS-485 module (MAX3485/SP3485) + buck converter | `esp32dev` | Set the pins in `platformio.ini`. Set `BUS_DE_PIN` if the module needs a driver-enable line. |

Check that the base or converter can take about 16 V. One owner measured 15.9 V on
the bus supply.

### Connecting to the spa

**Switch the spa off at the breaker before opening the pack.** The bus connector is low
voltage, but mains runs next to it.

The main board's client connector is a 4-pin Molex 43025-0400 / TE 794617-4 style
plug, the one the Wi-Fi module and panels use. Use a free socket on the board or a
Y-splitter.

| Pin | Signal | To the bridge |
| --- | --- | --- |
| 1 | +12-15 V | supply in (12 V terminal on the Atomic base) |
| 2 | RS-485 B | B |
| 3 | RS-485 A | A |
| 4 | Ground | GND |

The pin order comes from the community wiki and wire colours vary, so measure with a
meter first. Pin 1 to pin 4 should read 12-15 V. A and B each sit at 2-3 V, with B
slightly above A.

If A and B are swapped, the bridge can tell: bytes arrive but no valid frame. It
inverts its receiver to keep listening, blocks all writing, and the web page and LED
say so. Swap the wires, set the receiver polarity to *auto* in the settings, and
restart.

## First start

1. Build and flash over USB:

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

## Status LED

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
