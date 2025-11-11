// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include "udp_receiver.h"
#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>

#include <gst/gst.h>

#define UDP_MAX_PACKET (4 * 1024)

struct UdpReceiver {
    int udp_port;
    int vid_pt;
    GstAppSrc *video_appsrc;

    int sockfd;
    GThread *thread;
    GMutex lock;
    gboolean running;
    gboolean stop_requested;
    GstBufferPool *pool;
    gboolean pool_active;
#ifdef SO_RXQ_OVFL
    guint32 last_overflow_count;
    guint64 total_overflow_events;
#endif
    guint64 total_packets;
    guint64 truncated_packets;
};

static gboolean ensure_buffer_pool(UdpReceiver *ur) {
    if (ur == NULL) {
        return FALSE;
    }

    if (ur->pool != NULL) {
        if (!ur->pool_active) {
            if (!gst_buffer_pool_set_active(ur->pool, TRUE)) {
                LOGW("UDP receiver: failed to activate buffer pool");
                return FALSE;
            }
            ur->pool_active = TRUE;
        }
        return TRUE;
    }

    GstBufferPool *pool = gst_buffer_pool_new();
    if (pool == NULL) {
        LOGW("UDP receiver: failed to create buffer pool");
        return FALSE;
    }

    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, NULL, UDP_MAX_PACKET, 8, 32);
    if (!gst_buffer_pool_set_config(pool, config)) {
        LOGW("UDP receiver: failed to configure buffer pool");
        gst_object_unref(pool);
        return FALSE;
    }
    if (!gst_buffer_pool_set_active(pool, TRUE)) {
        LOGW("UDP receiver: failed to activate buffer pool");
        gst_object_unref(pool);
        return FALSE;
    }

    ur->pool = pool;
    ur->pool_active = TRUE;
    return TRUE;
}

static gboolean payload_type_matches(const guint8 *data, gssize len, int expected_pt) {
    if (expected_pt < 0) {
        return TRUE;
    }
    if (len < 2) {
        return FALSE;
    }
    guint8 payload_type = data[1] & 0x7Fu;
    return payload_type == (guint8)expected_pt;
}

static gpointer receiver_thread(gpointer data) {
    UdpReceiver *ur = (UdpReceiver *)data;
    guint8 *buffer = g_malloc(UDP_MAX_PACKET);
    if (buffer == NULL) {
        LOGE("UDP receiver: failed to allocate packet buffer");
        return NULL;
    }

    while (TRUE) {
        g_mutex_lock(&ur->lock);
        gboolean stop = ur->stop_requested;
        g_mutex_unlock(&ur->lock);
        if (stop) {
            break;
        }

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        struct iovec iov;
        iov.iov_base = buffer;
        iov.iov_len = UDP_MAX_PACKET;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
#ifdef SO_RXQ_OVFL
        guint8 control[CMSG_SPACE(sizeof(guint32))];
        memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
#endif

        ssize_t n = recvmsg(ur->sockfd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            LOGW("UDP receiver: recv failed: %s", g_strerror(errno));
            continue;
        }
        if (n == 0) {
            continue;
        }

#ifdef SO_RXQ_OVFL
        guint32 overflow_value = 0;
        gboolean overflow_present = FALSE;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(guint32))) {
                overflow_value = *((guint32 *)CMSG_DATA(cmsg));
                overflow_present = TRUE;
                break;
            }
        }
        if (overflow_present) {
            guint32 previous = ur->last_overflow_count;
            guint32 delta;
            if (overflow_value >= previous) {
                delta = overflow_value - previous;
            } else {
                delta = (G_MAXUINT32 - previous) + 1 + overflow_value;
            }
            ur->last_overflow_count = overflow_value;
            if (delta > 0) {
                ur->total_overflow_events += delta;
                LOGW("UDP receiver: kernel dropped %u datagrams before delivery (total=%" G_GUINT64_FORMAT ")", delta,
                     ur->total_overflow_events);
            }
        }
