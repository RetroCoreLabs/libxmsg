/*
 * qform.h - QFORM, the tagged value stream inside every COSMOS file-access message.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * A QFORM stream is a list of tagged values. One tag byte:
 *
 *   bit 7       set on every tag; a byte with bit 7 CLEAR ends the stream
 *   bits 6..4   class: 0 constructed, 1 integer, 2 typed integer, 3 byte string, 7 selector, ...
 *   bits 3..0   value length; 0 means the length is in the next byte
 *
 * A constructed value (class 0, seen as 8C) always has its length in the next byte, because its
 * low nibble is a subtype. A length byte of 0x80 is a marker and the real length is the byte
 * after it (8C 80 46 = 70 bytes). A constructed value's contents are themselves a tagged stream,
 * and the reader walks into them. A lone byte left at the end of a level is padding, whatever
 * its value: SINTRAN pads to an even length with 0xF0, which has bit 7 set.
 *
 * Source: RetroIO Qform/QformReader.cs and DOC/protocols/fa-qform.json; every rule above is
 * pinned there by captured frames.
 */
#ifndef QFORM_H
#define QFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Deepest nesting of constructed values the reader follows. 4 is seen in captures. */
#define QFORM_MAX_DEPTH 6

/** Tag bytes as seen on the wire. */
#define QFORM_TAG_CONSTRUCTED 0x8Cu /**< constructed value, escaped length follows */
#define QFORM_TAG_INT16       0x92u /**< integer, 2 bytes */
#define QFORM_TAG_TYPED16     0xA2u /**< typed integer, 2 bytes */
#define QFORM_TAG_TYPED32     0xA4u /**< typed integer, 4 bytes */
#define QFORM_TAG_STRING      0xB0u /**< byte string, escaped length follows */
#define QFORM_TAG_SELECTOR    0xF2u /**< field selector, 2 bytes */

/** Selector value that ends a field list (F2 00FF). */
#define QFORM_SELECTOR_END 0x00FFu

/** QFORM classes, (tag & 0x70) >> 4. */
#define QFORM_CLASS_CONSTRUCTED 0u
#define QFORM_CLASS_STRING      3u
#define QFORM_CLASS_SELECTOR    7u

/** One value found by the reader. */
typedef struct QformItem
{
    uint8_t tag;          /**< the tag byte */
    uint8_t cls;          /**< (tag & 0x70) >> 4 */
    uint8_t depth;        /**< 0 at the top level, +1 inside each constructed value */
    const uint8_t *value; /**< the value bytes (for a constructed value: its contents) */
    size_t len;           /**< their count */
    size_t offset;        /**< offset of the tag in the walked buffer */
} QformItem;

/** Walk state. Plain data; set up with qform_reader_init(). */
typedef struct QformReader
{
    const uint8_t *buf;
    size_t len;
    size_t pos;
    size_t ends[QFORM_MAX_DEPTH + 1]; /**< end offset of each open level; ends[0] = len */
    uint8_t depth;
    bool failed; /**< a length ran past the end of its level, or nesting was too deep */
} QformReader;

/**
 * @brief Start walking a QFORM stream.
 * @param r   The reader.
 * @param buf The stream (NULL allowed with len 0).
 * @param len Its length in bytes.
 */
void qform_reader_init(QformReader *r, const uint8_t *buf, size_t len);

/**
 * @brief Take the next value, in stream order, descending into constructed values.
 * @param r  The reader.
 * @param it Receives the value.
 * @return true with a value; false at the end of the stream or on a malformed one (then
 *         r->failed is set).
 */
bool qform_reader_next(QformReader *r, QformItem *it);

/**
 * @brief Find the value of a top-level field: the item that follows `F2 <selector>`.
 * @param buf      The stream.
 * @param len      Its length.
 * @param selector The selector number.
 * @param it       Receives the value.
 * @return true when found.
 */
bool qform_find_field(const uint8_t *buf, size_t len, uint16_t selector, QformItem *it);

/**
 * @brief Read an integer value (1 to 4 bytes, big-endian) of an item.
 * @param it  The item.
 * @param out Receives the value.
 * @return true when the item is 1 to 4 bytes long and not constructed.
 */
bool qform_item_uint(const QformItem *it, uint32_t *out);

/** Building a stream into a caller's buffer. A write that does not fit sets `failed`. */
typedef struct QformWriter
{
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool failed;
} QformWriter;

/** @brief Start writing at buf. */
void qform_writer_init(QformWriter *w, uint8_t *buf, size_t cap);

/** @brief Append raw bytes. */
void qform_put_raw(QformWriter *w, const uint8_t *bytes, size_t n);

/** @brief Append one byte. */
void qform_put_u8(QformWriter *w, uint8_t v);

/** @brief Append a 2-byte value with its tag (92 integer, A2 typed, F2 selector). */
void qform_put_tag16(QformWriter *w, uint8_t tag, uint16_t v);

/** @brief Append A4 and a 4-byte value. */
void qform_put_typed32(QformWriter *w, uint32_t v);

/**
 * @brief Append a byte string: Bx for 1..15 bytes, B0 nn for 16..255 (not 128).
 * @param w     The writer.
 * @param bytes The string.
 * @param n     Its length; 0, 128 or more than 255 sets `failed`.
 */
void qform_put_string(QformWriter *w, const uint8_t *bytes, size_t n);

/** @brief Append `F2 <selector>`. */
void qform_put_selector(QformWriter *w, uint16_t selector);

/**
 * @brief Open a constructed value: writes 8C and a length byte that qform_end_constructed()
 *        fills in.
 * @return The offset to pass to qform_end_constructed().
 */
size_t qform_begin_constructed(QformWriter *w);

/**
 * @brief Close a constructed value opened at `at`: writes its content length. At most 127,
 *        which is all any captured request needs; a longer one sets `failed`.
 */
void qform_end_constructed(QformWriter *w, size_t at);

#ifdef __cplusplus
}
#endif

#endif /* QFORM_H */
