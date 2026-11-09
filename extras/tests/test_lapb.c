/*
 * test_lapb.c - the ND LAPB state machine against the spec vectors S1-S7 and the measured rules.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * S1-S7 are the ND LAPB spec section 8.2 acceptance vectors as pinned byte for byte in RetroIO
 * LapbConformanceVectorsTests.cs (local node 100 = 00 64, peer node 102 = 00 66). The timer, slot,
 * gating and burst tests carry over the contracts of LapbTimerTests, LapbLinkUpGatingTests,
 * LapbIsUpIsTheSendGateTests, LapbFrmrTests and LapbAnnounceOrderingTests.
 */
#include "libxmsg/lapb/lapb.h"

#include "test_util.h"

#define L100 "00 64"
#define P102 "00 66"

/* Collected frames and information fields. */
typedef struct Collector
{
    uint8_t sent[32][XMSG_LAPB_N1 + 2];
    size_t sent_len[32];
    int sent_count;
    uint8_t got[16][XMSG_LAPB_N1];
    size_t got_len[16];
    int got_count;
    int state_changes;
    int up_changes;
    bool last_up;
    char last_reason[64];
} Collector;

static Collector s_c;

static void on_info(void *ctx, const uint8_t *info, size_t len)
{
    Collector *c = (Collector *)ctx;
    if (c->got_count < 16)
    {
        memcpy(c->got[c->got_count], info, len);
        c->got_len[c->got_count] = len;
    }
    c->got_count++;
}

static void on_state(void *ctx, LapbState state, bool up, const char *reason)
{
    Collector *c = (Collector *)ctx;
    (void)state;
    c->state_changes++;
    if (up != c->last_up)
    {
        c->up_changes++;
    }
    c->last_up = up;
    strncpy(c->last_reason, reason, sizeof c->last_reason - 1);
    c->last_reason[sizeof c->last_reason - 1] = '\0';
}

/* Pull every due frame into the collector. */
static void drain(LapbLink *l)
{
    for (;;)
    {
        uint8_t out[XMSG_LAPB_N1 + 2];
        int n = lapb_pull_tx(l, out, sizeof out);
        CHECK(n >= 0);
        if (n <= 0)
        {
            return;
        }
        if (s_c.sent_count < 32)
        {
            memcpy(s_c.sent[s_c.sent_count], out, (size_t)n);
            s_c.sent_len[s_c.sent_count] = (size_t)n;
        }
        s_c.sent_count++;
    }
}

static void clear_sent(void)
{
    s_c.sent_count = 0;
    s_c.got_count = 0;
}

static void deliver(LapbLink *l, uint32_t now, const char *hex)
{
    uint8_t frame[700];
    int n = test_hex_to_bytes(hex, frame, sizeof frame);
    CHECK(n >= 2);
    lapb_rx_frame(l, frame, (size_t)n, now);
    drain(l);
}

static void new_link(LapbLink *l, uint16_t node, const LapbConfig *custom)
{
    LapbConfig cfg;
    memset(&s_c, 0, sizeof s_c);
    if (custom != NULL)
    {
        cfg = *custom;
    }
    else
    {
        lapb_config_default(&cfg, node);
    }
    CHECK_EQ(lapb_init(l, &cfg, &s_c, on_info, on_state), XMSG_OK);
}

/* Our SABM, the peer's UA: connected as initiator, like the C# NewConnectedInitiator. */
static void new_connected_initiator(LapbLink *l, const LapbConfig *custom)
{
    new_link(l, 100, custom);
    lapb_connect(l, 0);
    drain(l);
    deliver(l, 0, "01 73 " P102);
    /* The peer has not sent its SABM yet; make the link fully up the ND way. */
    deliver(l, 0, "01 3F " P102);
    CHECK(lapb_is_up(l));
    clear_sent();
}

static int send_info(LapbLink *l, uint32_t now, const char *hex)
{
    uint8_t *info;
    size_t cap;
    int rc = lapb_tx_reserve(l, &info, &cap);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    int n = test_hex_to_bytes(hex, info, cap);
    CHECK(n > 0);
    rc = lapb_tx_commit(l, (size_t)n, now);
    drain(l);
    return rc;
}

/* ---------------------------------------------------------------- S1..S7 */

