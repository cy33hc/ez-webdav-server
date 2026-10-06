/*
 * Shared utility helpers for the WebDAV server.
 *
 * Util_Notify is the single implementation of the user-facing notifier that
 * both main.c and webdav_server.c use. It has two platform variants: console
 * builds post a system notification; native WSL/Linux builds log to stdout.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "util.h"

#ifdef PLATFORM_WSL

void Util_Notify(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

#else

typedef struct notify_request
{
    char useless1[45];
    char message[3075];
} notify_request_t;

extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

void Util_Notify(const char *fmt, ...)
{
    notify_request_t req;
    va_list args;
    memset(&req, 0, sizeof req);
    va_start(args, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

#endif
