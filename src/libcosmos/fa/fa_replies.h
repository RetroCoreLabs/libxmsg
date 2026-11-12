/*
 * fa_replies.h - reading what a COSMOS file-access server sends back.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A reply has the request's shape: 07F0 <our word> <counter> 00 90BB | 92 <op> | 92 <seq> |
 * fields | F2 00FF. A refusal carries F2 0001 A2 <SINTRAN error> (46 no such file, 48 wrong
 * password, 197 end of directory); a success has no selector 1. A data message is 1032 bytes:
 * the envelope and 1024 raw bytes, no QFORM.
 *
 * Source: capture-read.txt, capture-open-error.txt; RetroIO Fa/FaRefusalCodec.cs,
 * FaOpenFileCodec.cs.
 */
#ifndef FA_REPLIES_H
#define FA_REPLIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libcosmos/fa/fa_envelope.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** SINTRAN error numbers seen in refusals. */
#define FA_ERR_NO_SUCH_FILE     46u
#define FA_ERR_WRONG_PASSWORD   48u
#define FA_ERR_FILE_EXISTS      62u /**< 076 octal: a create of a file that exists */
#define FA_ERR_END_OF_DIRECTORY 197u

/** A parsed reply. */
typedef struct FaReply
{
    FaEnvelope env;
    uint16_t op;           /**< the operation it answers */
    uint16_t seq;          /**< the sequence of the request it answers */
    const uint8_t *fields; /**< the QFORM after op and seq */
    size_t fields_len;
    bool refused;          /**< selector 1 carries a SINTRAN error */
    uint16_t error;        /**< that error, when refused */
} FaReply;

/**
 * @brief Parse a reply body (not a data message).
 * @return true when it has an envelope and the 92 op 92 seq head.
 */
bool fa_reply_parse(const uint8_t *body, size_t len, FaReply *r);

/**
 * @brief Read an open reply: selector 2 the file number (A2), selector 3 the length in bytes (A4).
 * @return true when both are there.
 */
bool fa_reply_open_info(const FaReply *r, uint16_t *file_number, uint32_t *length);

/**
 * @brief Read the file number of an open reply: selector 2 (A2).
 *
 * An open for writing is answered with the file number only. MEASURED: capture-write.txt
 * 02:33:02.320, F2 0002 A2 0040 and no selector 3.
 * @return true when it is there.
 */
bool fa_reply_file_number(const FaReply *r, uint16_t *file_number);

/**
 * @brief Find the 64-byte object entry in a file information reply: F2 0002 8C 42 B0 40 <64>.
 * @return A pointer to the 64 bytes, or NULL.
 */
const uint8_t *fa_reply_object_entry(const FaReply *r);

/** Bytes in an NDFS object entry. */
#define FA_OBJECT_ENTRY_SIZE 64u

/**
 * One file as its 64-byte NDFS object entry describes it. Offsets (RetroIO
 * RetroIO.Storage.Ndfs ObjectEntry.cs, checked against the records of capture-list-files.txt):
 *   0 header word, 2 name (16, ended by an apostrophe), 18 type (4, ended by an apostrophe
 *   when shorter), 22/24 next/previous version (object indexes), 26 access word, 28 attribute
 *   word, 30 device number, 34 owning user, 35 object slot, 36 opened now, 38 opened in total,
 *   40 created, 44 last read, 48 last written (ND date words), 52 pages, 56 bytes minus one,
 *   60 file pointer.
 * The version NUMBER SINTRAN prints (";1") is not a field: it would come from the version chain.
 */
typedef struct FaFileEntry
{
    char name[17];         /**< without the terminator */
    char type[5];          /**< without the terminator; "" when the file has none */
    uint16_t header;       /**< 0x8000 entry used; bit 12 tracks a real byte count */
    uint16_t next_version; /**< object index of the next version (itself when only one) */
    uint16_t prev_version; /**< object index of the previous version */
    uint16_t access;       /**< the access word (public / friend / own tiers), raw */
    uint16_t attributes;   /**< 0x0008 indexed, 0x0020 contiguous (allocated), ... raw */
    uint16_t device;       /**< device number of a peripheral file */
    uint8_t owner_index;   /**< byte 34: the owning user's index */
    uint8_t slot;          /**< byte 35 */
    uint16_t open_now;     /**< opened now */
    uint16_t open_total;   /**< opened in total ("OPENED n TIMES") */
    uint32_t created;      /**< ND date word; 0 = none. See fa_nd_date_split() */
    uint32_t last_read;    /**< ND date word; 0 = never */
    uint32_t last_write;   /**< ND date word; 0 = never */
    uint32_t pages;        /**< pages in the file (2048 bytes each) */
    uint32_t bytes;        /**< bytes in the file; 0 when the entry carries none */
    bool has_bytes;        /**< false when the entry carries no byte count (FFFFFFFF) */
    uint32_t file_pointer; /**< type bits and page id, raw */
    /** The entry's slot in the user's directory, from the walk reply (0C/78), not from the 64
     *  bytes; 0 when the entry came another way. It is the FILE number SINTRAN prints: MEASURED
     *  equal for all 18 :MODE files of SYSTEM on D100 against SINTRAN's own LIST-FILES
     *  (38 to 239; HDLCGW session, 2026-11-12). Only files below 256 were compared; RetroIO's
     *  NDFS reader says a user's later blocks of 256 files number differently on disk
     *  (ObjectEntry.FileNumber), so a number of 256 or more is unverified. */
    uint16_t object_index;
} FaFileEntry;

/** @brief Read a 64-byte object entry (object_index is set to 0). */
void fa_entry_parse(const uint8_t *raw, FaFileEntry *e);

/** One ND date word taken apart. */
typedef struct FaNdDate
{
    uint16_t year; /**< 1950..2013 */
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} FaNdDate;

/**
 * @brief Split an ND date word: year-1950 in bits 31..26, month 25..22, day 21..17,
 *        hour 16..12, minute 11..6, second 5..0 (RetroIO NdDateTime.FromNdDate).
 * @return false for 0 (no date) or a value out of range.
 */
bool fa_nd_date_split(uint32_t raw, FaNdDate *d);

/**
 * @brief Read a next-entry reply (0C/78): F2 0002 8C <len> A2 <user index> A2 <object index>
 *        A2 <unknown, 0001 in every capture> B0 40 <entry>.
 * @return true with all three out values.
 */
bool fa_reply_walk_entry(const FaReply *r, uint16_t *user_index, uint16_t *object_index,
                         const uint8_t **entry);

/**
 * @brief Read the name out of a directory reply (0C/A4: 8C <len> B0 2A <42 bytes>, the name at
 *        byte 10) or a user reply (0C/8C: 8C <len> B0 10 <16 bytes>, the name at byte 0). The
 *        name ends at its apostrophe.
 * @param r      The reply.
 * @param offset 10 for a directory, 0 for a user.
 * @param out    Receives the name, NUL-terminated.
 * @param cap    Its size.
 * @return true when found.
 */
bool fa_reply_entry_name(const FaReply *r, size_t offset, char *out, size_t cap);

/** @brief Plain-words name of a SINTRAN error number from a refusal ("" when not known). */
const char *fa_error_name(uint16_t error);

#ifdef __cplusplus
}
#endif

#endif /* FA_REPLIES_H */
