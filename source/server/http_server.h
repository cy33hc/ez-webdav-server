#ifndef EZ_HTTP_SERVER_H
#define EZ_HTTP_SERVER_H

#include "http/httplib.h"

using namespace httplib;

namespace HttpServer
{
    void ServerThread();
    void Start();
    void Stop();
    bool IsStarted();
    void SetRestMode(bool toggle);
}

#endif
