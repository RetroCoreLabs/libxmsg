/*
 * fa_client.h - a COSMOS file-access client: read, write or list files on a SINTRAN machine.
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
 * A LISTING (LIST-FILES) and a FILE-STATISTICS are one and the same walk; the second just names
 * a file. MEASURED in capture-list-files.txt and the file-statistics capture (2026-07-29 20:15):
 *
 *   reserve (02)
 *   next entry (0C/78, name block, user index 0, cursor FFFF)   reply: user index, object index,
 *                                                               the 64-byte object entry
 *   directory entry (0C/A4, index 0)                            reply: "PACK-ONE"
 *   user entry (0C/8C, the user index)                          reply: "SYSTEM"
 *   next entry (0C/78, user index, last object index + 1) ...   until refused with 197
 *   release (03), session finished, the server's close
 *
 * Which files match the name is decided by the server.
 *
 * A WRITE, MEASURED in capture-write.txt (COPY-FILE writing WRTEST1:OUT, 17905 bytes):
 *   reserve (02) - open (05, the name in quotes and 'W) - set block size 2048 (07) -
 *   for each block: write (09, block number), then at once the block's two data messages
 *     (1032 bytes each, the end of the file padded with zeros); the server answers each of the
 *     three with a short ack and then sends the reply, which we ack -
 *   set end of file (0C/3B, the last byte's index) - close (06) - release (03) -
 *   session finished, the server's close
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
#include "libcosmos/fa/fa_replies.h"
#include "libcosmos/fa/fa_requests.h"
#include "libxmsg/xmsg/xmsg_node.h"
#include "libxmsg/xmsg/xmsg_session.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** The result code of a refused transfer (the SINTRAN error is passed beside it). */
#define FA_CLIENT_EREFUSED (-20)

#define FA_CLIENT_QUEUE 8u   /**< frames waiting to be sent */
#define FA_CLIENT_NAME  32u  /**< user names and the system name, with the NUL */
#define FA_CLIENT_SPEC  128u /**< the file specification, with the NUL */

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
    /** Listing: the directory and user being listed ("PACK-ONE", "SYSTEM"), once, after the
     *  first file. */
    void (*directory)(void *ctx, const char *directory, const char *user);
    /** Listing: one file. */
    void (*entry)(void *ctx, const FaFileEntry *e);
    /** Writing: copy the `n` file bytes that start at `offset` into `out` and return `n`.
     *  The same range can be asked for again (a frame sent twice), so the bytes must stay
     *  available until `done`. Returning anything but `n` ends the write with XMSG_EIO. */
    size_t (*source)(void *ctx, uint32_t offset, uint8_t *out, size_t n);
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
    FA_SEND_FINISH,
    FA_SEND_DATA /**< one half of a block being written; the item's seq is the half, 0 or 1 */
} FaClientSend;

