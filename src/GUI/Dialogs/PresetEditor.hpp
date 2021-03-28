#ifndef PRESETEDITOR_HPP
#define PRESETEDITOR_HPP

// stdlib
#include <string>
#include <functional>

// Libslic3r
#include "libslic3r.h"
#include "ConfigBase.hpp"
#include "Config.hpp"

// GUI
#include "misc_ui.hpp"
#include "Preset.hpp"

// Wx
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>

using namespace std::string_literals;
namespace Slic3r { namespace GUI {

class PresetPage;

class PresetEditor : public wxPanel {

public: 
    /// Member function to retrieve a list of options 
    /// that this preset governs. Subclasses should 
    /// call their own static method.
    virtual t_config_option_keys my_options() = 0;
    static t_config_option_keys options() { return t_config_option_keys {}; }

    static t_config_option_keys overridable_options() { return t_config_option_keys {}; };
    static t_config_option_keys overriding_options() { return t_config_option_keys {}; };

    virtual t_config_option_keys my_overridable_options() = 0;
    virtual t_config_option_keys my_overriding_options() = 0;

    wxSizer* sizer() { return _sizer; };
    PresetEditor(wxWindow* parent, t_config_option_keys options = {});
    PresetEditor() : PresetEditor(nullptr, {}) {};
    std::shared_ptr<Preset> current_preset;

    /// Check if there is a dirty config that is different than the loaded config.
    bool prompt_unsaved_changes();

    /// Perform a preset selection and possibly trigger the _on_select_preset 
    /// method.
    void select_preset(int id, bool force = false);
    void select_preset_by_name(const wxString& name, bool force = false);
    
    /// suppress the callback when the tree selection is changed.
    bool disable_tree_sel_changed_event {false};

    void save_preset();
    std::function<void (wxString, preset_t)> on_save_preset {nullptr};
    std::function<void (std::string)> on_value_change {nullptr};

    config_ptr config;
    void reload_config();
    void reload_preset();
    PresetPage* add_options_page(const wxString& _title, const wxString& _icon = "");

    virtual wxString title() = 0;
    virtual std::string name() = 0;
    virtual preset_t type() = 0; 
    virtual int typeId() = 0; 
protected:
    // Main sizer
    wxSizer* _sizer {nullptr};
    wxString presets;
    wxImageList* _icons {nullptr};
    wxTreeCtrl* _treectrl {nullptr};
    wxBitmapButton* _btn_save_preset {nullptr}; 
    wxBitmapButton* _btn_delete_preset {nullptr};
    wxChoice* _presets_choice {nullptr};
    int _iconcount {-1};

    /// Vector of PresetPage pointers; trust wxWidgets RTTI to avoid leaks
    std::vector<PresetPage*> _pages {};
    
    const unsigned int left_col_width {150};
    void _update_tree();
    void load_presets();

    virtual void _build() = 0;
    virtual void _update(const std::string& opt_key = "") = 0;
    virtual void _on_preset_loaded() = 0;

    void set_tooltips() { 
        this->_btn_save_preset->SetToolTip(wxString(_("Save current ")) + this->title());
        this->_btn_delete_preset->SetToolTip(_("Delete this preset."));
    }


    void reload_compatible_printers_widget();
    wxSizer* compatible_printers_widget();

    /// This method is called:
    ///  - upon first initialization;
    ///  - whenever user selects a preset from the dropdown;
    ///  - whenever select_preset() or select_preset_by_name() are called.
    void _on_select_preset(bool force = false);

    /// This method is supposed to be called whenever new values are loaded
    /// or changed by the user (including when presets are loaded).
    /// Pushes a callback onto the owning Slic3r app to be processed during 
    /// an idle event.
    void _on_value_change(std::string opt_key);

    virtual const std::string LogChannel() = 0; //< Which log these messages should go to.


};

class PrintEditor : public PresetEditor {
public:

    PrintEditor(wxWindow* parent, t_config_option_keys options = {});
    PrintEditor() : PrintEditor(nullptr, {}) {};

    wxString title() override { return _("Print Settings"); }
    std::string name() override { return "print"s; }

    preset_t type() override  { return preset_t::Print; }; 
    int typeId() override  { return static_cast<int>(preset_t::Print); }; 

    t_config_option_keys my_overridable_options() override { return PresetEditor::overridable_options(); };
    static t_config_option_keys overridable_options() { return PresetEditor::overridable_options(); };

