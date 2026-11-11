/*
 * fa_client.h - a COSMOS file-access client: read one file from a SINTRAN machine.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * The ladder, as the D100 client ran it in capture-read.txt
 * (RetroIO DOC/captures/FA-READ-WRITE-2026-08-04):
 *
 *   us -> port 0    connect letter for *FA-SERVER                 (setup frame, role E4)
 *   server -> us    07D2 <our word> <server conversation> 6400    from its session port
 *   then for each step:   us: request    server: short ack, reply    us: short ack
 *     reserve (02) - open (05) - set block size 2048 (07) - file information (0C/21) -
 *     read block 0..N-1 (08; each reply is followed by two 1032-byte data messages, each acked) -
 *     close (06) - release (03)
 *   us -> server    0782 session finished;  server -> us  07C0 close (its seat is free again)
 *
 * The number of blocks is the length from the open reply rounded up to 2048 (at least one); the
 * last block arrives padded and the padding is not handed on. A refusal (F2 0001 A2 <error>)
 * ends the work: we still send release and session finished, so the server's seat comes back.
 *
 * The client is an XmsgSession (the first member) and sends one frame at a time, waiting for the
 * XMSG acknowledgement before the next, as the TAD asker does.
 */
#ifndef FA_CLIENT_H
#define FA_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libcosmos/fa/fa_envelope.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_session.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** The result code of a refused transfer (the SINTRAN error is passed beside it). */
#define FA_CLIENT_EREFUSED (-20)

#define FA_CLIENT_QUEUE   8u  /**< frames waiting to be sent */
#define FA_CLIENT_NAME    32u /**< user names and the system name, with the NUL */
#define FA_CLIENT_SPEC    128u /**< the file specification, with the NUL */

/** Sub-header values, MEASURED in capture-read.txt (every FA frame after the letter). */
#define FA_ROLE_LETTER 0xE4u
#define FA_ROLE_DATA   0x84u

typedef enum FaClientState
{
    FA_CLIENT_IDLE = 0,
    FA_CLIENT_CONNECTING, /**< letter sent, waiting for the confirm */
    FA_CLIENT_RUNNING,    /**< working through the ladder */
    FA_CLIENT_FINISHING,  /**< session finished sent, waiting for the server's close */
    FA_CLIENT_DONE,       /**< over; see the result */
    FA_CLIENT_FAILED
} FaClientState;

typedef struct FaClientCallbacks
{
    void *ctx;
    /** The open reply came: the file is `length` bytes. */
    void (*opened)(void *ctx, uint32_t length);
    /** File bytes, in order, `offset` counting from 0. The padding of the last block is cut. */
    void (*data)(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n);
    /** The transfer is over: result 0, FA_CLIENT_EREFUSED with the SINTRAN error, or an XMSG_E*. */
    void (*done)(void *ctx, int result, uint16_t sintran_error);
} FaClientCallbacks;

typedef struct FaClientConfig
{
    uint16_t word;             /**< our conversation number in the letter (captured: 2, 4) */
    char asker[6];             /**< the asking process in the reserve, "BAK04" as captured */
    uint32_t ack_timeout_ms;   /**< XMSG acknowledgement of one frame */
    uint32_t reply_timeout_ms; /**< the confirm, or the reply to one request */
} FaClientConfig;

typedef struct FaClientStats
{
    uint32_t frames_out;
    uint32_t messages_in;
    uint32_t xense_resends;
    uint32_t restart_resends;
    uint32_t ignored; /**< messages that did not fit the step (logged) */
} FaClientStats;

typedef enum FaClientSend
{
    FA_SEND_NONE = 0,
    FA_SEND_LETTER,
    FA_SEND_REQUEST,
    FA_SEND_ACK,
    FA_SEND_FINISH
} FaClientSend;

typedef enum FaClientStep
{
    FA_STEP_RESERVE = 0,
    FA_STEP_OPEN,
    FA_STEP_SET_BLOCK_SIZE,
    FA_STEP_FILE_INFO,
    FA_STEP_READ,
    FA_STEP_CLOSE,
    FA_STEP_RELEASE
} FaClientStep;

/** One frame waiting: its kind and the numbers given to it when queued. */
typedef struct FaClientItem
{
    uint8_t kind;
    uint8_t counter;
    uint16_t seq;
} FaClientItem;

typedef struct FaClient
{
    XmsgSession base; /**< must be first */
    FaClientConfig cfg;
    FaClientCallbacks cb;
    FaClientState state;
    int result;
    uint16_t sintran_error;

    char system_name[FA_CLIENT_NAME];
    char local_user[FA_CLIENT_NAME];
    char remote_user[FA_CLIENT_NAME];
    char spec[FA_CLIENT_SPEC];
    uint16_t password;

    uint16_t server_conversation;
    FaCounter counter;
    uint8_t ack_counter; /**< last short-ack counter used (the first is 1) */
    uint16_t seq;        /**< last request sequence used (the first is 1) */
    bool first_request_sent;

    FaClientStep step;
    bool reply_in;    /**< the reply to the current step has come */
    uint8_t data_in;  /**< data messages of the current block received (0..2) */
    uint32_t block;   /**< current block when reading */
    uint32_t blocks;  /**< blocks in the file */
    uint32_t length;  /**< bytes in the file */
    uint16_t file_number;
    uint8_t object_entry[64];
    bool has_object_entry;

    FaClientItem queue[FA_CLIENT_QUEUE];
    uint8_t q_head;
    uint8_t q_count;
    FaClientItem inflight;
    uint16_t inflight_flags1;
    uint32_t inflight_since_ms;
    uint8_t xense_tries;
    bool resync_tried;
    uint32_t wait_since_ms; /**< when we started waiting for the confirm or a reply */

    FaClientStats stats;
} FaClient;

/** @brief Defaults: word 2, asker "BAK04", ack timeout 10 s, reply timeout 30 s. */
void fa_client_config_default(FaClientConfig *cfg);

/**
 * @brief Prepare a client. Nothing is sent.
 * @return XMSG_OK or XMSG_EINVAL.
 */
int fa_client_init(FaClient *c, const FaClientConfig *cfg, const FaClientCallbacks *cb);

/**
 * @brief Start reading one file.
 * @param c           The client (IDLE, DONE or FAILED).
 * @param node        The node to attach to.
 * @param peer_node   The server machine's node number.
 * @param peer_system Its system number (the letter names it "D<number>").
 * @param local_user  Who asks (goes in the reserve beside the asker), e.g. "SYSTEM".
 * @param remote_user Whose directory is opened on the server.
 * @param password    That user's password as typed ("" for none); only its fold is kept.
 * @param spec        The file: "NAME:TYPE" in remote_user's directory, or "(USER)NAME:TYPE".
 * @param now_ms      Now.
 * @return XMSG_OK; XMSG_EINVAL on a bad argument or a name too long; XMSG_ESTATE when busy;
 *         XMSG_ENOSPC when the node has no free session.
 */
int fa_client_read(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                   const char *local_user, const char *remote_user, const char *password,
                   const char *spec, uint32_t now_ms);

/** @brief The state. */
FaClientState fa_client_state(const FaClient *c);

/** @brief Plain-words name of a state. */
const char *fa_client_state_name(FaClientState st);

#ifdef __cplusplus
}
#endif

#endif /* FA_CLIENT_H */
