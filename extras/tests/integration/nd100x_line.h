/*
 * nd100x_line.h - what the live tests share: the line to nd100x, the environment, and the file
 * that keeps our Flags1 count and port-word seed between runs.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Every live test that talks as node 19999 must use the SAME count file
 * (ND100X_FLAGS1_FILE, default nd100x_tad.flags1): the host keeps one count per node, so two
 * files for one node would fight and the host would drop our frames in silence.
 * Test code: static functions, included by one test file each.
 */
#ifndef ND100X_LINE_H
#define ND100X_LINE_H

#include "libxmsg/xmsg/xmsg_node.h"

#include "pair/test_pair.h"
#include "test_util.h"

static bool parse_host(const char *spec, char *host, size_t host_cap, uint16_t *port)
{
    const char *colon = strrchr(spec, ':');
    char *end = NULL;
    long p;
    if (colon == NULL || (size_t)(colon - spec) >= host_cap)
    {
        return false;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p < 1 || p > 65535)
    {
        return false;
    }
    *port = (uint16_t)p;
    return true;
}

static uint16_t env_u16(const char *name, uint16_t fallback)
{
    const char *v = getenv(name);
    return (v != NULL) ? (uint16_t)strtol(v, NULL, 10) : fallback;
}

static const char *env_str(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v != NULL) ? v : fallback;
}

static TestSocket s_sock;

/* The Flags1 file: "node N" (our own node number), "peer next" for the one host this test talks
 * to, and "seed N" for the port-word generator. All are read at start and the whole file is
 * written on every change.
 * The first line, "node N", names OUR node number. The counts are a host's view of US, so a file
 * written while we were another node number must not be used: on 2026-11-09 a count kept for
 * node 19999 was used for node 101 and D100 then dropped node 101's frames in silence. A file
 * with another node number is ignored (with a message); a file with no "node" line is taken
 * as ours. */
typedef struct FileStore
{
    const char *path;
    unsigned own;
    unsigned peer;
    unsigned next;
    unsigned seed;
    bool has_next;
    bool has_seed;
} FileStore;

static void store_read(FileStore *st)
{
    FILE *f = fopen(st->path, "r");
    char word[16];
    unsigned v;
    if (f == NULL)
    {
        return;
    }
    while (fscanf(f, "%15s %u", word, &v) == 2)
    {
        if (strcmp(word, "node") == 0)
        {
            if (v != st->own)
            {
                printf("  %s was written for node %u, we are node %u: its counts are not used\n",
                       st->path, v, st->own);
                st->has_next = false;
                st->has_seed = false;
                break;
            }
        }
        else if (strcmp(word, "seed") == 0)
        {
            st->seed = v;
            st->has_seed = true;
        }
        else
        {
            st->peer = (unsigned)strtoul(word, NULL, 10);
            st->next = v;
            st->has_next = true;
        }
    }
    fclose(f);
}

static int store_write(const FileStore *st)
{
    FILE *f = fopen(st->path, "w");
    if (f == NULL)
    {
        return -1;
    }
    fprintf(f, "node %u\n", st->own);
    if (st->has_next)
    {
        fprintf(f, "%u %u\n", st->peer, st->next);
    }
    if (st->has_seed)
    {
        fprintf(f, "seed %u\n", st->seed);
    }
    return (fclose(f) == 0) ? 0 : -1;
}

static int store_load(void *ctx, uint16_t peer, uint16_t *next)
{
    const FileStore *st = (const FileStore *)ctx;
    if (!st->has_next || st->peer != peer)
    {
        return -1;
    }
    *next = (uint16_t)st->next;
    return 0;
}

static int store_save(void *ctx, uint16_t peer, uint16_t next)
{
    FileStore *st = (FileStore *)ctx;
    st->peer = peer;
    st->next = next;
    st->has_next = true;
    return store_write(st);
}

static int store_load_seed(void *ctx, uint16_t *seed)
{
    const FileStore *st = (const FileStore *)ctx;
    if (!st->has_seed)
    {
        return -1;
    }
    *seed = (uint16_t)st->seed;
    return 0;
}

static int store_save_seed(void *ctx, uint16_t seed)
{
    FileStore *st = (FileStore *)ctx;
    st->seed = seed;
    st->has_seed = true;
    return store_write(st);
}

static int line_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    int r = test_tcp_write(s_sock, b, n);
    return (r < 0) ? XMSG_ESTATE : r;
}

#endif /* ND100X_LINE_H */
