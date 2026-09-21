/*
 * Windroid IPC client implementation for wineandroid.drv
 *
 * Copyright 2026 Windroid Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "android.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#define WINDROID_IPC_MAGIC 0x574E4452 // "WNDR"

enum WindroidMessageType {
    WINDROID_MSG_HANDSHAKE = 1,
    WINDROID_MSG_SURFACE_STATUS = 2,
    WINDROID_MSG_INPUT_MOUSE = 3,
    WINDROID_MSG_INPUT_KEYBOARD = 4,
    WINDROID_MSG_INPUT_TOUCH = 5,
    WINDROID_MSG_SET_RESOLUTION = 6,
    WINDROID_MSG_CLIPBOARD = 7
};

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t payload_size;
} WindroidIpcHeader;

typedef struct {
    uint32_t is_valid;
    int32_t width;
    int32_t height;
} WindroidSurfaceStatusPayload;

typedef struct {
    float x;
    float y;
    int32_t button;
    uint8_t is_down;
    uint8_t is_relative;
    float wheel_delta_x;
    float wheel_delta_y;
} WindroidMousePayload;

typedef struct {
    int32_t scan_code;
    int32_t key_code;
    uint8_t is_down;
} WindroidKeyboardPayload;

typedef struct {
    int32_t action;
    int32_t pointer_id;
    float x;
    float y;
} WindroidTouchPayload;

#pragma pack(pop)

static pthread_t ipc_thread;
static volatile int ipc_running = 0;
static int current_button_mask = 0;

static void handle_ipc_message(const WindroidIpcHeader *hdr, const void *payload)
{
    switch (hdr->type)
    {
    case WINDROID_MSG_SURFACE_STATUS:
        if (hdr->payload_size >= sizeof(WindroidSurfaceStatusPayload))
        {
            const WindroidSurfaceStatusPayload *p = payload;
            TRACE("WINDROID_MSG_SURFACE_STATUS: valid=%u size=%dx%d\n", p->is_valid, p->width, p->height);
            if (p->width > 0 && p->height > 0)
            {
                desktop_changed(NULL, NULL, p->width, p->height);
            }
        }
        break;

    case WINDROID_MSG_INPUT_MOUSE:
        if (hdr->payload_size >= sizeof(WindroidMousePayload))
        {
            const WindroidMousePayload *p = payload;
            int action = AMOTION_EVENT_ACTION_MOVE;
            int btn_bit = 0;

            if (p->button == 1) btn_bit = AMOTION_EVENT_BUTTON_PRIMARY;
            else if (p->button == 2) btn_bit = AMOTION_EVENT_BUTTON_SECONDARY;
            else if (p->button == 3) btn_bit = AMOTION_EVENT_BUTTON_TERTIARY;

            if (p->wheel_delta_y != 0.0f)
            {
                action = AMOTION_EVENT_ACTION_SCROLL;
                motion_event(NULL, NULL, 0, action, (jint)p->x, (jint)p->y, current_button_mask, (jint)p->wheel_delta_y);
            }
            else if (p->button != 0)
            {
                if (p->is_down)
                {
                    action = AMOTION_EVENT_ACTION_BUTTON_PRESS;
                    current_button_mask |= btn_bit;
                }
                else
                {
                    action = AMOTION_EVENT_ACTION_BUTTON_RELEASE;
                    current_button_mask &= ~btn_bit;
                }
                motion_event(NULL, NULL, 0, action, (jint)p->x, (jint)p->y, current_button_mask, 0);
            }
            else
            {
                action = AMOTION_EVENT_ACTION_MOVE;
                motion_event(NULL, NULL, 0, action, (jint)p->x, (jint)p->y, current_button_mask, 0);
            }
        }
        break;

    case WINDROID_MSG_INPUT_KEYBOARD:
        if (hdr->payload_size >= sizeof(WindroidKeyboardPayload))
        {
            const WindroidKeyboardPayload *p = payload;
            int action = p->is_down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
            keyboard_event(NULL, NULL, 0, action, p->key_code, 0);
        }
        break;

    case WINDROID_MSG_INPUT_TOUCH:
        if (hdr->payload_size >= sizeof(WindroidTouchPayload))
        {
            const WindroidTouchPayload *p = payload;
            motion_event(NULL, NULL, 0, p->action, (jint)p->x, (jint)p->y, 0, 0);
        }
        break;

    default:
        TRACE("Unknown Windroid IPC message type=%u\n", hdr->type);
        break;
    }
}

static void *windroid_ipc_client_loop(void *arg)
{
    const char *socket_path = (const char *)arg;
    TRACE("Windroid IPC client thread started for %s\n", socket_path);

    while (ipc_running)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            usleep(200000);
            continue;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        lstrcpynA(addr.sun_path, socket_path, sizeof(addr.sun_path));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            close(fd);
            usleep(300000);
            continue;
        }

        TRACE("Connected to Windroid IPC server: %s\n", socket_path);

        while (ipc_running)
        {
            WindroidIpcHeader hdr;
            ssize_t n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
            if (n <= 0)
            {
                WARN("IPC server connection lost: %s\n", strerror(errno));
                break;
            }

            if (hdr.magic != WINDROID_IPC_MAGIC)
            {
                ERR("Invalid IPC header magic: 0x%08x\n", hdr.magic);
                break;
            }

            if (hdr.payload_size > 0 && hdr.payload_size <= 65536)
            {
                void *payload = malloc(hdr.payload_size);
                if (!payload) break;

                ssize_t pn = recv(fd, payload, hdr.payload_size, MSG_WAITALL);
                if (pn == (ssize_t)hdr.payload_size)
                {
                    handle_ipc_message(&hdr, payload);
                }
                free(payload);
                if (pn <= 0) break;
            }
            else if (hdr.payload_size == 0)
            {
                handle_ipc_message(&hdr, NULL);
            }
        }

        close(fd);
        usleep(200000);
    }

    free((void *)socket_path);
    TRACE("Windroid IPC client thread ended\n");
    return NULL;
}

void start_windroid_ipc_client(void)
{
    const char *sock_env = getenv("WINDROID_IPC_SOCKET");
    if (!sock_env || !*sock_env)
    {
        TRACE("WINDROID_IPC_SOCKET not set, skipping native IPC client\n");
        return;
    }

    if (ipc_running) return;

    ipc_running = 1;
    char *path_copy = strdup(sock_env);
    if (pthread_create(&ipc_thread, NULL, windroid_ipc_client_loop, path_copy) != 0)
    {
        ERR("Failed to create Windroid IPC thread\n");
        ipc_running = 0;
        free(path_copy);
    }
}
