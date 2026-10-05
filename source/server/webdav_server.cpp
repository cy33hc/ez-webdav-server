#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <functional>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <tinyxml2.h>
#include "http/httplib.h"
#include "server/webdav_server.h"
#include "util.h"
// #include "dbglogger.h"

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

struct RequestedProp
{
    std::string ns_prefix;
    std::string name;
};

enum class PropFindMode
{
    AllProp,
    PropName,
    Prop
};

struct PropFindRequest
{
    PropFindMode mode = PropFindMode::AllProp;
    std::vector<RequestedProp> props;
};

struct WebDavLock
{
    std::string path;
    std::string token;
    std::string type;
    std::string scope;
    std::string owner;
    int depth;
    std::chrono::steady_clock::time_point expiry; // when this lock lapses
};

#ifndef LOCK_TIMEOUT_SECONDS
#define LOCK_TIMEOUT_SECONDS 3600
#endif

Server *svr;
int http_server_port = 8880;
static bool stop_server = false;
static bool in_rest_mode = false;
constexpr int DOWNLOAD_SEGMENTS = 4;

std::map<std::string, WebDavLock> g_path_to_lock;
std::map<std::string, std::string> g_token_to_path;
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
        
        std::ostringstream ss;
        ss << std::put_time(&gmt, "%a, %d %b %Y %H:%M:%S GMT");
        return ss.str();
    }

    std::string format_http_date(std::time_t tt)
    {
        std::tm gmt = *std::gmtime(&tt);
        std::ostringstream ss;
        ss << std::put_time(&gmt, "%a, %d %b %Y %H:%M:%S GMT");
        return ss.str();
    }

    std::string format_iso8601_date(fs::file_time_type file_time)
    {
        auto sct = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        std::time_t tt = std::chrono::system_clock::to_time_t(sct);
        std::tm gmt = *std::gmtime(&tt);

        std::ostringstream ss;
        ss << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }

    std::string compute_etag(const fs::path& local_path, bool /*is_dir*/)
    {
        struct stat st;
        if (::stat(local_path.c_str(), &st) != 0)
        {
            return "W/\"0-0\"";
        }

        unsigned long long dev   = static_cast<unsigned long long>(st.st_dev);
        unsigned long long ino   = static_cast<unsigned long long>(st.st_ino);
        unsigned long long size  = static_cast<unsigned long long>(st.st_size);
        long long          sec   = static_cast<long long>(st.st_mtim.tv_sec);
        long               nsec  = static_cast<long>(st.st_mtim.tv_nsec);

        std::ostringstream ss;
        ss << "W/\"" << std::hex
           << dev << '-' << ino << '-' << size << '-' << sec << '-' << nsec
           << '"';
        return ss.str();
    }

    std::string urlEncodePath(const std::string& value)
    {
        std::ostringstream escaped;
        escaped << std::hex << std::uppercase;

        for (char c : value)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || 
                c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            {
                escaped << c;
            }
            else
            {
                escaped << '%' << std::setw(2) << std::setfill('0') 
                        << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return escaped.str();
    }

    mode_t put_create_mode(const fs::path& p)
    {
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".bin" || ext == ".elf" || ext == ".prx" || ext == ".sprx" || ext == ".self")
        {
            return 0777;
        }
        return 0666;
    }

    std::string guess_content_type(const fs::path& local_path)
    {
        auto ext = local_path.extension();
        if (ext == ".txt") return "text/plain";
        if (ext == ".html" || ext == ".htm") return "text/html";
        if (ext == ".css") return "text/css";
        if (ext == ".js") return "application/javascript";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        return "application/octet-stream";
    }

    void purge_expired_locks()
    {
        auto now = std::chrono::steady_clock::now();
        for (auto it = g_path_to_lock.begin(); it != g_path_to_lock.end(); )
        {
            if (it->second.expiry <= now)
            {
                g_token_to_path.erase(it->second.token);
                it = g_path_to_lock.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    long lock_seconds_remaining(const WebDavLock& lk)
    {
        auto now = std::chrono::steady_clock::now();
        if (lk.expiry <= now) return 0;
        return static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(lk.expiry - now).count());
    }

    void build_lockdiscovery(tinyxml2::XMLElement* lockdiscovery, const std::string& fs_path)
    {
        tinyxml2::XMLDocument* doc = lockdiscovery->GetDocument();

        std::lock_guard<std::mutex> guard(g_lock_mutex);
        purge_expired_locks();
        auto it = g_path_to_lock.find(fs_path);
        if (it == g_path_to_lock.end())
        {
            return; // No active lock (or it just expired): leave empty.
        }

        const WebDavLock& lk = it->second;

        auto* activelock = doc->NewElement("D:activelock");
        lockdiscovery->InsertEndChild(activelock);

        auto* locktype = doc->NewElement("D:locktype");
        locktype->InsertEndChild(doc->NewElement("D:write"));
        activelock->InsertEndChild(locktype);

        auto* lockscope = doc->NewElement("D:lockscope");
        lockscope->InsertEndChild(doc->NewElement(lk.scope == "shared" ? "D:shared" : "D:exclusive"));
        activelock->InsertEndChild(lockscope);

        auto* depth = doc->NewElement("D:depth");
        depth->SetText(lk.depth);
        activelock->InsertEndChild(depth);

        if (!lk.owner.empty())
        {
            auto* owner = doc->NewElement("D:owner");
            owner->SetText(lk.owner.c_str());
            activelock->InsertEndChild(owner);
        }

        auto* timeout = doc->NewElement("D:timeout");
        timeout->SetText(("Second-" + std::to_string(lock_seconds_remaining(lk))).c_str());
        activelock->InsertEndChild(timeout);

        auto* locktoken = doc->NewElement("D:locktoken");
        auto* token_href = doc->NewElement("D:href");
        token_href->SetText(lk.token.c_str());
        locktoken->InsertEndChild(token_href);
        activelock->InsertEndChild(locktoken);
    }

    struct ResolvedProp
    {
        std::string name; // e.g. "D:getetag"
        std::function<void(tinyxml2::XMLElement*)> fill;
    };

    ResolvedProp text_prop(std::string name, std::string value)
    {
        return ResolvedProp{std::move(name), [v = std::move(value)](tinyxml2::XMLElement* el)
        {
            el->SetText(v.c_str());
        }};
    }

    std::vector<ResolvedProp> collect_live_properties(const std::string& href_path, const fs::path& local_path, bool is_dir)
    {
        std::error_code ec;
        std::vector<ResolvedProp> props;

        std::string filename = local_path.filename().string();
        if (filename.empty())
        {
            std::string h = href_path;
            if (h.size() > 1 && h.back() == '/') h.pop_back();
            size_t slash = h.find_last_of('/');
            filename = (slash == std::string::npos) ? h : h.substr(slash + 1);
        }
        props.push_back(text_prop("D:displayname", filename));

        if (is_dir)
        {
            props.push_back(ResolvedProp{"D:resourcetype", [](tinyxml2::XMLElement* el)
            {
                el->InsertEndChild(el->GetDocument()->NewElement("D:collection"));
            }});
            props.push_back(text_prop("D:getcontenttype", "httpd/unix-directory"));
        }
        else
        {
            props.push_back(ResolvedProp{"D:resourcetype", nullptr});

            uintmax_t size = fs::file_size(local_path, ec);
            props.push_back(text_prop("D:getcontentlength", std::to_string(!ec ? size : 0)));
            props.push_back(text_prop("D:getcontenttype", guess_content_type(local_path)));
        }

        auto write_time = fs::last_write_time(local_path, ec);
        if (!ec)
        {
            props.push_back(text_prop("D:getlastmodified", format_http_date(write_time)));
            props.push_back(text_prop("D:creationdate", format_iso8601_date(write_time)));
        }

        props.push_back(text_prop("D:getetag", compute_etag(local_path, is_dir)));

        props.push_back(ResolvedProp{"D:supportedlock", [](tinyxml2::XMLElement* el)
        {
            tinyxml2::XMLDocument* doc = el->GetDocument();
            auto add_entry = [&](const char* scope_tag)
            {
                auto* entry = doc->NewElement("D:lockentry");
                auto* scope = doc->NewElement("D:lockscope");
                scope->InsertEndChild(doc->NewElement(scope_tag));
                entry->InsertEndChild(scope);
                auto* type = doc->NewElement("D:locktype");
                type->InsertEndChild(doc->NewElement("D:write"));
                entry->InsertEndChild(type);
                el->InsertEndChild(entry);
            };
            add_entry("D:exclusive");
            add_entry("D:shared");
        }});

        std::string fs_path = local_path.string();
        props.push_back(ResolvedProp{"D:lockdiscovery", [fs_path](tinyxml2::XMLElement* el)
        {
            build_lockdiscovery(el, fs_path);
        }});

        return props;
    }

    void fill_prop_element(tinyxml2::XMLElement* prop, const std::vector<ResolvedProp>& props, bool name_only)
    {
        tinyxml2::XMLDocument* doc = prop->GetDocument();
        for (const auto& p : props)
        {
            auto* el = doc->NewElement(p.name.c_str());
            if (!name_only && p.fill)
            {
                p.fill(el);
            }
            prop->InsertEndChild(el);
        }
    }

    void append_resource_xml(tinyxml2::XMLElement* multistatus, const std::string& href_path, const fs::path& local_path, const PropFindRequest& request)
    {
        tinyxml2::XMLDocument* doc = multistatus->GetDocument();
        std::error_code ec;
        bool is_dir = fs::is_directory(local_path, ec);

        auto* response = doc->NewElement("D:response");
        multistatus->InsertEndChild(response);

        std::string href = urlEncodePath(href_path);
        if (is_dir && !href_path.empty() && href_path.back() != '/')
        {
            href += "/";
        }
        auto* href_el = doc->NewElement("D:href");
        href_el->SetText(href.c_str());
        response->InsertEndChild(href_el);

        if (ec)
        {
            auto* propstat = doc->NewElement("D:propstat");
            propstat->InsertEndChild(doc->NewElement("D:prop"));
            auto* status = doc->NewElement("D:status");
            status->SetText("HTTP/1.1 404 Not Found");
            propstat->InsertEndChild(status);
            response->InsertEndChild(propstat);
            return;
        }

        std::vector<ResolvedProp> available = collect_live_properties(href_path, local_path, is_dir);

        if (request.mode == PropFindMode::Prop)
        {
            std::vector<ResolvedProp> found;
            std::vector<std::string> missing;

            for (const auto& want : request.props)
            {
                const ResolvedProp* match = nullptr;
                for (const auto& have : available)
                {
                    std::string have_local = have.name;
                    size_t colon = have_local.find(':');
                    if (colon != std::string::npos) have_local = have_local.substr(colon + 1);

                    if (have_local == want.name) { match = &have; break; }
                }
                if (match) found.push_back(*match);
                else missing.push_back(want.name);
            }

            if (!found.empty())
            {
                auto* propstat = doc->NewElement("D:propstat");
                auto* prop = doc->NewElement("D:prop");
                fill_prop_element(prop, found, false);
                propstat->InsertEndChild(prop);
                auto* status = doc->NewElement("D:status");
                status->SetText("HTTP/1.1 200 OK");
                propstat->InsertEndChild(status);
                response->InsertEndChild(propstat);
            }

            if (!missing.empty())
            {
                auto* propstat = doc->NewElement("D:propstat");
                auto* prop = doc->NewElement("D:prop");
                for (const auto& name : missing)
                {
                    // Preserve the client's requested prefix if any; default to D:.
                    std::string tag = (name.find(':') != std::string::npos) ? name : ("D:" + name);
                    prop->InsertEndChild(doc->NewElement(tag.c_str()));
                }
                propstat->InsertEndChild(prop);
                auto* status = doc->NewElement("D:status");
                status->SetText("HTTP/1.1 404 Not Found");
                propstat->InsertEndChild(status);
                response->InsertEndChild(propstat);
            }
        }
        else
        {
            bool name_only = (request.mode == PropFindMode::PropName);
            auto* propstat = doc->NewElement("D:propstat");
            auto* prop = doc->NewElement("D:prop");
            fill_prop_element(prop, available, name_only);
            propstat->InsertEndChild(prop);
            auto* status = doc->NewElement("D:status");
            status->SetText("HTTP/1.1 200 OK");
            propstat->InsertEndChild(status);
            response->InsertEndChild(propstat);
        }
    }

    void append_recursive_contents(tinyxml2::XMLElement* multistatus, const std::string& parent_href, const fs::path& local_path, const PropFindRequest& request)
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec))
        {
            std::string base_href = parent_href;
            if (base_href.back() != '/')
                base_href += "/";
            std::string child_href = base_href + entry.path().filename().string();

            append_resource_xml(multistatus, child_href, entry.path(), request);

            if (entry.is_directory(ec) && !ec)
            {
                append_recursive_contents(multistatus, child_href, entry.path(), request);
            }
        }
    }

    std::string get_mime_type(const std::string& path)
    {
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

    void split_qualified_name(const std::string& full_name, std::string& ns_prefix, std::string& local_name)
    {
        size_t colon_pos = full_name.find(':');
        if (colon_pos != std::string::npos)
        {
            ns_prefix = full_name.substr(0, colon_pos);
            local_name = full_name.substr(colon_pos + 1);
        }
        else
        {
            ns_prefix = "";
            local_name = full_name;
        }
    }

    PropFindRequest parse_propfind(const std::string& xml_body)
    {
        PropFindRequest request;
        request.mode = PropFindMode::AllProp;

        if (xml_body.empty())
        {
            return request;
        }

        tinyxml2::XMLDocument doc;
        if (doc.Parse(xml_body.c_str()) != tinyxml2::XML_SUCCESS)
        {
            return request; // Fall back to allprop on malformed XML.
        }

        auto* root = doc.FirstChildElement(); // <D:propfind>
        if (!root)
        {
            return request;
        }

        for (auto* child = root->FirstChildElement(); child != nullptr; child = child->NextSiblingElement())
        {
            std::string ns_prefix, local_name;
            split_qualified_name(child->Value(), ns_prefix, local_name);

            if (local_name == "propname")
            {
                request.mode = PropFindMode::PropName;
                return request;
            }

            if (local_name == "allprop")
            {
                request.mode = PropFindMode::AllProp;
                return request;
            }

            if (local_name == "prop")
            {
                request.mode = PropFindMode::Prop;
                for (auto* p = child->FirstChildElement(); p != nullptr; p = p->NextSiblingElement())
                {
                    RequestedProp rp;
                    split_qualified_name(p->Value(), rp.ns_prefix, rp.name);
                    request.props.push_back(rp);
                }
                return request;
            }
        }

        return request;
    }

    std::string local_name_of(const tinyxml2::XMLElement* el)
    {
        std::string ns, local;
        split_qualified_name(el->Value(), ns, local);
        return local;
    }

    struct LockInfo
    {
        std::string scope = "exclusive"; // "exclusive" or "shared"
        std::string owner;               // optional owner text
    };

    LockInfo parse_lockinfo(const std::string& xml_body)
    {
        LockInfo info;
        if (xml_body.empty()) return info;

        tinyxml2::XMLDocument doc;
        if (doc.Parse(xml_body.c_str()) != tinyxml2::XML_SUCCESS)
        {
            return info;
        }

        auto* root = doc.FirstChildElement(); // <D:lockinfo>
        if (!root) return info;

        for (auto* node = root->FirstChildElement(); node != nullptr; node = node->NextSiblingElement())
        {
            std::string name = local_name_of(node);

            if (name == "lockscope")
            {
                // The scope's single child element names the scope type.
                if (auto* scope_child = node->FirstChildElement())
                {
                    if (local_name_of(scope_child) == "shared") info.scope = "shared";
                    else info.scope = "exclusive";
                }
            }
            else if (name == "owner")
            {
                // Owner may be plain text or contain an <href>; capture whichever
                // text is available.
                if (const char* txt = node->GetText())
                {
                    info.owner = txt;
                }
                else if (auto* href = node->FirstChildElement())
                {
                    if (const char* htxt = href->GetText()) info.owner = htxt;
                }
            }
            // <locktype> is always write for this server; nothing to extract.
        }

        return info;
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

    std::string print_xml(const tinyxml2::XMLDocument& doc)
    {
        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        return std::string(printer.CStr());
    }

    std::string build_proppatch_success_response(const std::string& href, const std::vector<PropAction>& actions)
    {
        tinyxml2::XMLDocument doc;
        doc.InsertFirstChild(doc.NewDeclaration());

        auto* multistatus = doc.NewElement("D:multistatus");
        multistatus->SetAttribute("xmlns:D", "DAV:");
        doc.InsertEndChild(multistatus);

        auto* response = doc.NewElement("D:response");
        multistatus->InsertEndChild(response);

        auto* href_el = doc.NewElement("D:href");
        href_el->SetText(urlEncodePath(href).c_str());
        response->InsertEndChild(href_el);

        auto* propstat = doc.NewElement("D:propstat");
        response->InsertEndChild(propstat);

        auto* prop = doc.NewElement("D:prop");
        propstat->InsertEndChild(prop);

        for (const auto& action : actions)
        {
            std::string tag_name = action.ns_prefix.empty() ? action.name : (action.ns_prefix + ":" + action.name);
            prop->InsertEndChild(doc.NewElement(tag_name.c_str()));
        }

        auto* status = doc.NewElement("D:status");
        status->SetText("HTTP/1.1 200 OK");
        propstat->InsertEndChild(status);

        return print_xml(doc);
    }

    std::string build_lock_success_response(const std::string& href, const std::string& token,
                                            const std::string& scope, long timeout_seconds)
    {
        tinyxml2::XMLDocument doc;
        doc.InsertFirstChild(doc.NewDeclaration());

        auto* prop = doc.NewElement("D:prop");
        prop->SetAttribute("xmlns:D", "DAV:");
        doc.InsertEndChild(prop);

        auto* lockdiscovery = doc.NewElement("D:lockdiscovery");
        prop->InsertEndChild(lockdiscovery);

        auto* activelock = doc.NewElement("D:activelock");
        lockdiscovery->InsertEndChild(activelock);

        auto* locktype = doc.NewElement("D:locktype");
        locktype->InsertEndChild(doc.NewElement("D:write"));
        activelock->InsertEndChild(locktype);

        auto* lockscope = doc.NewElement("D:lockscope");
        lockscope->InsertEndChild(doc.NewElement(scope == "shared" ? "D:shared" : "D:exclusive"));
        activelock->InsertEndChild(lockscope);

        auto* depth = doc.NewElement("D:depth");
        depth->SetText("0");
        activelock->InsertEndChild(depth);

        auto* timeout = doc.NewElement("D:timeout");
        timeout->SetText(("Second-" + std::to_string(timeout_seconds)).c_str());
        activelock->InsertEndChild(timeout);

        std::string full_token_uri = "opaquelocktoken:" + token;
        auto* locktoken = doc.NewElement("D:locktoken");
        auto* token_href = doc.NewElement("D:href");
        token_href->SetText(urlEncodePath(full_token_uri).c_str());
        locktoken->InsertEndChild(token_href);
        activelock->InsertEndChild(locktoken);

        auto* lockroot = doc.NewElement("D:lockroot");
        auto* root_href = doc.NewElement("D:href");
        root_href->SetText(urlEncodePath(href).c_str());
        lockroot->InsertEndChild(root_href);
        activelock->InsertEndChild(lockroot);

        return print_xml(doc);
    }

    bool is_resource_locked(const std::string& path, const Request& req)
    {
        std::lock_guard<std::mutex> guard(g_lock_mutex);
        purge_expired_locks();

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
        std::ostringstream ss;
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
                    [](const fs::path& a, const fs::path& b)
                    {
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

    bool parse_content_range(const std::string& header, size_t& start, size_t& end, size_t& total)
    {
        std::regex regex(R"(bytes\s+(\d+)-(\d+)/(\d+|\*))");
        std::smatch match;
        if (std::regex_match(header, match, regex))
        {
            start = std::stoull(match[1].str());
            end = std::stoull(match[2].str());
            if (match[3].str() != "*")
            {
                total = std::stoull(match[3].str());
            }
            else
            {
                total = 0;
            }
            return true;
        }
        return false;
    }

    // Write the whole buffer, looping over short writes. Returns false on any
    // write() error (and leaves errno set by the failing call).
    bool write_all(int fd, const char* data, size_t length)
    {
        size_t written = 0;
        while (written < length)
        {
            ssize_t n = write(fd, data + written, length - written);
            if (n < 0)
            {
                if (errno == EINTR) continue; // retry interrupted syscall
                return false;
            }
            written += static_cast<size_t>(n);
        }
        return true;
    }

    void ServerThread()
    {
        svr->set_socket_options([](socket_t sock)
        {
            int send_buf_size = 2 * 1024 * 1024;
            
            if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0)
            {
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
                        reinterpret_cast<const char*>(&send_buf_size), 
                        sizeof(send_buf_size)) < 0)
            {
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
            snprintf(version, sizeof(version), "%.2f", static_cast<double>(APP_VERSION));
            res.set_content(version, "text/html");
        });

        svr->Options(R"((.*))", [&](const Request&, Response& res)
        {
            res.status = 200;
            res.set_header("Accept-Ranges", "bytes");
            res.set_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, COPY, MOVE, LOCK, UNLOCK");
            res.set_header("DAV", "1, 2"); // Signals class levels processing rules (Locks enabled)

            // simulate NextCloud to enable rclone streaming
            res.set_header("X-LFV", "1");
            res.set_header("OC-API-Version", "1.0");
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

                // Collect entries error-safely (a permission error on one
                // child shouldn't throw and abort the whole listing), then
                // sort: directories first, then case-insensitive by name.
                struct DirItem { fs::path path; std::string name; bool is_dir; };
                std::vector<DirItem> items;

                std::error_code dir_ec;
                for (fs::directory_iterator it(canonical_path, fs::directory_options::skip_permission_denied, dir_ec), end;
                     !dir_ec && it != end;
                     it.increment(dir_ec))
                {
                    std::error_code item_ec;
                    bool is_dir = it->is_directory(item_ec);
                    items.push_back({it->path(), it->path().filename().string(), !item_ec && is_dir});
                }

                std::sort(items.begin(), items.end(),
                    [](const DirItem& a, const DirItem& b)
                    {
                        if (a.is_dir != b.is_dir) return a.is_dir; // directories first
                        // case-insensitive name comparison
                        const std::string& x = a.name;
                        const std::string& y = b.name;
                        size_t n = std::min(x.size(), y.size());
                        for (size_t i = 0; i < n; ++i)
                        {
                            char cx = static_cast<char>(std::tolower(static_cast<unsigned char>(x[i])));
                            char cy = static_cast<char>(std::tolower(static_cast<unsigned char>(y[i])));
                            if (cx != cy) return cx < cy;
                        }
                        return x.size() < y.size();
                    });

                size_t id_counter = 0;
                for (const auto& entry : items)
                {
                    auto filename = entry.name;
                    std::string href = req.path;
                    if (!href.ends_with("/")) href += "/";
                    href += filename;
                    id_counter++;
                    std::string el_id = "file_" + std::to_string(id_counter);

                    if (entry.is_dir)
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

                // Determine the size with stat() rather than ifstream::tellg().
                // On PS5 the libc/filesystem reports an incorrect position for
                // an ate-opened stream, so tellg() yields a wrong size there;
                // st_size is accurate on both PS5 and Linux/WSL.
                struct stat file_stat;
                if (::stat(path_str.c_str(), &file_stat) != 0)
                {
                    res.status = 500;
                    return;
                }
                size_t file_size = static_cast<size_t>(file_stat.st_size);

                res.set_header("Accept-Ranges", "bytes");

                // Validators so clients can cache and issue conditional GETs.
                // Mirrors the ETag reported for this resource via PROPFIND.
                res.set_header("ETag", compute_etag(canonical_path, false));
                res.set_header("Last-Modified", format_http_date(static_cast<std::time_t>(file_stat.st_mtim.tv_sec)));

                std::string mime = get_mime_type(path_str);

                if (req.method == "HEAD")
                {
                    res.status = 200;
                    res.set_header("Content-Length", std::to_string(file_size));
                    res.set_header("Content-Type", mime);
                    return;
                }

                if (!req.has_header("Range"))
                {
                    res.status = 200;
                }

                res.set_content_provider(
                    file_size, mime,
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
            // Canonicalize the path the same way the GET handler does, so that
            // traversal/symlink handling is consistent across methods.
            std::error_code ec;
            fs::path local_path = fs::weakly_canonical(fs::path(req.path), ec);
            if (ec)
            {
                res.status = 403;
                res.set_content("Forbidden", "text/plain");
                return;
            }

            if (!fs::exists(local_path, ec))
            {
                res.status = 404;
                res.set_content("Not Found", "text/plain");
                return;
            }

            // Validate Depth. RFC 4918 defaults a missing Depth to "infinity".
            std::string depth = "infinity";
            if (req.has_header("Depth"))
            {
                depth = req.get_header_value("Depth");
            }
            if (depth != "0" && depth != "1" && depth != "infinity")
            {
                res.status = 400;
                res.set_content("Invalid Depth header.", "text/plain");
                return;
            }

            // Parse the request body to learn which properties the client wants.
            PropFindRequest request = parse_propfind(req.body);

            tinyxml2::XMLDocument doc;
            doc.InsertFirstChild(doc.NewDeclaration()); // <?xml version="1.0" encoding="UTF-8"?>

            auto* multistatus = doc.NewElement("D:multistatus");
            multistatus->SetAttribute("xmlns:D", "DAV:");
            doc.InsertEndChild(multistatus);

            append_resource_xml(multistatus, req.path, local_path, request);

            if (fs::is_directory(local_path, ec) && !ec)
            {
                if (depth == "1")
                {
                    for (const auto& entry : fs::directory_iterator(local_path, fs::directory_options::skip_permission_denied, ec))
                    {
                        std::string parent_href = req.path;
                        if (parent_href.back() != '/') parent_href += "/";
                        std::string child_href = parent_href + entry.path().filename().string();

                        append_resource_xml(multistatus, child_href, entry.path(), request);
                    }
                } 
                else if (depth == "infinity")
                {
                    append_recursive_contents(multistatus, req.path, local_path, request);
                }
            }

            res.status = 207; // Multi-Status
            res.set_content(print_xml(doc), "application/xml; charset=utf-8");
            res.set_header("DAV", "1, 2");
            res.set_header("Accept-Ranges", "bytes");

            // simulate NextCloud
            res.set_header("X-LFV", "1");
            res.set_header("OC-API-Version", "1.0");
        });

        svr->Put(R"((.*))", [&](const Request &req, Response &res, const ContentReader &content_reader)
        {
            fs::path target_path(req.path);

            // RFC 4918 9.7.1: PUT to a path whose parent collection does not
            // exist must fail with 409 Conflict (the client should MKCOL first).
            std::error_code pec;
            fs::path parent = target_path.parent_path();
            if (!parent.empty() && !fs::is_directory(parent, pec))
            {
                res.status = 409; // Conflict
                res.set_content("Parent collection does not exist.", "text/plain");
                return;
            }

            bool target_existed = fs::exists(target_path, pec);

            size_t range_start = 0, range_end = 0, total_file_size = 0;
            bool has_range = req.has_header("Content-Range");
            if (has_range)
            {
                parse_content_range(req.get_header_value("Content-Range"),
                                    range_start, range_end, total_file_size);
            }

            const size_t FLUSH_THRESHOLD = 1024 * 1024; // flush accumulator at 1 MB

            if (has_range)
            {
                // Partial/ranged PUT: write in place at the given offset. A
                // temp-file swap can't be used here because we're patching an
                // existing file rather than replacing it wholesale.
                int fd = open(req.path.c_str(), O_WRONLY | O_CREAT, 0666);
                if (fd < 0)
                {
                    res.status = (errno == ENOENT || errno == ENOTDIR) ? 409 : 500;
                    return;
                }
                // Enforce the extension-based mode on every PUT (create or
                // patch), so executable payloads (.elf/.self/.bin/.prx/.sprx)
                // always end up 0777 even when the file already existed.
                fchmod(fd, put_create_mode(target_path));
                if (range_start > 0 && lseek(fd, static_cast<off_t>(range_start), SEEK_SET) < 0)
                {
                    close(fd);
                    res.status = 500;
                    return;
                }

                std::vector<char> acc;
                acc.reserve(FLUSH_THRESHOLD);
                bool ok = content_reader([&](const char* data, size_t len)
                {
                    acc.insert(acc.end(), data, data + len);
                    if (acc.size() >= FLUSH_THRESHOLD)
                    {
                        if (!write_all(fd, acc.data(), acc.size())) return false;
                        acc.clear();
                    }
                    return true;
                });
                if (ok && !acc.empty()) ok = write_all(fd, acc.data(), acc.size());

                // Surface flush/close errors (e.g. ENOSPC) that write() may not report.
                if (ok && fsync(fd) != 0) ok = false;
                if (close(fd) != 0) ok = false;

                if (!ok)
                {
                    res.status = 500;
                    res.set_content("Write failed.", "text/plain");
                    return;
                }

                res.status = 206;
                res.set_content("Partial content written.", "text/plain");
                return;
            }

            // Full-body PUT: stream into a temp file in the same directory, then
            // atomically rename over the target. This truncates correctly on
            // overwrite, never corrupts an existing file on a failed upload, and
            // avoids leaving partial files behind.
            fs::path tmp_path = target_path;
            tmp_path += ".tmp-" + generate_uuid();

            int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0)
            {
                res.status = (errno == ENOENT || errno == ENOTDIR) ? 409 : 500;
                return;
            }
            // Set the final permission from the *target* extension (not the
            // temp name) with fchmod, so the mode is exact regardless of umask
            // and survives the rename below.
            fchmod(fd, put_create_mode(target_path));

            std::vector<char> acc;
            acc.reserve(FLUSH_THRESHOLD);
            bool ok = content_reader([&](const char* data, size_t len)
            {
                acc.insert(acc.end(), data, data + len);
                if (acc.size() >= FLUSH_THRESHOLD)
                {
                    if (!write_all(fd, acc.data(), acc.size())) return false;
                    acc.clear();
                }
                return true;
            });
            if (ok && !acc.empty()) ok = write_all(fd, acc.data(), acc.size());

            // fsync BEFORE rename so the data is durable; check both it and close
            // because disk-full/IO errors frequently surface only at flush time.
            if (ok && fsync(fd) != 0) ok = false;
            if (close(fd) != 0) ok = false;

            if (!ok)
            {
                std::error_code rm_ec;
                fs::remove(tmp_path, rm_ec); // don't leave a partial temp file
                res.status = 500;
                res.set_content("Write failed.", "text/plain");
                return;
            }

            std::error_code mv_ec;
            fs::rename(tmp_path, target_path, mv_ec);
            if (mv_ec)
            {
                std::error_code rm_ec;
                fs::remove(tmp_path, rm_ec);
                res.status = 500;
                res.set_content("Could not finalize upload.", "text/plain");
                return;
            }

            // 201 when a new resource was created, 204 when an existing one was
            // overwritten (RFC 4918 9.7.1).
            res.status = target_existed ? 204 : 201;
            if (!target_existed)
            {
                res.set_content("Created", "text/plain");
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
            
            /*
            if (is_resource_locked(req.path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }
            */

            if (!fs::exists(target))
            {
                res.status = 207;
                res.set_content(R"(
                    <D:multistatus xmlns:D="DAV:"><D:response><D:href>)" + urlEncodePath(target) +
                    R"(</D:href><D:status>HTTP/1.1 404 Not Found</D:status></D:response></D:multistatus>)", "application/xml; charset=\"utf-8\"");
                return;
            }

            if (auto failure = delete_recursive_strict(target))
            {
                res.status = 207;
                std::ostringstream ss;
                ss << R"(<D:multistatus xmlns:D="DAV:"><D:response><D:href>)" << urlEncodePath(failure->path) << R"(</D:href><D:status>HTTP/1.1 )";
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

            /*
            if (is_resource_locked(dest_path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }
            */

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

            try
            {
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
                }

                fs::copy(src_path, dest_path, options);

                res.status = dest_exists ? 204 : 201;
                res.set_header("Content-Length", "0");

            }
            catch (const fs::filesystem_error& e)
            {
                res.status = 500;
                res.set_content("Internal server error copying resource.", "text/plain");
            }
        });

        svr->CustomRoute("MOVE", R"((.*))", [&](const Request& req, Response& res)
        {
            if (!req.has_header("Destination"))
            {
                res.status = 400;
                res.set_content("Missing 'Destination' header.", "text/plain");
                return;
            }

            /*
            if (is_resource_locked(req.path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked.", "text/plain");
                return;
            }
            */

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
                        std::ostringstream ss;
                        ss << "Cross-device move fallback failed. Error: " << inner_ex.what();
                        res.set_content(ss.str(), "text/plain");
                        return;
                    }
                }

                res.status = 500;
                res.set_content("Internal server error moving file resource.", "text/plain");
            }
        });

        svr->CustomRoute("LOCK", R"((.*))", [](const Request& req, Response& res)
        {
            std::string target_path = req.path;

            LockInfo lock_info = parse_lockinfo(req.body);
            std::string lock_scope = lock_info.scope;
            std::string owner_info = lock_info.owner;

            std::lock_guard<std::mutex> guard(g_lock_mutex);
            purge_expired_locks(); // drop any lapsed locks before deciding

            auto now = std::chrono::steady_clock::now();
            auto new_expiry = now + std::chrono::seconds(LOCK_TIMEOUT_SECONDS);

            auto it = g_path_to_lock.find(target_path);
            if (it != g_path_to_lock.end())
            {
                bool token_matches = false;
                if (req.has_header("If"))
                {
                    token_matches = req.get_header_value("If").find(it->second.token) != std::string::npos;
                }

                if (token_matches)
                {
                    it->second.expiry = new_expiry;

                    std::string raw = it->second.token;
                    const std::string prefix = "opaquelocktoken:";
                    if (raw.rfind(prefix, 0) == 0) raw = raw.substr(prefix.size());

                    std::string xml_res = build_lock_success_response(
                        target_path, raw, it->second.scope, lock_seconds_remaining(it->second));
                    res.set_header("Lock-Token", "<" + it->second.token + ">");
                    res.status = 200;
                    res.set_content(xml_res, "text/xml; charset=utf-8");
                    return;
                }

                if (it->second.scope == "exclusive" || lock_scope == "exclusive")
                {
                    res.status = 423; // Locked
                    res.set_content("Resource already locked.", "text/plain");
                    return;
                }
            }

            std::string raw_token = generate_uuid();
            std::string full_lock_token = "opaquelocktoken:" + raw_token;

            WebDavLock new_lock;
            new_lock.path = target_path;
            new_lock.token = full_lock_token;
            new_lock.type = "write";
            new_lock.scope = lock_scope;
            new_lock.owner = owner_info;
            new_lock.depth = 0; // Default
            new_lock.expiry = new_expiry;

            g_path_to_lock[target_path] = new_lock;
            g_token_to_path[full_lock_token] = target_path;

            std::string xml_res = build_lock_success_response(
                target_path, raw_token, lock_scope, LOCK_TIMEOUT_SECONDS);

            res.set_header("Lock-Token", "<" + full_lock_token + ">");
            res.status = 200;
            res.set_content(xml_res, "text/xml; charset=utf-8");
        });

        svr->CustomRoute("UNLOCK", R"((.*))", [](const Request& req, Response& res)
        {
            if (!req.has_header("Lock-Token"))
            {
                res.status = 400;
                res.set_content("Missing 'Lock-Token' verification header.", "text/plain");
                return;
            }

            std::string clean_token = req.get_header_value("Lock-Token");

            if (clean_token.size() >= 2 && clean_token.front() == '<' && clean_token.back() == '>')
            {
                clean_token = clean_token.substr(1, clean_token.length() - 2);
            }

            std::lock_guard<std::mutex> guard(g_lock_mutex);
            purge_expired_locks();

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

        svr->CustomRoute("PROPPATCH", R"((.*))", [](const Request& req, Response& res)
        {
            auto actions = parse_proppatch(req.body);
            if (actions.empty())
            {
                res.status = 400; // Bad Request if XML is malformed
                res.set_content("Malformed or empty XML propertyupdate body.", "text/plain");
                return;
            }

            /*
            if (is_resource_locked(req.path, req))
            {
                res.status = 423; // Locked
                res.set_content("Resource is locked. Metadata alterations rejected.", "text/plain");
                return;
            }
            */

            // Do nothing with the change and just return success

            std::string xml_res = build_proppatch_success_response(req.path, actions);

            res.status = 207; // Multi-Status
            res.set_content(xml_res, "text/xml; charset=utf-8");
        });

        /*
        svr->set_logger([](const Request &req, const Response &res)
        {
            dbglogger_log("%s", log(req, res).c_str());
        });
        */

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
        std::string base_url = "http://127.0.0.1:" + std::to_string(http_server_port);
        Client client = Client(base_url);
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
