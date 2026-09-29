#ifndef slic3r_AnycubicLink_hpp_
#define slic3r_AnycubicLink_hpp_

#include <string>
#include <memory>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "AnycubicMqtt.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Talks to the Anycubic Kobra 3 Max's own local HTTP service (port 18910), the same
// service Anycubic Slicer Next itself uses -- confirmed 15/09/2026 by capturing a real
// Upload-and-Print from a live Slicer Next session against the real printer (see
// kobra-slicer-project memory for the full writeup). Two real endpoints on that service:
//   GET  http://<host>:18910/info          -> JSON including a ready-to-use "fileUploadurl"
//   POST <that fileUploadurl>              -> multipart/form-data file upload
// The upload URL's "s=" token is handed out by /info itself -- there is no signing scheme
// to replicate, the printer just tells the client where to POST.
//
// Starting the print after upload goes over the printer's real MQTT control channel, whose
// topic and payload shape were captured from a genuine Slicer Next print on 27/09/2026 (see
// AnycubicMqtt.hpp/.cpp and the kobra-slicer-project memory for the full capture, including
// four real bugs found and fixed the same day: a crash on a bodyless HTTP request, every
// number/bool being sent as a quoted JSON string, a missing package file the real firmware
// needs, and a raw socket close that looked like a crash to the printer). Confirmed live,
// real printer, real first layer -- but via a standalone script reusing this same protocol
// logic against a real Slicer-Next-produced file, not yet through this class's own upload()
// path with a file this fork itself produced. That's the next real test.
class AnycubicLink : public PrintHost
{
public:
    AnycubicLink(DynamicPrintConfig *config);
    ~AnycubicLink() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

    // Lets AnycubicPrintHostSendDialog::init() hand off its still-open MQTT session here
    // instead of disconnecting it, so upload() keeps using the same connection rather than
    // opening a second one -- matches the real client, which keeps one connection open for
    // the whole session.
    void attach_session(std::unique_ptr<AnycubicMqttSession> session) const { m_pending_session = std::move(session); }

protected:
    std::string m_host;
    mutable std::unique_ptr<AnycubicMqttSession> m_pending_session;

    // GET <host>:18910/info and pull out the two fields we need. Returns false (with a
    // populated error message) if the printer didn't respond with something that looks
    // like a real Anycubic /info response.
    bool fetch_upload_url(std::string &out_upload_url, std::string &out_model_name, wxString &err_msg) const;
    std::string make_info_url() const;
};

}

#endif
