/* Generated from DOC/protocols/tad-wire.json by generate_c.py. DO NOT EDIT. */
#ifndef XMSG_TAD_WIRE_H
#define XMSG_TAD_WIRE_H

#include <stdint.h>

/*
 * how a terminal on one machine reaches another: CONNECT-TO, the accept, then keystrokes and
 * screen output
 */

/*
 * TAD opcodes carried in the message body. Names from the ND sources; values confirmed on the
 * wire where an evidence pointer is given. The twelve ops added on 2026-08-18 (USCN, ISRQ,
 * ISRS, NOWT, TNOW, NWRE, RLOC, EDRS, TREP, CPCO, ERRS, REJE) come from the version J TAD
 * driver source. Their VALUES are measured - identical in the J, K03, L07 and M06 symbol
 * tables - and their ROLES are proven for J, but none of them has been seen on our own wire
 * yet, so they are carried as INFERRED for the Release L machines we drive.
 */
typedef uint8_t tad_op_t;

/*
 * here is some text - the characters going to or from the screen. The workhorse: almost all
 * terminal traffic is this -- MEASURED: 601-frame sweep
 */
#define TAD_OP_BDAT                        0x01

/* your turn to type - the far end is waiting for input -- MEASURED: login exchanges */
#define TAD_OP_RFI                         0x02

/*
 * decide who echoes what you type - the terminal itself, or the machine at the far end --
 * INFERRED: name from ND sources; seen in the login ladder
 */
#define TAD_OP_ECKM                        0x03

/*
 * BREAK parameters: a break-strategy byte, a 16-bit BRKMAX word, and for strategy 7 a 16-byte
 * break table. It decides WHICH characters break the input and how many may pile up first - it
 * is NOT the largest block the far end will accept, which is what this entry used to say --
 * MEASURED: BDBREA in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL - message type 7BMMX, strategy
 * clamped to 7, I-field 0o3 bytes normally and 0o23 (19 decimal) for strategy 7 = 1 + 2 + 16.
 * Refutes the earlier INFERRED reading 'the largest block the far end will accept in one go'.
 * Our captures only ever carry 00 04 03 strategy hi lo, which fits the source layout and not
 * the old one. value identical in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J,
 * K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_BMMX                        0x04

/*
 * the user pressed the escape key - break out of whatever is running -- MEASURED: bare-TAD
 * control frames
 */
#define TAD_OP_ESCA                        0x08

/* end the session and hang up -- MEASURED: the disconnect ladder, ND-TO-ND-2026-08-08 */
#define TAD_OP_DCON                        0x09

/*
 * how the terminal should behave - a line at a time or a character at a time, and similar
 * settings -- MEASURED: 100 sends it after CONNECTION ESTABLISHED
 */
#define TAD_OP_TMOD                        0x0C

/*
 * what kind of terminal this is, so the far end knows what it can draw -- MEASURED: same
 * exchange
 */
#define TAD_OP_TTYP                        0x0D

/*
 * enable (payload 1) or disable (payload 0) the escape function for this session. Choosing
 * WHICH key means escape is DESC (0x0F) - this entry used to describe DESC -- MEASURED: BCESC
 * in SINTRAN III version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL
 * builds the payload as 'IF DFOPP.DFLAG BIT 5IESC THEN A:=0 ELSE A:=1' - 5IESC is
 * inhibit-escape, so 1 = enabled and 0 = disabled. A responder with escape inhibited answers
 * ESCA with EDRS rather than ESRS (ESCDIS in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL). The CescState enum was already
 * right; the observed 0->1 step during login is escape being turned off while credentials are
 * typed and back on afterwards. value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_CESC                        0x0E

/* define what the escape key does -- INFERRED: name from ND sources */
#define TAD_OP_DESC                        0x0F

/*
 * the SYSTEM CONTROL word - one 16-bit payload word on a general control channel. Its
 * user-control twin is USCN (0x14). The login values we see stepping through a connect are one
 * USE of the channel, not its definition; 'get back in step', which this entry used to say, is
 * wrong -- MEASURED: CTOBAD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL sends 7SYCN for output-ioset control
 * code 23 (octal) and 7USCN for 24, one word by WORDPUT in both cases, flushing immediately
 * only for parameter values 1, 13 and 17 (octal). The observed login values
 * 0x0002/0x0003/0x0006/0x000A stay MEASURED from conn-to-d102 frames 62/64/68/70 - see
 * SycnState - but what writes them is above the driver and not in this source. value identical
 * in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by
 * hand 2026-08-18
 */
