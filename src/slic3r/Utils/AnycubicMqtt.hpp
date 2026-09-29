#ifndef slic3r_AnycubicMqtt_hpp_
#define slic3r_AnycubicMqtt_hpp_

#include <string>
#include <vector>
#include <memory>

namespace Slic3r {

// One real ACE Pro tray, as reported live by the printer's own "multiColorBox"/"getInfo"
// status. index is 0-based, matching the printer's own numbering.
struct AceTray
{
    int         index = -1;
    std::string material_type;
    std::string sku;
    int         r = 255, g = 255, b = 255;
};

// Per-print options from Slicer Next's own "Start Print" dialog (Bed leveling / Resonance
// compensation / Time-lapse / Flow calibration). Defaults match that dialog's own default
// state.
struct PrintTaskOptions
{
    bool auto_leveling            = true;
    bool vibration_compensation   = false;
    bool timelapse                = true;
    bool flow_calibration         = true;
};

// Minimal, purpose-built client for what Kobra Slicer needs from the K3M's real local MQTT
// control channel: query the ACE Pro's real live tray data, and publish a "print/start"
// command. NOT a general MQTT library -- QoS 0 publish/subscribe only, no reconnect, no
// keepalive loop. See ANYCUBIC_INTEGRATION_NOTES.md for the real protocol this was
// reverse-engineered against.
//
// Keeps ONE MQTT connection open for the whole session (dialog through print/start) rather
// than a fresh connection per call, matching the real client's own confirmed behaviour.
class AnycubicMqttSession
{
public:
    AnycubicMqttSession();
    ~AnycubicMqttSession();
    AnycubicMqttSession(const AnycubicMqttSession &) = delete;
    AnycubicMqttSession &operator=(const AnycubicMqttSession &) = delete;

    // Opens the one persistent connection for this session (credential discovery + MQTT
    // CONNECT). Everything else below requires this to have succeeded first.
    bool connect(const std::string &printer_host, std::string &out_error);

    // Fires the real, read-only confirmation/telemetry burst (print/query,
    // calibration/getInfo, extfilbox/getInfo, extrudeControl/getInfo, properties/read,
    // buried/PrintStart, info/net, getSliceParam) -- sent only *after* print/start is
    // accepted, matching the real client. Fire-and-forget; a failure here doesn't abort
    // anything.
    void send_startup_queries(const std::string &filename, const std::string &source_file_path,
                               size_t filesize, const PrintTaskOptions &options);

    // The one thing the real client sends immediately before print/start: a single
    // lastWill/query. Fire-and-forget.
    void send_pre_print_check();

    // Queries the printer's real, live ACE Pro tray data on the already-open connection.
    // Returns true and populates out_trays (one entry per tray actually reported) on success;
    // false with out_error set if the printer never answers in time.
    bool query_trays(std::vector<AceTray> &out_trays, std::string &out_error);

    // Queries the printer's current task settings so the Bed leveling/Resonance
    // compensation/Time-lapse/Flow calibration checkboxes default to its real state, not app
    // constants. The response schema is inferred from symmetry with print/start's own
    // task_settings object (never directly captured); out_options keeps its caller-given
    // defaults and this returns false, non-fatally, if nothing usable arrives in time.
    bool query_task_settings(PrintTaskOptions &out_options, std::string &out_error);

    // Confirms the uploaded file is present on the printer's own local storage -- a
    // "fileDetails" query, called after print/start as confirmation, not before it as a gate
    // (the real client never checks first). Returns true once the printer answers with
    // details for this exact filename.
    bool verify_uploaded_file(const std::string &filename, std::string &out_error);

    // Sends the real print/start command on the same already-open connection. chosen_tray,
    // when non-null, is the user's own confirmed choice (from AnycubicAceTraySelectDialog) and
    // is used as-is -- no further guessing. When null, falls back to the uploaded file's own
    // embedded paint_info. Returns true on success.
    bool send_print_start(const std::string &uploaded_filename, const std::string &source_file_path,
                           const AceTray *chosen_tray, const PrintTaskOptions &options, std::string &out_error);

    // Sends a real MQTT DISCONNECT and closes cleanly. Safe to call more than once; also
    // called from the destructor if not called explicitly.
    void disconnect();

    // Reads the tray this file was actually sliced for straight out of its own embedded
    // gcode (the "; paint_info" line GCode.cpp writes) -- used to pre-select/gate the
    // confirmation dialog. Returns false if the file has no readable paint_info. Doesn't need
    // a connection -- reads the local file only.
    static bool read_expected_tray(const std::string &source_file_path,
                                    int &out_index_0based, std::string &out_material_type);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