typedef enum FaClientStep
{
    FA_STEP_RESERVE = 0,
    FA_STEP_OPEN,
    FA_STEP_SET_BLOCK_SIZE,
    FA_STEP_FILE_INFO,
    FA_STEP_READ,
    FA_STEP_CLOSE,
    FA_STEP_RELEASE,
    FA_STEP_WALK,       /**< listing: next entry */
    FA_STEP_DIRECTORY,  /**< listing: the directory entry, once */
    FA_STEP_USER,       /**< listing: the user entry, once */
    FA_STEP_WRITE,      /**< writing: one block, with its two data messages */
    FA_STEP_END_OF_FILE /**< writing: set end of file (0C/3B) */
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
    bool reply_in;   /**< the reply to the current step has come */
    uint8_t data_in; /**< data messages of the current block received (0..2) */
    uint32_t block;  /**< current block when reading */
    uint32_t blocks; /**< blocks in the file */
    uint32_t length; /**< bytes in the file */
    uint16_t file_number;
    uint8_t object_entry[64];
    bool has_object_entry;

    bool listing;                           /**< a walk rather than a read */
    bool writing;                           /**< a write rather than a read */
    bool create;                            /**< writing: create (quoted name), not overwrite */
    bool cancelled;                         /**< fa_client_cancel: tearing down, no more
                                                callbacks but `done` */
    uint8_t walk_block[FA_WALK_BLOCK_SIZE]; /**< the name block every walk request carries */
    uint16_t user_index;                    /**< from the last entry; 0 before the first */
    uint16_t cursor;                        /**< FFFF first, then last object index + 1 */
    bool names_asked;                       /**< directory and user entries asked */
    uint32_t entries;                       /**< files reported */
    char directory_name[17];
    char user_name[17];

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

/**
 * @brief Start a listing (LIST-FILES) or file statistics (FILE-STATISTICS): one `entry` callback
 *        per matching file, then `done` with result 0. The end of the directory (SINTRAN error
 *        197) is the normal end, not an error.
 * @param filter What to match, as SINTRAN takes it: "" for every file, "NAME:TYPE" for one.
 *        The server does the matching. The name block holds "(<remote_user>)" and the filter
 *        twice, 62 bytes in all.
 * The other arguments and the return value are as fa_client_read().
 */
int fa_client_list(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                   const char *local_user, const char *remote_user, const char *password,
                   const char *filter, uint32_t now_ms);

/** Whether fa_client_write makes a new file or replaces one that exists. */
typedef enum FaWriteMode
{
    FA_WRITE_CREATE = 0, /**< name in quotes; a file that exists is refused with error 62 */
    FA_WRITE_OVERWRITE   /**< bare name; the file must exist (refusal otherwise, number not seen) */
} FaWriteMode;

/**
 * @brief Start writing one file: `length` bytes, taken through the `source` callback.
 *
 * FA_WRITE_CREATE opens the name in quotes, as SINTRAN's COPY-FILE does (capture-write.txt);
 * a file that exists is then refused with FA_ERR_FILE_EXISTS (62), and is left as it was
 * (measured on D100 2026-11-12). FA_WRITE_OVERWRITE opens the bare name, as RetroIO's sync
 * daemon did on D100 (2026-08-18). A shorter file written over a longer one reads back at its
 * new length; SINTRAN keeps the pages it had (MEASURED on D100 2026-11-12 through the gateway:
 * 2000, 3000, then 500 bytes, FILE-STATISTICS "2 PAGES , 500 BYTES IN FILE"). What
 * FA_WRITE_OVERWRITE gets back for a file that does not exist is not measured.
 * A caller that does not know whether the file exists can create, and overwrite on error 62.
 * `done` comes with 0 when the server has the whole file, with
 * FA_CLIENT_EREFUSED and the SINTRAN error when it refused a step (the file is then closed and
 * the seat given back), or with XMSG_EIO when `source` did not give the bytes (the file is
 * then left open on the server: BUGS.md C3).
 * @param length The file's length in bytes, at least 1 (no capture shows how an empty file is
 *               written, so 0 is refused rather than guessed).
 * The other arguments are as fa_client_read(); `spec` is "NAME:TYPE" in remote_user's
 * directory, without quotes.
 * @param mode   Create or overwrite.
 * @return As fa_client_read(); XMSG_EINVAL also when length is 0 or there is no `source`.
 */
int fa_client_write(FaClient *c, XmsgNode *node, uint16_t peer_node, uint16_t peer_system,
                    const char *local_user, const char *remote_user, const char *password,
                    const char *spec, uint32_t length, FaWriteMode mode, uint32_t now_ms);

/**
 * @brief Stop a read, a write or a listing early (the user pressed the escape key).
 *
 * From this call on no `opened`, `data`, `directory` or `entry` callback comes. The request
 * already sent is answered by the server and acknowledged; then the client gives the server's
 * seat back with the same teardown it uses after a refusal: close the file if one is open,
 * release, session finished, and the server's close. `done` then comes with XMSG_ECANCELED.
 * A write stopped this way ends without its set end of file: what the server keeps of the
 * blocks already written is not known from any capture.
 *
 * No capture shows what SINTRAN's own client sends when its user breaks a LIST-FILES or a
 * COPY-FILE (the captures we have were stopped while the walk ran, with nothing sent after),
 * so this is our normal teardown, not a copy of a measured break. Cancelled before the server
 * confirmed the connection, the client sends session finished at once when the confirm comes,
 * with no reserve and no release (unverified against a real server).
 *
 * @param c      Client.
 * @param now_ms Time.
 * @return XMSG_OK, also when nothing is running (then no `done` follows) and when the client
 *         is already ending (session finished sent: `done` comes with the normal result);
 *         XMSG_EINVAL for a NULL client.
 */
int fa_client_cancel(FaClient *c, uint32_t now_ms);

/** @brief The state. */
FaClientState fa_client_state(const FaClient *c);

/** @brief Plain-words name of a state. */
const char *fa_client_state_name(FaClientState st);

#ifdef __cplusplus
}
#endif

#endif /* FA_CLIENT_H */
