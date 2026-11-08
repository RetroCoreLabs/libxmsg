/*
 * test_log.c - the log macro gates before formatting, levels are per category, lines are cut safely.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/log/xmsg_log.h"

#include "test_util.h"

static char s_last_line[256];
static int s_last_category = -1;
static int s_last_level = -1;
static int s_calls = 0;

static void capture_sink(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line)
{
    (void)ctx;
    s_calls++;
    s_last_category = (int)category;
    s_last_level = (int)level;
    size_t n = strlen(line);
    if (n >= sizeof s_last_line)
    {
        n = sizeof s_last_line - 1;
    }
    memcpy(s_last_line, line, n);
    s_last_line[n] = '\0';
}

static int s_side_effects = 0;
static int side_effect(void)
{
    s_side_effects++;
    return 42;
}

static void no_sink_means_nothing_happens(void)
{
    xmsg_log_set_sink(NULL);
    CHECK(!xmsg_log_is_enabled(XMSG_LOG_LAPB, XMSG_LOG_ERROR));
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_ERROR, "value %d", side_effect());
    CHECK_EQ(s_side_effects, 0);
    CHECK_EQ(s_calls, 0);
}

static void disabled_level_never_evaluates_arguments(void)
{
    XmsgLogPort sink = {NULL, capture_sink};
    xmsg_log_set_sink(&sink);
    xmsg_log_set_level(XMSG_LOG_LAPB, XMSG_LOG_WARN);
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_DEBUG, "value %d", side_effect());
    CHECK_EQ(s_side_effects, 0);
    CHECK_EQ(s_calls, 0);
    XMSG_LOG(XMSG_LOG_LAPB, XMSG_LOG_WARN, "value %d", side_effect());
    CHECK_EQ(s_side_effects, 1);
    CHECK_EQ(s_calls, 1);
    CHECK(strcmp(s_last_line, "value 42") == 0);
    CHECK_EQ(s_last_category, (int)XMSG_LOG_LAPB);
    CHECK_EQ(s_last_level, (int)XMSG_LOG_WARN);
}

static void error_always_passes(void)
{
    xmsg_log_set_all_levels(XMSG_LOG_ERROR);
    CHECK(xmsg_log_is_enabled(XMSG_LOG_TAD, XMSG_LOG_ERROR));
    CHECK(!xmsg_log_is_enabled(XMSG_LOG_TAD, XMSG_LOG_WARN));
    int before = s_calls;
    XMSG_LOG(XMSG_LOG_TAD, XMSG_LOG_ERROR, "boom");
    CHECK_EQ(s_calls, before + 1);
}

static void levels_are_per_category(void)
{
    xmsg_log_set_all_levels(XMSG_LOG_ERROR);
    xmsg_log_set_level(XMSG_LOG_HDLC, XMSG_LOG_TRACE);
    CHECK(xmsg_log_is_enabled(XMSG_LOG_HDLC, XMSG_LOG_TRACE));
    CHECK(!xmsg_log_is_enabled(XMSG_LOG_DG, XMSG_LOG_TRACE));
    CHECK(!xmsg_log_is_enabled(XMSG_LOG_DG, XMSG_LOG_INFO));
}

static void long_lines_are_cut_at_the_limit(void)
{
    char big[400];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    xmsg_log_set_level(XMSG_LOG_NODE, XMSG_LOG_INFO);
    XMSG_LOG(XMSG_LOG_NODE, XMSG_LOG_INFO, "%s", big);
    CHECK_EQ(strlen(s_last_line), XMSG_LOG_LINE_MAX - 1);
}

static void out_of_range_category_is_ignored(void)
{
    /* Must not crash or write outside the table. */
    xmsg_log_set_level((XmsgLogCategory)99, XMSG_LOG_TRACE);
    xmsg_log_set_level((XmsgLogCategory)-1, XMSG_LOG_TRACE);
    CHECK(!xmsg_log_is_enabled((XmsgLogCategory)99, XMSG_LOG_INFO));
}

int main(void)
{
    printf("[log]\n");
    no_sink_means_nothing_happens();
    disabled_level_never_evaluates_arguments();
    error_always_passes();
    levels_are_per_category();
    long_lines_are_cut_at_the_limit();
    out_of_range_category_is_ignored();
    TEST_DONE();
}
