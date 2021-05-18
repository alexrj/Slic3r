#ifndef slic3r_Print_hpp_
#define slic3r_Print_hpp_

#include "libslic3r.h"
#include <set>
#include <string>
#include <vector>
#include <memory>
#include <exception>
#include <stdexcept>
#include <boost/thread.hpp>
#include "BoundingBox.hpp"
#include "Flow.hpp"
#include "PrintConfig.hpp"
#include "Config.hpp"
#include "Point.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "PlaceholderParser.hpp"
#include "SlicingAdaptive.hpp"
#include "LayerHeightSpline.hpp"
#include "SupportMaterial.hpp"

namespace Slic3r {

class InvalidPrintException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Print;
class PrintObject;
class ModelObject;
class SupportMaterial;

// Print step IDs for keeping track of the print state.
enum PrintStep {
    psSkirt, psBrim,
};
enum PrintObjectStep {
    posLayers, posSlice, posPerimeters, posDetectSurfaces,
    posPrepareInfill, posInfill, posSupportMaterial,
};

// To be instantiated over PrintStep or PrintObjectStep enums.
template <class StepType>
class PrintState
{
    public:
    std::set<StepType> started, done;
    
    bool is_started(StepType step) const;
    bool is_done(StepType step) const;
    void set_started(StepType step);
    void set_done(StepType step);
    bool invalidate(StepType step);
};

// A PrintRegion object represents a group of volumes to print
// sharing the same config (including the same assigned extruder(s))
class PrintRegion
{
    friend class Print;

    public:
    PrintRegionConfig config;

    Print* print();
    Flow flow(FlowRole role, double layer_height, bool bridge, bool first_layer, double width, const PrintObject &object) const;
    bool invalidate_state_by_config(const PrintConfigBase &config);

    private:
    Print* _print;
    
    PrintRegion(Print* print);
    ~PrintRegion();
};


typedef std::vector<Layer*> LayerPtrs;
typedef std::vector<SupportLayer*> SupportLayerPtrs;
class BoundingBoxf3;        // TODO: for temporary constructor parameter

class PrintObject
{
    friend class Print;

    public:
    /// map of (vectors of volume ids), indexed by region_id
    /// (we use map instead of vector so that we don't have to worry about
    /// resizing it and the [] operator adds new items automagically)
    std::map< size_t,std::vector<int> > region_volumes;
    PrintObjectConfig config; //< Configuration
    t_layer_height_ranges layer_height_ranges;
    
    LayerHeightSpline layer_height_spline;

    /// this is set to true when LayerRegion->slices is split in top/internal/bottom
    /// so that next call to make_perimeters() performs a union() before computing loops
    bool typed_slices;

    Point3 size;           //< XYZ in scaled coordinates

    // scaled coordinates to add to copies (to compensate for the alignment
    // operated when creating the object but still preserving a coherent API
    // for external callers)
    Point _copies_shift;

    // Slic3r::Point objects in scaled G-code coordinates in our coordinates
    Points _shifted_copies;

    LayerPtrs layers;
    SupportLayerPtrs support_layers;
    // TODO: Fill* fill_maker        => (is => 'lazy');
    PrintState<PrintObjectStep> state;
    
    Print* print() { return this->_print; };
    ModelObject* model_object() { return this->_model_object; };
    const ModelObject& model_object() const { return *(this->_model_object); };
    
    Points copies() const;
    bool add_copy(const Pointf &point);
    bool delete_last_copy();
    bool delete_all_copies();
    bool set_copies(const Points &points);
    bool reload_model_instances();
    BoundingBox bounding_box() const;
    std::set<size_t> extruders() const;
    std::set<size_t> support_material_extruders() const;
    
    // adds region_id, too, if necessary
    void add_region_volume(int region_id, int volume_id);

    size_t total_layer_count() const;
    size_t layer_count() const;
    void clear_layers();
    Layer* get_layer(int idx) { return this->layers.at(idx); };
    const Layer* get_layer(int idx) const { return this->layers.at(idx); };
    // print_z: top of the layer; slice_z: center of the layer.
    Layer* add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z);
    void delete_layer(int idx);

    SupportMaterial* _support_material();
    
    Flow _support_material_flow(FlowRole role = frSupportMaterial);
    size_t support_layer_count() const;
    void clear_support_layers();
    SupportLayer* get_support_layer(int idx) { return this->support_layers.at(idx); };
    const SupportLayer* get_support_layer(int idx) const { return this->support_layers.at(idx); };
    SupportLayer* add_support_layer(int id, coordf_t height, coordf_t print_z);
    void delete_support_layer(int idx);
    
    // methods for handling state
    bool invalidate_state_by_config(const PrintConfigBase &config);
    bool invalidate_step(PrintObjectStep step);
    bool invalidate_all_steps();
    
    bool has_support_material() const;
    void detect_surfaces_type();
    void process_external_surfaces();

    void bridge_over_infill();
    coordf_t adjust_layer_height(coordf_t layer_height) const;
    std::vector<coordf_t> generate_object_layers(coordf_t first_layer_height);
    void _slice();
    std::vector<ExPolygons> _slice_region(size_t region_id, std::vector<float> z, bool modifier);

    void _infill();

    /// Initialize and generate support material.
    void generate_support_material();

    /// Generate perimeters for this PrintObject.
    void make_perimeters();

    /// Combine fill surfaces across layers.
    /// Idempotence of this method is guaranteed by the fact that we don't remove things from
    /// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
    void combine_infill();

    /// Preparation step for generating infill.
    void prepare_infill();

    /// Generate infill for this PrintObject.
    void infill();

    /// Kick off the slice process for this object
    void slice();
    /// Find all horizontal shells in  this object
    void discover_horizontal_shells();

