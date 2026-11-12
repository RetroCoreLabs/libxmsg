/*
 * test_nd100x_fa.c - read one file from a running SINTRAN machine with the libcosmos FA client.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 *
 * Needs a machine on the line with XMSG and the COSMOS file server (*FA-SERVER) running, and a
 * link defined there for our node. Environment (the line, node and count file as in
 * test_nd100x_tad.c, see nd100x_line.h):
 *   ND100X_HDLC_HOST   host:port of the line (unset: SKIPPED, exit code 2)
 *   ND100X_OUR_NODE    our node number (default 19999)
 *   ND100X_PEER_NODE   the machine's node number (default 100)
 *   ND100X_PEER_SYSTEM its system number (default: the node number)
 *   ND100X_USER        the user whose directory is read (default SYSTEM)
 *   ND100X_PASSWORD    its password (default empty)
 *   ND100X_FA_FILE     the file to read, "NAME:TYPE" or "(USER)NAME:TYPE"
 *   ND100X_FA_LIST     instead of reading: list the files matching this name ("" = all; a
 *                      name = FILE-STATISTICS of that file). One of the two must be set, or the
 *                      test is SKIPPED.
 *   ND100X_FA_OUT      write what was read to this file (optional), to compare with ndtool
 *   ND100X_FLAGS1_FILE the shared count file (default nd100x_tad.flags1)
 *
 * A listing prints one line per file and checks it ends with result 0 (the end of the
 * directory is the normal end).
 * What is checked: the read reaches DONE with result 0 within two minutes, the open reply gave a
 * length, exactly that many bytes arrived in order, and the server answered our session
 * finished with its close (that is what frees its seat).
 */
#include "libcosmos/fa/fa_client.h"
#include "libcosmos/fa/fa_replies.h"
#include "libxmsg/log/xmsg_log.h"
#include "libxmsg/xmsg/xmsg_node.h"

#include "integration/nd100x_line.h"
#include "pair/test_pair.h"
#include "test_util.h"

static void log_sink(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line)
{
    (void)ctx;
    printf("  [%d/%d] %s\n", (int)category, (int)level, line);
}

typedef struct Got
{
    FILE *out;
    uint32_t length;
    uint32_t bytes;
    bool out_of_order;
    bool done;
    int result;
    uint16_t sintran_error;
} Got;

static void on_opened(void *ctx, uint32_t length)
{
    ((Got *)ctx)->length = length;
    printf("  the file is %u bytes\n", (unsigned)length);
}

static void on_data(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n)
{
    Got *g = (Got *)ctx;
    if (offset != g->bytes)
    {
        g->out_of_order = true;
        return;
    }
    if (g->out != NULL && fwrite(bytes, 1, n, g->out) != n)
    {
        printf("  writing the output file failed\n");
    }
    g->bytes += (uint32_t)n;
}

static unsigned s_entries;

static void on_directory(void *ctx, const char *directory, const char *user)
{
    (void)ctx;
    printf("  directory %s, user %s\n", directory, user);
}

static void on_entry(void *ctx, const FaFileEntry *e)
{
    FaNdDate d;
    (void)ctx;
    s_entries++;
    printf("  %-16s %-4s %6u pages %8u bytes  opened %u times", e->name, e->type,
           (unsigned)e->pages, (unsigned)e->bytes, (unsigned)e->open_total);
    if (fa_nd_date_split(e->created, &d))
    {
        printf("  created %04u-%02u-%02u %02u:%02u", (unsigned)d.year, (unsigned)d.month,
               (unsigned)d.day, (unsigned)d.hour, (unsigned)d.minute);
    }
    printf("\n");
}

static void on_done(void *ctx, int result, uint16_t sintran_error)
{
    Got *g = (Got *)ctx;
    g->done = true;
    g->result = result;
    g->sintran_error = sintran_error;
    printf("  done: result %d, SINTRAN error %u %s\n", result, (unsigned)sintran_error,
           fa_error_name(sintran_error));
}

