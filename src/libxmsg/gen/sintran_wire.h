/* Generated from DOC/protocols/sintran-wire.json by generate_c.py. DO NOT EDIT. */
#ifndef XMSG_SINTRAN_WIRE_H
#define XMSG_SINTRAN_WIRE_H

#include <stdint.h>

/*
 * the envelope every message rides in - the 7-word header, the bitfields, the ND link
 * underneath
 */

/* xmsg_frame_flags. */
typedef uint8_t xmsg_frame_flags_t;

/*
 * set on every observed frame -- UNKNOWN: 601-frame sweep; always set, purpose never
 * established
 */
#define XMSG_FRAME_FLAG_MARKER01           0x02

/*
 * set on setup and on 0x96 terminal frames, clear on 0x92 and 0x82 -- UNKNOWN: 601-frame
 * sweep; the one bit whose rule is not known. NOTE: unrelated to an XROUT letter despite the
 * name
 */
#define XMSG_FRAME_FLAG_LETTER             0x04

/* established terminal-phase frames -- INFERRED: 238 of 242 terminal-phase frames fit */
#define XMSG_FRAME_FLAG_DATA_PHASE         0x10

/* XFSYS system-mode call -- INFERRED: set on every observed frame */
#define XMSG_FRAME_FLAG_SYSTEM_MODE        0x80

/* xmsg_send_option. */
typedef uint16_t xmsg_send_option_t;

/*
 * XFROU set -> bit 13 means RemoteRoute and the target XROUT's system number comes from the A
 * register. XFROU clear -> bit 13 means HighPriority and the message is chained to the head of
 * the receiver's queue. -- MEASURED: COSMOS Programmer Guide appendix A section 3.2.11, the
 * 'acts as' rule TRAP: ONE BIT, TWO NAMES. Test the disambiguator FIRST. Getting this wrong
 * made a forwarded letter read as high priority - see XmsgKernel.ChooseType.
 */
#define XMSG_SEND_XFHIP                    0x2000

/*
 * XFROU set -> bit 13 means RemoteRoute and the target XROUT's system number comes from the A
 * register. XFROU clear -> bit 13 means HighPriority and the message is chained to the head of
 * the receiver's queue. -- MEASURED: COSMOS Programmer Guide appendix A section 3.2.11, the
 * 'acts as' rule TRAP: ONE BIT, TWO NAMES. Test the disambiguator FIRST. Getting this wrong
 * made a forwarded letter read as high priority - see XmsgKernel.ChooseType.
 */
#define XMSG_SEND_XFRRO                    0x2000

/* nd_link_frame_kind. */
typedef uint8_t nd_link_frame_kind_t;

/* ConnectionRequest -- MEASURED: D100 sends it; captured many times */
#define ND_LINK_KIND_CONNECTION_REQUEST    0x0F

/* Data -- MEASURED: every capture */
#define ND_LINK_KIND_DATA                  0x20

/* Acknowledge -- MEASURED: every capture */
#define ND_LINK_KIND_ACKNOWLEDGE           0x3F

/*
 * DisconnectRequestByNetworkService -- MEASURED: D100 answers an unexpected connection request
 * with it, 2026-08-11 and 2026-08-17
 */
#define ND_LINK_KIND_DISCONNECT_REQUEST_BY_NETWORK_SERVICE 0x6F

/*
 * the other disconnect - D100 sends this one repeatedly once it gives up on a conversation --
 * MEASURED: leaving it unrecognised produced an endless run of 'unknown ND frame kind 0x60' in
 * the runner log while the link stayed nominally up
 */
#define ND_LINK_KIND_DISCONNECT_REQUEST60  0x60

/*
 * The confirmation of a disconnect request. Read from its position in the exchange - a
 * connection request is refused with DisconnectRequest60 and the refused side answers with
 * this. -- INFERRED: three independent occurrences across DOC/captures/XMSG-DEGRADE-2026-08-24
 * (segment.pcap and after-fix.pcap), 2026-08-24 - every one immediately after a
 * DisconnectRequest60, link ids swapped, trailing field 0 where the request carried 0x0001, no
 * payload
 */
#define ND_LINK_KIND_DISCONNECT_CONFIRM    0x70

