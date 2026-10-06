#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

/*
 * Debug logging toggle.
 *
 * Define DEBUG at compile time (e.g. cmake -DDEBUG=ON) to route DBG_LOG()
 * through dbglogger_log(). In release builds DBG_LOG() compiles to a no-op and
 * its arguments are NOT evaluated, so logging costs nothing when disabled.
 *
 * This macro lives here (an always-included header) rather than in dbglogger.h
 * so that files can include dbglogger.h only in DEBUG builds and still use
 * DBG_LOG() unconditionally. In DEBUG builds the including file must also have
 * dbglogger.h in scope (that's where dbglogger_log is declared).
 *
 * Usage: DBG_LOG("value = %d", x);
 */
#ifdef DEBUG
#define DBG_LOG(...) dbglogger_log(__VA_ARGS__)
#else
#define DBG_LOG(...) ((void)0)
#endif

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
