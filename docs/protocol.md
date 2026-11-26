# The Balboa bus, as far as this firmware relies on it

Sources: the [balboa_worldwide_app wiki](https://github.com/ccutrer/balboa_worldwide_app/wiki)
(captures from many spas), pybalboa (what the Home Assistant integrations send and
expect), and the ESP32 gateways built on cribskip's work
([NorthernMan54](https://github.com/NorthernMan54/esp32_balboa_spa),
[shomanjk](https://github.com/shomanjk/esp32_balboa_spa)). Items marked *unverified*
have not been seen on a real bus by this project yet.

## Physical layer

RS-485, 115200 baud, 8N1. The main board's 4-pin client connector (Molex 43025-0400
or TE 794617-4 style, the one the Wi-Fi module and panels use):

| Pin | Signal | Level |
| --- | --- | --- |
| 1 | Supply | +12 to 15 V DC (one user measured 15.9 V) |
| 2 | RS-485 B | 2-3 V, slightly above A |
| 3 | RS-485 A | 2-3 V |
| 4 | Return (ground) | 0 V |

Wire colours vary. Measure before connecting.

## Frames

```
7E  LEN  CHANNEL  KIND  TYPE  PAYLOAD...  CRC  7E
```

LEN counts itself through CRC (frame = LEN + 2 bytes, LEN < 0x7E). KIND is 0xAF on the
broadcast channel 0xFF, else 0xBF. CRC-8, polynomial 0x07, init 0x02, final XOR 0x02,
over LEN to the last payload byte. Payloads are not escaped: 0x7E can occur inside
one, so a reader must check length and CRC, not just look for delimiters. This
firmware finds frames from their closing delimiter, so noise that looks like the start
of a long frame does not delay the real frame behind it.

## Who may write when

The main board owns the bus.

* **Clear to Send** `<ch> BF 06`, ~60 a second spread over the client channels. The
  client on `<ch>` may write one frame right after it, or `<ch> BF 07` (nothing to
  send). Captures show clients answering ~1 ms after the CTS. The board's real
  deadline is *unverified*. The firmware answers from the same call that parsed the
  CTS and reports the time it took (`answer_us`).
* **New Client CTS** `FE BF 00`, 10 a second after power-on and then once a second. A
  new client answers with **Channel Assignment Request** `FE BF 01 <type> <hash hi> <hash lo>`
  (panels send type 0x02). The board answers `FE BF 02 <ch> <hash hi> <hash lo?>`
  (the last byte does not always match). The client acknowledges with `<ch> BF 03`,
  and from then on the board sends it CTS. Channels count up from 0x10. The board
  stops sending CTS above 0x2F and only forgets channels when it loses power.
* **Existing Client Request** `<ch> BF 04`. At power-on the board asks 0x0A, 0x0C and
  0x10-0x3F. A client that is there answers **Existing Client Response**
  `<ch> BF 05 <3 bytes>`. Panels send `04 08 00` or `03 37 00`. This firmware sends
  `04 37 00`, as the shomanjk gateway does (*unverified* what the stock module sends).
* **0x0A** is the stock Wi-Fi module's channel. It does not go through the 0xFE
  assignment.

Safety rules in `BusClient`:

* It writes only right after a CTS or Existing Client Request on its own channel, or a
  New Client CTS while it asks for a channel.
* In listen-only mode it writes nothing.
* With an inverted receiver (A/B swapped) it writes nothing. An inverted transmitter
  would hold an auto-direction transceiver on.
* It asks for a channel at most 6 times per configuration. It refuses a channel above
  0x2F.
* After a software restart it first waits to see whether the board still polls its old
  channel (kept in RTC memory), and takes that channel back.
* It ignores its own writes when the transceiver echoes them back.
* Over TCP it passes on only toggles (0x11), temperature (0x20), time (0x21), settings
  requests (0x22), filter cycles (0x23) and preferences (0x27). Change Setup (0x2A),
  locks, test settings and the module's own Wi-Fi setup (0x92) are dropped.

## The TCP side (what the stock module does)

* UDP 30303. Any datagram gets the reply `BWGSPA\r\n00-15-27-XX-XX-XX\r\n`.
* TCP 4257 forwards frames on 0xFF and 0x0A both ways. This firmware rewrites between
  0x0A and its own channel.
* The app's "device present" `0A BF 04` is answered by the module itself with **Module
  Identification** `0A BF 94` (25 bytes). It holds the MAC at bytes 3-8, which is what
  pybalboa takes as the spa's identity, and an iDigi id at bytes 9-24.
* Status `FF AF 13` arrives about 3 times a second. The stock module takes one TCP
  client at a time. This firmware takes three.

## Known open questions

* Whether a BP2100G1 accepts toggles and set-temperature from a client that got its
  channel through 0xFE. The shomanjk gateway, which writes on 0x0A without
  registering, saw reads work but some writes ignored on another board.
* The exact CTS deadline.
* What the stock module answers to an Existing Client Request.