#define TAD_OP_SYCN                        0x13

/*
 * start clean. Sent twice while a session is being set up, which is why it appears in the
 * accept ladder -- MEASURED: port-assign ladder
 */
#define TAD_OP_RESE                        0x16

/*
 * reset confirm - the answer to a RESE. Not 'pick up a session again after it was
 * interrupted', which this entry used to say -- MEASURED: the driver's own prebuilt head is
 * named RESCF with the comment 'RESET-CONF MESSAGE' in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL, and an incoming RECO is treated only
 * as the awaited confirmation of a sent RESE (the CHRESO/RSPNUM arm there, and the RESE send
 * in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL). Also matches the bring-up ladder we
 * drive live. value identical in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J,
 * K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_RECO                        0x17

/*
 * a placeholder that carries nothing. Real machines send one during setup, so we do too --
 * MEASURED: port-assign ladder
 */
#define TAD_OP_DUMM                        0x18

/*
 * OPSV - OS / protocol version handshake (0x1F) -- INFERRED: named in the ND sources, via the
 * summary on the enum member
 */
#define TAD_OP_OPSV                        0x001F

/*
 * CERS - escape / CESC response (0x21). Asker-sent after each host burst / CESC change --
 * INFERRED: named in the ND sources, via the summary on the enum member
 */
#define TAD_OP_CERS                        0x0021

/*
 * the host answering the terminal ESCA - sent with Rese as the pair that lets the login prompt
 * follow -- MEASURED: conn-to-d102-from-100.pcapng frame 603, server side; and 7ESRS=000040
 * octal in SINTRAN/NPL-SOURCE/SYMBOLS/K03. Its disabled-escape twin EDRS (0x29) is built from
 * the prebuilt head EDRSP in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL
 */
#define TAD_OP_ESRS                        0x20

/*
 * the TAD logical-unit index in the port assignment; the unit the user sees is 768 + this
 * value -- MEASURED: 7LUN=000013 octal in SINTRAN/NPL-SOURCE/SYMBOLS. LU=768+value confirmed
 * live 2026-08-17 (index 0x02 printed "TAD LOGICAL UNIT NO: 770"). FIVE index values observed
 * across captures - 0x00 conn-to-102-from103-via100, 0x01
 * ethernet-conn-to-D100-from-102-WORKING, 0x02 new-conn, 0x03 ALLTEST-fa-connectto, 0x04
 * conn-to-d102-from-100. D102 is the server in BOTH the 0x00 and the 0x04 capture, so the
 * index is free-slot state on the serving machine: not constant, not per-machine, not
 * derivable from the session ordinal. The earlier claim that 0x00 was never observed is
 * REFUTED. We allocate per session and only guarantee uniqueness
 */
#define TAD_OP_LUN                         0x0B

/*
 * field/buffer size tag in the port assignment; we emit the two-byte value 01 08 copied from a
 * real capture -- INFERRED: 7FBSI=000025 octal in SINTRAN/NPL-SOURCE/SYMBOLS/K03 names the
 * TAG. What its two bytes MEAN is not established - treat 01 08 as an observed constant, not a
 * decoded field
 */
#define TAD_OP_FBSI                        0x15

/*
 * 8-bit mode negotiation for the terminal line; we neither send nor handle it -- INFERRED:
 * 78MOD=000054 / 7UMOD=000053 octal in SINTRAN/NPL-SOURCE/SYMBOLS/L07 and M06. ABSENT from K03
 * - these two arrived after Release K. Not implemented here; recorded so the value is not
 * re-carved
 */
#define TAD_OP_MOD8                        0x2C

/*
 * a mode message that appears alongside 78MOD from Release L onwards; meaning not established
 * -- INFERRED: 78MOD=000054 / 7UMOD=000053 octal in SINTRAN/NPL-SOURCE/SYMBOLS/L07 and M06.
 * ABSENT from K03 - these two arrived after Release K. Not implemented here; recorded so the
 * value is not re-carved
 */