    /// Only active if config->infill_only_where_needed. This step trims the sparse infill,
    /// so it acts as an internal support. It maintains all other infill types intact.
    /// Here the internal surfaces and perimeters have to be supported by the sparse infill.
    ///FIXME The surfaces are supported by a sparse infill, but the sparse infill is only as large as the area to support.
    /// Likely the sparse infill will not be anchored correctly, so it will not work as intended.
    /// Also one wishes the perimeters to be supported by a full infill.
    /// Idempotence of this method is guaranteed by the fact that we don't remove things from
    /// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
    void clip_fill_surfaces();
    
    void _simplify_slices(double distance);
      
    private:
    Print* _print;
    ModelObject* _model_object;
    Points _copies;      // Slic3r::Point objects in scaled G-code coordinates

    // TODO: call model_object->get_bounding_box() instead of accepting
        // parameter
    PrintObject(Print* print, ModelObject* model_object, const BoundingBoxf3 &modobj_bbox);
    ~PrintObject();

    /// Outer loop of logic for horizontal shell discovery
    void _discover_external_horizontal_shells(LayerRegion* layerm, const size_t& i, const size_t& region_id);
    /// Inner loop of logic for horizontal shell discovery
    void _discover_neighbor_horizontal_shells(LayerRegion* layerm, const size_t& i, const size_t& region_id, const SurfaceType& type, Polygons& solid, const size_t& solid_layers);  

};

typedef std::vector<PrintObject*> PrintObjectPtrs;
typedef std::vector<PrintRegion*> PrintRegionPtrs;

// The complete print tray with possibly multiple objects.
class Print
{
    public:
    PrintConfig config;
    PrintObjectConfig default_object_config;
    PrintRegionConfig default_region_config;
    PrintObjectPtrs objects;
    PrintRegionPtrs regions;
    PlaceholderParser placeholder_parser;
    
    std::function<void(int, const std::string&)> status_cb {nullptr};

    /// Function pointer for the UI side to call post-processing scripts.
    /// Vector is assumed to be the executable script and all arguments.
    std::function<void(std::vector<std::string>)> post_process_cb {nullptr};
    
    double total_used_filament, total_extruded_volume, total_cost, total_weight;
    std::map<size_t,float> filament_stats;
    PrintState<PrintStep> state;

    // ordered collections of extrusion paths to build skirt loops and brim
    ExtrusionEntityCollection skirt;
    std::vector<ExtrusionEntityCollection> brim;
    double skirt_height_z {-1.0};

    Print();
    ~Print();
    
    // methods for handling objects
    void clear_objects();
    PrintObject* get_object(size_t idx) { return this->objects.at(idx); };
    const PrintObject* get_object(size_t idx) const { return this->objects.at(idx); };
    void delete_object(size_t idx);
    void reload_object(size_t idx);
    bool reload_model_instances();

    // methods for handling regions
    PrintRegion* get_region(size_t idx) { return this->regions.at(idx); };
    const PrintRegion* get_region(size_t idx) const { return this->regions.at(idx); };
    PrintRegion* add_region();

    /// Triggers the rest of the print process
    void process(); 

    /// Performs a gcode export.
    void export_gcode(std::ostream& output, bool quiet = false);
    
    /// Performs a gcode export and then runs post-processing scripts (if any)
    void export_gcode(std::string filename, bool quiet = false);

    /// commands a gcode export to a temporary file and return its name
    std::string export_gcode(bool quiet = false);
    
    // methods for handling state
    bool invalidate_state_by_config(const PrintConfigBase &config);
    bool invalidate_step(PrintStep step);
    bool invalidate_all_steps();
    bool step_done(PrintObjectStep step) const;
    
    void add_model_object(ModelObject* model_object, int idx = -1);
    #ifndef SLIC3RXS
    /// Apply a provided configuration to the internal copy
    bool apply_config(config_ptr config);
    #endif // SLIC3RXS
    bool apply_config(DynamicPrintConfig config);
    bool has_infinite_skirt() const;
    bool has_skirt() const;
    // Throws exceptions if print is not valid.
    void validate() const;
    BoundingBox bounding_box() const;
    BoundingBox total_bounding_box() const;
    double skirt_first_layer_height() const;
    Flow brim_flow() const;
    Flow skirt_flow() const;
    void _make_brim(size_t i);

    /// Generates a skirt around the union of all of 
    /// the objects in the print.
    void make_skirt();

    /// Generates a brim around all of the objects in the print.
    void make_brim();
    
    
    std::set<size_t> object_extruders() const;
    std::set<size_t> support_material_extruders() const;
    std::set<size_t> extruders() const;
    size_t brim_extruder() const;
    void _simplify_slices(double distance);
    double max_allowed_layer_height() const;
    bool has_support_material() const;
    void auto_assign_extruders(ModelObject* model_object) const;
    std::string output_filename();
    std::string output_filepath(const std::string &path);
    private:
    void clear_regions();
    void delete_region(size_t idx);
    PrintRegionConfig _region_config_from_model_volume(const ModelVolume &volume);
};

using shared_Print = std::shared_ptr<Print>;

#define FOREACH_BASE(type, container, iterator) for (type::const_iterator iterator = (container).begin(); iterator != (container).end(); ++iterator)
#define FOREACH_REGION(print, region)       FOREACH_BASE(PrintRegionPtrs, (print)->regions, region)
#define FOREACH_OBJECT(print, object)       FOREACH_BASE(PrintObjectPtrs, (print)->objects, object)
#define FOREACH_LAYER(object, layer)        FOREACH_BASE(LayerPtrs, (object)->layers, layer)
#define FOREACH_LAYERREGION(layer, layerm)  FOREACH_BASE(LayerRegionPtrs, (layer)->regions, layerm)

}

#endif
