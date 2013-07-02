#include <glib.h>
#include <termios.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "qga/channel.h"
#include "qapi/qmp/json-streamer.h"

#ifdef CONFIG_SOLARIS
#include <stropts.h>
#endif

#define GA_CHANNEL_BAUDRATE_DEFAULT B38400 /* for isa-serial channels */


static int ga_channel_client_add(GAChannelListener *c, int fd);
static void ga_channel_client_free(GAChannelClient *chc);

static gboolean ga_channel_listen_accept(GIOChannel *channel,
                                         GIOCondition condition, gpointer data)
{
    GAChannelListener *chl = data;
    int ret, client_fd;
    bool accepted = false;
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);

    g_assert(channel != NULL);

    client_fd = qemu_accept(g_io_channel_unix_get_fd(channel),
                            (struct sockaddr *)&addr, &addrlen);
    if (client_fd == -1) {
        g_warning("error converting fd to gsocket: %s", strerror(errno));
        goto out;
    }
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    ret = ga_channel_client_add(chl, client_fd);
    if (ret) {
        g_warning("error setting up connection");
        close(client_fd);
        goto out;
    }
    if (chl->type != GA_CHANNEL_SESSION_CLIENT) {
        accepted = true;
    }

out:
    /* only accept 1 connection at a time for GAChannelType GA_CHANNEL_HOST */
    return !accepted;
}

/* start polling for readable events on listen fd, create==false
 * indicates we should use the existing s->listen_channel
 */
static void ga_channel_listen_add(GAChannelListener *chl, int listen_fd, bool create)
{
    if (create) {
        chl->channel = g_io_channel_unix_new(listen_fd);
    }
    g_io_add_watch(chl->channel, G_IO_IN, ga_channel_listen_accept, chl);
}

static void ga_channel_listen_close(GAChannelListener *chl)
{
    g_assert(chl->method == GA_CHANNEL_UNIX_LISTEN);
    g_assert(chl->channel);
    g_io_channel_shutdown(chl->channel, true, NULL);
    g_io_channel_unref(chl->channel);
    chl->channel = NULL;
}

/* cleanup state for closed connection/session, start accepting new
 * connections if we're in listening mode
 */
static void ga_channel_client_close(GAChannelClient *chc)
{
    GAChannelListener *chl = chc->listener;
    g_assert(chc->channel);
    g_io_channel_shutdown(chc->channel, true, NULL);
    g_io_channel_unref(chc->channel);
    chc->channel = NULL;
    if (chl->type == GA_CHANNEL_SESSION_CLIENT) {
        g_ptr_array_remove(chl->client.sessions, chc);
    } else {
        chl->client.host = NULL;
    }
    if (chl->method == GA_CHANNEL_UNIX_LISTEN && chl->channel && chl->type != GA_CHANNEL_SESSION_CLIENT) {
        ga_channel_listen_add(chl, 0, false);
    }
}

static gboolean ga_channel_client_event(GIOChannel *channel,
                                        GIOCondition condition, gpointer data)
{
    GAChannelClient *chc = data;
    gboolean client_cont;
    g_assert(chc);
    GAChannelCallback ecb = chc->listener->event_cb;

    if (ecb) {
        client_cont = ecb(condition, chc);
        if (!client_cont) {
            ga_channel_client_close(chc);
            ga_channel_client_free(chc);
            return false;
        }
    }
    return true;
}

static int ga_channel_client_add(GAChannelListener *chl, int fd)
{
    GIOChannel *client_channel;
    GAChannelClient *chc;
    GError *err = NULL;
    static guint counter = 1;

    g_assert(chl);
    client_channel = g_io_channel_unix_new(fd);
    g_assert(client_channel);
    g_io_channel_set_encoding(client_channel, "UTF-8", &err);
    if (err != NULL) {
        g_warning("error setting channel encoding to binary");
        g_error_free(err);
        return -1;
    }

    chc = (GAChannelClient *) malloc(sizeof(GAChannelClient));
    chc->channel = client_channel;
    json_message_parser_init(&chc->parser, chl->json_cb);
    chc->id = counter++;
    chc->delimit_response = false;
    chc->listener = chl;

    if (chl->type == GA_CHANNEL_SESSION_CLIENT) {
        g_ptr_array_add(chl->client.sessions,chc);
    } else {
        g_assert(chl->client.host == NULL);
        chl->client.host = chc;
    }

    g_io_add_watch(client_channel, G_IO_IN | G_IO_HUP,
                   ga_channel_client_event, chc);

    return 0;
}

static gboolean ga_channel_open(GAChannelListener *chl, const gchar *path, GAChannelMethod method)
{
    int ret;
    chl->method = method;

    switch (chl->method) {
    case GA_CHANNEL_VIRTIO_SERIAL: {
        int fd = qemu_open(path, O_RDWR | O_NONBLOCK
#ifndef CONFIG_SOLARIS
                           | O_ASYNC
#endif
                           );
        if (fd == -1) {
            g_critical("error opening channel: %s", strerror(errno));
            return false;
        }
#ifdef CONFIG_SOLARIS
        ret = ioctl(fd, I_SETSIG, S_OUTPUT | S_INPUT | S_HIPRI);
        if (ret == -1) {
            g_critical("error setting event mask for channel: %s",
                       strerror(errno));
            close(fd);
            return false;
        }
#endif
        ret = ga_channel_client_add(chl, fd);
        if (ret) {
            g_critical("error adding channel to main loop");
            close(fd);
            return false;
        }
        break;
    }
    case GA_CHANNEL_ISA_SERIAL: {
        struct termios tio;
        int fd = qemu_open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd == -1) {
            g_critical("error opening channel: %s", strerror(errno));
            return false;
        }
        tcgetattr(fd, &tio);
        /* set up serial port for non-canonical, dumb byte streaming */
        tio.c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK | ISTRIP |
                         INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY |
                         IMAXBEL);
        tio.c_oflag = 0;
        tio.c_lflag = 0;
        tio.c_cflag |= GA_CHANNEL_BAUDRATE_DEFAULT;
        /* 1 available byte min or reads will block (we'll set non-blocking
         * elsewhere, else we have to deal with read()=0 instead)
         */
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        /* flush everything waiting for read/xmit, it's garbage at this point */
        tcflush(fd, TCIFLUSH);
        tcsetattr(fd, TCSANOW, &tio);
        ret = ga_channel_client_add(chl, fd);
        if (ret) {
            g_critical("error adding channel to main loop");
            close(fd);
            return false;
        }
        break;
    }
    case GA_CHANNEL_UNIX_LISTEN: {
        Error *local_err = NULL;
        int fd = unix_listen(path, NULL, strlen(path), &local_err);
        if (local_err != NULL) {
            g_critical("%s", error_get_pretty(local_err));
            error_free(local_err);
            return false;
        }
        ga_channel_listen_add(chl, fd, true);
        break;
    }
    default:
        g_critical("error binding/listening to specified socket");
        return false;
    }

    return true;
}

GIOStatus ga_channel_write_all(GAChannelClient *chc, const gchar *buf, gsize size)
{
    GError *err = NULL;
    gsize written = 0;
    GIOStatus status = G_IO_STATUS_NORMAL;

    while (size) {
        status = g_io_channel_write_chars(chc->channel, buf, size,
                                          &written, &err);
        g_debug("sending data, count: %d", (int)size);
        if (err != NULL) {
            g_warning("error writing to channel: %s", err->message);
            return G_IO_STATUS_ERROR;
        }
        if (status != G_IO_STATUS_NORMAL) {
            break;
        }
        size -= written;
    }

    if (status == G_IO_STATUS_NORMAL) {
        status = g_io_channel_flush(chc->channel, &err);
        if (err != NULL) {
            g_warning("error flushing channel: %s", err->message);
            return G_IO_STATUS_ERROR;
        }
    }

    return status;
}

GIOStatus ga_channel_read(GAChannelClient *chc, gchar *buf, gsize size, gsize *count)
{
    GError *er = NULL;
    GIOStatus ret = g_io_channel_read_chars(chc->channel, buf, size, count, &er);
    g_warning("test");
    if (er != NULL) {
        g_warning("error: %s\n",er->message);
    }
    return ret;
}

GAChannelListener *ga_channel_new(GAChannelMethod method, const gchar *path,
                                  GAChannelCallback cb, JSONMessageParserCallback jcb, GAChannelType channel_type)
{
    GAChannelListener *chl = g_malloc0(sizeof(GAChannelListener));
    chl->event_cb = cb;
    chl->json_cb = jcb;
    chl->type = channel_type;

    if (channel_type == GA_CHANNEL_SESSION_CLIENT) {
        chl->client.sessions = g_ptr_array_new();
    }

    if (!ga_channel_open(chl, path, method)) {
        g_critical("error opening channel");
        ga_channel_listener_free(chl);
        return NULL;
    }

    return chl;
}

void ga_channel_client_free(GAChannelClient *chc)
{
    if (chc->channel) {
        ga_channel_client_close(chc);
    }
    g_free(chc);
}


void ga_channel_listener_free(GAChannelListener *chl)
{
    g_assert(chl);
    if (chl->method == GA_CHANNEL_UNIX_LISTEN && chl->channel) {
        ga_channel_listen_close(chl);
    }
    g_free(chl);
}
