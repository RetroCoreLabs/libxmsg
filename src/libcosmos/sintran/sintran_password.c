/*
 * sintran_password.c - the SINTRAN III password fold.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libcosmos/sintran/sintran_password.h"

#include <stddef.h>

uint16_t sintran_password_fold(const char *password)
{
    uint16_t acc = 0;

    if (password == NULL)
    {
        return 0;
    }
    for (size_t i = 0; password[i] != '\0'; i++)
    {
        uint16_t c = (uint8_t)password[i];

        /* Letters only. Clearing bit 5 blindly would change digits, which have it set. */
        if (c >= (uint16_t)'a' && c <= (uint16_t)'z')
        {
            c = (uint16_t)(c - 32u);
        }
        /* ROL16 by 3: the three bits shifted off the top come back at the bottom. */
        acc = (uint16_t)((uint16_t)(acc << 3u) | (uint16_t)(acc >> 13u));
        acc = (uint16_t)(acc + c);
    }
    return acc;
}
