/*
 * QEMU Guest Agent channel declarations
 *
 * Copyright IBM Corp. 2012
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QGA_CHANNEL_H
#define QGA_CHANNEL_H

#include <glib.h>
#include "qapi/qmp/json-streamer.h"

typedef struct GAChannelListener GAChannelListener;
typedef struct GAChannelClient GAChannelClient;

typedef enum {
    GA_CHANNEL_VIRTIO_SERIAL,
    GA_CHANNEL_ISA_SERIAL,
    GA_CHANNEL_UNIX_LISTEN,
} GAChannelMethod;

typedef enum {
    GA_CHANNEL_HOST,                //To Qemu Host
    GA_CHANNEL_SESSION_CLIENT,      //To Session Processes
    GA_CHANNEL_SESSION_HOST         //To Qemu Host for session client communication
} GAChannelType;

typedef gboolean (*GAChannelCallback)(GIOCondition condition, GAChannelClient *chc);
typedef void (*JSONMessageParserCallback)(JSONMessageParser *, QList *);

#ifndef _WIN32
struct GAChannelListener {
    GIOChannel *channel;
    GAChannelMethod method;
    GAChannelType type;
    GAChannelCallback event_cb;
    JSONMessageParserCallback json_cb;
    gpointer state;
    union {
        GPtrArray *sessions; //only used when type == SESSION_CLIENT
        GAChannelClient *host; //used otherwise
    } client;
};

struct GAChannelClient {
    GIOChannel *channel;
    JSONMessageParser parser;
    guint id;
    bool delimit_response;
    GAChannelListener *listener;
};
#endif
GAChannelListener *ga_channel_new(GAChannelMethod method, const gchar *path, GAChannelCallback cb,
                                  JSONMessageParserCallback jcb, GAChannelType channel_type);
void ga_channel_listener_free(GAChannelListener *chl);
GIOStatus ga_channel_read(GAChannelClient *chc, gchar *buf, gsize size, gsize *count);
GIOStatus ga_channel_write_all(GAChannelClient *chc, const gchar *buf, gsize size);
#endif