#endif

        if ((msg.msg_flags & MSG_TRUNC) != 0) {
            ur->truncated_packets++;
            if (ur->truncated_packets <= 5 || (ur->truncated_packets % 1000) == 0) {
                LOGW("UDP receiver: truncated RTP datagram detected (size=%zd, total truncations=%" G_GUINT64_FORMAT ")", n,
                     ur->truncated_packets);
            }
            continue;
        }

        if (!payload_type_matches(buffer, n, ur->vid_pt)) {
            continue;
        }

        ur->total_packets++;

        GstBuffer *gst_buf = NULL;
        if (ensure_buffer_pool(ur)) {
            GstFlowReturn acquire_ret = gst_buffer_pool_acquire_buffer(ur->pool, &gst_buf, NULL);
            if (acquire_ret != GST_FLOW_OK) {
                LOGW("UDP receiver: buffer pool acquisition failed: %s", gst_flow_get_name(acquire_ret));
                gst_buf = NULL;
            }
        }
        if (gst_buf == NULL) {
            gst_buf = gst_buffer_new_allocate(NULL, (gsize)n, NULL);
            if (gst_buf == NULL) {
                LOGW("UDP receiver: dropping packet (allocation failed)");
                continue;
            }
        }
        GstMapInfo map;
        if (gst_buffer_map(gst_buf, &map, GST_MAP_WRITE)) {
            gsize copy_size = (gsize)n;
            if (map.size < copy_size) {
                LOGW("UDP receiver: dropping packet (buffer too small: %" G_GSIZE_FORMAT " < %" G_GSIZE_FORMAT ")", map.size, copy_size);
                gst_buffer_unmap(gst_buf, &map);
                gst_buffer_unref(gst_buf);
                continue;
            }
            memcpy(map.data, buffer, (size_t)copy_size);
            gst_buffer_unmap(gst_buf, &map);
            gst_buffer_set_size(gst_buf, copy_size);
        } else {
            LOGW("UDP receiver: failed to map GstBuffer");
            gst_buffer_unref(gst_buf);
            continue;
        }

        GstFlowReturn flow = gst_app_src_push_buffer(ur->video_appsrc, gst_buf);
        if (flow != GST_FLOW_OK) {
            LOGW("UDP receiver: gst_app_src_push_buffer returned %s", gst_flow_get_name(flow));
        }
    }

    g_free(buffer);
    return NULL;
}

UdpReceiver *udp_receiver_create(int udp_port, int vid_pt, GstAppSrc *video_appsrc) {
    if (video_appsrc == NULL) {
        return NULL;
    }

    UdpReceiver *ur = g_new0(UdpReceiver, 1);
    if (ur == NULL) {
        return NULL;
    }

    ur->udp_port = udp_port;
    ur->vid_pt = vid_pt;
    ur->video_appsrc = GST_APP_SRC(gst_object_ref(video_appsrc));
    ur->sockfd = -1;
    g_mutex_init(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    ur->thread = NULL;
    ur->pool = NULL;
    ur->pool_active = FALSE;
#ifdef SO_RXQ_OVFL
    ur->last_overflow_count = 0;
    ur->total_overflow_events = 0;
#endif
    ur->total_packets = 0;
    ur->truncated_packets = 0;

    return ur;
}

int udp_receiver_start(UdpReceiver *ur) {
    if (ur == NULL) {
        return -1;
    }

    g_mutex_lock(&ur->lock);
    if (ur->running) {
        g_mutex_unlock(&ur->lock);
        return 0;
    }
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("UDP receiver: socket failed: %s", g_strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_REUSEADDR) failed: %s", g_strerror(errno));
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_RCVTIMEO) failed: %s", g_strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ur->udp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("UDP receiver: bind(%d) failed: %s", ur->udp_port, g_strerror(errno));
        close(fd);
        return -1;
    }

    g_mutex_lock(&ur->lock);
    ur->sockfd = fd;
    ur->running = TRUE;
    g_mutex_unlock(&ur->lock);

    ur->thread = g_thread_new("udp-receiver", receiver_thread, ur);
    if (ur->thread == NULL) {
        LOGE("UDP receiver: failed to create thread");
        g_mutex_lock(&ur->lock);
        ur->running = FALSE;
        g_mutex_unlock(&ur->lock);
        close(fd);
        ur->sockfd = -1;
        return -1;
    }
    return 0;
}

void udp_receiver_stop(UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }

    g_mutex_lock(&ur->lock);
    if (!ur->running) {
        g_mutex_unlock(&ur->lock);
        return;
    }
    ur->stop_requested = TRUE;
    g_mutex_unlock(&ur->lock);

    if (ur->sockfd >= 0) {
        shutdown(ur->sockfd, SHUT_RDWR);
    }

    if (ur->thread != NULL) {
        g_thread_join(ur->thread);
        ur->thread = NULL;
    }

    if (ur->sockfd >= 0) {
        close(ur->sockfd);
        ur->sockfd = -1;
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
}

void udp_receiver_destroy(UdpReceiver *ur) {
    if (ur == NULL) {
        return;
    }
    udp_receiver_stop(ur);
    if (ur->video_appsrc != NULL) {
        gst_object_unref(ur->video_appsrc);
        ur->video_appsrc = NULL;
    }
    if (ur->pool != NULL) {
        if (ur->pool_active) {
            gst_buffer_pool_set_active(ur->pool, FALSE);
            ur->pool_active = FALSE;
        }
        gst_object_unref(ur->pool);
        ur->pool = NULL;
    }
    g_mutex_clear(&ur->lock);
    g_free(ur);
}
