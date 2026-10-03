#undef main

#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>

#include "server/webdav_server.h"
#include "sceSystemService.h"
#include "util.h"
#include "dbglogger.h"

static bool in_rest_mode = false;
static bool stop_monitoring = false;

static void *SystemEventThread(void *argp)
{
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
                    in_rest_mode = true;
                    WebDAVServer::SetRestMode(in_rest_mode);
                    Util::Notify("ezRemote: Pausing downloads for rest mode");
                }
                break;

            case SCE_SYSTEM_SERVICE_EVENT_ON_RESUME:
                if (in_rest_mode)
                {
                    in_rest_mode = false;
                    WebDAVServer::SetRestMode(in_rest_mode);
                    Util::Notify("ezRemote: Resuming downloads");
                }
                break;
            }
        }

        // Poll every 2 seconds
        sleep(2);
    }

    return nullptr;
}

int main(int argc, char *argv[])
{
    dbglogger_init();
    dbglogger_log("If you see this you've set up dbglogger correctly.");

    if (WebDAVServer::IsStarted())
    {
        Util::Notify("ezRemote Server already started");
        return 0;
    }

    // Start system event monitoring thread
    pthread_t sys_event_thread;
    pthread_create(&sys_event_thread, NULL, SystemEventThread, NULL);

    WebDAVServer::Start();
    stop_monitoring = true;
    Util::Notify("ezRemote Server stopped.");

    return 0;
}