/*
 * sintran_header - 14 bytes, big-endian on the wire. Accessors rather than a packed struct: a
 * packed struct depends on the host's byte order and on the compiler honouring a pragma, while
 * these read the bytes as defined.
 */
#define SINTRAN_HEADER_SIZE 14

/*
 * XDROU - NETWORK INFO (VERSION, PROTOCOL, HOP COUNT). NOT a marker. High byte =
 * version/protocol: X5VRS = 20400 octal = 0x2100, left byte 0x21 = OP 0, VERSION 2, PROTOCOL
 * 1. Low byte = HOP COUNT, which is why a relayed frame reads 0x12 against a direct 0x13 - one
 * hop apart. -- MEASURED: XMSG-POFTABS-L03.SYMB (the POF tables of the L03 kernel we drive);
 * corroborated by every capture in DOC/captures
 */
static inline uint16_t sintran_header_read_marker(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
}

/*
 * XDTYP - DATAGRAM TYPE INFO (SD, ED, DC, CONTROL...). A BIT SET, not a type/subtype byte
 * pair: XD5CO=0 control, XD5SD=1 start, XD5ED=2 end, XD5DC=3 confirm-delivery, XD5CE=5
 * checksum enabled (with SD), XD5CP=6 checksum provided (with ED). On a control datagram the
 * bits are reused: XD5UT=1 outgoing, XD5BA=2 BAD STATUS (REJ/NAK), XD5IN=4 connection
 * initialise. Our high/low byte split is a reading of those bits. -- MEASURED:
 * XMSG-POFTABS-L03.SYMB (the POF tables of the L03 kernel we drive); every capture for the
 * observed values
 */
static inline uint16_t sintran_header_read_packet_type_and_subtype(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[2] << 8) | frame[3]);
}

/*
 * destination_node -- MEASURED: every capture; XMSG-POFTABS-L03.SYMB (the POF tables of the
 * L03 kernel we drive) names it DESTINATION NETWORK ADDRESS
 */
static inline uint16_t sintran_header_read_destination_node(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[4] << 8) | frame[5]);
}

/*
 * source_node -- MEASURED: every capture; XMSG-POFTABS-L03.SYMB (the POF tables of the L03
 * kernel we drive) names it SOURCE NETWORK ADDRESS
 */
static inline uint16_t sintran_header_read_source_node(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[6] << 8) | frame[7]);
}

/*
 * XDREF - CONNECTION REFERENCE FIELD (MESS NO). A message number belonging to a CONNECTION,
 * not a free-running link counter. Measured behaviour: one counter per (sender, peer), zeroed
 * only at bring-up, never in use. -- MEASURED: XMSG-POFTABS-L03.SYMB (the POF tables of the
 * L03 kernel we drive) names it; THE FLAGS 1 LAW is the measured behaviour;
 * PUSH-XENSE-2026-08-11 shows the refused number echoed in the error frame
 */
static inline uint16_t sintran_header_read_flags1(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[8] << 8) | frame[9]);
}

/*
 * XDSCR - SCRATCH (SIZE, DISPL, STATUS). Scratch space the layers reuse, which is why no
 * single fixed meaning has ever fitted it. Observed as frame class, equal to XMCSM >> 16; on
 * an error frame it is the negative error code instead. -- MEASURED: XMSG-POFTABS-L03.SYMB
 * (the POF tables of the L03 kernel we drive) names it; 601/601 data frames for the observed
 * use
 */
static inline uint16_t sintran_header_read_flags2(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[10] << 8) | frame[11]);
}

/*
 * ones-complement checksum over the other six words. There is NO channel, NO epoch and NO
 * per-link seed. -- MEASURED: carved from the kernel at 137314; verified 3595/3595 frames,
 * every subtype, no special cases. DOC/XMSG-HEADER-WORD6-IS-A-CHECKSUM-2026-07-31.md;
 * XMSG-POFTABS-L03.SYMB (the POF tables of the L03 kernel we drive) calls it CHECKSUM (COMP'T
 * OF ONES COMP'T ADD), independently the routine carved 2026-07-31 Rule:
 * ~ones_complement_sum(w0..w5)
 */
static inline uint16_t sintran_header_read_checksum(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
}

