#include "AnycubicLink.hpp"

#include <sstream>
#include <future>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/filesystem.hpp>
#include <wx/app.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/AnycubicAceTraySelectDialog.hpp"
#include "Http.hpp"
#include "AnycubicMqtt.hpp"
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

    // The real client's MQTT session exists before/during the upload, not opened fresh
    // afterward -- opened here, before the HTTP upload starts, to match.
    std::unique_ptr<AnycubicMqttSession> mqtt_session;
    const AceTray   *chosen_tray = nullptr;
    AceTray          chosen_tray_storage; // chosen_tray points here when set -- a plain local,
                                           // not static, so this is safe across concurrent/
                                           // repeated calls (this method runs on a per-upload
                                           // background thread, never shared across calls).
    PrintTaskOptions chosen_options;
    const bool       start_print_requested = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    // An MQTT session is opened for every upload, not just when the user asked to print
    // immediately. Without it, the printer's own display hangs at "handshake" after an
    // upload-only transfer -- the firmware waits for this connection/acknowledgment
    // regardless of whether a print is about to start. The file still finishes uploading via
    // plain HTTP either way; this only affects whether the printer's UI settles afterward.
    {
        // Reuses the same connection AnycubicPrintHostSendDialog already opened and handed off
        // via attach_session(), rather than opening a second one. m_pending_session is only
        // ever set by that dialog, on the main thread, before this background-thread method
        // starts -- no concurrent access.
        if (m_pending_session) {
            mqtt_session = std::move(m_pending_session);
        } else {
            mqtt_session.reset(new AnycubicMqttSession());
            std::string connect_err;
            if (!mqtt_session->connect(m_host, connect_err)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: could not connect before upload: %2%") % name % connect_err;
                if (start_print_requested) {
                    error_fn(GUI::format_wxstr("%s: %s", _L("Could not reach the printer to start the print"), connect_err));
                    return false;
                }
                // Upload-only: a failed handshake connection isn't fatal to the file transfer
                // itself, just log it and continue without a session.
                mqtt_session.reset();
            }
        }
    }
    if (start_print_requested && mqtt_session) {
        // Tray + options confirmation happens in AnycubicPrintHostSendDialog -- the same
        // single dialog the user clicked "Upload and Print" on. Choices arrive via
        // extended_info (the same mechanism ElegooPrintHostSendDialog/CrealityPrintHostSendDialog
        // use for their own per-vendor options); falls back to the file's own embedded
        // paint_info if that key is missing.
        auto find_info = [&](const char *key) -> std::string {
            auto it = upload_data.extended_info.find(key);
            return it != upload_data.extended_info.end() ? it->second : std::string();
        };
        std::string tray_index_str = find_info("anycubic_tray_index");
        if (!tray_index_str.empty()) {
            chosen_tray_storage.index         = std::atoi(tray_index_str.c_str());
            chosen_tray_storage.r             = std::atoi(find_info("anycubic_tray_r").c_str());
            chosen_tray_storage.g             = std::atoi(find_info("anycubic_tray_g").c_str());
            chosen_tray_storage.b             = std::atoi(find_info("anycubic_tray_b").c_str());
            chosen_tray_storage.material_type = find_info("anycubic_tray_material");
            if (chosen_tray_storage.index >= 0)
                chosen_tray = &chosen_tray_storage;
            chosen_options.auto_leveling          = find_info("anycubic_auto_leveling") == "1";
            chosen_options.vibration_compensation = find_info("anycubic_vibration_compensation") == "1";
            chosen_options.timelapse              = find_info("anycubic_timelapse") == "1";
            chosen_options.flow_calibration       = find_info("anycubic_flow_calibration") == "1";
        } else {
            BOOST_LOG_TRIVIAL(warning) << name << ": no tray choice in extended_info -- falling back to the file's own embedded paint_info";
        }
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% (%3% bytes) to %4%, filename: %5%")
        % name % upload_data.source_path % file_size % upload_url % upload_filename.string();

    bool res = true;
    auto http = Http::post(std::move(upload_url));
    http.header("X-File-Length", std::to_string(file_size));
    // Real multipart field names: a plain "filename" text field alongside the file, file
    // itself under field name "gcode" (not "file").
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

    if (res && mqtt_session && !start_print_requested) {
        // Upload-only: send the same handshake query print/start would have sent first, so
        // the printer's display clears the "handshake" state, then disconnect without ever
        // calling print/start. File stays uploaded and available to start manually.
        mqtt_session->send_pre_print_check();
        mqtt_session->disconnect();
    } else if (res && mqtt_session) {
        // Real sequence: a single lastWill/query, then print/start almost immediately. The
        // full confirmation burst (print/query, calibration/getInfo, etc.) and fileDetails
        // both fire only *after* the printer accepts the print (state "auto_leveling"), as
        // post-acceptance telemetry -- not as a pre-flight gate. See
        // ANYCUBIC_INTEGRATION_NOTES.md for the real capture this was confirmed against.
        mqtt_session->send_pre_print_check();

        std::string mqtt_err;
        bool sent = mqtt_session->send_print_start(upload_filename.string(), upload_data.source_path.string(), chosen_tray, chosen_options, mqtt_err);
        if (!sent) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: print/start failed: %2%") % name % mqtt_err;
            mqtt_session->disconnect();
            error_fn(GUI::format_wxstr("%s: %s", _L("Uploaded, but could not start the print"), mqtt_err));
            return false;
        }

        // Only sent once print/start is actually accepted.
        mqtt_session->send_startup_queries(upload_filename.string(), upload_data.source_path.string(), file_size, chosen_options, model_name);

        std::string verify_err;
        if (!mqtt_session->verify_uploaded_file(upload_filename.string(), verify_err)) {
            // Non-fatal, matching real Slicer Next: this is post-acceptance confirmation, not a
            // gate -- the printer already accepted and started the print by this point.
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: post-start file verification did not confirm (non-fatal): %2%") % name % verify_err;
        }
        mqtt_session->disconnect();
    } else if (mqtt_session) {
        mqtt_session->disconnect(); // upload itself failed -- don't leave the session dangling
    }

    return res;
}

}
