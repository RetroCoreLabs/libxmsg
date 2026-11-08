/* Generated from DOC/protocols/fa-qform.json by generate_c.py. DO NOT EDIT. */
#ifndef XMSG_FA_QFORM_H
#define XMSG_FA_QFORM_H

#include <stdint.h>

/*
 * open, read, write, create and list files on another machine - and the tag encoding its
 * bodies use
 */

/* fa_operation. */
typedef uint16_t fa_operation_t;

/* let go of a file slot -- MEASURED: captured ladders */
#define FA_OP_FILE_ENTRY_DISCONNECT        0x0001

/*
 * claim one of the far machine file slots before doing anything with a file -- MEASURED:
 * proved live, task 23
 */
#define FA_OP_RESERVE_FILE_ENTRY           0x0002

/* give that slot back when finished -- MEASURED: proved live, task 23 */
#define FA_OP_RELEASE_FILE_ENTRY           0x0003

/* renumber a slot -- INFERRED: named in the ND sources; never driven */
#define FA_OP_CHANGE_FILE_ENTRY_ID         0x0004

/* open a file by name, for reading or for writing -- MEASURED: proved live, tasks 23 and 38 */
#define FA_OP_OPEN_FILE                    0x0005

/* close it -- MEASURED: proved live */
#define FA_OP_CLOSE_FILE                   0x0006

/* agree how big each chunk of data will be -- MEASURED: proved live */
#define FA_OP_SET_BLOCK_SIZE               0x0007

/* send me the contents -- MEASURED: task 38 - 20400 bytes pulled off D100, SHA256 identical */
#define FA_OP_READ_FILE                    0x0008

/* here are the contents to store -- MEASURED: task 23 - 20400 bytes written to D100 */
#define FA_OP_WRITE_FILE                   0x0009

/*
 * make a new file with this name and this many pages -- MEASURED: task 23 - created by quoted
 * new name, confirmed by D100's own file server
 */
#define FA_OP_CREATE_FILE                  0x000A

/*
 * delete a file -- MEASURED: the request is pinned byte-for-byte against real ND-to-ND traffic
 * - D102 deleting XFERTEST:DATA on D100, claude-delete-file-102-to-100-2026-07-29.pcapng - and
 * FaDeleteFileCodec.BuildRequest reproduces it. NOT yet driven against a live server: what a
 * real server ANSWERS is still uncaptured, because the two deletes D100 sent us in August were
 * refused.
 */
#define FA_OP_DELETE_FILE                  0x000B

/* a SINTRAN-specific request -- INFERRED: named in the ND sources */
#define FA_OP_SIII_SPECIAL                 0x000C

/*
 * operate on a device rather than a file -- UNKNOWN: no operator command is known to drive it;
 * blocked since 2026-08-09
 */
#define FA_OP_DEVICE_FUNCTION              0x000D

/*
 * Returned in a rejection. These are SINTRAN error numbers and MUST match the real error table
 * - a made-up value is worse than none.
 */
typedef uint16_t fa_status_t;

/* it worked -- MEASURED: every successful exchange */
#define FA_STATUS_OK                       0

/*
 * no file of that name on that machine -- MEASURED: SINTRAN error table; observed on a missing
 * file
 */
#define FA_STATUS_NO_SUCH_FILE             46

/*
 * the file is protected and the password did not match -- MEASURED: observed as TypedInteger
 * 0x0030 in a rejected open
 */
#define FA_STATUS_WRONG_PASSWORD           48

/*
 * the machine could not write it - out of room, or the disc complained -- INFERRED: SINTRAN
 * error table
 */
#define FA_STATUS_STORE_ERROR              97

/* that machine does not offer this operation -- INFERRED: SINTRAN error table */
#define FA_STATUS_NOT_SUPPORTED            129

/* no more files - the end of a listing, not a fault -- MEASURED: ends every listing walk */
#define FA_STATUS_END_OF_DIRECTORY         197

/* the request did not make sense to the far end -- INFERRED: SINTRAN error table */
#define FA_STATUS_BAD_REQUEST              211

/*
 * fa_message_prefix - 14 bytes, big-endian on the wire. Accessors rather than a packed struct:
 * a packed struct depends on the host's byte order and on the compiler honouring a pragma,
 * while these read the bytes as defined.
 */
#define FA_MESSAGE_PREFIX_SIZE 14

/* message_type -- MEASURED: every FA capture */
static inline uint16_t fa_message_prefix_read_message_type(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
}

/* conversation -- MEASURED: every FA capture */
static inline uint16_t fa_message_prefix_read_conversation(const uint8_t *frame)
{
    return (uint16_t)(((uint16_t)frame[2] << 8) | frame[3]);
}

/* session_header -- MEASURED: every FA capture */
static inline uint32_t fa_message_prefix_read_session_header(const uint8_t *frame)
{
    return ((uint32_t)frame[4] << 24) | ((uint32_t)frame[5] << 16)
         | ((uint32_t)frame[6] << 8) | frame[7];
}

#endif /* XMSG_FA_QFORM_H */
