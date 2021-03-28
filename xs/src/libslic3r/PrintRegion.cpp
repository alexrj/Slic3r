#include "Print.hpp"

namespace Slic3r {

PrintRegion::PrintRegion(Print* print)
    : _print(print)
{
}

PrintRegion::~PrintRegion()
{
}

Print*
PrintRegion::print()
{
    return this->_print;
}

Flow
PrintRegion::flow(FlowRole role, double layer_height, bool bridge, bool first_layer, double width, const PrintObject &object) const
{
    ConfigOptionFloatOrPercent config_width;
    if (width != -1) {
        // use the supplied custom width, if any
        config_width.value = width;
        config_width.percent = false;
    } else {
        // otherwise, get extrusion width from configuration
        // (might be an absolute value, or a percent value, or zero for auto)
        if (first_layer && this->_print->config.first_layer_extrusion_width.value > 0) {
            config_width = this->_print->config.first_layer_extrusion_width;
        } else if (role == frExternalPerimeter) {
            config_width = this->config.external_perimeter_extrusion_width;
        } else if (role == frPerimeter) {
            config_width = this->config.perimeter_extrusion_width;
        } else if (role == frInfill) {
            config_width = this->config.infill_extrusion_width;
        } else if (role == frSolidInfill) {
            config_width = this->config.solid_infill_extrusion_width;
        } else if (role == frTopSolidInfill) {
            config_width = this->config.top_infill_extrusion_width;
        } else {
            CONFESS("Unknown role");
        }
    }
    if (config_width.value == 0) {
        config_width = object.config.extrusion_width;
    }
    
    // get the configured nozzle_diameter for the extruder associated
    // to the flow role requested
    size_t extruder = 0;    // 1-based
    if (role == frPerimeter || role == frExternalPerimeter) {
        extruder = this->config.perimeter_extruder;
    } else if (role == frInfill) {
        extruder = this->config.infill_extruder;
    } else if (role == frSolidInfill || role == frTopSolidInfill) {
        extruder = this->config.solid_infill_extruder;
    } else {
        CONFESS("Unknown role $role");
    }
    double nozzle_diameter = this->_print->config.nozzle_diameter.get_at(extruder-1);
    
    return Flow::new_from_config_width(role, config_width, nozzle_diameter, layer_height, bridge ? (float)this->config.bridge_flow_ratio : 0.0);
}

bool
PrintRegion::invalidate_state_by_config(const PrintConfigBase &config)
{
    const t_config_option_keys diff = this->config.diff(config);
    
    std::set<PrintObjectStep> steps;
    bool all = false;
    
    for (const t_config_option_key &opt_key : diff) {
        if (opt_key == "perimeters"
            || opt_key == "extra_perimeters"
            || opt_key == "min_shell_thickness"
            || opt_key == "gap_fill_speed"
            || opt_key == "overhangs"
            || opt_key == "first_layer_extrusion_width"
            || opt_key == "perimeter_extrusion_width"
            || opt_key == "thin_walls"
            || opt_key == "thin_walls_min_width"
            || opt_key == "thin_walls_overlap"
            || opt_key == "external_perimeters_first") {
            steps.insert(posPerimeters);
        } else if (opt_key == "first_layer_extrusion_width") {
            steps.insert(posSupportMaterial);
        } else if (opt_key == "solid_infill_below_area") {
            const float &cur_value = config.opt<ConfigOptionFloat>(opt_key)->value;
            const float &new_value = this->config.solid_infill_below_area.value;
            if (new_value >= cur_value) {
                steps.insert(posPrepareInfill);
            } else {
                // prepare_infill is not idempotent when solid_infill_below_area is reduced
                steps.insert(posPerimeters);
            }
        } else if (opt_key == "infill_every_layers"
            || opt_key == "solid_infill_every_layers"
            || opt_key == "bottom_solid_layers"
            || opt_key == "top_solid_layers"
            || opt_key == "min_top_bottom_shell_thickness"
            || opt_key == "min_shell_thickness"
            || opt_key == "infill_extruder"
            || opt_key == "solid_infill_extruder"
            || opt_key == "infill_extrusion_width") {
            steps.insert(posPrepareInfill);
        } else if (opt_key == "top_infill_pattern"
            || opt_key == "bottom_infill_pattern"
            || opt_key == "fill_angle"
            || opt_key == "fill_pattern"
            || opt_key == "top_infill_extrusion_width"
            || opt_key == "first_layer_extrusion_width"
            || opt_key == "infill_overlap") {
            steps.insert(posInfill);
        } else if (opt_key == "solid_infill_extrusion_width") {
            steps.insert(posPerimeters);
            steps.insert(posPrepareInfill);
        } else if (opt_key == "fill_density") {
            const float &cur_value = config.opt<ConfigOptionFloat>("fill_density")->value;
            const float &new_value = this->config.fill_density.value;
            if ((cur_value == 0) != (new_value == 0) || (cur_value == 100) != (new_value == 100))
                steps.insert(posPerimeters);
            
            steps.insert(posInfill);
        } else if (opt_key == "external_perimeter_extrusion_width"
            || opt_key == "perimeter_extruder") {
            steps.insert(posPerimeters);
            steps.insert(posSupportMaterial);
        } else if (opt_key == "bridge_flow_ratio") {
            steps.insert(posPerimeters);
            steps.insert(posInfill);
        } else if (opt_key == "bridge_speed"
            || opt_key == "external_perimeter_speed"
            || opt_key == "infill_speed"
            || opt_key == "perimeter_speed"
            || opt_key == "small_perimeter_speed"
            || opt_key == "solid_infill_speed"
            || opt_key == "top_solid_infill_speed") {
            // these options only affect G-code export, so nothing to invalidate
        } else {
            // for legacy, if we can't handle this option let's invalidate all steps
            all = true;
            break;
        }
    }
    
    if (!diff.empty())
        this->config.apply(config, true);
    
    bool invalidated = false;
    if (all) {
        for (PrintObject* object : this->print()->objects)
            if (object->invalidate_all_steps())
                invalidated = true;
    } else {
        for (const PrintObjectStep &step : steps)
            for (PrintObject* object : this->print()->objects)
                if (object->invalidate_step(step))
                    invalidated = true;
    }
    
    return invalidated;
}

}
