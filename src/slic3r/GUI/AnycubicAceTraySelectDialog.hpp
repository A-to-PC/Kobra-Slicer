#ifndef slic3r_GUI_AnycubicAceTraySelectDialog_hpp_
#define slic3r_GUI_AnycubicAceTraySelectDialog_hpp_

#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <vector>
#include <string>
#include <functional>

#include "slic3r/Utils/AnycubicMqtt.hpp"

namespace Slic3r { namespace GUI {

// The real, interactive confirmation step Jason found missing 28/09/2026, comparing this
// fork directly against Slicer Next's own "Start Print" dialog: a real print always shows the
// ACE Pro's actual live trays and lets the user pick/correct which one to use, since which
// spool is physically loaded in which bay can change between slicing and printing -- a slicer
// can never get this right by silently guessing. Deliberately small and purpose-built rather
// than reusing SelectMachineDialog (5,900+ lines, deeply tied to Bambu's own cloud device
// model that doesn't recognize Anycubic printers at all) -- see the kobra-slicer-project
// memory for that scoping decision.
class AceTraySwatch;

class AnycubicAceTraySelectDialog : public wxDialog
{
public:
    // trays: the real, live data from AnycubicMqttPrintStart::query_trays() (empty if that
    // query failed/timed out -- the dialog still shows, with a warning, letting the user
    // proceed with the file's own expected tray anyway rather than blocking them outright).
    // expected_index: the tray this file was actually sliced for (from its own paint_info),
    // pre-selected by default. expected_material: gates which trays are selectable, matching
    // Slicer Next's own "Only the same filament can be selected" real behaviour.
    // default_options: the printer's own real current task settings, queried via
    // AnycubicMqttSession::query_task_settings() before showing this dialog when possible; falls
    // back to PrintTaskOptions' own defaults if that query didn't get a usable answer in time.
    AnycubicAceTraySelectDialog(wxWindow *parent, const std::vector<AceTray> &trays,
                                 int expected_index, const std::string &expected_material,
                                 const PrintTaskOptions &default_options = PrintTaskOptions());

    // -1 if the dialog was cancelled.
    int GetSelectedTrayIndex() const { return m_selected_index; }

    // Per-print, user-toggleable checkboxes matching Slicer Next's own "Start Print" dialog,
    // mapping to print/start's task_settings.auto_leveling / vibration_compensation /
    // timelapse.status / flow_calibration. Defaults: leveling+timelapse+flow_cal on,
    // resonance off.
    bool GetAutoLeveling() const;
    bool GetVibrationCompensation() const;
    bool GetTimelapse() const;
    bool GetFlowCalibration() const;

private:
    void on_swatch_clicked(int index);
    void update_start_button();

    std::vector<AceTraySwatch *> m_swatches;
    int                          m_selected_index;
    std::string                  m_expected_material;
    wxButton                    *m_start_btn = nullptr;
    wxCheckBox                  *m_chk_leveling = nullptr;
    wxCheckBox                  *m_chk_resonance = nullptr;
    wxCheckBox                  *m_chk_timelapse = nullptr;
    wxCheckBox                  *m_chk_flow_cal = nullptr;
};

}} // namespace Slic3r::GUI

#endif