static void s1_balanced_bring_up(void)
{
    LapbLink l;
    new_link(&l, 100, NULL);
    lapb_connect(&l, 0);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 3F " L100); /* TX SABM P=1 + node 100 */
    CHECK(!lapb_is_up(&l));
    clear_sent();

    deliver(&l, 0, "01 3F " P102); /* RX SABM P=1 + node 102 */
    CHECK_EQ(s_c.sent_count, 2);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 73 " L100); /* TX UA F=1 + node 100 */
    CHECK_BYTES(s_c.sent[1], s_c.sent_len[1], "09 01 " L100); /* unsolicited RR */
    CHECK_EQ(lapb_peer_node(&l), 0x0066);
    CHECK_EQ(lapb_state(&l), LAPB_CONNECTED);
    CHECK(!lapb_is_up(&l)); /* our SABM is not yet acknowledged */
    clear_sent();

    deliver(&l, 0, "01 73 " P102); /* RX UA F=1 + node 102 */
    CHECK_EQ(lapb_state(&l), LAPB_CONNECTED);
    CHECK(lapb_is_up(&l));
    CHECK_EQ(l.vs, 0);
    CHECK_EQ(l.vr, 0);
    CHECK_EQ(l.va, 0);
    CHECK_EQ(s_c.sent_count, 0); /* a late UA sends nothing more */
}

static void s2_rej_go_back_n(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    CHECK_EQ(send_info(&l, 0, "D0"), XMSG_OK);
    CHECK_EQ(send_info(&l, 0, "D1"), XMSG_OK);
    CHECK_EQ(s_c.sent_count, 2);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 00 D0"); /* I N(S)=0 N(R)=0, odd -> 0x89 */
    CHECK_BYTES(s_c.sent[1], s_c.sent_len[1], "89 02 D1"); /* I N(S)=1 N(R)=0 */
    clear_sent();

    deliver(&l, 0, "09 09 " P102); /* RX REJ N(R)=0 */
    CHECK_EQ(s_c.sent_count, 2);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 00 D0");
    CHECK_BYTES(s_c.sent[1], s_c.sent_len[1], "89 02 D1");
    CHECK_EQ(l.stats.retransmissions, 2);
    CHECK_EQ(l.stats.rej_received, 1);
}

static void s3_rnr_hold_and_resume(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    CHECK_EQ(send_info(&l, 0, "C0"), XMSG_OK);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 00 C0");
    clear_sent();

    deliver(&l, 0, "09 25 " P102); /* RX RNR N(R)=1 */
    CHECK(l.peer_busy);
    CHECK_EQ(l.va, 1);
    /* A new frame is accepted into the window but held back while the peer is busy. */
    CHECK_EQ(send_info(&l, 0, "C1"), XMSG_OK);
    CHECK_EQ(s_c.sent_count, 0);

    deliver(&l, 0, "09 21 " P102); /* RX RR N(R)=1 */
    CHECK(!l.peer_busy);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 02 C1");
}

static void s4_t1_times_n2_reestablishes_when_opted_in(void)
{
    LapbLink l;
    LapbConfig fast;
    lapb_config_default(&fast, 100);
    fast.t1_ms = 100;
    fast.t3_ms = 1000;
    fast.n2 = 3;
    fast.reestablish_on_failure = true;
    new_connected_initiator(&l, &fast);
    CHECK_EQ(send_info(&l, 0, "B0"), XMSG_OK);
    clear_sent();

    lapb_tick(&l, 100);
    drain(&l); /* retry 1: RR P=1 */
    lapb_tick(&l, 200);
    drain(&l);
    lapb_tick(&l, 300);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 3);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 11 " L100); /* RR P=1 N(R)=0 */
    clear_sent();
    lapb_tick(&l, 400); /* retry 4 > N2 -> re-establish */
    drain(&l);
    CHECK_EQ(lapb_state(&l), LAPB_SABM_SENT);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 3F " L100);
}

static void s5_gap_single_rej(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    deliver(&l, 0, "09 02 21 13"); /* RX I N(S)=1: a gap */
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 09 " L100); /* TX REJ N(R)=0 */
    clear_sent();
    deliver(&l, 0, "09 04 21 13"); /* RX I N(S)=2: still ahead -> silent */
    CHECK_EQ(s_c.sent_count, 0);
    CHECK_EQ(s_c.got_count, 0);
    CHECK_EQ(l.stats.rej_sent, 1);
}

