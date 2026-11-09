/*
 * test_xmsg_flags1.c - the per-peer datagram number: allocate at send, persist at send, reset on
 * reachability, step down on XENSE, never 0xFFFF.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xmsg/xmsg_flags1.h"

#include "test_util.h"

/* A fake store: one value per node, plus a log of every save. */
typedef struct FakeStore
{
    uint16_t node[8];
    uint16_t value[8];
    bool has[8];
    int saves;
    int loads;
    uint16_t last_saved;
} FakeStore;

static int fake_load(void *ctx, uint16_t node, uint16_t *next)
{
    FakeStore *s = (FakeStore *)ctx;
    s->loads++;
    for (int i = 0; i < 8; i++)
    {
        if (s->has[i] && s->node[i] == node)
        {
            *next = s->value[i];
            return 0;
        }
    }
    return -1;
}

static int fake_save(void *ctx, uint16_t node, uint16_t next)
{
    FakeStore *s = (FakeStore *)ctx;
    s->saves++;
    s->last_saved = next;
    for (int i = 0; i < 8; i++)
    {
        if (s->has[i] && s->node[i] == node)
        {
            s->value[i] = next;
            return 0;
        }
    }
    for (int i = 0; i < 8; i++)
    {
        if (!s->has[i])
        {
            s->has[i] = true;
            s->node[i] = node;
            s->value[i] = next;
            return 0;
        }
    }
    return -1;
}

static void fresh_peer_counts_from_zero_and_saves_every_frame(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    xmsg_flags1_init(&t, &port, 1);
    uint16_t f;
    for (uint16_t i = 0; i < 5; i++)
    {
        CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
        CHECK_EQ(f, i);
        /* The store always promises the next number (persist at send). */
        CHECK_EQ(store.last_saved, i + 1);
    }
    CHECK_EQ(store.saves, 5);
    CHECK_EQ(xmsg_flags1_peek(&t, 100), 5);
}

static void restart_resumes_from_the_store(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, &port, 1);
    for (int i = 0; i < 7; i++)
    {
        xmsg_flags1_allocate(&t, 100, &f);
    }
    CHECK_EQ(f, 6);
    /* "Restart": a new table over the same store continues at 7, never behind. */
    xmsg_flags1_init(&t, &port, 1);
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 7);
}

static void persist_every_n_saves_a_bound_ahead(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, &port, 8);
    for (int i = 0; i < 20; i++)
    {
        xmsg_flags1_allocate(&t, 100, &f);
    }
    CHECK_EQ(f, 19);
    /* Saves at 0 (bound 8), 8 (bound 16), 16 (bound 24): three writes for twenty frames. */
    CHECK_EQ(store.saves, 3);
    CHECK_EQ(store.last_saved, 24);
    /* After a restart we resume at the bound: ahead by at most persist_every, never behind. */
    xmsg_flags1_init(&t, &port, 8);
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 24);
}

static void reachability_resets_to_zero_and_saves(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, &port, 1);
    for (int i = 0; i < 10; i++)
    {
        xmsg_flags1_allocate(&t, 100, &f);
    }
    xmsg_flags1_reset(&t, 100);
    CHECK_EQ(store.last_saved, 0);
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 0);
}

static void xense_steps_down_then_gives_up(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, &port, 1);
    for (int i = 0; i < 0x30; i++)
    {
        xmsg_flags1_allocate(&t, 100, &f);
    }
    CHECK_EQ(f, 0x2F);
    /* The peer refused 0x2F: the next frame goes out at 0x2E. */
    CHECK(xmsg_flags1_on_xense(&t, 100, 0x2F));
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 0x2E);
    /* Eight steps in a row are tolerated, the ninth gives up and leaves the count alone. */
    for (int i = 0; i < 7; i++)
    {
        CHECK(xmsg_flags1_on_xense(&t, 100, (uint16_t)(0x2E - i)));
    }
    uint16_t before = xmsg_flags1_peek(&t, 100);
    CHECK(!xmsg_flags1_on_xense(&t, 100, 0x27));
    CHECK_EQ(xmsg_flags1_peek(&t, 100), before);
    /* A delivery clears the run of steps. */
    xmsg_flags1_on_delivered(&t, 100);
    CHECK(xmsg_flags1_on_xense(&t, 100, 0x27));
    /* Refusing number 0 cannot be stepped below. */
    CHECK(!xmsg_flags1_on_xense(&t, 100, 0));
}

static void peers_are_independent_and_0xffff_is_skipped(void)
{
    FakeStore store;
    memset(&store, 0, sizeof store);
    store.has[0] = true;
    store.node[0] = 102;
    store.value[0] = 0xFFFE;
    XmsgFlags1Port port = {&store, fake_load, fake_save, NULL, NULL};
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, &port, 1);
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 0);
    CHECK_EQ(xmsg_flags1_allocate(&t, 102, &f), XMSG_OK);
    CHECK_EQ(f, 0xFFFE);
    CHECK_EQ(xmsg_flags1_allocate(&t, 102, &f), XMSG_OK);
    CHECK_EQ(f, 0); /* 0xFFFF is reachability, so the count wraps past it */
    CHECK_EQ(xmsg_flags1_allocate(&t, 100, &f), XMSG_OK);
    CHECK_EQ(f, 1);
}

static void table_full_is_reported(void)
{
    XmsgFlags1Table t;
    uint16_t f;
    xmsg_flags1_init(&t, NULL, 1); /* no store: fine, warns once */
    for (int i = 0; i < XMSG_MAX_PEERS; i++)
    {
        CHECK_EQ(xmsg_flags1_allocate(&t, (uint16_t)(200 + i), &f), XMSG_OK);
    }
    CHECK_EQ(xmsg_flags1_allocate(&t, 999, &f), XMSG_ENOSPC);
}

int main(void)
{
    printf("[xmsg_flags1]\n");
    fresh_peer_counts_from_zero_and_saves_every_frame();
    restart_resumes_from_the_store();
    persist_every_n_saves_a_bound_ahead();
    reachability_resets_to_zero_and_saves();
    xense_steps_down_then_gives_up();
    peers_are_independent_and_0xffff_is_skipped();
    table_full_is_reported();
    TEST_DONE();
}
