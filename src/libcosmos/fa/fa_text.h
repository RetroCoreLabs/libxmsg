/*
 * fa_text.h - the text forms SINTRAN prints for a file: access letters and words, dates, and
 * file names in SINTRAN and FTP form.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Sources, per form:
 *   - Access bits: R bit 0, W 1, A 2, C 3, D 4 in each tier; OWN bits 0-4, FRIEND 5-9, PUBLIC
 *     10-14 (RetroIO Storage.Ndfs SintranAccessForm).
 *   - Access words: D100's own FILE-STATISTICS of STARTX3:MODE, access 003777 (2026-11-11,
 *     read by the HDLCGW session): "READ, WRITE, APPEND, COMMON, DIRECTORY"; "NONE" from the
 *     SINTRAN printouts quoted in RetroIO's FaDirectoryListingTests.
 *   - Access letters, FTP date and the ";1" / "-1" names: SINTRAN's own TCP/IP FTP server
 *     (C07) printing LIST and NLST, measured by the HDLCGW session on 2026-11-12:
 *       no. own  fri  pub    pgs    bytes  last modified    name
 *        73 rwacdrwacdr----     4     7471  27 Sep 98 18:14 ECHOPL:PLNC;1
 *        59 rwa-drwa--r----     1      832  00 pre 50 00:00 AIP-CONFIG:SYMB;1
 *     and after its CONV command NLST gives NAME.TYPE-1.
 *   - FILE-STATISTICS date: "CREATED 16.55.35  JULY 6, 1998" (RetroIO FaDirectoryListingTests).
 */
#ifndef FA_TEXT_H
#define FA_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "libcosmos/fa/fa_replies.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Shifts of the three access tiers in the access word. */
#define FA_ACCESS_OWN_SHIFT    0u
#define FA_ACCESS_FRIEND_SHIFT 5u
#define FA_ACCESS_PUBLIC_SHIFT 10u

/** Room for each text, with the NUL. */
#define FA_TEXT_ACCESS_LETTERS 16u /**< "rwacdrwacdr----" */
#define FA_TEXT_ACCESS_WORDS   40u /**< "READ, WRITE, APPEND, COMMON, DIRECTORY" */
#define FA_TEXT_DATE_STAT      32u /**< "16.55.35  SEPTEMBER 30, 1998" */
#define FA_TEXT_DATE_FTP       16u /**< "27 Sep 98 18:14" */
#define FA_TEXT_NAME           32u /**< 16 + ":" + 4 + ";" + version */

/** The forms of a file name. */
typedef enum FaNameForm
{
    FA_NAME_SINTRAN = 0,     /**< NAME:TYPE (a file with no type: "NAME:") */
    FA_NAME_SINTRAN_VERSION, /**< NAME:TYPE;v */
    FA_NAME_FTP,             /**< NAME.TYPE (no type: "NAME.") */
    FA_NAME_FTP_VERSION      /**< NAME.TYPE-v */
} FaNameForm;

/**
 * @brief The access word as SINTRAN's FTP server prints it: own, friend, public, five letters
 *        each, "rwacd" with '-' for a right not given ("rwacdrwacdr----").
 * @param access The access word (FaFileEntry.access).
 * @param out    Buffer of at least FA_TEXT_ACCESS_LETTERS.
 * @param cap    Its size.
 * @return 15, or -1 when it does not fit.
 */
int fa_text_access_letters(uint16_t access, char *out, size_t cap);

/**
 * @brief One access tier as FILE-STATISTICS prints it: "READ, WRITE, APPEND, COMMON, DIRECTORY",
 *        the rights given in that order, or "NONE".
 * @param access The access word.
 * @param shift  FA_ACCESS_OWN_SHIFT, FA_ACCESS_FRIEND_SHIFT or FA_ACCESS_PUBLIC_SHIFT.
 * @param out    Buffer of at least FA_TEXT_ACCESS_WORDS.
 * @param cap    Its size.
 * @return The length written, or -1 on a bad argument or when it does not fit.
 */
int fa_text_access_words(uint16_t access, unsigned shift, char *out, size_t cap);

/**
 * @brief An ND date as FILE-STATISTICS prints it: "16.55.35  JULY 6, 1998" (two spaces after
 *        the time; the day without a leading zero).
 * @param raw An ND date word (FaFileEntry.created, last_read, last_write).
 * @return The length written, 0 when the word holds no date (0 or out of range; what SINTRAN
 *         prints then is the caller's choice), -1 on a bad argument or when it does not fit.
 */
int fa_text_date_stat(uint32_t raw, char *out, size_t cap);

/**
 * @brief An ND date as SINTRAN's FTP server prints it in LIST: "27 Sep 98 18:14", and
 *        "00 pre 50 00:00" for a word that holds no date (both measured).
 *
 * The day is written with two digits; a day below 10 is not in the measurement, so its leading
 * zero is inferred from the "00" of the no-date form (unverified). Only "Sep" is measured;
 * the other months are written the same way, "Jan" to "Dec" (unverified). Which of the entry's
 * dates the FTP server shows is not known; the caller picks one.
 * @return 15, or -1 on a bad argument or when it does not fit.
 */
int fa_text_date_ftp(uint32_t raw, char *out, size_t cap);

/**
 * @brief The version number SINTRAN prints after the name (";1"), as far as the entry tells it.
 *
 * A file with one version points its next-version word at itself, so that is version 1
 * (System Supervisor manual's annotated object entry, quoted in RetroIO FaFolderEntry, and every
 * record of capture-list-files.txt).
 * Which number a file with several versions has is not worked out (it would need the chain and
 * which end SINTRAN counts from), so that gives 0.
 * @param e The entry, with object_index filled in (fa_client fills it from the walk reply).
 * @return 1, or 0 when not known.
 */
uint16_t fa_text_version(const FaFileEntry *e);

/**
 * @brief A file name in one of the four forms.
 * @param e    The entry.
 * @param form Which form.
 * @param out  Buffer of at least FA_TEXT_NAME.
 * @param cap  Its size.
 * @return The length written; -1 on a bad argument, when it does not fit, or when a form with a
 *         version is asked for and fa_text_version() does not know it.
 */
int fa_text_name(const FaFileEntry *e, FaNameForm form, char *out, size_t cap);

/**
 * @brief The SINTRAN name for an FTP name, for a read, a write or a listing:
 *        "NAME.TYPE" and "NAME.TYPE-v" give "NAME:TYPE", "NAME." gives "NAME:".
 *
 * The last dot becomes the colon, and a "-" with only digits after it, past that dot, is the
 * version and is dropped. A name that already holds ':' is copied as it is, and so is one with
 * no dot (a bare name, or a prefix for a listing). Letter case is not changed.
 * @param ftp The FTP name.
 * @param out Buffer for the SINTRAN name.
 * @param cap Its size.
 * @return The length written, or -1 on a bad argument or when it does not fit.
 */
int fa_text_name_from_ftp(const char *ftp, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FA_TEXT_H */