static void s6_poll_answered_with_final(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    deliver(&l, 0, "09 11 " P102); /* RX RR P=1 N(R)=0 */
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 11 " L100); /* TX RR F=1 N(R)=0 */
}

static void s7_mid_session_sabm_reset(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    deliver(&l, 0, "09 00 21 13"); /* N(S)=0 -> V(R)=1 */
    deliver(&l, 0, "09 02 21 13"); /* N(S)=1 -> V(R)=2 */
    CHECK_EQ(send_info(&l, 0, "55"), XMSG_OK);
    CHECK_EQ(send_info(&l, 0, "56"), XMSG_OK);
    CHECK_EQ(send_info(&l, 0, "57"), XMSG_OK);
    CHECK_EQ(l.vs, 3);
    CHECK_EQ(l.vr, 2);
    CHECK_EQ(s_c.got_count, 2);
    clear_sent();

    deliver(&l, 0, "01 3F " P102); /* RX SABM mid-session */
    CHECK_EQ(lapb_state(&l), LAPB_CONNECTED);
    CHECK(lapb_is_up(&l));
    CHECK_EQ(l.vs, 0);
    CHECK_EQ(l.vr, 0);
    CHECK_EQ(l.va, 0);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 73 " L100);
    clear_sent();

    deliver(&l, 0, "09 00 21 13"); /* next I N(S)=0 delivered afresh */
    CHECK_EQ(s_c.got_count, 1);
    CHECK_EQ(l.vr, 1);
}

/* ------------------------------------------------------------- gating */

static void peer_sabm_alone_does_not_bring_the_link_up(void)
{
    /* LapbLinkUpGatingTests: we sent a SABM; the peer's SABM arrives first. CONNECTED, not up. */
    LapbLink l;
    new_link(&l, 100, NULL);
    lapb_connect(&l, 0);
    drain(&l);
    deliver(&l, 0, "01 3F " P102);
    CHECK_EQ(lapb_state(&l), LAPB_CONNECTED);
    CHECK(!lapb_is_up(&l));
    CHECK_EQ(lapb_tx_free_slots(&l), 0);
    uint8_t *info;
    size_t cap;
    CHECK_EQ(lapb_tx_reserve(&l, &info, &cap), XMSG_ESTATE);
    deliver(&l, 0, "01 73 " P102);
    CHECK(lapb_is_up(&l));
    CHECK_EQ(lapb_tx_free_slots(&l), 7);
}

static void unanswered_sabm_is_resent_after_the_peer_sabm(void)
{
    /* Measured 2026-11-09 on RetroCore D100: the SABMs crossed, D100 answered ours with an RR
     * instead of a UA. Our SABM must be sent again on T1 until a UA arrives. */
    LapbLink l;
    new_link(&l, 101, NULL);
    lapb_connect(&l, 0);
    drain(&l);
    deliver(&l, 10, "01 3F 00 64");
    drain(&l); /* our UA and RR */
    deliver(&l, 20, "09 01 00 64");
    CHECK(!lapb_is_up(&l));
    clear_sent();
    lapb_tick(&l, 3009);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 0); /* T1 runs from the peer SABM at 10 ms */
    lapb_tick(&l, 3010);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 3F 00 65");
    deliver(&l, 3050, "01 73 00 64");
    CHECK(lapb_is_up(&l));
    clear_sent();
    lapb_tick(&l, 9000); /* T1 stopped by the UA: no more SABMs, no polls */
    drain(&l);
    CHECK_EQ(s_c.sent_count, 0);
}

static void passive_station_comes_up_on_the_peer_sabm_alone(void)
{
    /* A station that never sent a SABM has no half of its own to wait for. */
    LapbLink l;
    new_link(&l, 102, NULL);
    deliver(&l, 0, "01 3F " L100);
    CHECK(lapb_is_up(&l));
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 73 " P102);
    CHECK_BYTES(s_c.sent[1], s_c.sent_len[1], "09 01 " P102);
    CHECK_EQ(lapb_peer_node(&l), 100);
}

