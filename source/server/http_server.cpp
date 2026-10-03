#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <optional>
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
constexpr int DOWNLOAD_SEGMENTS = 4;

// Structure to hold failure details
struct DeleteFailure {
    fs::path path;
    std::error_code error;
};

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

    // Helper: Convert file time to HTTP-date format required by WebDAV (RFC 1123)
    std::string format_http_date(fs::file_time_type file_time) {
        auto sct = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        std::time_t tt = std::chrono::system_clock::to_time_t(sct);
        std::tm gmt = *std::gmtime(&tt);
        
        std::stringstream ss;
        ss << std::put_time(&gmt, "%a, %d %b %Y %H:%M:%S GMT");
        return ss.str();
    }

    // Helper: Appends a singular resource node item into the WebDAV response stream
    void append_resource_xml(std::stringstream& xml, const std::string& href_path, const fs::path& local_path) {
        std::error_code ec;
        bool is_dir = fs::is_directory(local_path, ec);
        if (ec) return; // Prevent parsing if structural errors occur
        
        xml << "    <d:response>\n";
        xml << "        <d:href>" << href_path << (is_dir && href_path.back() != '/' ? "/" : "") << "</d:href>\n";
        xml << "        <d:propstat>\n";
        xml << "            <d:prop>\n";
        
        std::string filename = local_path == "/" ? "" : local_path.filename().string();
        xml << "                <d:displayname>" << filename << "</d:displayname>\n";
        
        if (is_dir) {
            xml << "                <d:resourcetype><d:collection/></d:resourcetype>\n";
            xml << "                <d:getcontenttype>httpd/unix-directory</d:getcontenttype>\n";
        } else {
            xml << "                <d:resourcetype/>\n";
            
            uintmax_t size = fs::file_size(local_path, ec);
            xml << "                <d:getcontentlength>" << (!ec ? size : 0) << "</d:getcontentlength>\n";
            
            if (local_path.extension() == ".txt") xml << "                <d:getcontenttype>text/plain</d:getcontenttype>\n";
            else if (local_path.extension() == ".html") xml << "                <d:getcontenttype>text/html</d:getcontenttype>\n";
            else xml << "                <d:getcontenttype>application/octet-stream</d:getcontenttype>\n";
        }
        
        auto write_time = fs::last_write_time(local_path, ec);
        if (!ec) {
            xml << "                <d:getlastmodified>" << format_http_date(write_time) << "</d:getlastmodified>\n";
        }

        xml << "            </d:prop>\n";
        xml << "            <d:status>HTTP/1.1 200 OK</d:status>\n";
        xml << "        </d:propstat>\n";
        xml << "    </d:response>\n";
    }

    // Helper: Recursively map contents for Depth=infinity requests
    void append_recursive_contents(std::stringstream& xml, const std::string& parent_href, const fs::path& local_path) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec)) {
            std::string base_href = parent_href;
            if (base_href.back() != '/') base_href += "/";
            std::string child_href = base_href + entry.path().filename().string();

            append_resource_xml(xml, child_href, entry.path());

            // Keep descending if child is a nested subdirectory
            if (entry.is_directory(ec) && !ec) {
                append_recursive_contents(xml, child_href, entry.path());
            }
        }
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

    std::optional<DeleteFailure> delete_recursive_strict(const fs::path& current) {
        std::error_code ec;

        if (fs::is_directory(current, ec)) {
            // Look inside the directory
            for (const auto& entry : fs::directory_iterator(current, ec)) {
                if (ec) return DeleteFailure{current, ec};

                // Recurse into the item
                if (auto failure = delete_recursive_strict(entry.path())) {
                    return failure; 
                }
            }
        }

        // Delete the item itself (file or empty folder)
        fs::remove(current, ec);
        if (ec) {
            return DeleteFailure{current, ec};
        }

        return std::nullopt; // Success
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
                dbglogger_log("Warning: Failed to maximize socket send buffer.");
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0) {
                dbglogger_log("Warning: Failed to maximize socket send buffer.");
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

        svr->Get(R"((.*))", [&](const Request& req, Response& res)
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

                if (!req.has_header("Range"))
                {
                    res.status = 200;
                }

                res.set_content_provider(
                    file_size, get_mime_type(path_str),
                    [path_str](size_t offset, size_t length, DataSink &sink)
                {
                    std::ifstream stream(path_str, std::ios::binary);
                    if (!stream)
                        return false;
                    stream.seekg(offset, std::ios::beg);

                    size_t total_to_read = length;
                    size_t total_read = 0;
                    size_t bytes_remaining = length;
                    std::vector<char> buffer(0x100000);

                    dbglogger_log("8");
                    while (total_read < total_to_read)
                    {
                        size_t bytes_to_read = std::min(bytes_remaining, buffer.size());

                        stream.read(buffer.data(), bytes_to_read);

                        std::streamsize bytes_read = stream.gcount();
                        if (bytes_read > 0)
                        {
                            sink.write(buffer.data(), bytes_read);
                            total_read += bytes_read;
                            bytes_remaining -= bytes_read;
                        }

                        if (stream.bad())
                        {
                            stream.close();
                            return false;
                        }

                        if (stream.eof() && total_read < total_to_read)
                        {
                            return false;
                        }
                    }

                    return true;
                });
            }
        });

        svr->CustomRoute("PROPFIND", R"((.*))", [&](const Request &req, Response &res)
        {
            fs::path local_path(req.path);

            std::error_code ec;
            if (!fs::exists(local_path, ec)) {
                res.status = 404;
                res.set_content("Not Found", "text/plain");
                return;
            }

            std::string depth = "infinity";
            if (req.has_header("Depth")) {
                depth = req.get_header_value("Depth");
            }

            std::stringstream xml;
            xml << "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n";
            xml << "<d:multistatus xmlns:d=\"DAV:\">\n";

            append_resource_xml(xml, req.path, local_path);

            if (fs::is_directory(local_path, ec) && !ec) {
                if (depth == "1") {
                    for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec)) {
                        std::string parent_href = req.path;
                        if (parent_href.back() != '/') parent_href += "/";
                        std::string child_href = parent_href + entry.path().filename().string();

                        append_resource_xml(xml, child_href, entry.path());
                    }
                } 
                else if (depth == "infinity") {
                    append_recursive_contents(xml, req.path, local_path);
                }
            }

            xml << "</d:multistatus>";

            res.status = 207; // Multi-Status
            res.set_content(xml.str(), "application/xml; charset=utf-8");
            res.set_header("DAV", "1");
        });

        svr->Put(R"((.*))", [&](const Request &req, Response &res, const ContentReader &content_reader)
        {
            std::string target_path = req.path;
            
            std::ofstream file(target_path, std::ios::binary);
            if (!file.is_open()) {
                res.status = 500; // Internal Server Error
                res.set_content("Internal Server Error: Cannot write to disk", "text/plain");
                return;
            } 

            uint64_t total_received_bytes = 0;
            bool read_success = content_reader([&](const char *data, size_t data_length)
            {
                file.write(data, data_length);

                total_received_bytes += data_length;
                dbglogger_log("Received chunk: %llu bytes. Total so far: %llu bytes", data_length, total_received_bytes);
                return true;
            });

            file.close();

            if (read_success)
            {
                dbglogger_log("Upload completed successfully! Total size: %llu bytes", total_received_bytes);
                res.status = 200;
                res.set_content("Created", "text/plain");
            }
            else
            {
                dbglogger_log("Upload interrupted or client disconnected early.");
                res.status = 400;
                res.set_content("Bad Request: Stream interrupted", "text/plain");
            }
        });

        svr->CustomRoute("MKCOL", R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target_dir = req.path;
            if (mkdir(target_dir.c_str(), 0777) == 0) {
                res.status = 201;
            } else {
                res.status = 405; // Not Allowed / Already Exists
            }
        });

        svr->Delete(R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target = req.path;
            
            if (!fs::exists(target))
            {
                res.status = 207;
                res.set_content(R"(
                    <d:multistatus xmlns:d="DAV:"><d:response><d:href>)" + target +
                    R"(</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response></d:multistatus>)", "application/xml; charset=\"utf-8\"");
                return;
            }

            if (auto failure = delete_recursive_strict(target))
            {
                res.status = 207;
                std::ostringstream ss;
                ss << R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>)" << failure->path << R"(</d:href><d:status>HTTP/1.1 )";
                if (failure->error == std::errc::permission_denied)
                {
                    ss <<  "403 Forbidden";
                }
                else if (failure->error == std::errc::resource_unavailable_try_again || failure->error == std::errc::device_or_resource_busy)
                {
                    ss << "423 Locked";
                }
                else if (failure->error == std::errc::read_only_file_system)
                {
                    ss << " 405 Method Not Allowed";
                }
                else
                {
                    ss << "500 Internal Server Error";
                }
                ss << "</d:status></d:response></d:multistatus>";
                res.set_content(ss.str(), "application/xml; charset=\"utf-8\"");
                return;                
            }
            
            res.status = 204;
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
