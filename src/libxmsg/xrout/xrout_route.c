/*
 * xrout_route.c - XSGSY routing table and messages (see xrout_route.h).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ronny Hansen
 */
#include "libxmsg/xrout/xrout_route.h"

#include <string.h>

#include "libxmsg/xmsg_endian.h"
#include "libxmsg/xrout/xrout_letter.h"

const char *xrout_route_how_name(uint16_t how)
{
    switch (how)
    {
    case XROUT_REACH_UNAVAILABLE:
        return "unavailable";
    case XROUT_REACH_NEIGHBOUR:
        return "neighbour";
    case XROUT_REACH_VIA:
        return "via system";
    case XROUT_REACH_NETSERVER:
        return "via network server";
    case XROUT_REACH_LOCAL:
        return "local";
    default:
        return "unknown";
    }
}

void xrout_route_init(XroutRouteTable *t)
{
    if (t != NULL)
    {
        memset(t, 0, sizeof *t);
    }
}

int xrout_route_put(XroutRouteTable *t, const XroutRouteEntry *e)
{
    uint8_t at;
    if (t == NULL || e == NULL || e->system == 0)
    {
        return XMSG_EINVAL;
    }
    for (at = 0; at < t->count; at++)
    {
        if (t->e[at].system == e->system)
        {
            t->e[at] = *e;
            return XMSG_OK;
        }
        if (t->e[at].system > e->system)
        {
            break;
        }
    }
    if (t->count >= XROUT_ROUTE_MAX)
    {
        return XMSG_ENOSPC;
    }
    /* Keep ascending order so the lookup is the C# ascending scan (InMemoryRoutingTable.cs). */
    memmove(&t->e[at + 1], &t->e[at], (size_t)(t->count - at) * sizeof t->e[0]);
    t->e[at] = *e;
    t->count++;
    return XMSG_OK;
}

bool xrout_route_has(const XroutRouteTable *t, uint16_t system)
{
    if (t == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < t->count; i++)
    {
        if (t->e[i].system == system)
        {
            return true;
        }
    }
    return false;
}

bool xrout_route_lookup(const XroutRouteTable *t, uint16_t asked, XroutRouteEntry *out)
{
    memset(out, 0, sizeof *out);
    if (t == NULL)
    {
        return false;
    }
    for (uint8_t i = 0; i < t->count; i++)
    {
        if (t->e[i].system >= asked)
        {
            *out = t->e[i];
            return true;
        }
    }
    return false;
}

static void put_int_param(uint8_t *out, uint8_t number, uint16_t value)
{
    out[0] = number;
    out[1] = 0x02; /* every captured XSGSY value is a 2-byte integer (C# XsgsyWire.cs) */
    xmsg_be16_write(out + 2, value);
}

void xrout_route_request_write(uint8_t *out, uint16_t asked)
{
    xrout_head_write(out, XROUT_ROUTE_SERIAL, XROUT_ROUTE_SERVICE, 4);
    put_int_param(out + 4, 1, asked);
}

int xrout_route_request_read(const uint8_t *body, size_t len, uint8_t *serial, uint16_t *asked)
{
    XroutMessage m;
    int rc;
    int32_t v;
    if (body == NULL || asked == NULL)
    {
        return XMSG_EINVAL;
    }
    rc = xrout_parse(body, len, &m);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    if (m.service != XROUT_ROUTE_SERVICE)
    {
        return XMSG_EPROTO;
    }
    v = xrout_param_int(&m, 1);
    if (v < 0)
    {
        return XMSG_EPROTO; /* C# ParseRequestQuery throws here too: there is nothing to answer */
    }
    if (serial != NULL)
    {
        *serial = m.serial;
    }
    *asked = (uint16_t)v;
    return XMSG_OK;
}

void xrout_route_reply_write(uint8_t *out, uint8_t serial, const XroutRouteEntry *e)
{
    /* Status 0x00 = XRSOK: "values in byte 1 of message, bit 6 set => service request", the
     * return message has bit 6 clear (xmsg-pl-values-l.incl, XMSG-PROTOCOL.md section 9.1). A
     * miss is still XRSOK, with system 0 (capture li-route-d103-tree, asked 1111). */
    xrout_head_write(out, serial, 0x00, 16);
    put_int_param(out + 4, 1, e->system);
    put_int_param(out + 8, 2, e->how);
    put_int_param(out + 12, 3, e->detail);
    put_int_param(out + 16, 4, e->distance);
}

int xrout_route_reply_read(const uint8_t *body, size_t len, uint8_t *status, XroutRouteEntry *e)
{
    XroutMessage m;
    int rc;
    int32_t v;
    if (body == NULL || e == NULL)
    {
        return XMSG_EINVAL;
    }
    memset(e, 0, sizeof *e);
    rc = xrout_parse(body, len, &m);
    if (rc != XMSG_OK)
    {
        return rc;
    }
    if (status != NULL)
    {
        *status = m.service;
    }
    if (m.service != 0x00)
    {
        return XMSG_EPROTO;
    }
    v = xrout_param_int(&m, 1);
    e->system = (v < 0) ? 0u : (uint16_t)v;
    v = xrout_param_int(&m, 2);
    e->how = (v < 0) ? 0u : (uint16_t)v;
    v = xrout_param_int(&m, 3);
    e->detail = (v < 0) ? 0u : (uint16_t)v;
    v = xrout_param_int(&m, 4);
    e->distance = (v < 0) ? 0u : (uint16_t)v;
    return XMSG_OK;
}