static void up_callback_fires_once_per_change(void)
{
    LapbLink l;
    new_link(&l, 100, NULL);
    lapb_connect(&l, 0);
    drain(&l);
    deliver(&l, 0, "01 3F " P102);
    deliver(&l, 0, "01 73 " P102);
    CHECK(s_c.last_up);
    CHECK_EQ(s_c.up_changes, 1);
    lapb_disconnect(&l, 0);
    drain(&l);
    CHECK_BYTES(s_c.sent[s_c.sent_count - 1], s_c.sent_len[s_c.sent_count - 1], "01 53 " L100);
    deliver(&l, 0, "01 73 " P102); /* UA to our DISC */
    CHECK_EQ(lapb_state(&l), LAPB_DISCONNECTED);
    CHECK(!s_c.last_up);
    CHECK_EQ(s_c.up_changes, 2);
}

/* ------------------------------------------------------------- timers */

static void n2_by_default_takes_the_link_down_with_no_sabm(void)
{
    LapbLink l;
    LapbConfig fast;
    lapb_config_default(&fast, 100);
    fast.t1_ms = 100;
    fast.n2 = 2;
    new_connected_initiator(&l, &fast);
    CHECK_EQ(send_info(&l, 0, "B0"), XMSG_OK);
    clear_sent();
    lapb_tick(&l, 100);
    lapb_tick(&l, 200);
    lapb_tick(&l, 300);
    drain(&l);
    CHECK_EQ(lapb_state(&l), LAPB_DISCONNECTED);
    CHECK(!lapb_is_up(&l));
    for (int i = 0; i < s_c.sent_count; i++)
    {
        CHECK(!(s_c.sent[i][0] == 0x01 && s_c.sent[i][1] == 0x3F)); /* no SABM anywhere */
    }
    CHECK(strstr(s_c.last_reason, "N2") != NULL);
    CHECK_EQ(l.stats.t1_timeouts, 3);
}

static void t3_sends_one_poll_and_does_not_start_t1(void)
{
    LapbLink l;
    LapbConfig cfg;
    lapb_config_default(&cfg, 100);
    cfg.t3_ms = 1000;
    cfg.t1_ms = 100;
    new_connected_initiator(&l, &cfg);
    CHECK(l.t3_running);
    CHECK(!l.t1_running);
    CHECK_EQ(lapb_tick(&l, 999), 0);
    CHECK_EQ(lapb_tick(&l, 1000), 1);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 11 " L100); /* RR P=1 */
    CHECK(!l.t3_running);
    CHECK(!l.t1_running); /* an ignored poll must not cascade */
    clear_sent();
    /* No answer for a long time: nothing more is sent, the link stays up. */
    for (uint32_t t = 1100; t < 60000; t += 100)
    {
        lapb_tick(&l, t);
    }
    drain(&l);
    CHECK_EQ(s_c.sent_count, 0);
    CHECK(lapb_is_up(&l));
    /* The peer answers with F=1: T3 re-arms. */
    deliver(&l, 60000, "09 11 " P102);
    CHECK(l.t3_running);
}

static void t1_in_sabm_sent_resends_the_sabm(void)
{
    LapbLink l;
    LapbConfig cfg;
    lapb_config_default(&cfg, 100);
    cfg.t1_ms = 100;
    new_link(&l, 100, &cfg);
    lapb_connect(&l, 0);
    drain(&l);
    clear_sent();
    lapb_tick(&l, 100);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 3F " L100);
}

static void t1_in_connected_polls_then_rej_drives_go_back_n(void)
{
    LapbLink l;
    LapbConfig cfg;
    lapb_config_default(&cfg, 100);
    cfg.t1_ms = 100;
    new_connected_initiator(&l, &cfg);
    CHECK_EQ(send_info(&l, 0, "A0"), XMSG_OK);
    CHECK_EQ(send_info(&l, 0, "A1"), XMSG_OK);
    clear_sent();
    lapb_tick(&l, 100);
    drain(&l);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 11 " L100); /* RR P=1 */
    clear_sent();
    deliver(&l, 110, "09 31 " P102); /* RR F=1 N(R)=1: the first frame got through */
    CHECK_EQ(l.va, 1);
    CHECK(!l.poll_outstanding);
    /* Timer recovery: the final answer is not answered, but what it did not acknowledge (frame 1)
     * is retransmitted at once, with the current N(R). */
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 02 A1");
    CHECK_EQ(l.stats.retransmissions, 1);
    clear_sent();
    deliver(&l, 120, "09 29 " P102); /* REJ N(R)=1: the peer still wants it; resend again */
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 02 A1");
}

/* ------------------------------------------------------------- slots */

