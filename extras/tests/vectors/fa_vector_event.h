/*
 * fa_vector_event.h - one captured FA message: who sent it and its body as hex.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#ifndef FA_VECTOR_EVENT_H
#define FA_VECTOR_EVENT_H

typedef struct FaVectorEvent
{
    char who;        /**< 'C' the client sent it, 'S' the server */
    const char *hex; /**< the message body */
} FaVectorEvent;

#endif /* FA_VECTOR_EVENT_H */
