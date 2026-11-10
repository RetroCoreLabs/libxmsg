# Control frames

Control frames are how a host PC talks to a **gateway's own firmware** over the same serial
link that carries HDLC frames for the line. They run in both directions: commands from the host,
and replies, log lines, statistics and events from the firmware.

The code is `src/libhdlc/control_frame.h` / `.c`. This page explains what the frames are for and
how they work. The header has the exact numbers.

Users today:

| Who | Side | Where |
|---|---|---|
| HDLCGW | firmware, ESP32-S2 | the HDLCGW repository |
| RC422Dev | firmware, Raspberry Pi Pico | the RC422Dev repository |
| HdlcGatewayTest / hdlc-loopback | host PC, C# | `ControlFrame.cs` in the HdlcGatewayTest repository |

All three must agree **byte for byte**. The C code in this library is the reference. A change
here is a protocol change and has to reach the C# side too.

## Why they exist

A gateway's USB serial port carries line traffic: every HDLC frame from the synchronous line goes
up to the PC, byte-stuffed (RFC 1662, see `byte_frame.h`), and every frame the PC sends goes out on
the line. While that runs, the PC still needs to talk to the firmware itself: to ask its version,
run a console command, read counters, or see log messages.

The old way was to type ESC ESC ESC to drop into a text console. That takes the whole port out
of forwarding, stops the line traffic, and leaves the PC scraping text meant for a person. Control
frames replace it. A control frame travels on the same byte-stuffed channel as a data frame, but
the firmware **keeps it**. It never reaches the line, and line traffic carries on around it.
(Ronny's decision, 2026-11-08.)

## How a control frame is told apart from a data frame: two locks

Every frame that arrives from the host is checked once, before anything else:

```c
if (control_frame_parse(frame, len, &cf))
    handle_control_frame(&cf);      /* the firmware keeps it */
else
    send_to_line(frame, len);       /* everything else is line data */
```

A frame is a control frame only when **both** of these hold:

1. **The address byte is `0xFE`** (`CONTROL_FRAME_ADDRESS`). ISO 13239 reserves `0x00` (no
   station) and `0xFF` (all stations). X.25 and the ND LAPB links use `0x01` and `0x03`. `0xFE`
   is none of those, and it is not a byte that gets escaped on the wire.
2. **The two check bytes are the CRC-16/X.25 stored WITHOUT the usual inversion.** It is the same
   CRC as the HDLC frame check (`hdlc_crc_byte`). Run over a good HDLC frame including its FCS,
   the CRC always leaves `0xF0B8` (`HDLC_CRC_GOOD`). Run over a control frame including its check
   bytes, it always leaves `0x0000` (`CONTROL_FRAME_RESIDUE`).

A byte string cannot leave both residues, so **a data frame with a good FCS can never be taken for
a control frame, even when it is sent to address `0xFE`**.

There is one way to fool it. A host could send address `0xFE` with a deliberately WRONG FCS (to
test the line's error handling) whose wrong value happens to leave residue 0. That is 1 chance in
65536. Hosts avoid address `0xFE` for such tests.

The message type is not checked when deciding. A frame with an unknown type is still a control
frame and is answered with a NAK, so a message from a newer host never gets clocked onto the line
by an older firmware.

## Layout

Inside the byte stuffing. Multi-byte numbers are little-endian (low byte first).

| Offset | Field | Meaning |
|---|---|---|
| 0 | address | always `0xFE` |
| 1 | type | message type. Bit 7 clear = host to firmware, bit 7 set = firmware to host |
| 2 | id | request id 1..255, chosen by the host and copied into every reply to that request. 0 = not a reply to anything (log, statistics, events) |
| 3 | flags | bit 0 LAST, bit 1 ERROR, bits 4..7 chunk counter |
| 4.. | payload | 0 to `CONTROL_FRAME_MAX_PAYLOAD` bytes (1018: the 1024-byte frame limit minus 6) |
| last 2 | check | the un-inverted CRC over everything before it, low byte first |

`control_frame_build()` writes this and `control_frame_parse()` reads it back. The payload pointer
that parse returns points into the caller's buffer.

## Messages, host to firmware

| Type | Name | Payload | What the firmware does |
|---|---|---|---|
| `0x01` | HELLO | none | Answers HELLO_REPLY. Also starts the host session quiet: log level off and statistics push off. A host sends this first. |
| `0x02` | COMMAND | one console command line, no CR/LF | Runs it as if typed at the console. The output comes back as REPLY frames. A command the firmware will not run through a frame (HDLCGW refuses PACKETGEN, because it stops the frame parser) gets a REPLY with the ERROR flag. |
| `0x03` | LOG_LEVEL | 1 byte: 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace | Sets which LOG frames are sent. Answered by a REPLY, or a NAK (bad length) when the byte is missing or above 5. |
| `0x04` | STAT_PUSH | u16 period in ms, 0 = off | Sends a STAT frame every period. HDLCGW raises anything between 1 and 99 to 100. Answered by a REPLY, or a NAK (bad length) when the payload is not exactly 2 bytes. |
| `0x05` | PING | none | Answers an empty REPLY with LAST set. Use it to check the link and measure round-trip time. |

## Messages, firmware to host

| Type | Name | id | Payload |
|---|---|---|---|
| `0x81` | HELLO_REPLY | the HELLO's id | 1 byte protocol version (`CONTROL_FRAME_VERSION`, now 1), then text lines `key=value\n`. HDLCGW sends `name`, `version`, `build`, `maxframe`, `caps`, `chip`, `board`. A host reads the keys it knows and ignores the rest, so a firmware can add keys without breaking anyone. |
| `0x82` | REPLY | the request's id | Text output of a command, in chunks (below). An empty REPLY with LAST answers PING. |
| `0x83` | LOG | 0 | 1 byte level, u32 milliseconds since boot, then the text (no CR/LF). Only levels at or below the LOG_LEVEL the host set are sent. |
| `0x84` | STAT | 0 | A list of u32 counters. **The order and meaning of the fields belong to each firmware** and are documented there (HDLCGW: its README, the STAT section). A host must check which firmware it is talking to (HELLO_REPLY `name`) before reading the fields. |
| `0x85` | EVENT | 0 | 1 byte event code (below), then text. |
| `0x86` | NAK | the request's id | 1 byte reason (below), then optional text. |

### Replies in chunks

A command can print more than fits in one frame. The firmware collects the output and sends it as
REPLY frames of up to its chunk size (HDLCGW: 512 bytes):

- every chunk carries the request's **id**;
- bits 4..7 of flags count the chunks 0, 1, 2, ... 15, 0, ... so the host can see a lost chunk;
- the **last** chunk has the LAST flag (bit 0). It can be empty;
- when the command failed or was refused, the last chunk also has ERROR (bit 1).

The host joins the chunks in order until it sees LAST.

### Event codes

Checked against the source of each firmware on 2026-11-09 (RC422Dev at commit 765d7df, by its
own session).

| Code | Name | HDLCGW sends it | RC422Dev sends it | Text |
|---|---|---|---|---|
| 1 | BOOT | no (defined, never sent) | yes, "RC422Dev <version>" at start | banner |
| 2 | MODE_CHANGE | yes | yes: FORWARDING, PACKET_GEN, DEBUG | the new mode, e.g. FORWARDING / DEBUG / PACKET_GEN |
| 3 | LINE_RX_ACTIVE | yes | no | first byte from the line after an idle spell |
| 4 | LINE_RX_IDLE | yes | no | no byte from the line for 2 s |
| 5 | CLOCK_ON | no (defined, never sent) | no | |
| 6 | CLOCK_OFF | no (defined, never sent) | no | |
| 7 | USB_HOST_LOST | no (defined, never sent) | no | meant for the NEXT host, in its log |
| 16 | WIFI_CONNECTING | yes, WiFi mode only | no | `ssid=` |
| 17 | WIFI_UP | yes, WiFi mode only | no | `ip= rssi=` |
| 18 | WIFI_DOWN | yes, WiFi mode only | no | state |
| 19 | TCP_LISTENING | yes, WiFi mode only | no | `port=` |
| 20 | TCP_PEER_CONNECTED | yes, WiFi mode only | no | `peer=ip:port`, or `host= port=` |
| 21 | TCP_PEER_LOST | yes, WiFi mode only | no | line frames go to USB again |
| 22 | TCP_CONNECTING | yes, WiFi mode only | no | `host= port=` |
| 23 | TCP_CONNECT_FAILED | yes, WiFi mode only | no | retried after 5 s |

All codes live in this one list, so two firmwares never use the same number for different
things. A new code is added here first.

### NAK reasons

| Code | Name | When |
|---|---|---|
| 1 | UNKNOWN_TYPE | the type is not one this firmware knows |
| 2 | BAD_LENGTH | the payload length is wrong for the type, or a value is out of range |
| 3 | REFUSED | the firmware will not do it now |

## Worked examples

All bytes are hex. "On the wire" is after the RFC 1662 byte stuffing: every byte below `0x20`, and
`0x7E` / `0x7D`, is sent as `7D` followed by the byte XOR `0x20`. These exact strings are checked
by `extras/tests/test_control_frame.c`, and they were computed with a separate Python CRC, not by
this library.

| Message | Frame | On the wire |
|---|---|---|
| PING, id 7 | `FE 05 07 00 FD AE` | `7E FE 7D 25 7D 27 7D 20 FD AE 7E` |
| HELLO, id 1 | `FE 01 01 00 4C 99` | |
| REPLY "OK", id 7, LAST | `FE 82 07 01 4F 4B 28 59` | |
| LOG_LEVEL 3 (info), id 2 | `FE 03 02 00 03 78 6C` | |
| STAT_PUSH 1000 ms, id 3 | `FE 04 03 00 E8 03 6E EC` | `7E FE 7D 24 7D 23 7D 20 E8 7D 23 6E EC 7E` |

A typical session:

```
host -> HELLO      id 1                      firmware -> HELLO_REPLY id 1, LAST: 01 "name=HDLCGW\n..."
host -> LOG_LEVEL  id 2  [03]                firmware -> REPLY       id 2, LAST: "Log level 3"
host -> STAT_PUSH  id 3  [E8 03]             firmware -> REPLY       id 3, LAST: "Stat push every 1000 ms"
host -> COMMAND    id 4  "STAT"              firmware -> REPLY       id 4, chunk 0 ... chunk n with LAST
                                             firmware -> STAT  id 0  (every second from now on)
                                             firmware -> LOG   id 0  (when something happens)
```

Line frames keep flowing in both directions the whole time.

## Rules for a firmware

- Check every frame from the host with `control_frame_parse()` **before** treating it as line data.
- Answer every request with the same id: one HELLO_REPLY, one or more REPLY chunks ending in
  LAST, or one NAK.
- Unknown type: NAK `UNKNOWN_TYPE`. Never send it to the line.
- After HELLO: log level off, statistics push off. A new host starts quiet.
- Frames with id 0 (LOG, STAT, EVENT) may come at any time, between the chunks of a reply too.