static void window_full_refuses_until_acknowledged(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    for (int i = 0; i < 7; i++)
    {
        CHECK_EQ(send_info(&l, 0, "11"), XMSG_OK);
    }
    CHECK_EQ(lapb_tx_free_slots(&l), 0);
    uint8_t *info;
    size_t cap;
    CHECK_EQ(lapb_tx_reserve(&l, &info, &cap), XMSG_EAGAIN);
    CHECK_EQ(s_c.sent_count, 7);
    deliver(&l, 0, "09 61 " P102); /* RR N(R)=3: three acknowledged */
    CHECK_EQ(lapb_tx_free_slots(&l), 3);
    CHECK_EQ(lapb_tx_reserve(&l, &info, &cap), XMSG_OK);
    CHECK_EQ(cap, XMSG_LAPB_N1);
    CHECK_EQ(lapb_tx_reserve(&l, &info, &cap), XMSG_EAGAIN); /* one reservation at a time */
    lapb_tx_cancel(&l);
    CHECK_EQ(lapb_tx_free_slots(&l), 3);
    CHECK_EQ(lapb_tx_commit(&l, 1, 0), XMSG_ESTATE); /* nothing reserved */
}

static void pull_order_is_control_first_then_ns_order(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    uint8_t *info;
    size_t cap;
    CHECK_EQ(lapb_tx_reserve(&l, &info, &cap), XMSG_OK);
    info[0] = 0xE0;
    info[1] = 0xE1;
    CHECK_EQ(lapb_tx_commit(&l, 2, 0), XMSG_OK);
    /* A poll arrives before we pulled: its RR answer goes out before the I-frame. */
    uint8_t rr[] = {0x09, 0x11, 0x00, 0x66};
    lapb_rx_frame(&l, rr, sizeof rr, 0);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 2);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 11 " L100);
    CHECK_BYTES(s_c.sent[1], s_c.sent_len[1], "09 00 E0 E1"); /* even length -> 0x09 */
}

static void retransmission_carries_the_current_nr(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    CHECK_EQ(send_info(&l, 0, "A0"), XMSG_OK);
    deliver(&l, 0, "09 00 21 13"); /* peer I N(S)=0 -> our V(R)=1 */
    clear_sent();
    deliver(&l, 0, "09 29 " P102); /* REJ N(R)=1: acknowledges our frame 0; nothing to resend */
    CHECK_EQ(s_c.sent_count, 0);
    CHECK_EQ(send_info(&l, 0, "A1"), XMSG_OK);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 22 A1"); /* N(S)=1, N(R)=1 */
    clear_sent();
    deliver(&l, 0, "09 22 21 13"); /* peer I N(S)=1 N(R)=1 -> V(R)=2 */
    clear_sent();
    deliver(&l, 0, "09 29 " P102); /* REJ N(R)=1: resend frame 1 with N(R)=2 now */
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "89 42 A1");
}

static void too_long_info_draws_frmr_y_and_bad_nr_draws_frmr_z(void)
{
    LapbLink l;
    LapbConfig cfg;
    lapb_config_default(&cfg, 100);
    cfg.n1 = 4;
    new_connected_initiator(&l, &cfg);
    deliver(&l, 0, "09 00 01 02 03 04 05"); /* 5-byte info > N1 4 */
    CHECK_EQ(lapb_state(&l), LAPB_FRMR_SENT);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 87 00 00 04"); /* FRMR F=0: ctrl, vs/vr, Y */
    CHECK_EQ(s_c.got_count, 0);
    clear_sent();
    /* Any I/S frame in FRMR_SENT re-sends the FRMR with F=1. */
    deliver(&l, 0, "09 01 " P102);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 97 00 00 04");
    CHECK_EQ(l.stats.frmr_sent, 2);

    LapbLink m;
    new_connected_initiator(&m, NULL);
    deliver(&m, 0, "09 41 " P102); /* RR N(R)=2 with nothing outstanding */
    CHECK_EQ(lapb_state(&m), LAPB_FRMR_SENT);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 87 41 00 08"); /* reason Z */
}

static void frmr_received_while_connected_reestablishes(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    deliver(&l, 0, "01 87 00 00 08");
    CHECK_EQ(lapb_state(&l), LAPB_SABM_SENT);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 3F " L100);
    CHECK_EQ(l.stats.frmr_received, 1);
}