int main(void)
{
    const char *spec = getenv("ND100X_HDLC_HOST");
    const char *file = getenv("ND100X_FA_FILE");
    const char *out_path = getenv("ND100X_FA_OUT");
    const char *list = getenv("ND100X_FA_LIST");
    char host[128];
    uint16_t port;
    if (spec == NULL || !parse_host(spec, host, sizeof host, &port) ||
        (file == NULL && list == NULL))
    {
        printf("SKIPPED: set ND100X_HDLC_HOST=host:port and ND100X_FA_FILE or ND100X_FA_LIST\n");
        return 2;
    }
    uint16_t own = env_u16("ND100X_OUR_NODE", 19999);
    uint16_t peer = env_u16("ND100X_PEER_NODE", 100);
    uint16_t peer_system = env_u16("ND100X_PEER_SYSTEM", peer);
    const char *user = env_str("ND100X_USER", "SYSTEM");
    const char *password = env_str("ND100X_PASSWORD", "");

    XmsgLogPort sink = {NULL, log_sink};
    xmsg_log_set_sink(&sink);
    xmsg_log_set_all_levels(XMSG_LOG_INFO);
    printf("[nd100x_fa] %s:%u as node %u: %s \"%s\" on node %u (system %u) as %s\n", host,
           (unsigned)port, (unsigned)own, (list != NULL) ? "list" : "read",
           (list != NULL) ? list : file, (unsigned)peer, (unsigned)peer_system, user);

    CHECK_EQ(test_tcp_startup(), 0);
    s_sock = test_tcp_connect_host(host, port);
    if (s_sock == -1)
    {
        printf("cannot connect to %s:%u\n", host, (unsigned)port);
        CHECK(false);
        TEST_DONE();
    }

    static XmsgNode node;
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    xmsg_node_config_default(&cfg, own);
    memset(&ports, 0, sizeof ports);
    ports.transport.write = line_write;
    static FileStore store;
    store.path = env_str("ND100X_FLAGS1_FILE", "nd100x_tad.flags1");
    store.own = own;
    store_read(&store);
    ports.flags1.ctx = &store;
    ports.flags1.load = store_load;
    ports.flags1.save = store_save;
    ports.flags1.load_port_seed = store_load_seed;
    ports.flags1.save_port_seed = store_save_seed;
    ports.log = sink;
    memset(&cb, 0, sizeof cb);
    CHECK_EQ(xmsg_node_init(&node, &cfg, &ports, &cb), XMSG_OK);

    static Got got;
    if (out_path != NULL)
    {
        got.out = fopen(out_path, "wb");
        CHECK(got.out != NULL);
    }
    static FaClient client;
    FaClientCallbacks fcb = {&got, on_opened, on_data, on_done, on_directory, on_entry, NULL};
    CHECK_EQ(fa_client_init(&client, NULL, &fcb), XMSG_OK);

    uint32_t start = test_now_ms();
    uint32_t now = start;
    bool started = false;
    uint8_t chunk[2048];
    xmsg_node_link_start(&node, now);

    while (now - start < 120000u && !got.done)
    {
        now = test_now_ms();
        int r = test_tcp_read(s_sock, chunk, sizeof chunk);
        if (r < 0)
        {
            printf("  the line closed\n");
            break;
        }
        if (r > 0)
        {
            xmsg_node_feed(&node, chunk, (size_t)r, now);
        }
        xmsg_node_tick(&node, now);
        if (!started && xmsg_node_is_ready(&node))
        {
            if (list != NULL)
            {
                CHECK_EQ(fa_client_list(&client, &node, peer, peer_system, user, user, password,
                                        list, now),
                         XMSG_OK);
            }
            else
            {
                CHECK_EQ(fa_client_read(&client, &node, peer, peer_system, user, user, password,
                                        file, now),
                         XMSG_OK);
            }
            started = true;
        }
        test_tcp_wait_readable(s_sock, 10);
    }
    /* Let our last acknowledgements go out. */
    for (uint32_t until = now + 500u; now < until; now = test_now_ms())
    {
        int r = test_tcp_read(s_sock, chunk, sizeof chunk);
        if (r > 0)
        {
            xmsg_node_feed(&node, chunk, (size_t)r, now);
        }
        xmsg_node_tick(&node, now);
        test_tcp_wait_readable(s_sock, 10);
    }

    if (got.out != NULL)
    {
        CHECK_EQ(fclose(got.out), 0);
    }
    CHECK(started);
    CHECK(got.done);
    CHECK_EQ(got.result, 0);
    CHECK_EQ(fa_client_state(&client), FA_CLIENT_DONE);
    CHECK(!got.out_of_order);
    CHECK_EQ(got.bytes, got.length);
    if (list != NULL)
    {
        printf("  %u files\n", s_entries);
    }
    CHECK_EQ(node.stats.bad_headers, 0);
    printf("  %u bytes in %u blocks, %u frames out, %u messages in, %u XENSE resends, %u ms\n",
           (unsigned)got.bytes, (unsigned)client.blocks, (unsigned)client.stats.frames_out,
           (unsigned)client.stats.messages_in, (unsigned)client.stats.xense_resends,
           (unsigned)(now - start));
    xmsg_node_link_stop(&node, now);
    test_tcp_close(s_sock);
    TEST_DONE();
}