#define TAD_OP_UMOD                        0x2B

/*
 * the USER CONTROL word - one 16-bit payload word; the sender then waits for an ERRS response
 * -- INFERRED: CTOBAD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL builds 7USCN for output-ioset control
 * code 24 (octal), stores 7ERRS as the awaited response and suspends the caller. Twin of SYCN.
 * value identical in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07,
 * M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_USCN                        0x14

/*
 * remote ISIZE request - empty I-field. Asks the far end how many input characters are waiting
 * -- INFERRED: BISIZ/OISIZ in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL send it with a requested I-field size
 * of zero when a program calls ISIZE (MON 66) or IBRSIZ (MON 313) and the local input buffer
 * is empty, then suspend the caller. value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_ISRQ                        0x22

/*
 * remote ISIZE response - two data bytes, big-endian character count -- INFERRED: prebuilt
 * head ISZRS := (7ISRS\2) in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; the driver reads the count as
 * byte6*256 + byte7 and returns it to the suspended caller. value identical in all four symbol
 * tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_ISRS                        0x23

/*
 * nowait status - one status byte; the variant chosen when the entry status is zero --
 * INFERRED: NWSTA in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL: 'IF A=0 THEN A:=7NOWT ELSE A:=7TNOW
 * FI', then one byte by STORBYT. value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_NOWT                        0x24

/*
 * nowait status - one status byte; the variant chosen when the entry status is non-zero --
 * INFERRED: NWSTA in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL, the other arm of the same test as
 * NOWT. value identical in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03,
 * L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_TNOW                        0x25

/*
 * nowait restart - high priority, empty. The receiver bounces it straight back and then
 * restarts its suspended user program -- INFERRED: prebuilt head NWREM := (7NWRE\0) in SINTRAN
 * III version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; the BDRINP
 * high-priority arm sends the message back with XFSND before going to DATRES. value identical
 * in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by
 * hand 2026-08-18
 */
#define TAD_OP_NWRE                        0x26

/*
 * remote local / rubout for NORD-NET - high priority, empty. Handled in the same branch as
 * ESCA -- INFERRED: prebuilt head RLOCA := (7RLOC\0) with the comment 'REMOTE LOCAL (RUBOUT
 * NORD-NET)' in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; ESCDIS delivers the configured local
 * character when the 5LCHAR flag is set and 0o177 otherwise. value identical in all four
 * symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand
 * 2026-08-18
 */
#define TAD_OP_RLOC                        0x27

/*
 * escape response sent when the escape function is DISABLED - high priority, empty. This is
 * the answer to ESCA or RLOC that a responder with escape inhibited sends INSTEAD of ESRS --
 * INFERRED: prebuilt buffer EDRSP := (7EDRS\0,0,2) with the comment 'ESCAPE RESPONSE ESCAPE
 * DISABLED BUFFER' in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; ESCDIS sends it and runs no escape
 * handling at all in that case. value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_EDRS                        0x29

/*
 * terminal report status - two data bytes, big-endian. Bit 2 buffer overrun, bit 3 parity
 * error, bit 4 framing error -- INFERRED: prebuilt head TREPS := (7TREP\2) in SINTRAN III
 * version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; the receiver
 * bounces the message back and folds bits 2/3/4 into its own TINFO word as 5BFUL/5PAER/5FRER.
 * value identical in all four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07,
 * M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_TREP                        0x2A

/*
 * completion code - four data bytes, two 16-bit words -- INFERRED: SNDCP in SINTRAN III
 * version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL creates the
 * header with the even-start builder and stores two words with whole-word stores, advancing
 * the byte pointer by 4. value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_CPCO                        0xFA

/*
 * error response - two data bytes, big-endian; the answer to USCN -- INFERRED: prebuilt head
 * ERRSP := (7ERRS\2) in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL; read big-endian from head bytes 6-7
 * like ISRS, and it releases the caller CTOBAD suspended after a USCN. value identical in all
 * four symbol tables under SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand
 * 2026-08-18
 */
#define TAD_OP_ERRS                        0xFB

