#ifndef UTIL_H
#define UTIL_H

#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <stdarg.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

typedef struct notify_request
{
    char useless1[45];
    char message[3075];
} notify_request_t;

extern "C"
{
    int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
} 

namespace Util
{
    static void Notify(const char *fmt, ...)
    {
        notify_request_t req;
        va_list args;
    
        bzero(&req, sizeof req);
        va_start(args, fmt);
        vsnprintf(req.message, sizeof req.message, fmt, args);
        va_end(args);
    
        sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    }
}
#endif
