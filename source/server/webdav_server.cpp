#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <tinyxml2.h>
#include "http/httplib.h"
#include "server/webdav_server.h"
#include "util.h"
#include "dbglogger.h"

using namespace httplib;
namespace fs = std::filesystem;

struct DeleteFailure
{
    fs::path path;
    std::error_code error;
};

struct PropAction
{
    std::string ns_prefix;
    std::string name;
    std::string value;
    bool is_remove;
};

struct WebDavLock
{
    std::string path;
    std::string token;
    std::string type;
    std::string scope;
    std::string owner;
    int depth;
};

Server *svr;
int http_server_port = 6702;
static bool stop_server = false;
static bool in_rest_mode = false;
constexpr int DOWNLOAD_SEGMENTS = 4;

std::map<std::string, WebDavLock> g_path_to_lock; // Key: file system path
std::map<std::string, std::string> g_token_to_path; // Key: lock token
std::mutex g_lock_mutex;

namespace WebDAVServer
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

    std::string format_http_date(fs::file_time_type file_time)
    {
        auto sct = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        std::time_t tt = std::chrono::system_clock::to_time_t(sct);
        std::tm gmt = *std::gmtime(&tt);
        
        std::stringstream ss;
        ss << std::put_time(&gmt, "%a, %d %b %Y %H:%M:%S GMT");
        return ss.str();
    }

    void append_resource_xml(std::stringstream& xml, const std::string& href_path, const fs::path& local_path)
    {
        std::error_code ec;
        bool is_dir = fs::is_directory(local_path, ec);
        if (ec) return; // Prevent parsing if structural errors occur
        
        xml << "    <D:response>\n";
        xml << "        <D:href>" << href_path << (is_dir && href_path.back() != '/' ? "/" : "") << "</D:href>\n";
        xml << "        <D:propstat>\n";
        xml << "            <D:prop>\n";
        
        std::string filename = local_path == "/" ? "" : local_path.filename().string();
        xml << "                <D:displayname>" << filename << "</D:displayname>\n";
        
        if (is_dir) {
            xml << "                <D:resourcetype><D:collection/></D:resourcetype>\n";
            xml << "                <D:getcontenttype>httpd/unix-directory</D:getcontenttype>\n";
        } else {
            xml << "                <D:resourcetype/>\n";
            
            uintmax_t size = fs::file_size(local_path, ec);
            xml << "                <D:getcontentlength>" << (!ec ? size : 0) << "</D:getcontentlength>\n";
            
            if (local_path.extension() == ".txt") xml << "                <D:getcontenttype>text/plain</D:getcontenttype>\n";
            else if (local_path.extension() == ".html") xml << "                <D:getcontenttype>text/html</D:getcontenttype>\n";
            else xml << "                <D:getcontenttype>application/octet-stream</D:getcontenttype>\n";
        }
        
        auto write_time = fs::last_write_time(local_path, ec);
        if (!ec) {
            xml << "                <D:getlastmodified>" << format_http_date(write_time) << "</D:getlastmodified>\n";
        }

        xml << "            </D:prop>\n";
        xml << "            <D:status>HTTP/1.1 200 OK</D:status>\n";
        xml << "        </D:propstat>\n";
        xml << "    </D:response>\n";
    }

    void append_recursive_contents(std::stringstream& xml, const std::string& parent_href, const fs::path& local_path)
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec))
        {
            std::string base_href = parent_href;
            if (base_href.back() != '/')
                base_href += "/";
            std::string child_href = base_href + entry.path().filename().string();

            append_resource_xml(xml, child_href, entry.path());

            // Keep descending if child is a nested subdirectory
            if (entry.is_directory(ec) && !ec)
            {
                append_recursive_contents(xml, child_href, entry.path());
            }
        }
    }

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

    std::optional<DeleteFailure> delete_recursive_strict(const fs::path& current)
    {
        std::error_code ec;

        if (fs::is_directory(current, ec))
        {
            for (const auto& entry : fs::directory_iterator(current, ec))
            {
                if (ec) return DeleteFailure{current, ec};

                if (auto failure = delete_recursive_strict(entry.path()))
                {
                    return failure; 
                }
            }
        }

        fs::remove(current, ec);
        if (ec)
        {
            return DeleteFailure{current, ec};
        }

        return std::nullopt; // Success
    }

    std::vector<PropAction> parse_proppatch(const std::string& xml_body)
    {
        std::vector<PropAction> actions;
        tinyxml2::XMLDocument doc;
        
        if (doc.Parse(xml_body.c_str()) != tinyxml2::XML_SUCCESS)
        {
            return actions; 
        }

        auto* root = doc.FirstChildElement();
        if (!root) return actions;

        for (auto* action_node = root->FirstChildElement(); action_node != nullptr; action_node = action_node->NextSiblingElement())
        {
            std::string action_type = action_node->Value();
            
            size_t colon_pos = action_type.find(':');
            if (colon_pos != std::string::npos)
            {
                action_type = action_type.substr(colon_pos + 1);
            }

            bool is_remove = (action_type == "remove");
            if (action_type != "set" && !is_remove) continue;

            auto* prop_node = action_node->FirstChildElement();
            if (!prop_node) continue;
            
            std::string prop_val = prop_node->Value();
            if (prop_val.find("prop") == std::string::npos) continue;

            // Extract individual properties inside <prop>
            for (auto* p = prop_node->FirstChildElement(); p != nullptr; p = p->NextSiblingElement())
            {
                PropAction action;
                action.is_remove = is_remove;
                
                std::string full_name = p->Value();
                size_t p_colon = full_name.find(':');
                
                if (p_colon != std::string::npos)
                {
                    action.ns_prefix = full_name.substr(0, p_colon); 
                    action.name = full_name.substr(p_colon + 1);
                }
                else
                {
                    action.ns_prefix = "";
                    action.name = full_name;
                }
                
                if (!is_remove && p->GetText())
                {
                    action.value = p->GetText();
                }

                actions.push_back(action);
            }
        }
        return actions;
    }

    // Generates the compliant WebDAV 207 Multi-Status XML payload for PROPPATCH
    std::string build_proppatch_success_response(const std::string& href, const std::vector<PropAction>& actions)
    {
        tinyxml2::XMLDocument doc;
        
        auto* decl = doc.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\"");
        doc.InsertEndChild(decl);

        auto* multistatus = doc.NewElement("D:multistatus");
        multistatus->SetAttribute("xmlns:D", "DAV:");
        // Match common standard custom payload extensions
        multistatus->SetAttribute("xmlns:Z", "http://example.com");
        doc.InsertEndChild(multistatus);

        auto* response = doc.NewElement("D:response");
        multistatus->InsertEndChild(response);

        auto* href_node = doc.NewElement("D:href");
        href_node->SetText(href.c_str());
        response->InsertEndChild(href_node);

        auto* propstat = doc.NewElement("D:propstat");
        response->InsertEndChild(propstat);

        auto* prop_container = doc.NewElement("D:prop");
        propstat->InsertEndChild(prop_container);

        for (const auto& action : actions)
        {
            // Maintain the incoming element namespace formatting
            std::string tag_name = action.ns_prefix.empty() ? action.name : (action.ns_prefix + ":" + action.name);
            auto* p_node = doc.NewElement(tag_name.c_str());
            prop_container->InsertEndChild(p_node);
        }

        auto* status_node = doc.NewElement("D:status");
        status_node->SetText("HTTP/1.1 200 OK");
        propstat->InsertEndChild(status_node);

        tinyxml2::XMLPrinter printer;
        doc.Accept(&printer);
        return printer.CStr();
    }

    // Generates the required WebDAV XML body indicating a successful resource Lock setup
    std::string build_lock_success_response(const std::string& href, const std::string& token)
    {
        tinyxml2::XMLDocument doc;
        
        auto* decl = doc.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\"");
        doc.InsertEndChild(decl);

        auto* prop = doc.NewElement("D:prop");
        prop->SetAttribute("xmlns:D", "DAV:");
        doc.InsertEndChild(prop);

        auto* lockdiscovery = doc.NewElement("D:lockdiscovery");
        prop->InsertEndChild(lockdiscovery);

        auto* activelock = doc.NewElement("D:activelock");
        lockdiscovery->InsertEndChild(activelock);

        // Type
        auto* locktype = doc.NewElement("D:locktype");
        locktype->InsertEndChild(doc.NewElement("D:write"));
        activelock->InsertEndChild(locktype);

        // Scope
        auto* lockscope = doc.NewElement("D:lockscope");
        lockscope->InsertEndChild(doc.NewElement("D:exclusive"));
        activelock->InsertEndChild(lockscope);

        // Depth
        auto* depth = doc.NewElement("D:depth");
        depth->SetText("0");
        activelock->InsertEndChild(depth);

        // Timeout duration response
        auto* timeout = doc.NewElement("D:timeout");
        timeout->SetText("Second-3600");
        activelock->InsertEndChild(timeout);

        // Target Active Token URI
        auto* locktoken = doc.NewElement("D:locktoken");
        auto* href_token = doc.NewElement("D:href");
        href_token->SetText(("opaquelocktoken:" + token).c_str());
        locktoken->InsertEndChild(href_token);
        activelock->InsertEndChild(locktoken);

        // Root URI tracking target
        auto* lockroot = doc.NewElement("D:lockroot");
        auto* href_root = doc.NewElement("D:href");
        href_root->SetText(href.c_str());
        lockroot->InsertEndChild(href_root);
        activelock->InsertEndChild(lockroot);

        tinyxml2::XMLPrinter printer;
        doc.Accept(&printer);
        return printer.CStr();
    }

    bool is_resource_locked(const std::string& path, const Request& req)
    {
        std::lock_guard<std::mutex> guard(g_lock_mutex);
        
        // Look up if the exact path is locked
        auto it = g_path_to_lock.find(path);
        if (it == g_path_to_lock.end())
        {
            return false; // Not locked
        }
        
        if (req.has_header("If"))
        {
            std::string if_header = req.get_header_value("If");
            
            if (if_header.find(it->second.token) != std::string::npos)
            {
                return false; // Token matches! Allow the operation.
            }
        }
        
        return true; // Resource is locked and token didn't match. Block it.
    }

    std::string generate_uuid()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 32; ++i)
        {
            if (i == 8 || i == 12 || i == 16 || i == 20) ss << "-";
            ss << dis(gen);
        }
        return ss.str();
    }

    std::string parse_destination_path(std::string dest_header)
    {
        size_t protocol_pos = dest_header.find("://");
        if (protocol_pos != std::string::npos)
        {
            size_t path_pos = dest_header.find('/', protocol_pos + 3);
            if (path_pos != std::string::npos)
            {
                dest_header = dest_header.substr(path_pos);
            }
            else 
            {
                dest_header = "/";
            }
        }
        
        // 2. Percent-decode the URI (e.g., convert "%20" back to spaces)
        std::string decoded;
        decoded.reserve(dest_header.length());
        for (size_t i = 0; i < dest_header.length(); ++i)
        {
            if (dest_header[i] == '%' && i + 2 < dest_header.length())
            {
                int value = std::stol(dest_header.substr(i + 1, 2), nullptr, 16);
                decoded += static_cast<char>(value);
                i += 2;
            }
            else
            {
                decoded += dest_header[i];
            }
        }
        return decoded;
    }

    void safe_remove_all(const fs::path& target)
    {
        std::error_code ec;
        if (!fs::exists(target, ec)) return;

        if (fs::is_directory(target, ec))
        {
            std::vector<fs::path> directories_to_delete;

            auto it = fs::recursive_directory_iterator(target, 
                        fs::directory_options::skip_permission_denied, ec);
            
            while (!ec && it != fs::recursive_directory_iterator())
            {
                std::error_code item_ec;

                if (fs::is_directory(it->path(), item_ec))
                {
                    directories_to_delete.push_back(it->path());
                }
                else
                {
                    fs::remove(it->path(), item_ec);
                }
                it.increment(ec);
            }

            std::sort(directories_to_delete.begin(), directories_to_delete.end(),
                    [](const fs::path& a, const fs::path& b) {
                        return a.string().length() > b.string().length();
                    });

            for (const auto& dir : directories_to_delete)
            {
                std::error_code remove_ec;
                fs::remove(dir, remove_ec);
            }
        }
        
        fs::remove(target, ec);
    }

    void ServerThread()
    {
        svr->set_socket_options([](socket_t sock)
        {
            int send_buf_size = 2 * 1024 * 1024;
            
            if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0) {
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0) {
            }

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

        svr->Options(R"((.*))", [&](const Request&, Response& res)
        {
            res.status = 200;
            res.set_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, COPY, MOVE, LOCK, UNLOCK");
            res.set_header("DAV", "1, 2"); // Signals class levels processing rules (Locks enabled)
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
                for (const auto& entry : fs::directory_iterator(canonical_path))
                {
                    auto filename = entry.path().filename().string();
                    std::string href = req.path;
                    if (!href.ends_with("/")) href += "/";
                    href += filename;
                    id_counter++;
                    std::string el_id = "file_" + std::to_string(id_counter);

                    if (fs::is_directory(entry.path()))
                    {
                        filename += "/"; href += "/";
                        html << "<li><div class='row'><a href=\"" << href 
                            << "\">" << filename << "</a></div></li>";
                    }
                    else
                    {
                        html << "<li><div class='row'><a href=\"" << href << "\">" 
                            << filename << "</a>"
                            << "<button onclick=\"downloadParallel('" << href 
                            << "', '" << filename << "', '" << el_id 
                            << "')\">Fast Download</button></div>"
                            << "<div class='progress-container' id='progress-" 
                            << el_id << "'>";
                        
                        // Automatically renders matching progress components
                        for (int t = 0; t < DOWNLOAD_SEGMENTS; t++)
                        {
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
            if (!fs::exists(local_path, ec))
            {
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
            xml << "<D:multistatus xmlns:D=\"DAV:\">\n";

            append_resource_xml(xml, req.path, local_path);

            if (fs::is_directory(local_path, ec) && !ec)
            {
                if (depth == "1")
                {
                    for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec))
                    {
                        std::string parent_href = req.path;
                        if (parent_href.back() != '/') parent_href += "/";
                        std::string child_href = parent_href + entry.path().filename().string();

                        append_resource_xml(xml, child_href, entry.path());
                    }
                } 
                else if (depth == "infinity")
                {
                    append_recursive_contents(xml, req.path, local_path);
                }
            }

            xml << "</D:multistatus>";

            res.status = 207; // Multi-Status
            res.set_content(xml.str(), "application/xml; charset=utf-8");
            res.set_header("DAV", "1, 2");
        });

        svr->Put(R"((.*))", [&](const Request &req, Response &res, const ContentReader &content_reader)
        {
            std::string target_path = req.path;
            
            std::ofstream file(target_path, std::ios::binary);
            if (!file.is_open())
            {
                res.status = 500; // Internal Server Error
                res.set_content("Internal Server Error: Cannot write to disk", "text/plain");
                return;
            } 

            uint64_t total_received_bytes = 0;
            bool read_success = content_reader([&](const char *data, size_t data_length)
            {
                file.write(data, data_length);

                total_received_bytes += data_length;
                return true;
            });

            file.close();

            if (read_success)
            {
                res.status = 200;
                res.set_content("Created", "text/plain");
            }
            else
            {
                res.status = 400;
                res.set_content("Bad Request: Stream interrupted", "text/plain");
            }
        });

        svr->CustomRoute("MKCOL", R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target_dir = req.path;
            if (mkdir(target_dir.c_str(), 0777) == 0)
            {
                res.status = 201;
            }
            else
            {
                res.status = 405; // Not Allowed / Already Exists
            }
        });

        svr->Delete(R"((.*))", [&](const Request &req, Response &res)
        {
            std::string target = req.path;
            
            if (is_resource_locked(req.path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }

            if (!fs::exists(target))
            {
                res.status = 207;
                res.set_content(R"(
                    <D:multistatus xmlns:D="DAV:"><D:response><D:href>)" + target +
                    R"(</D:href><D:status>HTTP/1.1 404 Not Found</D:status></D:response></D:multistatus>)", "application/xml; charset=\"utf-8\"");
                return;
            }

            if (auto failure = delete_recursive_strict(target))
            {
                res.status = 207;
                std::ostringstream ss;
                ss << R"(<D:multistatus xmlns:D="DAV:"><D:response><D:href>)" << failure->path << R"(</D:href><D:status>HTTP/1.1 )";
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
                ss << "</D:status></D:response></D:multistatus>";
                res.set_content(ss.str(), "application/xml; charset=\"utf-8\"");
                return;                
            }
            
            res.status = 204;
        });

        svr->CustomRoute("COPY", R"((.*))", [&](const Request& req, Response& res)
        {
            if (!req.has_header("Destination"))
            {
                res.status = 400;
                res.set_content("Missing 'Destination' header.", "text/plain");
                return;
            }

            // Map paths directly to the FreeBSD filesystem
            fs::path src_path(req.path);
            fs::path dest_path(parse_destination_path(req.get_header_value("Destination")));

            if (is_resource_locked(dest_path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }

            if (!fs::exists(src_path))
            {
                res.status = 404; // Not Found
                res.set_content("Source resource not found.", "text/plain");
                return;
            }

            std::string overwrite = req.has_header("Overwrite") ? req.get_header_value("Overwrite") : "T";
            bool dest_exists = fs::exists(dest_path);

            if (dest_exists && overwrite == "F")
            {
                res.status = 412; // Precondition Failed
                res.set_content("Destination exists and Overwrite is False.", "text/plain");
                return;
            }

            if (!fs::exists(dest_path.parent_path()))
            {
                res.status = 409; // Conflict (Parent collection missing)
                res.set_content("Conflict: Destination parent collection does not exist.", "text/plain");
                return;
            }

            std::string depth = req.has_header("Depth") ? req.get_header_value("Depth") : "infinity";

            try {
                if (dest_exists)
                {
                    safe_remove_all(dest_path);
                }

                fs::copy_options options = fs::copy_options::none;
                if (depth == "infinity")
                {
                    options = fs::copy_options::recursive;
                }
                else if (depth == "0")
                {
                    // If it's a directory, Depth: 0 means create an empty target directory
                    if (fs::is_directory(src_path))
                    {
                        fs::create_directory(dest_path);
                        res.status = dest_exists ? 204 : 201;
                        res.set_header("Content-Length", "0");
                        return;
                    }
                    // For regular files, Depth 0 acts as a standard single file copy
                }

                // Perform the system filesystem copy
                dbglogger_log("copy src_path=%s, dest_path=%s, options=%d", src_path.c_str(), dest_path.c_str(), options);
                fs::copy(src_path, dest_path, options);

                res.status = dest_exists ? 204 : 201;
                res.set_header("Content-Length", "0");

            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "[COPY ERROR] Filesystem exception: " << e.what() << "\n";
                res.status = 500;
                res.set_content("Internal server error copying resource.", "text/plain");
            }
        });

        svr->CustomRoute("MOVE", R"((.*))", [&](const Request& req, Response& res) {
            if (!req.has_header("Destination"))
            {
                res.status = 400;
                res.set_content("Missing 'Destination' header.", "text/plain");
                return;
            }

            if (is_resource_locked(req.path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }

            // Use the request path and destination header directly as system paths
            fs::path src_path(req.path);
            fs::path dest_path(parse_destination_path(req.get_header_value("Destination")));

            if (!fs::exists(src_path))
            {
                res.status = 404; 
                res.set_content("Source resource not found.", "text/plain");
                return;
            }

            std::string overwrite = req.has_header("Overwrite") ? req.get_header_value("Overwrite") : "T";
            bool dest_exists = fs::exists(dest_path);

            if (dest_exists && overwrite == "F")
            {
                res.status = 412; // Precondition Failed
                res.set_content("Destination exists and Overwrite is False.", "text/plain");
                return;
            }

            if (!fs::exists(dest_path.parent_path()))
            {
                res.status = 409; // Conflict (Parent collection missing)
                res.set_content("Conflict: Destination parent collection does not exist.", "text/plain");
                return;
            }

            try
            {
                // Remove target first if overwriting (prevents directory-not-empty failures)
                if (dest_exists)
                {
                    safe_remove_all(dest_path);
                }

                // Perform atomic rename system call on FreeBSD
                fs::rename(src_path, dest_path);

                // 201 if created completely fresh, 204 if an existing resource was overwritten
                res.status = dest_exists ? 204 : 201;
                res.set_header("Content-Length", "0");

            }
            catch (const fs::filesystem_error& e)
            {
                std::cerr << "[MOVE ERROR] Filesystem exception: " << e.what() << "\n";
                
                // Fallback for cross-device links (e.g., moving files across different ZFS datasets or mount points)
                if (e.code() == std::errc::cross_device_link)
                {
                    try
                    {
                        fs::copy(src_path, dest_path, fs::copy_options::recursive);
                        safe_remove_all(src_path);
                        res.status = dest_exists ? 204 : 201;
                        res.set_header("Content-Length", "0");
                        return;
                    }
                    catch (const std::exception& inner_ex)
                    {
                        res.status = 500;
                        std::stringstream ss;
                        ss << "Cross-device move fallback failed. Error: " << inner_ex.what();
                        res.set_content(ss.str(), "text/plain");
                        return;
                    }
                }

                res.status = 500;
                res.set_content("Internal server error moving file resource.", "text/plain");
            }
        });

        svr->CustomRoute("LOCK", R"((.*))", [](const Request& req, Response& res) {
            std::string target_path = req.path;

            std::string owner_info = "";
            std::string lock_scope = "exclusive"; // Default standard fallback

            // Parse incoming XML payload if present (Initial Lock Request)
            if (!req.body.empty())
            {
                tinyxml2::XMLDocument doc;
                if (doc.Parse(req.body.c_str()) == tinyxml2::XML_SUCCESS)
                {
                    auto* root = doc.FirstChildElement();
                    if (root)
                    {
                        // Extract scope: exclusive or shared
                        auto* lockscope_node = root->FirstChildElement();
                        if (lockscope_node && std::string(lockscope_node->Value()).find("lockscope") != std::string::npos)
                        {
                            auto* scope_child = lockscope_node->FirstChildElement();
                            if (scope_child)
                            {
                                std::string scope_val = scope_child->Value();
                                if (scope_val.find("shared") != std::string::npos) lock_scope = "shared";
                            }
                        }

                        // Extract owner text info if submitted by client
                        auto* owner_node = root->FirstChildElement("D:owner");
                        if (!owner_node)
                            owner_node = root->FirstChildElement("owner");
                        if (owner_node && owner_node->GetText())
                        {
                            owner_info = owner_node->GetText();
                        }
                    }
                }
            }

            std::lock_guard<std::mutex> guard(g_lock_mutex);

            // Check if the resource is already exclusively locked
            auto it = g_path_to_lock.find(target_path);
            if (it != g_path_to_lock.end() && it->second.scope == "exclusive")
            {
                res.status = 423; // Locked
                res.set_content("Resource already exclusively locked.", "text/plain");
                return;
            }

            // Generate tracking tokens
            std::string raw_token = generate_uuid();
            std::string full_lock_token = "opaquelocktoken:" + raw_token;

            // Register the active lock state
            WebDavLock new_lock;
            new_lock.path = target_path;
            new_lock.token = full_lock_token;
            new_lock.type = "write";
            new_lock.scope = lock_scope;
            new_lock.owner = owner_info;
            new_lock.depth = 0; // Default

            g_path_to_lock[target_path] = new_lock;
            g_token_to_path[full_lock_token] = target_path;

            // Construct compliant WebDAV response XML payload
            std::string xml_res = build_lock_success_response(target_path, raw_token);

            res.set_header("Lock-Token", "<" + full_lock_token + ">");
            res.status = 200;
            res.set_content(xml_res, "text/xml; charset=utf-8");
        });

        svr->CustomRoute("UNLOCK", R"((.*))", [](const Request& req, Response& res) {
            if (!req.has_header("Lock-Token"))
            {
                res.status = 400;
                res.set_content("Missing 'Lock-Token' verification header.", "text/plain");
                return;
            }

            // Standard WebDAV headers wrap tokens in angle brackets, e.g., <opaquelocktoken:UUID>
            std::string raw_header_token = req.get_header_value("Lock-Token");
            std::string clean_token = raw_header_token;
            
            if (clean_token.front() == '<' && clean_token.back() == '>')
            {
                clean_token = clean_token.substr(1, clean_token.length() - 2);
            }

            std::lock_guard<std::mutex> guard(g_lock_mutex);

            // Validate if token exists and corresponds to this actual execution path
            auto token_it = g_token_to_path.find(clean_token);
            if (token_it == g_token_to_path.end() || token_it->second != req.path)
            {
                res.status = 412; // Precondition Failed
                res.set_content("Lock token does not match or resource is not locked.", "text/plain");
                return;
            }

            // Erase records to clear state tracking matrices completely
            std::string path_to_clear = token_it->second;
            g_path_to_lock.erase(path_to_clear);
            g_token_to_path.erase(token_it);

            res.status = 204; // No Content
        });

        svr->CustomRoute("PROPPATCH", R"((.*))", [&](const httplib::Request& req, httplib::Response& res)
        {
            auto actions = parse_proppatch(req.body);

            if (actions.empty()) {
                res.status = 400; // Malformed payload parsing structure state error output
                return;
            }

            // Output parsing debug validation checks directly to system server logs console
            for (const auto& act : actions)
            {
                std::cout << "  -> Action: " << (act.is_remove ? "REMOVE" : "SET")
                << " | Prefix: [" << act.ns_prefix << "] | Name: [" << act.name
                << "] | Value: [" << act.value << "]\n";
            }

            // Build atomic response execution sequence output maps payload target tracking fields structures
            std::string xml_res = build_proppatch_success_response(req.path, actions);
            res.status = 207; // Multi-Status
            res.set_content(xml_res, "text/xml; charset=utf-8");
        });

        svr->set_logger([](const Request &req, const Response &res)
        {
            dbglogger_log("%s", log(req, res).c_str());
        });

        // Support upto 500GB of file upload
        svr->set_payload_max_length(500ULL * 1024 * 1024 * 1024);
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
                Util::Notify("Starting WebDav Server %.2f on port %d", APP_VERSION, http_server_port);
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