/*
 * reject - one data byte, the type of the message being rejected. Three bytes on the wire: FE
 * 01 type -- INFERRED: REJECT in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL writes 7REJE, a count of 1, then
 * CURMES AND 0xFF. Sent for a normal-priority type the driver does not accept, for a message
 * claiming more bytes than the buffer holds, and for an unrecognised high-priority head;
 * SNDREJ appends an RFI when the rejected message was data. The asking program sees SINTRAN
 * error TER01 (0o315). value identical in all four symbol tables under
 * SINTRAN/NPL-SOURCE/SYMBOLS (J, K03, L07, M06), re-checked by hand 2026-08-18
 */
#define TAD_OP_REJE                        0xFE

/*
 * The XMCSM word. Its top byte feeds nothing on the wire directly, but the whole word appears
 * in the body and identifies what kind of exchange this is.
 */
typedef uint32_t xmcsm_service_t;

/*
 * XROUT connect letter / setup. Low byte 0x41 is the XROUT service code. -- MEASURED: every
 * captured connect and accept
 */
#define XMCSM_XSLET_LETTER                 0x04000041

/* session setup control word -- MEASURED: port-assign frames */
#define XMCSM_SESSION_SETUP                0x04000000

/* terminal data phase -- MEASURED: 601-frame sweep */
#define XMCSM_TERMINAL_DATA                0x01080000

/*
 * bare TAD control - ESCA, DCON, the 0xFD notify -- MEASURED: control frames in ND-TO-ND
 * captures
 */
#define XMCSM_BARE_TAD_CONTROL             0x00080000

/*
 * tell the other side something about the session itself, rather than carrying data --
 * INFERRED: named in the ND sources, via the summary on the enum member
 */
#define XMCSM_SESSION_NOTIFY               0x00060000

/* ask another machine what it knows about a system -- MEASURED: answered live by D100 */
#define XMCSM_XSGSY_REQUEST                0x0100014B

/* the answer to that question -- MEASURED: answered live by D100 */
#define XMCSM_XSGSY_REPLY                  0x01000100

/* tad_terminal_mode. */
typedef uint8_t tad_terminal_mode_t;

/*
 * fold the line's input to capital letters (SINTRAN flag 5CAPITAL) -- INFERRED: sender CTMOD
 * in SINTRAN III version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL
 * sets it from 5CAPITAL and receiver BDTMOD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL reads it back into the same flag -
 * both halves of the J driver agree. Proven for J; no L source read and this bit has not been
 * seen set on our wire.
 */
#define TAD_TERMINAL_MODE_CAPITAL_LETTERS_ONLY 0x01

/*
 * the terminal needs filler time after a carriage return (SINTRAN flag 5CRDLY) -- INFERRED:
 * CTMOD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL sets it from 5CRDLY; BDTMOD in
 * SINTRAN III version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL
 * reads it back. Proven for J; not yet seen set on our wire.
 */
#define TAD_TERMINAL_MODE_CARRIAGE_RETURN_DELAY 0x02

/*
 * pause output when the screen fills instead of scrolling on (the datafield's SCREEN counter)
 * -- INFERRED: CTMOD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL sets it when SCREEN is non-zero;
 * BDTMOD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL sets SCREEN from it. Proven for J;
 * not yet seen set on our wire.
 */
#define TAD_TERMINAL_MODE_STOP_ON_FULL_PAGE 0x04

/*
 * log the user out when the line's carrier goes away (SINTRAN flag 5LBLOG) -- MEASURED: CTMOD
 * in SINTRAN III version J NPL source, SINTRAN/NPL-SOURCE-2/NPL-CLEAN/06-COS-TAD-RES-CODE.NPL
 * sets it from 5LBLOG and BDTMOD in SINTRAN III version J NPL source,
 * SINTRAN/NPL-SOURCE-2/NPL-CLEAN/20-COS-TAD-POF-CODE.NPL reads it back. This is the one bit we
 * have on the wire: real clients send TMOD 0x08 in the terminal-setup burst, which under this
 * decode reads as 'log me out if the carrier drops' - a sensible request from a remote
 * terminal and an independent check on the decode.
 */
#define TAD_TERMINAL_MODE_LOGOUT_ON_MISSING_CARRIER 0x08

#endif /* XMSG_TAD_WIRE_H */
