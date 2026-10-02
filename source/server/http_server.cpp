#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include "http/httplib.h"
#include "server/http_server.h"
#include "util.h"
#include "dbglogger.h"

#define SUCCESS_MSG "{ \"result\": { \"success\": true, \"error\": null } }"
#define FAILURE_MSG "{ \"result\": { \"success\": false, \"error\": \"%s\" } }"

using namespace httplib;
namespace fs = std::filesystem;

Server *svr;
int http_server_port = 6702;
static bool stop_server = false;
static bool in_rest_mode = false;
constexpr int DOWNLOAD_SEGMENTS = 6;

namespace HttpServer
{
    std::string dump_headers(const Headers &headers)
    {
        std::string s;
        char buf[BUFSIZ];

        for (auto it = headers.begin(); it != headers.end(); ++it)
        {
            const auto &x = *it;
            snprintf(buf, sizeof(buf), "%s: %s\n", x.first.c_str(), x.second.c_str());
            s += buf;
        }

        return s;
    }

    std::string log(const Request &req, const Response &res)
    {
        std::string s;
        char buf[BUFSIZ];

        s += "================================\n";

        snprintf(buf, sizeof(buf), "%s %s %s", req.method.c_str(),
                 req.version.c_str(), req.path.c_str());
        s += buf;

        std::string query;
        for (auto it = req.params.begin(); it != req.params.end(); ++it)
        {
            const auto &x = *it;
            snprintf(buf, sizeof(buf), "%c%s=%s",
                     (it == req.params.begin()) ? '?' : '&', x.first.c_str(),
                     x.second.c_str());
            query += buf;
        }
        snprintf(buf, sizeof(buf), "%s\n", query.c_str());
        s += buf;

        s += dump_headers(req.headers);

        s += "--------------------------------\n";

        snprintf(buf, sizeof(buf), "%d %s\n", res.status, res.version.c_str());
        s += buf;
        s += dump_headers(res.headers);
        s += "\n";

        if (!res.body.empty())
        {
            s += res.body;
        }

        s += "\n";

        return s;
    }

