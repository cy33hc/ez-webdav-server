#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

/*
 * Show a short message to the user.
 *
 * On console builds this posts a system notification via
 * sceKernelSendNotificationRequest; on native WSL/Linux builds (PLATFORM_WSL)
 * it prints to stdout instead. Format string works like printf.
 */
void Util_Notify(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif
