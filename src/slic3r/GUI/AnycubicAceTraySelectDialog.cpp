#include "AnycubicAceTraySelectDialog.hpp"

#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <boost/format.hpp>
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

namespace {
constexpr int SWATCH_SIZE = 64;
}

// A single clickable, coloured circle representing one real ACE Pro tray -- deliberately
// simple custom drawing rather than reusing AMSLib (see the header comment for why).
class AceTraySwatch : public wxPanel
{
public:
    AceTraySwatch(wxWindow *parent, int index, const wxString &label, wxColour colour, bool enabled)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(SWATCH_SIZE + 20, SWATCH_SIZE + 40))
        , m_index(index), m_label(label), m_colour(colour), m_enabled(enabled)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &AceTraySwatch::on_paint, this);
        if (m_enabled) {
            Bind(wxEVT_LEFT_DOWN, &AceTraySwatch::on_click, this);
            SetCursor(wxCursor(wxCURSOR_HAND));
        }
    }

    void SetSelected(bool sel) { m_selected = sel; Refresh(); }
    int  GetIndex() const { return m_index; }

    std::function<void(int)> on_selected;

private:
    void on_click(wxMouseEvent &)
    {
        if (on_selected) on_selected(m_index);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.Clear();
        wxSize sz = GetSize();
        int cx = sz.x / 2;
        int cy = SWATCH_SIZE / 2 + 4;
        int r  = SWATCH_SIZE / 2;

        dc.SetBrush(wxBrush(m_enabled ? m_colour : wxColour(200, 200, 200)));
        dc.SetPen(m_selected ? wxPen(wxColour(0, 150, 136), 3) : wxPen(wxColour(160, 160, 160), 1));
        dc.DrawCircle(cx, cy, r);

        dc.SetTextForeground(m_enabled ? wxColour(30, 30, 30) : wxColour(160, 160, 160));
        wxString text = m_label;
        wxSize text_sz = dc.GetTextExtent(text);
        dc.DrawText(text, cx - text_sz.x / 2, SWATCH_SIZE + 10);
    }

    int      m_index;
    wxString m_label;
    wxColour m_colour;
    bool     m_enabled;
    bool     m_selected = false;
};

AnycubicAceTraySelectDialog::AnycubicAceTraySelectDialog(wxWindow *parent, const std::vector<AceTray> &trays,
                                                           int expected_index, const std::string &expected_material,
                                                           const PrintTaskOptions &default_options)
    : wxDialog(parent, wxID_ANY, "Confirm ACE Pro tray", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_selected_index(expected_index)
    , m_expected_material(expected_material)
{
    auto *root = new wxBoxSizer(wxVERTICAL);

    wxString hint = trays.empty()
        ? _("Could not reach the ACE Pro's live tray data -- printing with the tray this file "
            "was sliced for. Check the printer is on and reachable, or pick a tray below if it "
            "appears once the page refreshes.")
        : _("Confirm which ACE Pro tray to print from -- only trays with the same filament type "
            "as this file can be selected.");
    auto *hint_text = new wxStaticText(this, wxID_ANY, hint, wxDefaultPosition, wxSize(420, -1));
    hint_text->Wrap(420);
    root->Add(hint_text, 0, wxALL, 12);

    auto *swatch_sizer = new wxBoxSizer(wxHORIZONTAL);
    // ACE Pro units report 4 trays -- if the live query came back empty, still show 4 slots
    // so the user isn't stuck with zero options, just with no real data to label them.
    // Uses each tray's own .index as the swatch identity (not vector position), so a sparse
    // or out-of-order report can never resolve to the wrong tray.
    const int tray_count = trays.empty() ? 4 : static_cast<int>(trays.size());
    for (int i = 0; i < tray_count; ++i) {
        bool has_data = i < static_cast<int>(trays.size());
        const AceTray *t = has_data ? &trays[i] : nullptr;
        int real_index = t ? t->index : i;
        wxColour colour = t ? wxColour(t->r, t->g, t->b) : wxColour(220, 220, 220);
        wxString label  = t ? wxString::Format("%d: %s", real_index + 1, t->material_type.empty() ? "?" : t->material_type)
                             : wxString::Format("%d: ?", i + 1);
        bool enabled = !t || expected_material.empty() || t->material_type == expected_material;

        auto *swatch = new AceTraySwatch(this, real_index, label, colour, enabled);
        swatch->SetSelected(real_index == expected_index);
        swatch->on_selected = [this](int idx) { on_swatch_clicked(idx); };
        m_swatches.push_back(swatch);
        swatch_sizer->Add(swatch, 0, wxALL, 8);
    }
    root->Add(swatch_sizer, 0, wxALIGN_CENTER | wxALL, 8);

    // Matches Anycubic Slicer Next's own "Start Print" dialog: leveling/resonance on one
    // row, timelapse/flow calibration on the next.
    auto *opts_grid = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(16));
    m_chk_leveling  = new wxCheckBox(this, wxID_ANY, _("Bed leveling"));
    m_chk_resonance = new wxCheckBox(this, wxID_ANY, _("Resonance compensation"));
    m_chk_timelapse = new wxCheckBox(this, wxID_ANY, _("Time-lapse"));
    m_chk_flow_cal  = new wxCheckBox(this, wxID_ANY, _("Flow calibration"));
    m_chk_leveling->SetValue(default_options.auto_leveling);
    m_chk_resonance->SetValue(default_options.vibration_compensation);
    m_chk_timelapse->SetValue(default_options.timelapse);
    m_chk_flow_cal->SetValue(default_options.flow_calibration);
    opts_grid->Add(m_chk_leveling);
    opts_grid->Add(m_chk_resonance);
    opts_grid->Add(m_chk_timelapse);
    opts_grid->Add(m_chk_flow_cal);
    root->Add(opts_grid, 0, wxALIGN_CENTER | wxALL, 8);

    auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *cancel_btn = new wxButton(this, wxID_CANCEL, _("Cancel"));
    m_start_btn = new wxButton(this, wxID_OK, _("Start Print"));
    btn_sizer->Add(cancel_btn, 0, wxRIGHT, 8);
    btn_sizer->Add(m_start_btn, 0);
    root->Add(btn_sizer, 0, wxALIGN_RIGHT | wxALL, 12);

    SetSizerAndFit(root);
    CentreOnParent();
    update_start_button();
}

void AnycubicAceTraySelectDialog::on_swatch_clicked(int index)
{
    m_selected_index = index;
    for (auto *s : m_swatches)
        s->SetSelected(s->GetIndex() == index);
    update_start_button();
}

void AnycubicAceTraySelectDialog::update_start_button()
{
    if (m_start_btn)
        m_start_btn->Enable(m_selected_index >= 0);
}

bool AnycubicAceTraySelectDialog::GetAutoLeveling() const { return m_chk_leveling && m_chk_leveling->GetValue(); }
bool AnycubicAceTraySelectDialog::GetVibrationCompensation() const { return m_chk_resonance && m_chk_resonance->GetValue(); }
bool AnycubicAceTraySelectDialog::GetTimelapse() const { return m_chk_timelapse && m_chk_timelapse->GetValue(); }
bool AnycubicAceTraySelectDialog::GetFlowCalibration() const { return m_chk_flow_cal && m_chk_flow_cal->GetValue(); }

}} // namespace Slic3r::GUI