    std::string generate_propfind_xml(const std::string &path)
    {
        std::string xml = R"(<?xml version="1.0" encoding="utf-8" ?>
            <d:multistatus xmlns:d="DAV:">
            <d:response>
                <d:href>)" + path + R"(</d:href>
                <d:propstat>
                <d:prop>
                    <d:resourcetype><d:collection/></d:resourcetype>
                </d:prop>
                <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
            </d:multistatus>)";
        return xml;
    }

    // Simple MIME type helper
    std::string get_mime_type(const std::string& path) {
        if (path.ends_with(".html") || path.ends_with(".htm")) return "text/html";
        if (path.ends_with(".css")) return "text/css";
        if (path.ends_with(".js")) return "application/javascript";
        if (path.ends_with(".png")) return "image/png";
        if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
        if (path.ends_with(".txt")) return "text/plain";
        return "application/octet-stream";
    }

    std::string get_html_head()
    {
        std::ostringstream ss;
        ss << R"(
            <!DOCTYPE html><html><head>
            <style>
            body{font-family:sans-serif;padding:20px;background:#f5f5f7;color:#1d1d1f;}
            ul{list-style:none;padding:0;}
            li{background:white;padding:12px;margin-bottom:6px;border-radius:6px;
            display:flex;flex-direction:column;justify-content:center;
            border:1px solid #d2d2d7;}
            .row{display:flex;align-items:center;justify-content:space-between;width:100%;}
            a{color:#0066cc;text-decoration:none;font-weight:500;}
            button{background:#0071e3;color:white;border:none;padding:6px 12px;
                border-radius:4px;cursor:pointer;font-size:13px;font-weight:bold;}
            button:hover{background:#0077ed;}
            #status-bar{margin:15px 0;font-weight:bold;color:#ff9500;font-size:16px;}
            .progress-container{width:100%;margin-top:10px;display:none;background:#e5e5ea;
                                border-radius:4px;padding:10px;box-sizing:border-box;}
            .progress-row{display:flex;align-items:center;justify-content:space-between;
                        margin:5px 0;font-size:12px;}
            progress{width:70%;height:14px;border-radius:6px;}
            </style>
            <script>
            function downloadSegment(url, start, end, index, onProg) {
            return new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.open('GET', url, true);
                xhr.responseType = 'blob';
                xhr.setRequestHeader('Range', 'bytes=' + start + '-' + end);
                xhr.onprogress = (e) => { 
                if (e.lengthComputable) onProg(index, e.loaded, e.total); 
                };
                xhr.onload = () => { 
                if (xhr.status === 206 || xhr.status === 200) resolve(xhr.response); 
                else reject(new Error('Status ' + xhr.status)); 
                };
                xhr.onerror = () => reject(new Error('Network error'));
                xhr.send();
            });
            }
            async function downloadParallel(fileUrl, saveName, elementId) {
            const status = document.getElementById('status-bar');
            const pContainer = document.getElementById('progress-' + elementId);
            status.innerText = 'Checking remote file headers...';
            try {
                const headRes = await fetch(fileUrl, { method: 'HEAD' });
                const totalSize = parseInt(headRes.headers.get('Content-Length'));
                const acceptRanges = headRes.headers.get('Accept-Ranges');
                if (!acceptRanges || isNaN(totalSize)) {
                status.innerText = 'Falling back to standard download...';
                window.location.href = fileUrl; return;
                }
            )";
                
                // Injecting config directly into javascript loop definitions
                ss << "    const threadCount = " << DOWNLOAD_SEGMENTS << ";\n";
                
                ss << R"(
                const chunkSize = Math.ceil(totalSize / threadCount);
                pContainer.style.display = 'block';
                status.innerText = 'Downloading segments in parallel...';
                const updateUI = (idx, loaded, total) => {
                const progBar = document.getElementById('bar-' + elementId + '-' + idx);
                const label = document.getElementById('lbl-' + elementId + '-' + idx);
                progBar.value = loaded; progBar.max = total;
                const pct = Math.round((loaded / total) * 100);
                label.innerText = 'Segment ' + (idx + 1) + ': ' + pct + '% (' + 
                    (loaded/1024/1024).toFixed(1) + 'MB / ' + 
                    (total/1024/1024).toFixed(1) + 'MB)';
                };
                const promises = [];
                for (let i = 0; i < threadCount; i++) {
                const start = i * chunkSize;
                const end = Math.min(start + chunkSize - 1, totalSize - 1);
                promises.push(downloadSegment(fileUrl, start, end, i, updateUI));
                }
                const blobs = await Promise.all(promises);
                status.innerText = 'Stitching fragments into local storage...';
                const finalBlob = new Blob(blobs, { type: 'application/octet-stream' });
                const link = document.createElement('a');
                link.href = URL.createObjectURL(finalBlob);
                link.download = saveName;
                link.click();
                status.innerText = 'Download Complete!';
                pContainer.style.display = 'none';
            } catch (err) {
                status.innerText = 'Error: ' + err.message;
            }
            }
            </script></head><body>
            )";

        return ss.str();
    }

    void ServerThread()
    {
        svr->set_socket_options([](socket_t sock)
        {
            int send_buf_size = 2 * 1024 * 1024; // Allocate a spacious 2MB buffer slot
            
            // Apply the option directly to the socket layer
            if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0) {
                std::cerr << "Warning: Failed to maximize socket send buffer.\n";
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0) {
                std::cerr << "Warning: Failed to maximize socket send buffer.\n";
            }

            // Optional: Disable Nagle's algorithm to eliminate tiny TCP pack delay cycles
            int nodelay = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, 
                    reinterpret_cast<const char*>(&nodelay), 
                    sizeof(nodelay));
        });

        svr->Get("/stop", [&](const Request & /*req*/, Response & /*res*/)
        {
            stop_server = true;
            svr->stop();
        });

        svr->Get("/version", [&](const Request & req, Response &res)
        {
            res.status = 200;
            char version[20];
            sprintf(version, "%.2f", 1.0f);
            res.set_content(version, "text/html");
        });

        // 3. CATCH-ALL ROUTE (Serving directly from the System Root "/")
        svr->Get(R"(/.*)", [&](const Request& req, Response& res)
        {
            fs::path canonical_path = fs::path(req.path);
            std::error_code ec;
            canonical_path = fs::weakly_canonical(canonical_path, ec);

            if (ec)
            {
                res.status = 403;
                res.set_content("<h1>Forbidden</h1>", "text/html");
                return;
            }

            if (!fs::exists(canonical_path))
            {
                res.status = 404;
                res.set_content("<h1>Not Found</h1>", "text/html");
                return;
            }

            if (fs::is_directory(canonical_path))
            {
                std::ostringstream html;
                html << get_html_head();
                html << "<h2>File Index: " << req.path << "</h2>";
                html << "<div id='status-bar'>Ready</div><hr><ul>";

                if (req.path != "/" && !req.path.empty())
                {
                    std::string cur = req.path;
                    if (cur.ends_with("/")) cur.pop_back();
                    fs::path parent = fs::path(cur).parent_path();
                    std::string p_str = parent.string();
                    if (!p_str.ends_with("/")) p_str += "/";
                    html << "<li><div class='row'><a href=\"" << p_str 
                        << "\">.. (Parent Directory)</a></div></li>";
                }

                size_t id_counter = 0;
                for (const auto& entry : fs::directory_iterator(canonical_path)) {
                    auto filename = entry.path().filename().string();
                    std::string href = req.path;
                    if (!href.ends_with("/")) href += "/";
                    href += filename;
                    id_counter++;
                    std::string el_id = "file_" + std::to_string(id_counter);

                    if (fs::is_directory(entry.path())) {
                        filename += "/"; href += "/";
                        html << "<li><div class='row'><a href=\"" << href 
                            << "\">" << filename << "</a></div></li>";
                    } else {
                        html << "<li><div class='row'><a href=\"" << href << "\">" 
                            << filename << "</a>"
                            << "<button onclick=\"downloadParallel('" << href 
                            << "', '" << filename << "', '" << el_id 
                            << "')\">Fast Download</button></div>"
                            << "<div class='progress-container' id='progress-" 
                            << el_id << "'>";
                        
                        // Automatically renders matching progress components
                        for (int t = 0; t < DOWNLOAD_SEGMENTS; t++) {
                            html << "<div class='progress-row'><span id='lbl-" 
                                << el_id << "-" << t << "'>Segment " << (t + 1) 
                                << ": 0%</span><progress id='bar-" << el_id 
                                << "-" << t << "' value='0' max='100'></progress>"
                                << "</div>";
                        }
                        html << "</div></li>";
                    }
                }
                html << "</ul><hr></body></html>";
                res.set_content(html.str(), "text/html");
            } 
            else
            {
                std::string path_str = canonical_path.string();
                std::ifstream file(path_str, std::ios::binary | std::ios::ate);
                if (!file) { res.status = 500; return; }
                size_t file_size = file.tellg();
                file.close();

                res.set_header("Accept-Ranges", "bytes");

                size_t start_byte = 0;
                size_t end_byte = file_size - 1;
                bool is_range = false;

                if (req.has_header("Range")) {
                    std::string r_val = req.get_header_value("Range");
                    size_t eq_pos = r_val.find('=');
                    size_t dash_pos = r_val.find('-');
                    if (eq_pos != std::string::npos && 
                        dash_pos != std::string::npos) {
                        try {
                            start_byte = std::stoull(
                            r_val.substr(eq_pos + 1, dash_pos - eq_pos - 1)
                            );
                            if (dash_pos + 1 < r_val.size()) {
                                end_byte = std::stoull(r_val.substr(dash_pos + 1));
                            }
                            is_range = true;
                        } catch (...) {}
                    }
                }

                size_t send_length = end_byte - start_byte + 1;

                if (is_range)
                {
                    res.status = 206;
                    std::string c_range = "bytes " + std::to_string(start_byte) + "-" + std::to_string(end_byte) + "/" + std::to_string(file_size);

                    res.set_header("Content-Range", c_range);
                }

                res.set_content_provider(
                    send_length, get_mime_type(path_str),
                    [path_str, start_byte](size_t offset, size_t length, DataSink &sink)
                {
                    std::ifstream stream(path_str, std::ios::binary);
                    if (!stream) return false;
                    stream.seekg(start_byte + offset, std::ios::beg);
                    std::vector<char> buffer(length);
                    stream.read(buffer.data(), length);
                    std::streamsize bytes_read = stream.gcount();
                    if (bytes_read > 0) sink.write(buffer.data(), bytes_read);
                    return true;
                });
            }
        });

        // 1. Handle WebDAV Directory Listing & Metadata queries
        svr->CustomRoute("PROPFIND", R"((.*))", [&](const Request &req, Response &res)
        {
            // Enforce WebDAV headers
            res.set_header("DAV", "1, 2");
            res.set_content(generate_propfind_xml(req.path), "application/xml; charset=utf-8");
            res.status = 207; // Multi-Status
        });
            
        // 2. Handle File Uploads (WebDAV PUT)
        svr->Put(R"((.*))", [&](const Request &req, Response &res)
        {
            // Map the URI path directly to the PS5 sandbox file system (e.g., /data/)
            std::string target_path = "/data" + req.path;
            
            std::ofstream file(target_path, std::ios::binary);
            if (file.is_open()) {
                file.write(req.body.data(), req.body.size());
                file.close();
                res.status = 201; // Created
            } else {
                res.status = 500; // Internal Server Error
            } 
        });

        // 3. Handle Directory Creation (WebDAV MKCOL)
        svr->CustomRoute("MKCOL", R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target_dir = "/data" + req.path;
            if (mkdir(target_dir.c_str(), 0777) == 0) {
                res.status = 201;
            } else {
                res.status = 405; // Not Allowed / Already Exists
            }
        });

        // 4. Handle Deletions (WebDAV DELETE)
        svr->Delete(R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target = "/data" + req.path;
            if (unlink(target.c_str()) == 0 || rmdir(target.c_str()) == 0) {
                res.status = 204; // No Content
            } else {
                res.status = 404; // Not Found
            }
        });

        svr->set_error_handler([](const Request & /*req*/, Response &res)
        {
            const char *fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
            char buf[BUFSIZ];
            snprintf(buf, sizeof(buf), fmt, res.status);
            res.set_content(buf, "text/html");
        });

        svr->set_logger([](const Request &req, const Response &res)
        {
            dbglogger_log("%s", log(req, res).c_str());
        });
       
        svr->set_payload_max_length(1024 * 1024 * 12);
        svr->set_tcp_nodelay(true);
        svr->set_mount_point("/", "/");

        svr->listen("0.0.0.0", http_server_port);
    }

    void Start()
    {
        if (svr == nullptr)
            svr = new Server();
        if (!svr->is_valid())
        {
            return;
        }

        while (!stop_server)
        {
            if (!in_rest_mode)
            {
                Util::Notify("Starting WebDav Server on port %d", http_server_port);
                ServerThread();
            }

            if (!stop_server)
            {
                if (!in_rest_mode)
                {
                    delete svr;
                    svr = new Server();
                }
                else
                {
                    sleep(2);
                }
            }
        }
    }

    void Stop()
    {
        stop_server = true;
        svr->stop();
    }

    bool IsStarted()
    {
        Client client = Client("http://127.0.0.1:6702");
        if (auto res = client.Get("/version"))
        {
            return true;
        }
        return false;
    }

    void SetRestMode(bool toggle)
    {
        in_rest_mode = toggle;
    }
}