/*
 * nd_link_header - 11 bytes, big-endian on the wire. Accessors rather than a packed struct: a
 * packed struct depends on the host's byte order and on the compiler honouring a pragma, while
 * these read the bytes as defined.
 */
#define ND_LINK_HEADER_SIZE 11

/*
 * Always 0x0B = 11, which IS the length of this header, not a magic number. The sender
 * computes it, so searching an ND binary for a literal 0B02 finds nothing. -- MEASURED: every
 * frame in every Ethernet capture; NdLinkHeader.Signature0
 */
static inline uint8_t nd_link_header_read_header_length(const uint8_t *frame)
{
    return frame[0];
}

/*
 * Always 0x02. Meaning unknown - it is used together with byte 0 only to recognise the header.
 * -- MEASURED: every frame in every Ethernet capture; NdLinkHeader.Signature1
 */
static inline uint8_t nd_link_header_read_signature(const uint8_t *frame)
{
    return frame[1];
}

/*
 * Which kind of link frame this is. See the nd_link_frame_kind bitfield. -- MEASURED:
 * DOC/captures/XMSG-DEGRADE-2026-08-24/segment.pcap - 1050 frames of a D100/D102 Ethernet
 * segment, every frame the hub carried
 */
static inline uint8_t nd_link_header_read_frame_kind(const uint8_t *frame)
{
    return frame[2];
}

/*
 * Zero on every frame we have ever captured, and our own parser skips it. Whether it carries
 * anything on some frame we have not seen is NOT established. -- UNKNOWN: 0x00 in all 1050
 * frames of DOC/captures/XMSG-DEGRADE-2026-08-24/segment.pcap - 1050 frames of a D100/D102
 * Ethernet segment, every frame the hub carried
 */
static inline uint8_t nd_link_header_read_unused(const uint8_t *frame)
{
    return frame[3];
}

/*
 * Send sequence, ONE byte. Each direction runs its own. An Acknowledge carries the received
 * sequence PLUS ONE - the next expected value. -- MEASURED:
 * DOC/captures/XMSG-DEGRADE-2026-08-24/segment.pcap - 1050 frames of a D100/D102 Ethernet
 * segment, every frame the hub carried; observed stepping 0x43,0x44,0x45 in the 2026-08-03
 * both-ends capture
 */
static inline uint8_t nd_link_header_read_sequence(const uint8_t *frame)
{
    return frame[4];
}

/*
 * The link reference of the machine this frame is FOR. Zero on a connection request, because
 * the sender does not know the peer's reference yet. -- MEASURED: Lined up across a full setup
 * in DOC/captures/XMSG-DEGRADE-2026-08-24/segment.pcap - 1050 frames of a D100/D102 Ethernet
 * segment, every frame the hub carried: D100's request carries 0000/0789, D102's confirm
 * 0789/0A17, and D100's following data 0A17/0789 - only destination-then-source is consistent
 * with all three.
 */
static inline uint16_t nd_link_header_read_destination_link_id(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[5] << 8) | frame[6]);
}

/*
 * The sender's own link reference. A node LEARNS the peer's from this field and takes its own
 * from configuration - synthesising one from the node number would be a fabricated constant.
 * The ids are per-session: 0x5062/0x59C1 in one capture, 0x2F48/0x28A4 in another. --
 * MEASURED: same three-frame line-up as destination_link_id
 */
static inline uint16_t nd_link_header_read_source_link_id(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[7] << 8) | frame[8]);
}

/*
 * NOT always a length. On Data it is the payload length. On a ConnectionRequest it is the
 * SENDER'S OWN SYSTEM NUMBER (0x0066 = 102) with the real payload length zero. On a
 * DisconnectRequestByNetworkService it is 0x0101, meaning unknown. The 802.3 length field is
 * the authority on how many payload bytes are present. -- MEASURED: both-ends capture
 * 2026-08-03; confirmed again throughout DOC/captures/XMSG-DEGRADE-2026-08-24/segment.pcap -
 * 1050 frames of a D100/D102 Ethernet segment, every frame the hub carried
 */
static inline uint16_t nd_link_header_read_trailing_field(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[9] << 8) | frame[10]);
}

#endif /* XMSG_SINTRAN_WIRE_H */
