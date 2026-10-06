/*
 * Entry point for the C / libmicrohttpd WebDAV server.
 *
 * C port of the former main.cpp. On console builds it also runs a background
 * thread that watches for sleep/resume events so transfers can be paused and
 * resumed around rest mode; that path is compiled out for native WSL builds.
 */

/* Some console SDK headers redefine `main`; neutralize that like main.cpp did. */
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "server/webdav_server.h"
#ifdef DEBUG
#include "dbglogger.h"
#endif
#include "util.h"

#ifndef PLATFORM_WSL
#include "sceSystemService.h"
#endif

/* ------------------------------------------------------------------ */
/* Console sleep/resume monitoring                                   */
/* ------------------------------------------------------------------ */

#ifndef PLATFORM_WSL
static int in_rest_mode = 0;
static volatile int stop_monitoring = 0;

/* Console-only: watch for sleep/resume to pause and resume transfers. */
static void *SystemEventThread(void *argp)
{
    (void)argp;
    SceSystemServiceEvent event;

    while (!stop_monitoring)
    {
        int ret = sceSystemServiceReceiveEvent(&event);
        if (ret == 0)
        {
            switch (event.eventType)
            {
            case SCE_SYSTEM_SERVICE_EVENT_BEFORE_SLEEP:
                if (!in_rest_mode)
                {
                    in_rest_mode = 1;
                    WebDAVServer_SetRestMode(in_rest_mode);
                    Util_Notify("Pausing WebDAV Server");
                }
                break;

            case SCE_SYSTEM_SERVICE_EVENT_ON_RESUME:
                if (in_rest_mode)
                {
                    in_rest_mode = 0;
                    WebDAVServer_SetRestMode(in_rest_mode);
                    Util_Notify("Resuming WebDAV Server");
                }
                break;

            default:
                break;
            }
        }

        /* Poll every 2 seconds */
        sleep(2);
    }

    return NULL;
}
#endif

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Only initialize the debug logger in DEBUG builds; in release builds no
     * logging is emitted (DBG_LOG compiles to a no-op), so there's no reason to
     * open the logger socket/file. */
#ifdef DEBUG
    dbglogger_init();
    DBG_LOG("If you see this you've set up dbglogger correctly.");
#endif

    if (WebDAVServer_IsStarted())
    {
        Util_Notify("WebDAV Server already started");
        return 0;
    }

#ifndef PLATFORM_WSL
    /* Start system event monitoring thread (console sleep/resume handling). */
    pthread_t sys_event_thread;
    pthread_create(&sys_event_thread, NULL, SystemEventThread, NULL);
#endif

    WebDAVServer_Start();

#ifndef PLATFORM_WSL
    stop_monitoring = 1;
#endif

    Util_Notify("WebDAV Server stopped.");

    return 0;
}
