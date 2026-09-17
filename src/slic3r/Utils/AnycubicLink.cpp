#include "AnycubicLink.hpp"

#include <sstream>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/filesystem.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

namespace {
    // Real, confirmed port -- the same local service the printer's own /info, /ctrl (MQTT
    // credential handshake, see LanCredentialDiscovery in Kobra LAN Monitor) and gcode
    // upload all live on.
    constexpr int ANYCUBIC_LINK_PORT = 18910;
}

AnycubicLink::AnycubicLink(DynamicPrintConfig *config) :
    m_host(config->opt_string("print_host"))
{}

const char* AnycubicLink::get_name() const { return "AnycubicLink"; }

std::string AnycubicLink::make_info_url() const
{
    // m_host is expected to be a bare IP/hostname (as entered in Physical Printer settings),
    // matching how Kobra LAN Monitor's own printer settings store it.
    std::string host = m_host;
    if (host.find("http://") == 0)
        host = host.substr(7);
    else if (host.find("https://") == 0)
        host = host.substr(8);
    // Strip a trailing slash or any path the user may have typed.
    auto slash = host.find('/');
    if (slash != std::string::npos)
        host = host.substr(0, slash);
    auto colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);

    return (boost::format("http://%1%:%2%/info") % host % ANYCUBIC_LINK_PORT).str();
}

bool AnycubicLink::fetch_upload_url(std::string &out_upload_url, std::string &out_model_name, wxString &err_msg) const
{
    const char* name = get_name();
    bool        res  = true;
    auto        url  = make_info_url();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Fetching %2%") % name % url;

    auto http = Http::get(std::move(url));
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting /info: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res     = false;
            err_msg = format_error(body, error, status);
        })
        .on_complete([&](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got /info: %2%") % name % body;
            try {
                std::stringstream ss(body);
                pt::ptree         root;
                pt::read_json(ss, root);

                auto model_name = root.get_optional<std::string>("modelName");
                auto upload_url = root.get_optional<std::string>("fileUploadurl");
                if (!upload_url) {
                    res     = false;
                    err_msg = format_error(body, _u8L("Anycubic printer not detected (no fileUploadurl in /info response)"), 0);
                    return;
                }
                out_upload_url = *upload_url;
                out_model_name = model_name.value_or(std::string());
            } catch (const std::exception &ex) {
                res     = false;
                err_msg = format_error(body, ex.what(), 0);
            }
        })
        .perform_sync();

    return res;
}

bool AnycubicLink::test(wxString &curl_msg) const
{
    std::string upload_url, model_name;
    return fetch_upload_url(upload_url, model_name, curl_msg);
}

wxString AnycubicLink::get_test_ok_msg() const
{
    return _L("Connection to the Anycubic printer's local link is working correctly.");
}

wxString AnycubicLink::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Anycubic printer"), msg);
}

bool AnycubicLink::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    std::string upload_url, model_name;
    wxString    err_msg;
    if (!fetch_upload_url(upload_url, model_name, err_msg)) {
        error_fn(std::move(err_msg));
        return false;
    }

    const auto upload_filename = upload_data.upload_path.filename();
    const auto file_size       = fs::file_size(upload_data.source_path);

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% (%3% bytes) to %4%, filename: %5%")
        % name % upload_data.source_path % file_size % upload_url % upload_filename.string();

    bool res = true;
    auto http = Http::post(std::move(upload_url));
    http.header("X-File-Length", std::to_string(file_size));
    // Real, captured multipart form field names (16/09/2026) -- the server expects a plain
    // "filename" text field alongside the file, and the file itself under field name "gcode",
    // not "file". Confirmed from a real Slicer Next upload's actual multipart body, not assumed:
    // the earlier guess ("file", copied from OctoPrint's own implementation) meant the server's
    // form parser likely never found the file content at all, independent of anything about the
    // gcode content itself -- which is why every content-level fix made no difference.
    http.form_add("filename", upload_filename.string());
    http.form_add_file("gcode", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
            try {
                std::stringstream ss(body);
                pt::ptree         root;
                pt::read_json(ss, root);
                auto code = root.get_optional<int>("code");
                if (!code || *code != 200) {
                    res = false;
                    error_fn(format_error(body, _u8L("Anycubic printer rejected the upload"), status));
                    return;
                }
                if (auto gcode = root.get_child_optional("data")) {
                    auto stored_name = gcode->get_optional<std::string>("gcode");
                    if (stored_name)
                        info_fn(L"anycubic_stored_filename", boost::nowide::widen(*stored_name));
                }
            } catch (const std::exception &ex) {
                // A 200 with a body we can't parse still means the bytes made it there --
                // don't fail the upload over that, just log it.
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not parse upload response: %2%") % name % ex.what();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
        .perform_sync();

    return res;
}

}