    t_config_option_keys my_overriding_options() override { return PresetEditor::overriding_options(); };
    static t_config_option_keys overriding_options() { return PresetEditor::overriding_options(); };

    /// Static method to retrieve list of options that this preset governs.
    static t_config_option_keys options() {
        return t_config_option_keys
        {
            "layer_height"s, "first_layer_height"s,
            "adaptive_slicing"s, "adaptive_slicing_quality"s, "match_horizontal_surfaces"s,
            "perimeters"s, "spiral_vase"s,
            "top_solid_layers"s, "bottom_solid_layers"s,
            "extra_perimeters"s, "avoid_crossing_perimeters"s, 
            "thin_walls"s, "thin_walls_min_width"s, "thin_walls_overlap"s, "overhangs"s,
            "seam_position"s, "external_perimeters_first"s,
            "fill_density"s, "fill_pattern"s, "top_infill_pattern"s, "bottom_infill_pattern"s, "fill_gaps"s,
            "infill_every_layers"s, "infill_only_where_needed"s,
            "solid_infill_every_layers"s, "fill_angle"s, "solid_infill_below_area"s, ""s,
            "only_retract_when_crossing_perimeters"s, "infill_first"s,
            "max_print_speed"s, "max_volumetric_speed"s,
            "perimeter_speed"s, "small_perimeter_speed"s, "external_perimeter_speed"s, "infill_speed"s, ""s,
            "solid_infill_speed"s, "top_solid_infill_speed"s, "support_material_speed"s,
            "support_material_interface_speed"s, "bridge_speed"s, "gap_fill_speed"s,
            "travel_speed"s,
            "first_layer_speed"s,
            "perimeter_acceleration"s, "infill_acceleration"s, "bridge_acceleration"s,
            "first_layer_acceleration"s, "default_acceleration"s,
            "skirts"s, "skirt_distance"s, "skirt_height"s, "min_skirt_length"s,
            "brim_connections_width"s, "brim_ears"s, "brim_ears_max_angle"s, "brim_width"s, "interior_brim_width"s,
            "support_material"s, "support_material_threshold"s, "support_material_max_layers"s, "support_material_enforce_layers"s,
            "raft_layers"s,
            "support_material_pattern"s, "support_material_spacing"s, "support_material_angle"s, ""s,
            "support_material_interface_layers"s, "support_material_interface_spacing"s,
            "support_material_contact_distance"s, "support_material_buildplate_only"s, "dont_support_bridges"s,
            "notes"s,
            "complete_objects"s, "extruder_clearance_radius"s, "extruder_clearance_height"s,
            "gcode_comments"s, "output_filename_format"s,
            "post_process"s,
            "perimeter_extruder"s, "infill_extruder"s, "solid_infill_extruder"s,
            "support_material_extruder"s, "support_material_interface_extruder"s,
            "ooze_prevention"s, "standby_temperature_delta"s,
            "interface_shells"s, "regions_overlap"s,
            "extrusion_width"s, "first_layer_extrusion_width"s, "perimeter_extrusion_width"s, ""s,
            "external_perimeter_extrusion_width"s, "infill_extrusion_width"s, "solid_infill_extrusion_width"s,
            "top_infill_extrusion_width"s, "support_material_extrusion_width"s,
            "support_material_interface_extrusion_width"s, "infill_overlap"s, "bridge_flow_ratio"s,
            "xy_size_compensation"s, "resolution"s, "shortcuts"s, "compatible_printers"s,
            "print_settings_id"s
        };
    }

    t_config_option_keys my_options() override { return PrintEditor::options(); }

protected:
    void _update(const std::string& opt_key = "") override;
    void _build() override;
    void _on_preset_loaded() override;
    const std::string LogChannel() override {return "PrintEditor"s; } //< Which log these messages should go to.
    
};

class PrinterEditor : public PresetEditor {
public:
    PrinterEditor(wxWindow* parent, t_config_option_keys options = {});
    PrinterEditor() : PrinterEditor(nullptr, {}) {};

    wxString title() override { return _("Printer Settings"); }
    std::string name() override { return "printer"s; }
    preset_t type() override  { return preset_t::Printer; }; 
    int typeId() override  { return static_cast<int>(preset_t::Printer); }; 

    static t_config_option_keys overridable_options() { return t_config_option_keys 
    {
        "pressure_advance"s,
        "retract_length"s, "retract_lift"s, "retract_speed"s, "retract_restart_extra"s,
        "retract_before_travel"s, "retract_layer_change"s, "wipe"s
    }; };
    static t_config_option_keys overriding_options() { return PresetEditor::overriding_options(); };

    t_config_option_keys my_overridable_options() override { return PrinterEditor::overridable_options(); };
    t_config_option_keys my_overriding_options() override { return PrinterEditor::overriding_options(); };

    
    static t_config_option_keys options() {
        return t_config_option_keys
        {
            "bed_shape"s, "z_offset"s, "z_steps_per_mm"s, "has_heatbed"s,
            "gcode_flavor"s, "use_relative_e_distances"s,
            "serial_port"s, "serial_speed"s,
            "host_type"s, "print_host"s, "octoprint_apikey"s,
            "use_firmware_retraction"s, "pressure_advance"s, "vibration_limit"s,
            "use_volumetric_e"s,
            "start_gcode"s, "end_gcode"s, "before_layer_gcode"s, "layer_gcode"s, "toolchange_gcode"s, "between_objects_gcode"s,
            "nozzle_diameter"s, "extruder_offset"s, "min_layer_height"s, "max_layer_height"s,
            "retract_length"s, "retract_lift"s, "retract_speed"s, "retract_restart_extra"s, "retract_before_travel"s, "retract_layer_change"s, "wipe"s,
            "retract_length_toolchange"s, "retract_restart_extra_toolchange"s, "retract_lift_above"s, "retract_lift_below"s,
            "printer_settings_id"s,
            "printer_notes"s,
            "use_set_and_wait_bed"s, "use_set_and_wait_extruder"s
        };
    }
    
    t_config_option_keys my_options() override { return PrinterEditor::options(); }
protected:
    void _update(const std::string& opt_key = "") override;
    void _build() override;
    void _on_preset_loaded() override;
    const std::string LogChannel() override {return "PrinterEditor"s;} //< Which log these messages should go to.
};

class MaterialEditor : public PresetEditor {
public:

    wxString title() override { return _("Material Settings"); }
    std::string name() override { return "material"s; }
    preset_t type() override  { return preset_t::Material; }; 
    int typeId() override  { return static_cast<int>(preset_t::Material); }; 
    MaterialEditor(wxWindow* parent, t_config_option_keys options = {});
    MaterialEditor() : MaterialEditor(nullptr, {}) {};
    
    t_config_option_keys my_overridable_options() override { return PresetEditor::overridable_options(); };
    t_config_option_keys my_overriding_options() override { return PrinterEditor::overridable_options(); };

    static t_config_option_keys overriding_options() { return PrinterEditor::overridable_options(); };
    static t_config_option_keys overridable_options() { return PresetEditor::overridable_options(); };

    static t_config_option_keys options() {
        return t_config_option_keys
        {
            "filament_colour"s, "filament_diameter"s, "filament_notes"s, "filament_max_volumetric_speed"s, "extrusion_multiplier"s, "filament_density"s, "filament_cost"s,
            "temperature"s, "first_layer_temperature"s, "bed_temperature"s, "first_layer_bed_temperature"s,
            "fan_always_on"s, "cooling"s, "compatible_printers"s,
            "min_fan_speed"s, "max_fan_speed"s, "bridge_fan_speed"s, "disable_fan_first_layers"s,
            "fan_below_layer_time"s, "slowdown_below_layer_time"s, "min_print_speed"s,
            "start_filament_gcode"s, "end_filament_gcode"s,
            "filament_settings_id"s
        };
    }
    
    t_config_option_keys my_options() override { return MaterialEditor::options(); }
protected:
    void _update(const std::string& opt_key = "") override;
    void _build() override;
    void _on_preset_loaded() override;
    const std::string LogChannel() override {return "MaterialEditor"s;} //< Which log these messages should go to.
};




class PresetPage : public wxScrolledWindow {
public:
    PresetPage(wxWindow* parent, wxString _title, int _iconID) : 
        wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL),
        title(_title), iconID(_iconID) {
            this->vsizer = new wxBoxSizer(wxVERTICAL);
            this->SetSizer(this->vsizer);
            this->SetScrollRate(ui_settings->scroll_step(), ui_settings->scroll_step());
        }
protected:
    wxSizer* vsizer {nullptr};
    wxString title {""};
    int iconID {0};
};

}} // namespace Slic3r::GUI
#endif // PRESETEDITOR_HPP