static void duplicate_is_reacknowledged_not_rejected(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    deliver(&l, 0, "09 00 21 13");
    CHECK_EQ(s_c.got_count, 1);
    clear_sent();
    deliver(&l, 0, "09 00 21 13"); /* the same frame again: ahead by 7 = duplicate */
    CHECK_EQ(s_c.got_count, 0);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 21 " L100); /* RR N(R)=1 */
    CHECK_EQ(l.stats.duplicates, 1);
    CHECK_EQ(l.stats.rej_sent, 0);
}

static void sabm_burst_is_answered_in_full_and_the_link_stays_consistent(void)
{
    /* LapbAnnounceOrderingTests: six SABMs in one segment. Six UA + six RR go out, and the
     * variables are zero at the end. */
    LapbLink l;
    new_link(&l, 100, NULL);
    lapb_connect(&l, 0);
    drain(&l);
    clear_sent();
    uint8_t sabm[] = {0x01, 0x3F, 0x00, 0x66};
    for (int i = 0; i < 6; i++)
    {
        lapb_rx_frame(&l, sabm, sizeof sabm, 0);
    }
    drain(&l);
    CHECK_EQ(s_c.sent_count, 12);
    CHECK_EQ(l.stats.ctl_queue_full, 0);
    for (int i = 0; i < 12; i += 2)
    {
        CHECK_BYTES(s_c.sent[i], s_c.sent_len[i], "01 73 " L100);
        CHECK_BYTES(s_c.sent[i + 1], s_c.sent_len[i + 1], "09 01 " L100);
    }
    CHECK_EQ(l.vs, 0);
    CHECK_EQ(l.vr, 0);
    deliver(&l, 0, "01 73 " P102);
    CHECK(lapb_is_up(&l));
}

static void frames_outside_connected_draw_dm_or_silence(void)
{
    LapbLink l;
    new_link(&l, 100, NULL);
    deliver(&l, 0, "09 01 " P102); /* RR while DISCONNECTED -> DM */
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "01 0F " L100);
    clear_sent();
    lapb_connect(&l, 0);
    drain(&l);
    clear_sent();
    deliver(&l, 0, "09 01 " P102); /* RR while SABM_SENT -> ignored */
    CHECK_EQ(s_c.sent_count, 0);
    deliver(&l, 0, "01 1F " P102); /* DM answers our SABM: down */
    CHECK_EQ(lapb_state(&l), LAPB_DISCONNECTED);
}

/* RetroCore D100 sometimes sends an I-frame with address 0x07 (0x87 when the info length is
 * odd) instead of 0x09/0x89 - MEASURED 2026-11-09, twice in one TADA run. The C# takes any
 * address; so do we, and count it. Dropping them cost a 1 s T1 resend each time. */
static void unusual_address_is_taken_and_counted(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    int before = s_c.got_count;
    uint8_t even[] = {0x07, 0x00, 0xAA, 0xBB}; /* I, N(S)=0 N(R)=0, two info bytes */
    CHECK_EQ(lapb_rx_frame(&l, even, sizeof even, 0), XMSG_OK);
    uint8_t odd[] = {0x87, 0x02, 0xCC}; /* I, N(S)=1, one info byte */
    CHECK_EQ(lapb_rx_frame(&l, odd, sizeof odd, 0), XMSG_OK);
    CHECK_EQ(s_c.got_count, before + 2);
    CHECK_EQ(l.stats.unusual_address, 2);
    if (s_c.got_count == before + 2)
    {
        CHECK_BYTES(s_c.got[before], s_c.got_len[before], "AA BB");
        CHECK_BYTES(s_c.got[before + 1], s_c.got_len[before + 1], "CC");
    }
    uint8_t runt[] = {0x09};
    CHECK_EQ(lapb_rx_frame(&l, runt, 1, 0), XMSG_EPROTO);
}

