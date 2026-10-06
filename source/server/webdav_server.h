#ifndef EZ_HTTP_SERVER_H
#define EZ_HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C WebDAV server built on libmicrohttpd.
 *
 * This replaces the former cpp-httplib based C++ implementation. The public
 * surface mirrors the old WebDAVServer namespace so main.c can drive it the
 * same way the old main.cpp drove the C++ version.
 */

/* Blocks until the server is told to stop (via GET /stop). */
void WebDAVServer_Start(void);

/* Signals the running server to stop and tears down the daemon. */
void WebDAVServer_Stop(void);

/* Returns 1 if a server instance is already answering on the local port. */
int WebDAVServer_IsStarted(void);

/* Toggles rest mode (console sleep/resume): pauses/resumes serving. */
void WebDAVServer_SetRestMode(int toggle);

#ifdef __cplusplus
}
#endif

#endif
