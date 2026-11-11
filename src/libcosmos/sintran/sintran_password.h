/*
 * sintran_password.h - the SINTRAN III password fold.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * SINTRAN keeps a password as ONE 16-bit word: acc = ROL16(acc, 3) + c for each typed character,
 * with only the letters a..z made upper case first. The empty password is 0. A file-access
 * request carries this word, never the text. Not a cryptographic hash: the word is as good as
 * the password itself.
 *
 * Source: RetroIO Xmsg.Api SintranPassword.cs, carved from L-VSX-500 segment S3CP routine LOGIN
 * (octal 060616) and confirmed on the wire ("secret" -> 6D 2A in a captured reserve).
 */
#ifndef SINTRAN_PASSWORD_H
#define SINTRAN_PASSWORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Fold a typed password into the word SINTRAN stores and sends.
 * @param password NUL-terminated text; NULL or "" gives 0.
 * @return The word.
 */
uint16_t sintran_password_fold(const char *password);

#ifdef __cplusplus
}
#endif

#endif /* SINTRAN_PASSWORD_H */