static void frame_helpers_decode_every_kind(void)
{
    LapbFrameView v;
    uint8_t i[] = {0x89, 0x62, 0xAA};
    CHECK(lapb_frame_parse(i, sizeof i, &v));
    CHECK_EQ(v.kind, LAPB_FRAME_INFORMATION);
    CHECK_EQ(v.ns, 1);
    CHECK_EQ(v.nr, 3);
    CHECK(!v.pf);
    CHECK_EQ(v.info_len, 1);
    uint8_t s[] = {0x09, 0xB5, 0x00, 0x66};
    CHECK(lapb_frame_parse(s, sizeof s, &v));
    CHECK_EQ(v.kind, LAPB_FRAME_SUPERVISORY);
    CHECK_EQ(v.s_type, LAPB_S_RNR);
    CHECK_EQ(v.nr, 5);
    CHECK(v.pf);
    uint8_t u[] = {0x01, 0x73, 0x00, 0x66};
    CHECK(lapb_frame_parse(u, sizeof u, &v));
    CHECK_EQ(v.kind, LAPB_FRAME_UNNUMBERED);
    CHECK_EQ(v.u_type, LAPB_U_UA);
    CHECK(v.pf);
    CHECK_EQ(lapb_frame_data_address(5), 0x89);
    CHECK_EQ(lapb_frame_data_address(6), 0x09);
    CHECK_EQ(lapb_frame_i_control(2, 5, false), 0xA4);
    CHECK_EQ(lapb_frame_s_control(LAPB_S_REJ, 1, true), 0x39);
    CHECK_EQ(lapb_frame_u_control(LAPB_U_SABM, true), 0x3F);
}

static void busy_sends_rnr_and_acks_as_rnr_until_released(void)
{
    LapbLink l;
    new_connected_initiator(&l, NULL);
    lapb_set_busy(&l, true);
    drain(&l);
    CHECK_EQ(s_c.sent_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 05 " L100); /* RNR N(R)=0 */
    clear_sent();
    deliver(&l, 0, "09 00 21 13"); /* an in-window I-frame is still accepted ... */
    CHECK_EQ(s_c.got_count, 1);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 25 " L100); /* ... and acknowledged with RNR */
    clear_sent();
    deliver(&l, 0, "09 11 " P102); /* a poll is answered RNR F=1 */
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 35 " L100);
    clear_sent();
    lapb_set_busy(&l, true); /* no change: nothing sent */
    drain(&l);
    CHECK_EQ(s_c.sent_count, 0);
    lapb_set_busy(&l, false);
    drain(&l);
    CHECK_BYTES(s_c.sent[0], s_c.sent_len[0], "09 21 " L100); /* RR N(R)=1 releases the peer */
}

static void init_rejects_bad_config(void)
{
    LapbLink l;
    LapbConfig cfg;
    lapb_config_default(&cfg, 1);
    cfg.window = 8;
    CHECK_EQ(lapb_init(&l, &cfg, NULL, NULL, NULL), XMSG_EINVAL);
    cfg.window = 7;
    cfg.n1 = XMSG_LAPB_N1 + 1;
    CHECK_EQ(lapb_init(&l, &cfg, NULL, NULL, NULL), XMSG_EINVAL);
    cfg.n1 = XMSG_LAPB_N1;
    CHECK_EQ(lapb_init(&l, &cfg, NULL, NULL, NULL), XMSG_OK);
    CHECK_EQ(lapb_init(NULL, &cfg, NULL, NULL, NULL), XMSG_EINVAL);
}

int main(void)
{
    printf("[lapb]\n");
    s1_balanced_bring_up();
    s2_rej_go_back_n();
    s3_rnr_hold_and_resume();
    s4_t1_times_n2_reestablishes_when_opted_in();
    s5_gap_single_rej();
    s6_poll_answered_with_final();
    s7_mid_session_sabm_reset();
    peer_sabm_alone_does_not_bring_the_link_up();
    unanswered_sabm_is_resent_after_the_peer_sabm();
    passive_station_comes_up_on_the_peer_sabm_alone();
    up_callback_fires_once_per_change();
    n2_by_default_takes_the_link_down_with_no_sabm();
    t3_sends_one_poll_and_does_not_start_t1();
    t1_in_sabm_sent_resends_the_sabm();
    t1_in_connected_polls_then_rej_drives_go_back_n();
    window_full_refuses_until_acknowledged();
    pull_order_is_control_first_then_ns_order();
    retransmission_carries_the_current_nr();
    too_long_info_draws_frmr_y_and_bad_nr_draws_frmr_z();
    frmr_received_while_connected_reestablishes();
    duplicate_is_reacknowledged_not_rejected();
    sabm_burst_is_answered_in_full_and_the_link_stays_consistent();
    frames_outside_connected_draw_dm_or_silence();
    unusual_address_is_taken_and_counted();
    frame_helpers_decode_every_kind();
    busy_sends_rnr_and_acks_as_rnr_until_released();
    init_rejects_bad_config();
    TEST_DONE();
}
