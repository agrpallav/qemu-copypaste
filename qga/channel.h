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

typedef struct GAChannel GAChannel;

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

typedef gboolean (*GAChannelCallback)(GIOCondition condition, gpointer opaque, GAChannel *channel);

#ifndef _WIN32
struct GAChannelListener {
    GIOChannel *listen_channel;
    GAChannelMethod method;
    GAChannelType type;
    GAChannelCallback event_cb;
    gpointer session;

    GPtrArray *channel_session_clients; //only used when type == SESSION_CLIENT
    GASessionClient *client_channel; //used otherwise
};

struct GAChannelClient {
    GIOChannel *client;
    guint id;
    JSONMessageParser parser;
    bool delimit_response;

    GAChannelListener *listener;
};
#endif

GAChannel *ga_channel_new(GAChannelMethod method, const gchar *path,
                          GAChannelCallback cb, gpointer opaque, GAChannelType channel_type);
void ga_channel_free(GAChannel *c);
GIOStatus ga_channel_read(GAChannel *c, gchar *buf, gsize size, gsize *count);
GIOStatus ga_channel_write_all(GAChannel *c, const gchar *buf, gsize size);
#endif
