#include "Print.hpp"
#include "PrintGCode.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Fill/Fill.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "SupportMaterial.hpp"
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <sstream>

#ifdef __cpp_lib_quoted_string_io
    #include <iomanip>
#else
    #include <boost/algorithm/string.hpp>
#endif

namespace Slic3r {

template <class StepClass>
bool
PrintState<StepClass>::is_started(StepClass step) const
{
    return this->started.find(step) != this->started.end();
}

template <class StepClass>
bool
PrintState<StepClass>::is_done(StepClass step) const
{
    return this->done.find(step) != this->done.end();
}

template <class StepClass>
void
PrintState<StepClass>::set_started(StepClass step)
{
    this->started.insert(step);
}

template <class StepClass>
void
PrintState<StepClass>::set_done(StepClass step)
{
    this->done.insert(step);
}

template <class StepClass>
bool
PrintState<StepClass>::invalidate(StepClass step)
{
    bool invalidated = this->started.erase(step) > 0;
    this->done.erase(step);
    return invalidated;
}

template class PrintState<PrintStep>;
template class PrintState<PrintObjectStep>;


Print::Print()
:   total_used_filament(0),
    total_extruded_volume(0)
{
}

Print::~Print()
{
    clear_objects();
    clear_regions();
}

void
Print::clear_objects()
{
    for (int i = this->objects.size()-1; i >= 0; --i)
        this->delete_object(i);

    this->clear_regions();
}

void
Print::delete_object(size_t idx)
{
    PrintObjectPtrs::iterator i = this->objects.begin() + idx;
    if (i >= this->objects.end()) 
        throw std::out_of_range("Object not found");
    
    // before deleting object, invalidate all of its steps in order to 
    // invalidate all of the dependent ones in Print
    (*i)->invalidate_all_steps();
    
    // destroy object and remove it from our container
    delete *i;
    this->objects.erase(i);

    // TODO: purge unused regions
}

void
Print::process() 
{
    /// No need to call this as we call it as part of prepare_infill()
    /// until we fix the idempotency issue.
//    if (this->status_cb != nullptr)
//        this->status_cb(20, "Generating perimeters");
  //  for(auto& obj : this->objects) { obj->make_perimeters(); }
    if (this->status_cb != nullptr)
        this->status_cb(70, "Infilling layers");
    for(auto& obj : this->objects) { obj->infill(); }
    for(auto& obj : this->objects) { obj->generate_support_material(); }

    this->make_skirt();
    this->make_brim(); // must follow make_skirt
}

void
Print::make_brim() 
{
    if (this->state.is_done(psBrim)) return;
    // prereqs
    for(auto& obj: this->objects) {
        obj->make_perimeters();
        obj->infill();
        obj->generate_support_material();
    }
    this->state.set_started(psBrim);
    if (this->status_cb != nullptr)
        this->status_cb(88, "Generating brim");
    this->_make_brim();
    this->state.set_done(psBrim);
}

void
Print::make_skirt()
{
    if (this->state.is_done(psSkirt)) return;
    this->state.set_started(psSkirt);
    
    // prereqs
    for (auto* obj: this->objects) {
        obj->make_perimeters();
        obj->infill();
        obj->generate_support_material();
    }

    // since this method must be idempotent, we clear skirt paths *before*
    // checking whether we need to generate them
    this->skirt.clear();

    if (!this->has_skirt()) {
        this->state.set_done(psSkirt);
        return;
    }

    if (this->status_cb != nullptr)
        this->status_cb(88, "Generating skirt");

    // First off we need to decide how tall the skirt must be.
    // The skirt_height option from config is expressed in layers, but our
    // object might have different layer heights, so we need to find the print_z
    // of the highest layer involved.
    // Note that unless has_infinite_skirt() == true
    // the actual skirt might not reach this $skirt_height_z value since the print
    // order of objects on each layer is not guaranteed and will not generally
    // include the thickest object first. It is just guaranteed that a skirt is
    // prepended to the first 'n' layers (with 'n' = skirt_height).
    // $skirt_height_z in this case is the highest possible skirt height for safety.
    this->skirt_height_z = -1.0;
    for (const auto* object : this->objects) {
        const size_t skirt_height {
            this->has_infinite_skirt()
                ? object->layer_count()
                : std::min(size_t(this->config.skirt_height()), object->layer_count())
        };
        const Layer* highest_layer { object->get_layer(skirt_height - 1) };
        this->skirt_height_z = std::max(skirt_height_z, highest_layer->print_z);
    }

    // collect points from all layers contained in skirt height
    Points points;
    for (auto* object : this->objects) {
        Points object_points;
        
        // get object layers up to this->skirt_height_z
        for (const auto* layer : object->layers) {
            if (layer->print_z > this->skirt_height_z) break;
            for (const ExPolygon ex : layer->slices)
                append_to(object_points, static_cast<Points>(ex));
        }
        
        // get support layers up to this->skirt_height_z
        for (const auto* layer : object->support_layers) {
            if (layer->print_z > this->skirt_height_z) break;
            for (auto* ee : layer->support_fills)
                append_to(object_points, ee->as_polyline().points);
            for (auto* ee : layer->support_interface_fills)
                append_to(object_points, ee->as_polyline().points);
        }
        
        // repeat points for each object copy
        for (const auto& copy : object->_shifted_copies) {
            for (Point p : object_points) {
                p.translate(copy);
                points.push_back(p);
            }
        }
    }
    if (points.size() < 3) return;  // at least three points required for a convex hull
    
    // find out convex hull
    const Polygon convex = Geometry::convex_hull(points);
    
    // skirt may be printed on several layers, having distinct layer heights,
    // but loops must be aligned so can't vary width/spacing
    // TODO: use each extruder's own flow
    const auto first_layer_height = this->skirt_first_layer_height();
    const auto flow = this->skirt_flow();
    const auto spacing = flow.scaled_spacing();
    const auto mm3_per_mm = flow.mm3_per_mm();
    
    int skirts = this->config.skirts();
    if (skirts == 0 && this->has_infinite_skirt())
        skirts = 1;
    
    const std::set<size_t> extruders{ this->extruders() };
    auto extruder_it = extruders.cbegin();
    std::vector<float> e_per_mm{0}, extruded_length{0};
    if (this->config.min_skirt_length() > 0)
        for (auto i : extruders)
            e_per_mm[i] = Extruder(i, &this->config).e_per_mm(mm3_per_mm);
    
    // draw outlines from outside to inside
    // loop while we have less skirts than required or any extruder hasn't reached the min length if any
    float distance = scale_(std::max(this->config.skirt_distance(), this->config.brim_width()));
    for (int i = skirts; i > 0; i--) {
        distance += spacing;
        const Polygon loop = offset(Polygons{convex}, distance, 1, jtRound, scale_(0.1)).at(0);
        auto epath = ExtrusionPath(
            erSkirt,
            mm3_per_mm,        // this will be overridden at G-code export time
            flow.width,
            first_layer_height // this will be overridden at G-code export time
        );
        epath.polyline = loop.split_at_first_point();
        auto eloop = ExtrusionLoop(epath, elrSkirt);
        this->skirt.append(eloop);
        
        if (this->config.min_skirt_length() > 0) {
            extruded_length[*extruder_it] += unscale(loop.length()) * e_per_mm[*extruder_it];
            for (auto j : extruders) {
                if (extruded_length[j] < this->config.min_skirt_length()) {
                    ++i;
                    break;
                }
            }
            if (extruded_length[*extruder_it] >= this->config.min_skirt_length() && extruder_it != extruders.end())
                ++extruder_it;
        }
    }

    this->skirt.reverse();
    this->state.set_done(psSkirt);
}

void
Print::reload_object(size_t idx)
{
    /* TODO: this method should check whether the per-object config and per-material configs
        have changed in such a way that regions need to be rearranged or we can just apply
        the diff and invalidate something.  Same logic as apply_config()
        For now we just re-add all objects since we haven't implemented this incremental logic yet.
        This should also check whether object volumes (parts) have changed. */
    
    // collect all current model objects
    ModelObjectPtrs model_objects;
    FOREACH_OBJECT(this, object) {
        model_objects.push_back((*object)->model_object());
    }
    
    // remove our print objects
    this->clear_objects();
    
    // re-add model objects
    for (ModelObjectPtrs::iterator it = model_objects.begin(); it != model_objects.end(); ++it) {
        this->add_model_object(*it);
    }
}

bool
Print::reload_model_instances()
{
    bool invalidated = false;
    FOREACH_OBJECT(this, object) {
        if ((*object)->reload_model_instances()) invalidated = true;
    }
    return invalidated;
}

void
Print::clear_regions()
{
    for (int i = this->regions.size()-1; i >= 0; --i)
        this->delete_region(i);
}

PrintRegion*
Print::add_region()
{
    PrintRegion *region = new PrintRegion(this);
    regions.push_back(region);
    return region;
}

void
Print::delete_region(size_t idx)
{
    PrintRegionPtrs::iterator i = this->regions.begin() + idx;
    delete *i;
    this->regions.erase(i);
}

bool
Print::invalidate_state_by_config(const PrintConfigBase &config)
{
    const t_config_option_keys diff = this->config.diff(config);
    
    std::set<PrintStep> steps;
    std::set<PrintObjectStep> osteps;
    bool all = false;
    
    // this method only accepts PrintConfig option keys
    for (const t_config_option_key &opt_key : diff) {
        if (opt_key == "skirts"
            || opt_key == "skirt_height"
            || opt_key == "skirt_distance"
            || opt_key == "min_skirt_length"
            || opt_key == "ooze_prevention") {
            steps.insert(psSkirt);
        } else if (opt_key == "brim_width") {
            steps.insert(psBrim);
            steps.insert(psSkirt);
            osteps.insert(posSupportMaterial);
        } else if (opt_key == "brim_width"
            || opt_key == "interior_brim_width"
            || opt_key == "brim_ears"
            || opt_key == "brim_ears_max_angle"
            || opt_key == "brim_overlap"
            || opt_key == "brim_connections_width") {
            steps.insert(psBrim);
            steps.insert(psSkirt);
        } else if (opt_key == "nozzle_diameter") {
                osteps.insert(posLayers);
        } else if (opt_key == "resolution"
            || opt_key == "z_steps_per_mm") {
            osteps.insert(posSlice);
        } else if (opt_key == "avoid_crossing_perimeters"
            || opt_key == "bed_shape"
            || opt_key == "bed_temperature"
            || opt_key == "between_objects_gcode"
            || opt_key == "bridge_acceleration"
            || opt_key == "bridge_fan_speed"
            || opt_key == "complete_objects"
            || opt_key == "cooling"
            || opt_key == "default_acceleration"
            || opt_key == "disable_fan_first_layers"
            || opt_key == "duplicate_distance"
            || opt_key == "end_gcode"
            || opt_key == "extruder_clearance_height"
            || opt_key == "extruder_clearance_radius"
            || opt_key == "extruder_offset"
            || opt_key == "extrusion_axis"
            || opt_key == "extrusion_multiplier"
            || opt_key == "fan_always_on"
            || opt_key == "fan_below_layer_time"
            || opt_key == "filament_colour"
            || opt_key == "filament_diameter"
            || opt_key == "filament_notes"
            || opt_key == "first_layer_acceleration"
            || opt_key == "first_layer_bed_temperature"
            || opt_key == "first_layer_speed"
            || opt_key == "first_layer_temperature"
            || opt_key == "gcode_arcs"
            || opt_key == "gcode_comments"
            || opt_key == "gcode_flavor"
            || opt_key == "infill_acceleration"
            || opt_key == "infill_first"
            || opt_key == "layer_gcode"
            || opt_key == "min_fan_speed"
            || opt_key == "max_fan_speed"
            || opt_key == "min_print_speed"
            || opt_key == "notes"
            || opt_key == "only_retract_when_crossing_perimeters"
            || opt_key == "output_filename_format"
            || opt_key == "perimeter_acceleration"
            || opt_key == "post_process"
            || opt_key == "pressure_advance"
            || opt_key == "printer_notes"
            || opt_key == "retract_before_travel"
            || opt_key == "retract_layer_change"
            || opt_key == "retract_length"
            || opt_key == "retract_length_toolchange"
            || opt_key == "retract_lift"
            || opt_key == "retract_lift_above"
            || opt_key == "retract_lift_below"
            || opt_key == "retract_restart_extra"
            || opt_key == "retract_restart_extra_toolchange"
            || opt_key == "retract_speed"
            || opt_key == "slowdown_below_layer_time"
            || opt_key == "spiral_vase"
            || opt_key == "standby_temperature_delta"
            || opt_key == "start_gcode"
            || opt_key == "temperature"
            || opt_key == "threads"
            || opt_key == "toolchange_gcode"
            || opt_key == "travel_speed"
            || opt_key == "use_firmware_retraction"
            || opt_key == "use_relative_e_distances"
            || opt_key == "vibration_limit"
            || opt_key == "wipe"
            || opt_key == "z_offset") {
            // these options only affect G-code export, so nothing to invalidate
        } else if (opt_key == "first_layer_extrusion_width") {
            osteps.insert(posPerimeters);
            osteps.insert(posInfill);
            osteps.insert(posSupportMaterial);
            steps.insert(psSkirt);
            steps.insert(psBrim);
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
        if (this->invalidate_all_steps())
            invalidated = true;
        
        for (PrintObject* object : this->objects)
            if (object->invalidate_all_steps())
                invalidated = true;
    } else {
        for (const PrintStep &step : steps)
            if (this->invalidate_step(step))
                invalidated = true;
    
        for (const PrintObjectStep &ostep : osteps)
            for (PrintObject* object : this->objects)
                if (object->invalidate_step(ostep))
                    invalidated = true;
    }
    
    return invalidated;
}

bool
Print::invalidate_step(PrintStep step)
{
    bool invalidated = this->state.invalidate(step);
    
    // propagate to dependent steps
    if (step == psSkirt) {
        invalidated |= this->invalidate_step(psBrim);
    }
    
    return invalidated;
}

bool
Print::invalidate_all_steps()
{
    // make a copy because when invalidating steps the iterators are not working anymore
    std::set<PrintStep> steps = this->state.started;
    
    bool invalidated = false;
    for (std::set<PrintStep>::const_iterator step = steps.begin(); step != steps.end(); ++step) {
        if (this->invalidate_step(*step)) invalidated = true;
    }
    return invalidated;
}

// returns true if an object step is done on all objects
// and there's at least one object
bool
Print::step_done(PrintObjectStep step) const
{
    if (this->objects.empty()) return false;
    FOREACH_OBJECT(this, object) {
        if (!(*object)->state.is_done(step))
            return false;
    }
    return true;
}

// returns 0-based indices of used extruders
std::set<size_t>
Print::object_extruders() const
{
    std::set<size_t> extruders;
    
    FOREACH_REGION(this, region) {
        // these checks reflect the same logic used in the GUI for enabling/disabling
        // extruder selection fields
        if ((*region)->config.perimeters.value > 0
            || this->config.brim_width.value > 0
            || this->config.interior_brim_width.value > 0
            || this->config.brim_connections_width.value > 0)
            extruders.insert((*region)->config.perimeter_extruder - 1);
        
        if ((*region)->config.fill_density.value > 0)
            extruders.insert((*region)->config.infill_extruder - 1);
        
        if ((*region)->config.top_solid_layers.value > 0
            || (*region)->config.bottom_solid_layers.value > 0
            || (*region)->config.min_top_bottom_shell_thickness.value > 0)
            extruders.insert((*region)->config.solid_infill_extruder - 1);
    }
    
    return extruders;
}

// returns 0-based indices of used extruders
std::set<size_t>
Print::support_material_extruders() const
{
    std::set<size_t> extruders;
    
    FOREACH_OBJECT(this, object) {
        if ((*object)->has_support_material()) {
            extruders.insert((*object)->config.support_material_extruder - 1);
            extruders.insert((*object)->config.support_material_interface_extruder - 1);
        }
    }
    
    return extruders;
}

// returns 0-based indices of used extruders
std::set<size_t>
Print::extruders() const
{
    std::set<size_t> extruders = this->object_extruders();
    
    std::set<size_t> s_extruders = this->support_material_extruders();
    extruders.insert(s_extruders.begin(), s_extruders.end());
    
    return extruders;
}

size_t
Print::brim_extruder() const
{
    size_t e = this->get_region(0)->config.perimeter_extruder;
    for (const PrintObject* object : this->objects) {
        if (object->config.raft_layers > 0)
            e = object->config.support_material_extruder;
    }
    return e;
}

void
Print::_simplify_slices(double distance)
{
    FOREACH_OBJECT(this, object) {
        FOREACH_LAYER(*object, layer) {
            (*layer)->slices.simplify(distance);
            FOREACH_LAYERREGION(*layer, layerm) {
                (*layerm)->slices.simplify(distance);
            }
        }
    }
}

double
Print::max_allowed_layer_height() const
{
    std::vector<double> nozzle_diameter;
    
    std::set<size_t> extruders = this->extruders();
    for (std::set<size_t>::const_iterator e = extruders.begin(); e != extruders.end(); ++e) {
        nozzle_diameter.push_back(this->config.nozzle_diameter.get_at(*e));
    }
    
    return *std::max_element(nozzle_diameter.begin(), nozzle_diameter.end());
}

/*  Caller is responsible for supplying models whose objects don't collide
    and have explicit instance positions */
void
Print::add_model_object(ModelObject* model_object, int idx)
{
    DynamicPrintConfig object_config = model_object->config;  // clone
    object_config.normalize();

    // initialize print object and store it at the given position
    PrintObject* o;
    {
        BoundingBoxf3 bb = model_object->raw_bounding_box();
        if (idx != -1) {
            // replacing existing object
            PrintObjectPtrs::iterator old_it = this->objects.begin() + idx;
            // before deleting object, invalidate all of its steps in order to 
            // invalidate all of the dependent ones in Print
            (*old_it)->invalidate_all_steps();
            delete *old_it;
            
            this->objects[idx] = o = new PrintObject(this, model_object, bb);
        } else {
            o = new PrintObject(this, model_object, bb);
            objects.push_back(o);
    
            // invalidate steps
            this->invalidate_step(psSkirt);
            this->invalidate_step(psBrim);
        }
    }

    for (ModelVolumePtrs::const_iterator v_i = model_object->volumes.begin(); v_i != model_object->volumes.end(); ++v_i) {
        size_t volume_id = v_i - model_object->volumes.begin();
        ModelVolume* volume = *v_i;
        
        // get the config applied to this volume
        PrintRegionConfig config = this->_region_config_from_model_volume(*volume);
        
        // find an existing print region with the same config
        int region_id = -1;
        for (PrintRegionPtrs::const_iterator region = this->regions.begin(); region != this->regions.end(); ++region) {
            if (config.equals((*region)->config)) {
                region_id = region - this->regions.begin();
                break;
            }
        }
        
        // if no region exists with the same config, create a new one
        if (region_id == -1) {
            PrintRegion* r = this->add_region();
            r->config.apply(config);
            region_id = this->regions.size() - 1;
        }
        
        // assign volume to region
        o->add_region_volume(region_id, volume_id);
    }

    // apply config to print object
    o->config.apply(this->default_object_config);
    o->config.apply(object_config, true);
    
    // update placeholders
    {
        // get the first input file name
        std::string input_file;
        std::vector<std::string> v_scale;
        FOREACH_OBJECT(this, object) {
            const ModelObject &mobj = *(*object)->model_object();
            v_scale.push_back( boost::lexical_cast<std::string>(mobj.instances[0]->scaling_factor*100) + "%" );
            if (input_file.empty())
                input_file = mobj.input_file;
        }
        
        PlaceholderParser &pp = this->placeholder_parser;
        pp.set("scale", v_scale);
        if (!input_file.empty()) {
            // get basename with and without suffix
            const std::string input_basename = boost::filesystem::path(input_file).filename().string();
            pp.set("input_filename", input_basename);
            const std::string input_basename_base = input_basename.substr(0, input_basename.find_last_of("."));
            pp.set("input_filename_base", input_basename_base);
        }
    }
}

void
Print::export_gcode(std::ostream& output, bool quiet)
{
    // prerequisites
    this->process();
    
    if (this->status_cb != nullptr) 
        this->status_cb(90, "Exporting G-Code...");
    
    Slic3r::PrintGCode(*this, output).output();
}

void
Print::export_gcode(std::string outfile, bool quiet)
{
    // compute the actual output filepath
    outfile = this->output_filepath(outfile);
    
    // write G-code to a temporary file in order to make the export atomic
    const std::string tempfile{ outfile + ".tmp" };
    std::ofstream outstream(tempfile);
    this->export_gcode(outstream);
    
    // rename the temporary file to the destination file
    // When renaming, some other application (thank you, Windows Explorer) 
    // may keep the file locked. Try to wait a bit and then rename the file again.
    for (int i = 0; std::rename(tempfile.c_str(), outfile.c_str()) != 0; ++i) {
        if (i == 4) {
            std::stringstream ss;
            ss << "Failed to remove the output G-code file from "
                << tempfile << " to " << outfile << ". Is " << tempfile << " locked?";
            throw std::runtime_error(ss.str());
        } else {
            // Wait for 1/4 seconds and try to rename once again.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    
    // run post-processing scripts
    if (!this->config.post_process.values.empty()) {
        if (this->status_cb != nullptr) 
            this->status_cb(95, "Running post-processing scripts...");
        
        this->config.setenv_();
        for (std::string ppscript : this->config.post_process.values) {
            #ifdef __cpp_lib_quoted_string_io
                std::stringstream _tmp_string(ppscript);
                _tmp_string << " " << std::quoted(outfile);
                ppscript = _tmp_string.str();
            #else
                boost::replace_all(ppscript, "\"", "\\\"");
                ppscript += " \"" + outfile + "\"";
            #endif
            system(ppscript.c_str());
        
            // TODO: system() should be only used if user enabled an option for explicitly
            // supporting arguments, otherwise we should use exec*() and call the executable
            // directly without launching a shell. #4000
        }
    }
}

#ifndef SLIC3RXS
bool
Print::apply_config(config_ptr config) {
    // dereference the stored pointer and pass the resulting data to apply_config()
    return this->apply_config(config->config());
}
#endif

bool
Print::apply_config(DynamicPrintConfig config)
{
    // we get a copy of the config object so we can modify it safely
    config.normalize();
    
    // apply variables to placeholder parser
    this->placeholder_parser.apply_config(config);
    
    // handle changes to print config
    bool invalidated = this->invalidate_state_by_config(config);
    
    // handle changes to object config defaults
    this->default_object_config.apply(config, true);
    for (PrintObject* object : this->objects) {
        // we don't assume that config contains a full ObjectConfig,
        // so we base it on the current print-wise default
        PrintObjectConfig new_config = this->default_object_config;
        new_config.apply(config, true);
        
        // we override the new config with object-specific options
        {
            DynamicPrintConfig model_object_config = object->model_object()->config;
            model_object_config.normalize();
            new_config.apply(model_object_config, true);
        }
        
        // check whether the new config is different from the current one
        if (object->invalidate_state_by_config(new_config))
            invalidated = true;
    }
    
    // handle changes to regions config defaults
    this->default_region_config.apply(config, true);
    
    // All regions now have distinct settings.
    // Check whether applying the new region config defaults we'd get different regions.
    bool rearrange_regions = false;
    std::vector<PrintRegionConfig> other_region_configs;
    FOREACH_REGION(this, it_r) {
        size_t region_id = it_r - this->regions.begin();
        PrintRegion* region = *it_r;
        
        std::vector<PrintRegionConfig> this_region_configs;
        FOREACH_OBJECT(this, it_o) {
            PrintObject* object = *it_o;
            
            std::vector<int> &region_volumes = object->region_volumes[region_id];
            for (std::vector<int>::const_iterator volume_id = region_volumes.begin(); volume_id != region_volumes.end(); ++volume_id) {
                ModelVolume* volume = object->model_object()->volumes.at(*volume_id);
                
                PrintRegionConfig new_config = this->_region_config_from_model_volume(*volume);
                
                for (std::vector<PrintRegionConfig>::iterator it = this_region_configs.begin(); it != this_region_configs.end(); ++it) {
                    // if the new config for this volume differs from the other
                    // volume configs currently associated to this region, it means
                    // the region subdivision does not make sense anymore
                    if (!it->equals(new_config)) {
                        rearrange_regions = true;
                        goto NEXT_REGION;
                    }
                }
                this_region_configs.push_back(new_config);
                
                for (std::vector<PrintRegionConfig>::iterator it = other_region_configs.begin(); it != other_region_configs.end(); ++it) {
                    // if the new config for this volume equals any of the other
                    // volume configs that are not currently associated to this
                    // region, it means the region subdivision does not make
                    // sense anymore
                    if (it->equals(new_config)) {
                        rearrange_regions = true;
                        goto NEXT_REGION;
                    }
                }
                
                // if we're here and the new region config is different from the old
                // one, we need to apply the new config and invalidate all objects
                // (possible optimization: only invalidate objects using this region)
                if (region->invalidate_state_by_config(new_config))
                    invalidated = true;
            }
        }
        other_region_configs.insert(other_region_configs.end(), this_region_configs.begin(), this_region_configs.end());
        
        NEXT_REGION:
            continue;
    }
    
    if (rearrange_regions) {
        // the current subdivision of regions does not make sense anymore.
        // we need to remove all objects and re-add them
        ModelObjectPtrs model_objects;
        FOREACH_OBJECT(this, o) {
            model_objects.push_back((*o)->model_object());
        }
        this->clear_objects();
        for (ModelObjectPtrs::iterator it = model_objects.begin(); it != model_objects.end(); ++it) {
            this->add_model_object(*it);
        }
        invalidated = true;
    }
    
    return invalidated;
}

bool Print::has_infinite_skirt() const
{
    return (this->config.skirt_height == -1 && this->config.skirts > 0)
        || (this->config.ooze_prevention && this->extruders().size() > 1);
}

bool Print::has_skirt() const
{
    return (this->config.skirt_height > 0 && this->config.skirts > 0)
        || this->has_infinite_skirt();
}

void
Print::validate() const
{
    if (this->config.complete_objects) {
        // check horizontal clearance
        {
            Polygons a;
            FOREACH_OBJECT(this, i_object) {
                PrintObject* object = *i_object;
                
                /*  get convex hull of all meshes assigned to this print object
                    (this is the same as model_object()->raw_mesh.convex_hull()
                    but probably more efficient */
                Polygon convex_hull;
                {
                    Polygons mesh_convex_hulls;
                    for (size_t i = 0; i < this->regions.size(); ++i) {
                        for (std::vector<int>::const_iterator it = object->region_volumes[i].begin(); it != object->region_volumes[i].end(); ++it) {
                            Polygon hull = object->model_object()->volumes[*it]->mesh.convex_hull();
                            mesh_convex_hulls.push_back(hull);
                        }
                    }
                
                    // make a single convex hull for all of them
                    convex_hull = Slic3r::Geometry::convex_hull(mesh_convex_hulls);
                }
                
                // apply the same transformations we apply to the actual meshes when slicing them
                object->model_object()->instances.front()->transform_polygon(&convex_hull);
                
                // grow convex hull with the clearance margin
                convex_hull = offset(
                    convex_hull,
                    // safety_offset in intersection() is not enough for preventing false positives
                    scale_(this->config.extruder_clearance_radius.value)/2 - scale_(0.01),
                    CLIPPER_OFFSET_SCALE,
                    jtRound, scale_(0.1)
                ).front();
                
                // now we check that no instance of convex_hull intersects any of the previously checked object instances
                for (Points::const_iterator copy = object->_shifted_copies.begin(); copy != object->_shifted_copies.end(); ++copy) {
                    Polygon p = convex_hull;
                    p.translate(*copy);
                    
                    if (!intersection(a, p).empty())
                        throw InvalidPrintException{"Some objects are too close; your extruder will collide with them."};
                    
                    a = union_(a, p);
                }
            }
        }
        
        // check vertical clearance
        {
            std::vector<coord_t> object_height;
            FOREACH_OBJECT(this, i_object) {
                PrintObject* object = *i_object;
                object_height.insert(object_height.end(), object->copies().size(), object->size.z);
            }
            std::sort(object_height.begin(), object_height.end());
            // ignore the tallest *copy* (this is why we repeat height for all of them):
            // it will be printed as last one so its height doesn't matter
            object_height.pop_back();
            if (!object_height.empty() && object_height.back() > scale_(this->config.extruder_clearance_height.value))
                throw InvalidPrintException{"Some objects are too tall and cannot be printed without extruder collisions."};
        }
    } // end if (this->config.complete_objects)
    
    if (this->config.spiral_vase) {
        size_t total_copies_count = 0;
        FOREACH_OBJECT(this, i_object) total_copies_count += (*i_object)->copies().size();
        if (total_copies_count > 1 && !this->config.complete_objects.getBool())
            throw InvalidPrintException{"The Spiral Vase option can only be used when printing a single object."};
        if (this->regions.size() > 1)
            throw InvalidPrintException{"The Spiral Vase option can only be used when printing single material objects."};
    }
    
    if (this->extruders().empty())
        throw InvalidPrintException{"The supplied settings will cause an empty print."};
}

// the bounding box of objects placed in copies position
// (without taking skirt/brim/support material into account)
BoundingBox
Print::bounding_box() const
{
    BoundingBox bb;
    FOREACH_OBJECT(this, object) {
        for (Points::const_iterator copy = (*object)->_shifted_copies.begin(); copy != (*object)->_shifted_copies.end(); ++copy) {
            bb.merge(*copy);
            
            Point p = *copy;
            p.translate((*object)->size);
            bb.merge(p);
        }
    }
    return bb;
}

// the total bounding box of extrusions, including skirt/brim/support material
// this methods needs to be called even when no steps were processed, so it should
// only use configuration values
BoundingBox
Print::total_bounding_box() const
{
    // get objects bounding box
    BoundingBox bb = this->bounding_box();
    
    // we need to offset the objects bounding box by at least half the perimeters extrusion width
    Flow perimeter_flow = this->objects.front()->get_layer(0)->get_region(0)->flow(frPerimeter);
    double extra = perimeter_flow.width/2;
    
    // consider support material
    if (this->has_support_material()) {
        extra = std::max(extra, SUPPORT_MATERIAL_MARGIN);
    }
    
    // consider brim and skirt
    if (this->config.brim_width.value > 0) {
        Flow brim_flow = this->brim_flow();
        extra = std::max(extra, this->config.brim_width.value + brim_flow.width/2);
    }
    if (this->has_skirt()) {
        int skirts = this->config.skirts.value;
        if (skirts == 0 && this->has_infinite_skirt()) skirts = 1;
        Flow skirt_flow = this->skirt_flow();
        extra = std::max(
            extra,
            this->config.brim_width.value
                + this->config.skirt_distance.value
                + skirts * skirt_flow.spacing()
                + skirt_flow.width/2
        );
    }
    
    if (extra > 0)
        bb.offset(scale_(extra));
    
    return bb;
}

double
Print::skirt_first_layer_height() const
{
    if (this->objects.empty()) CONFESS("skirt_first_layer_height() can't be called without PrintObjects");
    return this->objects.front()->config.get_abs_value("first_layer_height");
}

// This will throw an exception when called without PrintObjects
Flow
Print::brim_flow() const
{
    ConfigOptionFloatOrPercent width = this->config.first_layer_extrusion_width;
    if (width.value == 0) width = this->regions.front()->config.perimeter_extrusion_width;
    if (width.value == 0) width = this->objects.front()->config.extrusion_width;
    
    /* We currently use a random region's perimeter extruder.
       While this works for most cases, we should probably consider all of the perimeter
       extruders and take the one with, say, the smallest index.
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    Flow flow = Flow::new_from_config_width(
        frPerimeter,
        width, 
        this->config.nozzle_diameter.get_at(this->regions.front()->config.perimeter_extruder-1),
        this->skirt_first_layer_height(),
        0
    );
    
    // Adjust extrusion width in order to fill the total brim width with an integer number of lines.
    flow.set_solid_spacing(this->config.brim_width.value);
    
    return flow;
}

// This will throw an exception when called without PrintObjects
Flow
Print::skirt_flow() const
{
    ConfigOptionFloatOrPercent width = this->config.first_layer_extrusion_width;
    if (width.value == 0) width = this->regions.front()->config.perimeter_extrusion_width;
    if (width.value == 0) width = this->objects.front()->config.extrusion_width;
    
    /* We currently use a random object's support material extruder.
       While this works for most cases, we should probably consider all of the support material
       extruders and take the one with, say, the smallest index;
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    return Flow::new_from_config_width(
        frPerimeter,
        width, 
        this->config.nozzle_diameter.get_at(this->objects.front()->config.support_material_extruder-1),
        this->skirt_first_layer_height(),
        0
    );
}

void
Print::_make_brim()
{
    if (this->state.is_done(psBrim)) return;
    this->state.set_started(psBrim);
    
    // since this method must be idempotent, we clear brim paths *before*
    // checking whether we need to generate them
    this->brim.clear();
    
    if (this->objects.empty()
        || (this->config.brim_width == 0
            && this->config.interior_brim_width == 0
            && this->config.brim_connections_width == 0)) {
        this->state.set_done(psBrim);
        return;
    }
    
    // brim is only printed on first layer and uses perimeter extruder
    const Flow flow  = this->brim_flow();
    const double mm3_per_mm = flow.mm3_per_mm();
    
    const coord_t grow_distance = flow.scaled_width()/2;
    Polygons islands;
    Points pt_ears;
    
    for (PrintObject* object : this->objects) {
        const Layer* layer0 = object->get_layer(0);
        
        Polygons object_islands = layer0->slices.contours();
        
        if (!object->support_layers.empty()) {
            const SupportLayer* support_layer0 = object->get_support_layer(0);
            
            for (const ExtrusionEntity* e : support_layer0->support_fills.entities)
                append_to(object_islands, offset(e->as_polyline(), grow_distance));
            
            for (const ExtrusionEntity* e : support_layer0->support_interface_fills.entities)
                append_to(object_islands, offset(e->as_polyline(), grow_distance));
        }
        for (const Point &copy : object->_shifted_copies) {
            for (Polygon p : object_islands) {
                p.translate(copy);
                islands.push_back(p);
                if(this->config.brim_ears)
                    for (const Point &p_corner : p.convex_points(this->config.brim_ears_max_angle.value * PI / 180.0)) {
                        pt_ears.push_back(p_corner);
                    }
            }
        }
    }

    Polygons loops;
    const int num_loops = floor(this->config.brim_width / flow.width + 0.5);
    for (int i = num_loops; i >= 1; --i) {
        // JT_SQUARE ensures no vertex is outside the given offset distance
        // -0.5 because islands are not represented by their centerlines
        // (first offset more, then step back - reverse order than the one used for 
        // perimeters because here we're offsetting outwards)
        append_to(loops, offset2(
            islands,
            (flow.scaled_width() * 0.5 + flow.scaled_spacing() *
             (i - 0.5 - this->config.brim_overlap.get_abs_value(1.0))),
            flow.scaled_spacing() * -0.5,
            100000,
            ClipperLib::jtSquare
        ));
    }
    
    if(this->config.brim_ears){
        
        //create ear pattern
        coord_t size_ear = (scale_(this->config.brim_width.value) - flow.scaled_spacing());
        Polygon point_round;
        point_round.points.push_back(Point(size_ear*1, 0*size_ear));
        point_round.points.push_back(Point(size_ear*0.966, 0.26*size_ear));
        point_round.points.push_back(Point(size_ear*0.87, 0.5*size_ear));
        point_round.points.push_back(Point(size_ear*0.7, 0.7*size_ear));
        point_round.points.push_back(Point(size_ear*0.5, 0.87*size_ear));
        point_round.points.push_back(Point(size_ear*0.26, 0.966*size_ear));
        point_round.points.push_back(Point(size_ear*0, 1*size_ear));
        point_round.points.push_back(Point(size_ear*-0.26, 0.966*size_ear));
        point_round.points.push_back(Point(size_ear*-0.5, 0.87*size_ear));
        point_round.points.push_back(Point(size_ear*-0.7, 0.7*size_ear));
        point_round.points.push_back(Point(size_ear*-0.87, 0.5*size_ear));
        point_round.points.push_back(Point(size_ear*-0.966, 0.26*size_ear));
        point_round.points.push_back(Point(size_ear*-1, 0*size_ear));
        point_round.points.push_back(Point(size_ear*-0.966, -0.26*size_ear));
        point_round.points.push_back(Point(size_ear*-0.87, -0.5*size_ear));
        point_round.points.push_back(Point(size_ear*-0.7, -0.7*size_ear));
        point_round.points.push_back(Point(size_ear*-0.5, -0.87*size_ear));
        point_round.points.push_back(Point(size_ear*-0.26, -0.966*size_ear));
        point_round.points.push_back(Point(size_ear*0, -1*size_ear));
        point_round.points.push_back(Point(size_ear*0.26, -0.966*size_ear));
        point_round.points.push_back(Point(size_ear*0.5, -0.87*size_ear));
        point_round.points.push_back(Point(size_ear*0.7, -0.7*size_ear));
        point_round.points.push_back(Point(size_ear*0.87, -0.5*size_ear));
        point_round.points.push_back(Point(size_ear*0.966, -0.26*size_ear));
        
        //create ears
        Polygons mouse_ears;
        for (Point pt : pt_ears) {
            mouse_ears.push_back(point_round);
            mouse_ears.back().translate(pt);
        }
        
        //intersection
        Polylines lines = intersection_pl(union_pt_chained(loops), mouse_ears);
        
        //reorder them
        Polylines lines_sorted;
        Polyline* previous = NULL;
        Polyline* best = NULL;
        double best_dist = -1;
        size_t best_idx = 0;
        while (lines.size() > 0) {
            if (previous == NULL) {
                lines_sorted.push_back(lines.back());
                previous = &lines_sorted.back();
                lines.erase(lines.end() - 1);
            } else {
                best = NULL;
                best_dist = -1;
                best_idx = 0;
                for (size_t i = 0; i < lines.size(); ++i) {
                    Polyline &viewed_line = lines[i];
                    double dist = viewed_line.points.front().distance_to(previous->points.front());
                    dist = std::min(dist, viewed_line.points.front().distance_to(previous->points.back()));
                    dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.front()));
                    dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.back()));
                    if (dist < best_dist || best == NULL) {
                        best = &viewed_line;
                        best_dist = dist;
                        best_idx = i;
                    }
                }
                if (best != NULL) {
                    //copy new line inside the sorted array.
                    lines_sorted.push_back(lines[best_idx]);
                    lines.erase(lines.begin() + best_idx);
                    
                    //connect if near enough
                    if (lines_sorted.size() > 1) {
                        size_t idx = lines_sorted.size() - 2;
                        bool connect = false;
                        if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                            connect = true;
                        } else if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                            lines_sorted[idx + 1].reverse();
                            connect = true;
                        } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                            lines_sorted[idx].reverse();
                            connect = true;
                        } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                            lines_sorted[idx].reverse();
                            lines_sorted[idx + 1].reverse();
                            connect = true;
                        }
                        
                        if (connect) {
                            //connect them
                            lines_sorted[idx].points.insert(
                                lines_sorted[idx].points.end(),
                                lines_sorted[idx + 1].points.begin(),
                                lines_sorted[idx + 1].points.end());
                            lines_sorted.erase(lines_sorted.begin() + idx + 1);
                            idx--;
                        }
                    }
                    
                    //update last position
                    previous = &lines_sorted.back();
                } else {
                    previous = NULL;
                }
                
            }
        }
        
        //push into extrusions
        for (Polyline &to_extrude : lines_sorted) {
            ExtrusionPath path(erSkirt, mm3_per_mm, flow.width, flow.height);
            path.polyline = to_extrude;
            this->brim.append(path);
        }
    }
    else
    {
        Polygons chained = union_pt_chained(loops);
        for (Polygons::const_reverse_iterator p = chained.rbegin(); p != chained.rend(); ++p) {
            ExtrusionPath path(erSkirt, mm3_per_mm, flow.width, flow.height);
            path.polyline = p->split_at_first_point();
            this->brim.append(ExtrusionLoop(path));
        }
    }
    
    if (this->config.brim_connections_width > 0) {
        // get islands to connect
        for (Polygon &p : islands)
            p = Geometry::convex_hull(p.points);
        
        islands = offset(islands, flow.scaled_spacing() * (num_loops-0.2), 10000, jtSquare);
        
        // compute centroid for each island
        Points centroids;
        centroids.reserve(islands.size());
        for (const Polygon &p : islands)
            centroids.push_back(p.centroid());
        
        // in order to check visibility we need to account for the connections width,
        // so let's use grown islands
        const double scaled_width = scale_(this->config.brim_connections_width);
        const Polygons grown = offset(islands, +scaled_width/2);
        
        // find pairs of islands having direct visibility
        Lines lines;
        for (size_t i = 0; i < islands.size(); ++i) {
            for (size_t j = (i+1); j < islands.size(); ++j) {
                // check visibility
                Line line(centroids[i], centroids[j]);
                if (diff_pl((Polyline)line, grown).size() != 1) continue;
                lines.push_back(line);
            }
        }
        
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
        filler->min_spacing  = flow.spacing();
        filler->dont_adjust  = true;
        filler->density      = 1;
        
        // subtract already generated connections in order to prevent crossings
        // and overextrusion
        Polygons other;
        
        for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line) {
            ExPolygons expp = diff_ex(
                offset((Polyline)*line, scaled_width/2),
                islands + other
            );
            
            filler->angle = line->direction();
            for (ExPolygons::const_iterator ex = expp.begin(); ex != expp.end(); ++ex) {
                append_to(other, (Polygons)*ex);
                
                const Polylines paths = filler->fill_surface(Surface(stBottom, *ex));
                for (Polylines::const_iterator pl = paths.begin(); pl != paths.end(); ++pl) {
                    ExtrusionPath path(erSkirt, mm3_per_mm, flow.width, flow.height);
                    path.polyline = *pl;
                    this->brim.append(path);
                }
            }
        }
    }
    
    if (this->config.interior_brim_width > 0) {
        // collect all island holes to fill
        Polygons holes;
        for (const PrintObject* object : this->objects) {
            const Layer &layer0 = *object->get_layer(0);
            
            Polygons o_holes = layer0.slices.holes();
            
            // When we have no infill on this layer, consider the internal part
            // of the model as a hole.
            for (const LayerRegion* layerm : layer0.regions) {
                if (layerm->fills.empty())
                    append_to(o_holes, (Polygons)layerm->fill_surfaces);
            }
            
            for (const Point &copy : object->_shifted_copies) {
                for (Polygon p : o_holes) {
                    p.translate(copy);
                    holes.push_back(p);
                }
            }
        }
        
        Polygons loops;
        const int num_loops = floor(this->config.interior_brim_width / flow.width + 0.5);
        for (int i = 1; i <= num_loops; ++i) {
            append_to(loops, offset2(
                holes,
                -flow.scaled_spacing() * (i + 0.5),
                flow.scaled_spacing()
            ));
        }
        
        loops = union_pt_chained(loops);
        for (const Polygon &p : loops) {
            ExtrusionPath path(erSkirt, mm3_per_mm, flow.width, flow.height);
            path.polyline = p.split_at_first_point();
            this->brim.append(ExtrusionLoop(path));
        }
    }
    
    this->state.set_done(psBrim);
}


PrintRegionConfig
Print::_region_config_from_model_volume(const ModelVolume &volume)
{
    PrintRegionConfig config = this->default_region_config;
    {
        DynamicPrintConfig other_config = volume.get_object()->config;
        other_config.normalize();
        config.apply(other_config, true);
    }
    {
        DynamicPrintConfig other_config = volume.config;
        other_config.normalize();
        config.apply(other_config, true);
    }
    if (!volume.material_id().empty()) {
        DynamicPrintConfig material_config = volume.material()->config;
        material_config.normalize();
        config.apply(material_config, true);
    }
    return config;
}

bool
Print::has_support_material() const
{
    FOREACH_OBJECT(this, object) {
        if ((*object)->has_support_material()) return true;
    }
    return false;
}

/*  This method assigns extruders to the volumes having a material
    but not having extruders set in the volume config. */
void
Print::auto_assign_extruders(ModelObject* model_object) const
{
    // only assign extruders if object has more than one volume
    if (model_object->volumes.size() < 2) return;
    
    for (ModelVolumePtrs::const_iterator v = model_object->volumes.begin(); v != model_object->volumes.end(); ++v) {
        if (!(*v)->material_id().empty()) {
            //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
            size_t extruder_id = (v - model_object->volumes.begin()) + 1;
            if (!(*v)->config.has("extruder"))
                (*v)->config.opt<ConfigOptionInt>("extruder", true)->value = extruder_id;
        }
    }
}

std::string
Print::output_filename()
{
    this->placeholder_parser.update_timestamp();
    return this->placeholder_parser.process(this->config.output_filename_format.value);
}

std::string
Print::output_filepath(const std::string &path)
{
    // if we were supplied no path, generate an automatic one based on our first object's input file
    if (path.empty()) {
        // get the first input file name
        std::string input_file;
        FOREACH_OBJECT(this, object) {
            input_file = (*object)->model_object()->input_file;
            if (!input_file.empty()) break;
        }
        return (boost::filesystem::path(input_file).parent_path() / this->output_filename()).string();
    }
    
    // if we were supplied a directory, use it and append our automatically generated filename
    boost::filesystem::path p(path);
    if (boost::filesystem::is_directory(p))
        return (p / this->output_filename()).string();
    
    // if we were supplied a file which is not a directory, use it
    return path;
}

}
