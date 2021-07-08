#include "Print.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Log.hpp"
#include "TransformationMatrix.hpp"
#include <boost/version.hpp>
#if BOOST_VERSION >= 107300
#include <boost/bind/bind.hpp>
#endif
#include <algorithm>
#include <vector>
#include <limits>
#include <stdexcept>

namespace Slic3r {

#if BOOST_VERSION >= 107300
using boost::placeholders::_1;
#endif

PrintObject::PrintObject(Print* print, ModelObject* model_object, const BoundingBoxf3 &modobj_bbox)
:   layer_height_spline(model_object->layer_height_spline),
    typed_slices(false),
    _print(print),
    _model_object(model_object)
{
    // Compute the translation to be applied to our meshes so that we work with smaller coordinates
    {
        // Translate meshes so that our toolpath generation algorithms work with smaller
        // XY coordinates; this translation is an optimization and not strictly required.
        // A cloned mesh will be aligned to 0 before slicing in _slice_region() since we
        // don't assume it's already aligned and we don't alter the original position in model.
        // We store the XY translation so that we can place copies correctly in the output G-code
        // (copies are expressed in G-code coordinates and this translation is not publicly exposed).
        this->_copies_shift = Point(
            scale_(modobj_bbox.min.x), scale_(modobj_bbox.min.y));

        // Scale the object size and store it
        Pointf3 size = modobj_bbox.size();
        this->size = Point3(scale_(size.x), scale_(size.y), scale_(size.z));
    }
    
    this->reload_model_instances();
    this->layer_height_ranges = model_object->layer_height_ranges;
}

PrintObject::~PrintObject()
{
}

Points
PrintObject::copies() const
{
    return this->_copies;
}

bool
PrintObject::add_copy(const Pointf &point)
{
    Points points = this->_copies;
    points.push_back(Point::new_scale(point.x, point.y));
    return this->set_copies(points);
}

bool
PrintObject::delete_last_copy()
{
    Points points = this->_copies;
    points.pop_back();
    return this->set_copies(points);
}

bool
PrintObject::delete_all_copies()
{
    Points points;
    return this->set_copies(points);
}

bool
PrintObject::set_copies(const Points &points)
{
    this->_copies = points;
    
    // order copies with a nearest neighbor search and translate them by _copies_shift
    this->_shifted_copies.clear();
    this->_shifted_copies.reserve(points.size());
    
    // order copies with a nearest-neighbor search
    std::vector<Points::size_type> ordered_copies;
    Slic3r::Geometry::chained_path(points, ordered_copies);
    
    for (std::vector<Points::size_type>::const_iterator it = ordered_copies.begin(); it != ordered_copies.end(); ++it) {
        Point copy = points[*it];
        copy.translate(this->_copies_shift);
        this->_shifted_copies.push_back(copy);
    }
    
    bool invalidated = false;
    if (this->_print->invalidate_step(psSkirt)) invalidated = true;
    if (this->_print->invalidate_step(psBrim)) invalidated = true;
    return invalidated;
}

bool
PrintObject::reload_model_instances()
{
    Points copies;
    for (ModelInstancePtrs::const_iterator i = this->_model_object->instances.begin(); i != this->_model_object->instances.end(); ++i) {
        copies.push_back(Point::new_scale((*i)->offset.x, (*i)->offset.y));
    }
    return this->set_copies(copies);
}

BoundingBox
PrintObject::bounding_box() const
{
    // since the object is aligned to origin, bounding box coincides with size
    Points pp;
    pp.push_back(Point(0,0));
    pp.push_back(this->size);
    return BoundingBox(pp);
}

// returns 0-based indices of used extruders
std::set<size_t>
PrintObject::extruders() const
{
    std::set<size_t> extruders = this->_print->extruders();
    std::set<size_t> sm_extruders = this->support_material_extruders();
    extruders.insert(sm_extruders.begin(), sm_extruders.end());
    return extruders;
}

// returns 0-based indices of used extruders
std::set<size_t>
PrintObject::support_material_extruders() const
{
    std::set<size_t> extruders;
    if (this->has_support_material()) {
        extruders.insert(this->config.support_material_extruder - 1);
        extruders.insert(this->config.support_material_interface_extruder - 1);
    }
    return extruders;
}

void
PrintObject::add_region_volume(int region_id, int volume_id)
{
    region_volumes[region_id].push_back(volume_id);
}

/*  This is the *total* layer count (including support layers)
    this value is not supposed to be compared with Layer::id
    since they have different semantics */
size_t
PrintObject::total_layer_count() const
{
    return this->layer_count() + this->support_layer_count();
}

size_t
PrintObject::layer_count() const
{
    return this->layers.size();
}

void
PrintObject::clear_layers()
{
    for (int i = this->layers.size()-1; i >= 0; --i)
        this->delete_layer(i);
}

Layer*
PrintObject::add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    layers.emplace_back(new Layer(id, this, height, print_z, slice_z));
    return layers.back();
}

void
PrintObject::delete_layer(int idx)
{
    LayerPtrs::iterator i = this->layers.begin() + idx;
    delete *i;
    this->layers.erase(i);
}

size_t
PrintObject::support_layer_count() const
{
    return this->support_layers.size();
}

void
PrintObject::clear_support_layers()
{
    for (int i = this->support_layers.size()-1; i >= 0; --i)
        this->delete_support_layer(i);
}

SupportLayer*
PrintObject::add_support_layer(int id, coordf_t height, coordf_t print_z)
{
    SupportLayer* layer = new SupportLayer(id, this, height, print_z, -1);
    support_layers.push_back(layer);
    return layer;
}

void
PrintObject::delete_support_layer(int idx)
{
    SupportLayerPtrs::iterator i = this->support_layers.begin() + idx;
    delete *i;
    this->support_layers.erase(i);
}

bool
PrintObject::invalidate_state_by_config(const PrintConfigBase &config)
{
    const t_config_option_keys diff = this->config.diff(config);
    
    std::set<PrintObjectStep> steps;
    bool all = false;
    
    // this method only accepts PrintObjectConfig and PrintRegionConfig option keys
    for (const t_config_option_key &opt_key : diff) {
        if (opt_key == "layer_height"
            || opt_key == "first_layer_height"
            || opt_key == "adaptive_slicing"
            || opt_key == "adaptive_slicing_quality"
            || opt_key == "match_horizontal_surfaces"
            || opt_key == "regions_overlap") {
            steps.insert(posLayers);
        } else if (opt_key == "xy_size_compensation"
            || opt_key == "raft_layers") {
            steps.insert(posSlice);
        } else if (opt_key == "support_material_contact_distance") {
            steps.insert(posSlice);
            steps.insert(posPerimeters);
            steps.insert(posSupportMaterial);
        } else if (opt_key == "support_material") {
            steps.insert(posPerimeters);
            steps.insert(posSupportMaterial);
        } else if (opt_key == "support_material_angle"
            || opt_key == "support_material_extruder"
            || opt_key == "support_material_extrusion_width"
            || opt_key == "support_material_interface_layers"
            || opt_key == "support_material_interface_extruder"
            || opt_key == "support_material_interface_extrusion_width"
            || opt_key == "support_material_interface_spacing"
            || opt_key == "support_material_interface_speed"
            || opt_key == "support_material_buildplate_only"
            || opt_key == "support_material_pattern"
            || opt_key == "support_material_spacing"
            || opt_key == "support_material_threshold"
            || opt_key == "support_material_pillar_size"
            || opt_key == "support_material_pillar_spacing"
            || opt_key == "dont_support_bridges") {
            steps.insert(posSupportMaterial);
        } else if (opt_key == "interface_shells"
            || opt_key == "infill_only_where_needed") {
            steps.insert(posPrepareInfill);
        } else if (opt_key == "seam_position"
            || opt_key == "support_material_speed") {
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
        invalidated = this->invalidate_all_steps();
    } else {
        for (const PrintObjectStep &step : steps)
            if (this->invalidate_step(step))
                invalidated = true;
    }
    
    return invalidated;
}

bool
PrintObject::invalidate_step(PrintObjectStep step)
{
    bool invalidated = this->state.invalidate(step);
    
    // propagate to dependent steps
    if (step == posPerimeters) {
        invalidated |= this->invalidate_step(posPrepareInfill);
        invalidated |= this->_print->invalidate_step(psSkirt);
        invalidated |= this->_print->invalidate_step(psBrim);
    } else if (step == posDetectSurfaces) {
        invalidated |= this->invalidate_step(posPrepareInfill);
    } else if (step == posPrepareInfill) {
        invalidated |= this->invalidate_step(posInfill);
    } else if (step == posInfill) {
        invalidated |= this->_print->invalidate_step(psSkirt);
        invalidated |= this->_print->invalidate_step(psBrim);
    } else if (step == posSlice) {
        invalidated |= this->invalidate_step(posPerimeters);
        invalidated |= this->invalidate_step(posDetectSurfaces);
        invalidated |= this->invalidate_step(posSupportMaterial);
    }else if (step == posLayers) {
        invalidated |= this->invalidate_step(posSlice);
    } else if (step == posSupportMaterial) {
        invalidated |= this->_print->invalidate_step(psSkirt);
        invalidated |= this->_print->invalidate_step(psBrim);
    }
    
    return invalidated;
}

bool
PrintObject::invalidate_all_steps()
{
    // make a copy because when invalidating steps the iterators are not working anymore
    std::set<PrintObjectStep> steps = this->state.started;
    
    bool invalidated = false;
    for (std::set<PrintObjectStep>::const_iterator step = steps.begin(); step != steps.end(); ++step) {
        if (this->invalidate_step(*step)) invalidated = true;
    }
    return invalidated;
}

bool
PrintObject::has_support_material() const
{
    return this->config.support_material
        || this->config.raft_layers > 0
        || this->config.support_material_enforce_layers > 0;
}

// This will assign a type (top/bottom/internal) to layerm->slices
// and transform layerm->fill_surfaces from expolygon 
// to typed top/bottom/internal surfaces;
void
PrintObject::detect_surfaces_type()
{
    if (this->state.is_done(posDetectSurfaces)) return;
    this->state.set_started(posDetectSurfaces);
    
    // prerequisites
    this->slice();
    
    parallelize<Layer*>(
        std::queue<Layer*>(std::deque<Layer*>(this->layers.begin(), this->layers.end())),  // cast LayerPtrs to std::queue<Layer*>
        boost::bind(&Slic3r::Layer::detect_surfaces_type, _1),
        this->_print->config.threads.value
    );
    
    this->typed_slices = true;
    this->state.set_done(posDetectSurfaces);
}

void
PrintObject::process_external_surfaces()
{
    parallelize<Layer*>(
        std::queue<Layer*>(std::deque<Layer*>(this->layers.begin(), this->layers.end())),  // cast LayerPtrs to std::queue<Layer*>
        boost::bind(&Slic3r::Layer::process_external_surfaces, _1),
        this->_print->config.threads.value
    );
}

/* This method applies bridge flow to the first internal solid layer above
   sparse infill */
void
PrintObject::bridge_over_infill()
{
    FOREACH_REGION(this->_print, region) {
        const size_t region_id = region - this->_print->regions.begin();
        
        // skip bridging in case there are no voids
        if ((*region)->config.fill_density.value == 100) continue;
        
        // get bridge flow
        const Flow bridge_flow = (*region)->flow(
            frSolidInfill,
            -1,     // layer height, not relevant for bridge flow
            true,   // bridge
            false,  // first layer
            -1,     // custom width, not relevant for bridge flow
            *this
        );
        
        // get the average extrusion volume per surface unit
        const double mm3_per_mm  = bridge_flow.mm3_per_mm();
        const double mm3_per_mm2 = mm3_per_mm / bridge_flow.width;
        
        FOREACH_LAYER(this, layer_it) {
            // skip first layer
            if (layer_it == this->layers.begin()) continue;
            
            Layer* layer        = *layer_it;
            LayerRegion* layerm = layer->get_region(region_id);
            
            // extract the stInternalSolid surfaces that might be transformed into bridges
            Polygons internal_solid;
            layerm->fill_surfaces.filter_by_type((stInternal | stSolid), &internal_solid);
            if (internal_solid.empty()) continue;
            
            // check whether we should bridge or not according to density
            {
                // get the normal solid infill flow we would use if not bridging
                const Flow normal_flow = layerm->flow(frSolidInfill, false);
                
                // Bridging over sparse infill has two purposes:
                // 1) cover better the gaps of internal sparse infill, especially when
                //    printing at very low densities;
                // 2) provide a greater flow when printing very thin layers where normal
                //    solid flow would be very poor.
                // So we calculate density threshold as interpolation according to normal flow.
                // If normal flow would be equal or greater than the bridge flow, we can keep
                // a low threshold like 25% in order to bridge only when printing at very low
                // densities, when sparse infill has significant gaps.
                // If normal flow would be equal or smaller than half the bridge flow, we
                // use a higher threshold like 50% in order to bridge in more cases.
                // We still never bridge whenever fill density is greater than 50% because
                // we would overstuff.
                const float min_threshold = 25.0;
                const float max_threshold = 50.0;
                const float density_threshold = std::max(
                    std::min<float>(
                        min_threshold
                            + (max_threshold - min_threshold)
                            * (normal_flow.mm3_per_mm() - mm3_per_mm)
                            / (mm3_per_mm/2 - mm3_per_mm),
                        max_threshold
                    ),
                    min_threshold
                );
                
                if ((*region)->config.fill_density.value > density_threshold) continue;
            }
            
            // check whether the lower area is deep enough for absorbing the extra flow
            // (for obvious physical reasons but also for preventing the bridge extrudates
            // from overflowing in 3D preview)
            ExPolygons to_bridge;
            {
                Polygons to_bridge_pp = internal_solid;
                
                // Only bridge where internal infill exists below the solid shell matching
                // these two conditions:
                // 1) its depth is at least equal to our bridge extrusion diameter;
                // 2) its free volume (thus considering infill density) is at least equal
                //    to the volume needed by our bridge flow.
                double excess_mm3_per_mm2 = mm3_per_mm2;
                
                // iterate through lower layers spanned by bridge_flow
                const double bottom_z = layer->print_z - bridge_flow.height;
                for (int i = (layer_it - this->layers.begin()) - 1; i >= 0; --i) {
                    const Layer* lower_layer = this->layers[i];
                    
                    // subtract the void volume of this layer
                    excess_mm3_per_mm2 -= lower_layer->height * (100 - (*region)->config.fill_density.value)/100;
                    
                    // stop iterating if both conditions are matched
                    if (lower_layer->print_z < bottom_z && excess_mm3_per_mm2 <= 0) break;
                    
                    // iterate through regions and collect internal surfaces
                    Polygons lower_internal;
                    FOREACH_LAYERREGION(lower_layer, lower_layerm_it)
                        (*lower_layerm_it)->fill_surfaces.filter_by_type(stInternal, &lower_internal);
                    
                    // intersect such lower internal surfaces with the candidate solid surfaces
                    to_bridge_pp = intersection(to_bridge_pp, lower_internal);
                }
                
                // don't bridge if the volume condition isn't matched
                if (excess_mm3_per_mm2 > 0) continue;
                
                // there's no point in bridging too thin/short regions
                {
                    const double min_width = bridge_flow.scaled_width() * 3;
                    to_bridge_pp = offset2(to_bridge_pp, -min_width, +min_width);
                }
                
                if (to_bridge_pp.empty()) continue;
                
                // convert into ExPolygons
                to_bridge = union_ex(to_bridge_pp);
            }
            
            #ifdef SLIC3R_DEBUG
            printf("Bridging %zu internal areas at layer %zu\n", to_bridge.size(), layer->id());
            #endif
            
            // compute the remaining internal solid surfaces as difference
            const ExPolygons not_to_bridge = diff_ex(internal_solid, to_polygons(to_bridge), true);
            
            // build the new collection of fill_surfaces
            {
                Surfaces new_surfaces;
                for (Surfaces::const_iterator surface = layerm->fill_surfaces.surfaces.begin(); surface != layerm->fill_surfaces.surfaces.end(); ++surface) {
                    if (surface->surface_type != (stInternal | stSolid))
                        new_surfaces.push_back(*surface);
                }
                
                for (ExPolygons::const_iterator ex = to_bridge.begin(); ex != to_bridge.end(); ++ex)
                    new_surfaces.push_back(Surface( (stInternal | stBridge), *ex));
                
                for (ExPolygons::const_iterator ex = not_to_bridge.begin(); ex != not_to_bridge.end(); ++ex)
                    new_surfaces.push_back(Surface( (stInternal | stSolid), *ex));
                
                layerm->fill_surfaces.surfaces = new_surfaces;
            }
            
            /*
            # exclude infill from the layers below if needed
            # see discussion at https://github.com/slic3r/Slic3r/issues/240
            # Update: do not exclude any infill. Sparse infill is able to absorb the excess material.
            if (0) {
                my $excess = $layerm->extruders->{infill}->bridge_flow->width - $layerm->height;
                for (my $i = $layer_id-1; $excess >= $self->get_layer($i)->height; $i--) {
                    Slic3r::debugf "  skipping infill below those areas at layer %d\n", $i;
                    foreach my $lower_layerm (@{$self->get_layer($i)->regions}) {
                        my @new_surfaces = ();
                        # subtract the area from all types of surfaces
                        foreach my $group (@{$lower_layerm->fill_surfaces->group}) {
                            push @new_surfaces, map $group->[0]->clone(expolygon => $_),
                                @{diff_ex(
                                    [ map $_->p, @$group ],
                                    [ map @$_, @$to_bridge ],
                                )};
                            push @new_surfaces, map Slic3r::Surface->new(
                                expolygon       => $_,
                                surface_type    => S_TYPE_INTERNAL + S_TYPE_VOID,
                            ), @{intersection_ex(
                                [ map $_->p, @$group ],
                                [ map @$_, @$to_bridge ],
                            )};
                        }
                        $lower_layerm->fill_surfaces->clear;
                        $lower_layerm->fill_surfaces->append($_) for @new_surfaces;
                    }
                    
                    $excess -= $self->get_layer($i)->height;
                }
            }
            */
        }
    }
}

// adjust the layer height to the next multiple of the z full-step resolution
coordf_t PrintObject::adjust_layer_height(coordf_t layer_height) const
{
    coordf_t result = layer_height;
    if(this->_print->config.z_steps_per_mm > 0) {
        coordf_t min_dz = 1 / this->_print->config.z_steps_per_mm;
        result = int(layer_height / min_dz + 0.5) * min_dz;
    }

    return result > 0 ? result : layer_height;
}

// generate a vector of print_z coordinates in object coordinate system (starting with 0) but including
// the first_layer_height if provided.
std::vector<coordf_t> PrintObject::generate_object_layers(coordf_t first_layer_height) {

    std::vector<coordf_t> result;

    // collect values from config
    coordf_t min_nozzle_diameter = std::numeric_limits<double>::max();
    coordf_t min_layer_height = 0.0;
    coordf_t max_layer_height = std::numeric_limits<double>::max();
    std::set<size_t> object_extruders = this->_print->object_extruders();
    for (std::set<size_t>::const_iterator it_extruder = object_extruders.begin(); it_extruder != object_extruders.end(); ++ it_extruder) {
        min_nozzle_diameter = std::min(min_nozzle_diameter, this->_print->config.nozzle_diameter.get_at(*it_extruder));
        min_layer_height = std::max(min_layer_height, this->_print->config.min_layer_height.get_at(*it_extruder));
        max_layer_height = std::min(max_layer_height, this->_print->config.max_layer_height.get_at(*it_extruder));

    }
    coordf_t layer_height = std::min(min_nozzle_diameter, this->config.layer_height.getFloat());
    layer_height = this->adjust_layer_height(layer_height);
    this->config.layer_height.value = layer_height;

    // respect first layer height
    if(first_layer_height) {
        result.push_back(first_layer_height);
    }

    coordf_t print_z = first_layer_height;
    coordf_t height = first_layer_height;

    // Update object size at the spline object to define upper border
    this->layer_height_spline.setObjectHeight(unscale(this->size.z));
    if (this->state.is_done(posLayers)) {
        // layer heights are already generated, just update layers from spline
        // we don't need to respect first layer here, it's correctly provided by the spline object
        result = this->layer_height_spline.getInterpolatedLayers();
    } else { // create new set of layers
        // create stateful objects and variables for the adaptive slicing process
        SlicingAdaptive as;
        coordf_t adaptive_quality = this->config.adaptive_slicing_quality.value;
        if(this->config.adaptive_slicing.value) {
            const ModelVolumePtrs volumes = this->model_object()->volumes;
            for (ModelVolumePtrs::const_iterator it = volumes.begin(); it != volumes.end(); ++ it)
                if (! (*it)->modifier)
                    as.add_mesh(&(*it)->mesh);
            as.prepare(unscale(this->size.z));
        }

        // loop until we have at least one layer and the max slice_z reaches the object height
        while ((scale_(print_z + EPSILON)) < this->size.z) {

            if (this->config.adaptive_slicing.value) {
                height = 999;

                // determine next layer height
                height = as.next_layer_height(print_z, adaptive_quality, min_layer_height, max_layer_height);

                // check for horizontal features and object size
                if(this->config.match_horizontal_surfaces.value) {
                    coordf_t horizontal_dist = as.horizontal_facet_distance(print_z + height, min_layer_height);
                    if((horizontal_dist < min_layer_height) && (horizontal_dist > 0)) {
                        #ifdef SLIC3R_DEBUG
                        std::cout << "Horizontal feature ahead, distance: " << horizontal_dist << std::endl;
                        #endif
                        // can we shrink the current layer a bit?
                        if(height-(min_layer_height - horizontal_dist) > min_layer_height) {
                            // yes we can
                            height -= (min_layer_height - horizontal_dist);
                            #ifdef SLIC3R_DEBUG
                            std::cout << "Shrink layer height to " << height << std::endl;
                            #endif
                        }else{
                            // no, current layer would become too thin
                            height += horizontal_dist;
                            #ifdef SLIC3R_DEBUG
                            std::cout << "Widen layer height to " << height << std::endl;
                            #endif
                        }
                    }
                }
            }else{
                height = layer_height;
            }

            // look for an applicable custom range
            for (t_layer_height_ranges::const_iterator it_range = this->layer_height_ranges.begin(); it_range != this->layer_height_ranges.end(); ++ it_range) {
                if(print_z >= it_range->first.first && print_z <= it_range->first.second) {
                    if(it_range->second > 0) {
                        height = it_range->second;
                    }
                }
            }

            print_z += height;

            result.push_back(print_z);
        }

        // Store layer vector for interactive manipulation
        this->layer_height_spline.setLayers(result);
        if (this->config.adaptive_slicing.value) {
            // smoothing after adaptive algorithm
            result = this->layer_height_spline.getInterpolatedLayers();
            // remove top layer if empty
            coordf_t slice_z = result.back() - (result[result.size()-1] - result[result.size()-2])/2.0;
            if(slice_z > unscale(this->size.z)) {
                result.pop_back();
            }
        }

        // Reduce or thicken the top layer in order to match the original object size.
        // This is not actually related to z_steps_per_mm but we only enable it in case
        // user provided that value, as it means they really care about the layer height
        // accuracy and we don't provide unexpected result for people noticing the last
        // layer has a different layer height.
        if ((this->_print->config.z_steps_per_mm > 0 || this->config.adaptive_slicing.value) && result.size() > 1) {
            coordf_t diff = result.back() - unscale(this->size.z);
            int last_layer = result.size()-1;

            if (diff < 0) {
                // we need to thicken last layer
                coordf_t new_h = result[last_layer] - result[last_layer-1];
                if(this->config.adaptive_slicing.value) { // use min/max layer_height values from adaptive algo.
                    new_h = std::min(max_layer_height, new_h - diff); // add (negative) diff value
                }else{
                    new_h = std::min(min_nozzle_diameter, new_h - diff); // add (negative) diff value
                }
                result[last_layer] = result[last_layer-1] + new_h;
            } else {
                // we need to reduce last layer
                coordf_t new_h = result[last_layer] - result[last_layer-1];
                if(this->config.adaptive_slicing.value) { // use min/max layer_height values from adaptive algo.
                    new_h = std::max(min_layer_height, new_h - diff); // subtract (positive) diff value
                }else{
                    if(min_nozzle_diameter/2 < new_h) { //prevent generation of a too small layer
                        new_h = std::max(min_nozzle_diameter/2, new_h - diff); // subtract (positive) diff value
                    }
                }
                result[last_layer] = result[last_layer-1] + new_h;
            }
        }

        this->state.set_done(posLayers);
    }

    // push modified spline object back to model
    this->_model_object->layer_height_spline = this->layer_height_spline;

    // apply z-gradation.
    // For static layer height: the adjusted layer height is still useful
    // to have the layer height a multiple of a printable z-interval.
    // If we don't do this, we might get aliasing effects if a small error accumulates
    // over multiple layer until we get a slightly thicker layer.
    if(this->_print->config.z_steps_per_mm > 0) {
        coordf_t gradation = 1 / this->_print->config.z_steps_per_mm;
        coordf_t last_z = 0;
        coordf_t height;
        for(std::vector<coordf_t>::iterator l = result.begin(); l != result.end(); ++l) {
            height = *l - last_z;

            coordf_t gradation_effect = unscale((scale_(height)) % (scale_(gradation)));
            if(gradation_effect > gradation/2 && (height + (gradation-gradation_effect)) <= max_layer_height) { // round up
                height = height + (gradation-gradation_effect);
            }else{ // round down
                height = height - gradation_effect;
            }
            // limiting the height to max_ / min_layer_height can violate the gradation requirement
            // we now might exceed the layer height limits a bit, but that is probably better than having
            // systematic errors introduced by the steppers...
            //height = std::min(std::max(height, min_layer_height), max_layer_height);
            *l = last_z + height;
            last_z = *l;
        }
    }

    return result;
}

// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
//
// this should be idempotent
void PrintObject::_slice()
{

    coordf_t raft_height = 0;
    coordf_t first_layer_height = this->config.first_layer_height.get_abs_value(this->config.layer_height.value);


    // take raft layers into account
    int id = 0;

    if (this->config.raft_layers > 0) {
        id = this->config.raft_layers;

        coordf_t min_support_nozzle_diameter = 1.0;
        std::set<size_t> support_material_extruders = this->_print->support_material_extruders();
        for (std::set<size_t>::const_iterator it_extruder = support_material_extruders.begin(); it_extruder != support_material_extruders.end(); ++ it_extruder) {
            min_support_nozzle_diameter = std::min(min_support_nozzle_diameter, this->_print->config.nozzle_diameter.get_at(*it_extruder));
        }
        coordf_t support_material_layer_height = 0.75 * min_support_nozzle_diameter;

        // raise first object layer Z by the thickness of the raft itself
        // plus the extra distance required by the support material logic
        raft_height += first_layer_height;
        raft_height += support_material_layer_height * (this->config.raft_layers - 1);

        // reset for later layer generation
        first_layer_height = 0;

        // detachable support
        if(this->config.support_material_contact_distance > 0) {
            first_layer_height = min_support_nozzle_diameter;
            raft_height += this->config.support_material_contact_distance;

        }
    }

    // Initialize layers and their slice heights.
    std::vector<float> slice_zs;
    {
        this->clear_layers();
        // All print_z values for this object, without the raft.
        std::vector<coordf_t> object_layers = this->generate_object_layers(first_layer_height);
        // Reserve object layers for the raft. Last layer of the raft is the contact layer.
        slice_zs.reserve(object_layers.size());
        Layer *prev = nullptr;
        coordf_t lo = raft_height;
        coordf_t hi = lo;
        for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer++) {
            lo = hi;  // store old value
            hi = object_layers[i_layer] + raft_height;
            coordf_t slice_z = 0.5 * (lo + hi) - raft_height;
            Layer *layer = this->add_layer(id++, hi - lo, hi, slice_z);
            slice_zs.push_back(float(slice_z));
            if (prev != nullptr) {
                prev->upper_layer = layer;
                layer->lower_layer = prev;
            }
            // Make sure all layers contain layer region objects for all regions.
            for (size_t region_id = 0; region_id < this->_print->regions.size(); ++ region_id)
                layer->add_region(this->print()->regions[region_id]);
            prev = layer;
        }
    }

    if (this->print()->regions.size() == 1) {
        // Optimized for a single region. Slice the single non-modifier mesh.
        std::vector<ExPolygons> expolygons_by_layer = this->_slice_region(0, slice_zs, false);
        for (size_t layer_id = 0; layer_id < expolygons_by_layer.size(); ++ layer_id)
            this->layers[layer_id]->regions.front()->slices.append(std::move(expolygons_by_layer[layer_id]), stInternal);
    } else {
        // Slice all non-modifier volumes.
        for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id) {
            std::vector<ExPolygons> expolygons_by_layer = this->_slice_region(region_id, slice_zs, false);
            for (size_t layer_id = 0; layer_id < expolygons_by_layer.size(); ++ layer_id)
                this->layers[layer_id]->regions[region_id]->slices.append(std::move(expolygons_by_layer[layer_id]), stInternal);
        }
        // Slice all modifier volumes.
        for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id) {
            std::vector<ExPolygons> expolygons_by_layer = this->_slice_region(region_id, slice_zs, true);
            // loop through the other regions and 'steal' the slices belonging to this one
            for (size_t other_region_id = 0; other_region_id < this->print()->regions.size(); ++ other_region_id) {
                if (region_id == other_region_id)
                    continue;
                for (size_t layer_id = 0; layer_id < expolygons_by_layer.size(); ++ layer_id) {
                    Layer       *layer = layers[layer_id];
                    LayerRegion *layerm = layer->regions[region_id];
                    LayerRegion *other_layerm = layer->regions[other_region_id];
                    if (layerm == nullptr || other_layerm == nullptr)
                        continue;
                    Polygons other_slices = to_polygons(other_layerm->slices);
                    ExPolygons my_parts = intersection_ex(other_slices, to_polygons(expolygons_by_layer[layer_id]));
                    if (my_parts.empty())
                        continue;
                    // Remove such parts from original region.
                    other_layerm->slices.set(diff_ex(other_slices, to_polygons(my_parts)), stInternal);
                    // Append new parts to our region.
                    layerm->slices.append(std::move(my_parts), stInternal);
                }
            }
        }
    }

    // remove last layer(s) if empty
    bool done = false;
    while (! this->layers.empty()) {
        const Layer *layer = this->layers.back();
        for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id)
            if (layer->regions[region_id] != nullptr && ! layer->regions[region_id]->slices.empty()) {
                done = true;
                break;
            }
        if(done) {
            break;
        }
        this->delete_layer(int(this->layers.size()) - 1);
    }

    // remove collinear points from slice polygons (artifacts from stl-triangulation)
    std::queue<SurfaceCollection*> queue;
    for (Layer* layer : this->layers) {
        for (LayerRegion* layerm : layer->regions) {
            queue.push(&layerm->slices);
        }
    }
    parallelize<SurfaceCollection*>(
        queue,
        boost::bind(&Slic3r::SurfaceCollection::remove_collinear_points, _1),
        this->_print->config.threads.value
    );

    // Apply size compensation and perform clipping of multi-part objects.
    const coord_t xy_size_compensation = scale_(this->config.xy_size_compensation.value);
    for (Layer* layer : this->layers) {
        if (abs(xy_size_compensation) > 0) {
            if (layer->regions.size() == 1) {
                // Single region, growing or shrinking.
                LayerRegion* layerm = layer->regions.front();
                layerm->slices.set(
                    offset_ex(to_expolygons(std::move(layerm->slices.surfaces)), xy_size_compensation),
                    stInternal
                );
            } else {
                // Multiple regions, growing, shrinking or just clipping one region by the other.
                // When clipping the regions, priority is given to the first regions.
                Polygons processed;
                for (size_t region_id = 0; region_id < layer->regions.size(); ++region_id) {
                    LayerRegion* layerm = layer->regions[region_id];
                    Polygons slices = layerm->slices;
                    
                    if (abs(xy_size_compensation) > 0)
                        slices = offset(slices, xy_size_compensation);
                    
                    if (region_id > 0)
                        // Trim by the slices of already processed regions.
                        slices = diff(std::move(slices), processed);
                    
                    if (region_id + 1 < layer->regions.size())
                        // Collect the already processed regions to trim the to be processed regions.
                        append_to(processed, slices);
                    
                    layerm->slices.set(union_ex(slices), stInternal);
                }
            }
        }
        
        // Merge all regions' slices to get islands, chain them by a shortest path.
        layer->make_slices();
        
        // Apply regions overlap
        if (this->config.regions_overlap.value > 0) {
            const coord_t delta = scale_(this->config.regions_overlap.value)/2;
            for (LayerRegion* layerm : layer->regions)
                layerm->slices.set(
                    intersection_ex(
                        offset(layerm->slices, +delta),
                        layer->slices
                    ),
                    stInternal
                );
        }
    }
}

// called from slice()
std::vector<ExPolygons>
PrintObject::_slice_region(size_t region_id, std::vector<float> z, bool modifier)
{
    std::vector<ExPolygons> layers;
    std::vector<int> &region_volumes = this->region_volumes[region_id];
    if (region_volumes.empty()) return layers;
    
    ModelObject &object = *this->model_object();
    
    // compose mesh
    TriangleMesh mesh;

    // we ignore the per-instance transformations currently and only 
    // consider the first one
    TransformationMatrix trafo = object.instances[0]->get_trafo_matrix(true);

    // align mesh to Z = 0 (it should be already aligned actually) and apply XY shift
    trafo.applyLeft(TransformationMatrix::mat_translation(
        -unscale(this->_copies_shift.x),
        -unscale(this->_copies_shift.y),
        -object.bounding_box().min.z
    ));

    for (const auto& i : region_volumes) {
        
        const ModelVolume &volume = *(object.volumes[i]);

        if (volume.modifier != modifier) continue;
        
        mesh.merge(volume.get_transformed_mesh(trafo));
    }
    if (mesh.facets_count() == 0) return layers;

    // perform actual slicing
    TriangleMeshSlicer<Z>(&mesh).slice(z, &layers);
    return layers;
}

/*
    1) Decides Z positions of the layers,
    2) Initializes layers and their regions
    3) Slices the object meshes
    4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
    5) Applies size compensation (offsets the slices in XY plane)
    6) Replaces bad slices by the slices reconstructed from the upper/lower layer
    Resulting expolygons of layer regions are marked as Internal.

    This should be idempotent.
*/
void
PrintObject::slice()
{
    if (this->state.is_done(posSlice)) return;
    this->state.set_started(posSlice);
    if (_print->status_cb != nullptr) {
        _print->status_cb(10, "Processing triangulated mesh");
    }
    
    this->_slice(); 

    // detect slicing errors
    if (std::any_of(this->layers.cbegin(), this->layers.cend(),
        [](const Layer* l){ return l->slicing_errors; }))
        Slic3r::Log::warn("PrintObject") << "The model has overlapping or self-intersecting facets. " 
                                         << "I tried to repair it, however you might want to check " 
                                         << "the results or repair the input file and retry.\n";
    
    bool warning_thrown = false;
    for (size_t i = 0; i < this->layer_count(); ++i) {
        Layer* layer{ this->get_layer(i) };
        if (!layer->slicing_errors) continue;
        if (!warning_thrown) {
            Slic3r::Log::warn("PrintObject") << "The model has overlapping or self-intersecting facets. " 
                                             << "I tried to repair it, however you might want to check " 
                                             << "the results or repair the input file and retry.\n";
            warning_thrown = true;
        }
        
        // try to repair the layer surfaces by merging all contours and all holes from
        // neighbor layers
        #ifdef SLIC3R_DEBUG
        std::cout << "Attempting to repair layer " << i << std::endl;
        #endif
        
        for (size_t region_id = 0; region_id < layer->region_count(); ++region_id) {
            LayerRegion* layerm{ layer->get_region(region_id) };
            
            ExPolygons slices;
            for (size_t j = i+1; j < this->layer_count(); ++j) {
                const Layer* upper = this->get_layer(j);
                if (!upper->slicing_errors) {
                    append_to(slices, (ExPolygons)upper->get_region(region_id)->slices);
                    break;
                }
            }
            for (int j = i-1; j >= 0; --j) {
                const Layer* lower = this->get_layer(j);
                if (!lower->slicing_errors) {
                    append_to(slices, (ExPolygons)lower->get_region(region_id)->slices);
                    break;
                }
            }
            
            // TODO: do we actually need to split contours and holes before performing the diff?
            Polygons contours, holes;
            for (ExPolygon ex : slices)
                contours.push_back(ex.contour);
            for (ExPolygon ex : slices)
                append_to(holes, ex.holes);
            
            const ExPolygons diff = diff_ex(contours, holes);
            
            layerm->slices.clear();
            layerm->slices.append(diff, stInternal);
        }
            
        // update layer slices after repairing the single regions
        layer->make_slices();
    }
    
    // remove empty layers from bottom
    while (!this->layers.empty() && this->get_layer(0)->slices.empty()) {
        this->delete_layer(0);
        for (Layer* layer : this->layers)
            layer->set_id(layer->id()-1);
    }
    
    // simplify slices if required
    if (this->_print->config.resolution() > 0)
        this->_simplify_slices(scale_(this->_print->config.resolution()));
    
    if (this->layers.empty()) {
        Slic3r::Log::error("PrintObject") << "slice(): " << "No layers were detected. You might want to repair your STL file(s) or check their size or thickness and retry.\n";
        return; // make this throw an exception instead?
    }
    
    this->typed_slices = false;
    this->state.set_done(posSlice);
}

void
PrintObject::make_perimeters()
{
    if (this->state.is_done(posPerimeters)) return;
    
    // Temporary workaround for detect_surfaces_type() not being idempotent (see #3764).
    // We can remove this when idempotence is restored. This make_perimeters() method
    // will just call merge_slices() to undo the typed slices and invalidate posDetectSurfaces.
    if (this->typed_slices) {
        this->invalidate_step(posSlice);
    }
    this->state.set_started(posPerimeters);

    // prerequisites
    this->slice();
    
    // merge slices if they were split into types
    // This is not currently taking place because since merge_slices + detect_surfaces_type
    // are not truly idempotent we are invalidating posSlice here (see the Perl part of 
    // this method).
    if (this->typed_slices) {
        // merge_slices() undoes detect_surfaces_type()
        FOREACH_LAYER(this, layer_it)
            (*layer_it)->merge_slices();
        this->typed_slices = false;
        this->state.invalidate(posDetectSurfaces);
    }
    
    // compare each layer to the one below, and mark those slices needing
    // one additional inner perimeter, like the top of domed objects-
    
    // this algorithm makes sure that at least one perimeter is overlapping
    // but we don't generate any extra perimeter if fill density is zero, as they would be floating
    // inside the object - infill_only_where_needed should be the method of choice for printing
    // hollow objects
    FOREACH_REGION(this->_print, region_it) {
        size_t region_id = region_it - this->_print->regions.begin();
        const PrintRegion &region = **region_it;
        
        if (!region.config.extra_perimeters
            || region.config.perimeters == 0
            || region.config.fill_density == 0
            || this->layer_count() < 2) continue;
        
        for (size_t i = 0; i <= (this->layer_count()-2); ++i) {
            LayerRegion &layerm                     = *this->get_layer(i)->get_region(region_id);
            const LayerRegion &upper_layerm         = *this->get_layer(i+1)->get_region(region_id);
            
            // In order to avoid diagonal gaps (GH #3732) we ignore the external half of the upper
            // perimeter, since it's not truly covering this layer.
            const Polygons upper_layerm_polygons = offset(
                upper_layerm.slices,
                -upper_layerm.flow(frExternalPerimeter).scaled_width()/2
            );
            
            // Filter upper layer polygons in intersection_ppl by their bounding boxes?
            // my $upper_layerm_poly_bboxes= [ map $_->bounding_box, @{$upper_layerm_polygons} ];
            double total_loop_length = 0;
            for (Polygons::const_iterator it = upper_layerm_polygons.begin(); it != upper_layerm_polygons.end(); ++it)
                total_loop_length += it->length();
            
            const coord_t perimeter_spacing     = layerm.flow(frPerimeter).scaled_spacing();
            const Flow ext_perimeter_flow       = layerm.flow(frExternalPerimeter);
            const coord_t ext_perimeter_width   = ext_perimeter_flow.scaled_width();
            const coord_t ext_perimeter_spacing = ext_perimeter_flow.scaled_spacing();
            
            for (Surfaces::iterator slice = layerm.slices.surfaces.begin();
                slice != layerm.slices.surfaces.end(); ++slice) {
                while (true) {
                    // compute the total thickness of perimeters
                    const coord_t perimeters_thickness = ext_perimeter_width/2 + ext_perimeter_spacing/2
                        + (region.config.perimeters-1 + slice->extra_perimeters) * perimeter_spacing;
                    
                    // define a critical area where we don't want the upper slice to fall into
                    // (it should either lay over our perimeters or outside this area)
                    const coord_t critical_area_depth = perimeter_spacing * 1.5;
                    const Polygons critical_area = diff(
                        offset(slice->expolygon, -perimeters_thickness),
                        offset(slice->expolygon, -(perimeters_thickness + critical_area_depth))
                    );
                    
                    // check whether a portion of the upper slices falls inside the critical area
                    const Polylines intersection = intersection_pl(
                        upper_layerm_polygons,
                        critical_area
                    );
                    
                    // only add an additional loop if at least 30% of the slice loop would benefit from it
                    {
                        double total_intersection_length = 0;
                        for (Polylines::const_iterator it = intersection.begin(); it != intersection.end(); ++it)
                            total_intersection_length += it->length();
                        if (total_intersection_length <= total_loop_length*0.3) break;
                    }
                    
                    /*
                    if (0) {
                        require "Slic3r/SVG.pm";
                        Slic3r::SVG::output(
                            "extra.svg",
                            no_arrows   => 1,
                            expolygons  => union_ex($critical_area),
                            polylines   => [ map $_->split_at_first_point, map $_->p, @{$upper_layerm->slices} ],
                        );
                    }
                    */
                    
                    slice->extra_perimeters++;
                }
                
                #ifdef DEBUG
                    if (slice->extra_perimeters > 0)
                        printf("  adding %d more perimeter(s) at layer %zu\n", slice->extra_perimeters, i);
                #endif
            }
        }
    }
    
    parallelize<Layer*>(
        std::queue<Layer*>(std::deque<Layer*>(this->layers.begin(), this->layers.end())),  // cast LayerPtrs to std::queue<Layer*>
        boost::bind(&Slic3r::Layer::make_perimeters, _1),
        this->_print->config.threads.value
    );
    
    /*
        simplify slices (both layer and region slices),
        we only need the max resolution for perimeters
    ### This makes this method not-idempotent, so we keep it disabled for now.
    ###$self->_simplify_slices(&Slic3r::SCALED_RESOLUTION);
    */
    
    this->state.set_done(posPerimeters);
}

void
PrintObject::infill()
{
    if (this->state.is_done(posInfill)) return;
    this->state.set_started(posInfill);
    
    // prerequisites
    this->prepare_infill();
    
    parallelize<Layer*>(
        std::queue<Layer*>(std::deque<Layer*>(this->layers.begin(), this->layers.end())),  // cast LayerPtrs to std::queue<Layer*>
        boost::bind(&Slic3r::Layer::make_fills, _1),
        this->_print->config.threads.value
    );
    
    /*  we could free memory now, but this would make this step not idempotent
    ### $_->fill_surfaces->clear for map @{$_->regions}, @{$object->layers};
    */
    
    this->state.set_done(posInfill);
}

void
PrintObject::prepare_infill()
{
    if (this->state.is_done(posPrepareInfill)) return;
    
    // This prepare_infill() is not really idempotent.
    // TODO: It should clear and regenerate fill_surfaces at every run 
    // instead of modifying it in place.

    this->state.invalidate(posPerimeters);
    this->make_perimeters();

    this->state.set_started(posPrepareInfill);

    // prerequisites
    this->detect_surfaces_type();

    if (this->_print->status_cb != nullptr) 
        this->_print->status_cb(30, "Preparing infill");
    
    // decide what surfaces are to be filled
    for (auto& layer : this->layers)
        for (auto& layerm : layer->regions)
            layerm->prepare_fill_surfaces();

    // this will detect bridges and reverse bridges
    // and rearrange top/bottom/internal surfaces
    this->process_external_surfaces();

    // detect which fill surfaces are near external layers
    // they will be split in internal and internal-solid surfaces
    this->discover_horizontal_shells();
    this->clip_fill_surfaces();

    // the following step needs to be done before combination because it may need
    // to remove only half of the combined infill
    this->bridge_over_infill();

    // combine fill surfaces to honor the "infill every N layers" option
    this->combine_infill();

    this->state.set_done(posPrepareInfill);
}


// combine fill surfaces across layers
// Idempotence of this method is guaranteed by the fact that we don't remove things from
// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
void
PrintObject::combine_infill()
{
    // Work on each region separately.
    for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id) {
        const PrintRegion *region = this->print()->regions[region_id];
        const int every = region->config.infill_every_layers();
        if (every < 2 || region->config.fill_density == 0.)
            continue;
        
        // Limit the number of combined layers to the maximum height allowed by this regions' nozzle.
        // FIXME: limit the layer height to max_layer_height
        const double nozzle_diameter = std::min(
            this->_print->config.nozzle_diameter.get_at(region->config.infill_extruder.value - 1),
            this->_print->config.nozzle_diameter.get_at(region->config.solid_infill_extruder.value - 1)
        );
        
        // define the combinations
        std::vector<size_t> combine(this->layers.size(), 0); // layer_idx => number of additional combined lower layers
        {
            double current_height = 0.;
            size_t num_layers = 0;
            for (size_t layer_idx = 0; layer_idx < this->layers.size(); ++layer_idx) {
                const Layer *layer = this->layers[layer_idx];
                
                // Skip first print layer (which may not be first layer in array because of raft).
                if (layer->id() == 0)
                    continue;
                
                // Check whether the combination of this layer with the lower layers' buffer
                // would exceed max layer height or max combined layer count.
                if (current_height + layer->height >= nozzle_diameter + EPSILON || num_layers >= static_cast<size_t>(every) ) {
                    // Append combination to lower layer.
                    combine[layer_idx - 1] = num_layers;
                    current_height = 0.;
                    num_layers = 0;
                }
                current_height += layer->height;
                ++num_layers;
            }

            // Append lower layers (if any) to uppermost layer.
            combine[this->layers.size() - 1] = num_layers;
        }

        // loop through layers to which we have assigned layers to combine
        for (size_t layer_idx = 0; layer_idx < combine.size(); ++layer_idx) {
            const size_t& num_layers = combine[layer_idx];
            if (num_layers <= 1)
                continue;
            
            // Get all the LayerRegion objects to be combined.
            std::vector<LayerRegion*> layerms;
            layerms.reserve(num_layers);
            for (size_t i = layer_idx + 1 - num_layers; i <= layer_idx; ++i)
                layerms.push_back(this->layers[i]->regions[region_id]);
            
            // We need to perform a multi-layer intersection, so let's split it in pairs.
            
            // Initialize the intersection with the candidates of the lowest layer.
            ExPolygons intersection = to_expolygons(layerms.front()->fill_surfaces.filter_by_type(stInternal));
            
            // Start looping from the second layer and intersect the current intersection with it.
            for (size_t i = 1; i < layerms.size(); ++i)
                intersection = intersection_ex(
                    to_polygons(intersection),
                    to_polygons(layerms[i]->fill_surfaces.filter_by_type(stInternal))
                );
            
            // Remove ExPolygons whose area is <= infill_area_threshold()
            const double area_threshold = layerms.front()->infill_area_threshold();
            intersection.erase(std::remove_if(intersection.begin(), intersection.end(), 
                [area_threshold](const ExPolygon &expoly) { return expoly.area() <= area_threshold; }), 
                intersection.end());
            
            if (intersection.empty())
                continue;
            
            #ifdef SLIC3R_DEBUG
            std::cout << "  combining " << intersection.size()
                << " internal regions from layers " << (layer_idx-(every-1))
                << "-" << layer_idx << std::endl;
            #endif
            
            // intersection now contains the regions that can be combined across the full amount of layers,
            // so let's remove those areas from all layers.
            
            const float clearance_offset = 
                0.5f * layerms.back()->flow(frPerimeter).scaled_width() +
                    // Because fill areas for rectilinear and honeycomb are grown 
                    // later to overlap perimeters, we need to counteract that too.
                    ((region->config.fill_pattern == ipRectilinear   ||
                      region->config.fill_pattern == ipGrid          ||
                      region->config.fill_pattern == ipHoneycomb) ? 1.5f : 0.5f)
                      * layerms.back()->flow(frSolidInfill).scaled_width();
            
            Polygons intersection_with_clearance;
            intersection_with_clearance.reserve(intersection.size());
            for (const ExPolygon &expoly : intersection)
                polygons_append(intersection_with_clearance, offset(expoly, clearance_offset));
            
            for (LayerRegion *layerm : layerms) {
                const Polygons internal = to_polygons(layerm->fill_surfaces.filter_by_type(stInternal));
                layerm->fill_surfaces.remove_type(stInternal);
                
                layerm->fill_surfaces.append(
                    diff_ex(internal, intersection_with_clearance),
                    stInternal
                );
                
                if (layerm == layerms.back()) {
                    // Apply surfaces back with adjusted depth to the uppermost layer.
                    Surface templ(stInternal, ExPolygon());
                    templ.thickness = 0.;
                    for (const LayerRegion *layerm2 : layerms)
                        templ.thickness += layerm2->layer()->height;
                    templ.thickness_layers = (unsigned short)layerms.size();
                    layerm->fill_surfaces.append(intersection, templ);
                } else {
                    // Save void surfaces.
                    layerm->fill_surfaces.append(
                            intersection_ex(internal, intersection_with_clearance),
                            (stInternal | stVoid));
                }
            }
        }
    }
}

SupportMaterial *
PrintObject::_support_material()
{
    // TODO what does this line do //= FLOW_ROLE_SUPPORT_MATERIAL;
    Flow first_layer_flow = Flow::new_from_config_width(
        frSupportMaterial,
        print()->config
            .first_layer_extrusion_width,  // check why this line is put || config.support_material_extrusion_width,
        static_cast<float>(print()->config.nozzle_diameter.get_at(static_cast<size_t>(
                                                                      config.support_material_extruder
                                                                          - 1))), // Check why this is put in perl "// $self->print->config->nozzle_diameter->[0]"
        static_cast<float>(config.get_abs_value("first_layer_height")),
        0 // No bridge flow ratio.
    );

    return new SupportMaterial(
        &print()->config,
        &config,
        first_layer_flow,
        _support_material_flow(),
        _support_material_flow(frSupportMaterialInterface)
    );
}

Flow
PrintObject::_support_material_flow(FlowRole role)
{
    const int extruder = (role == frSupportMaterial)
        ? this->config.support_material_extruder.value
        : this->config.support_material_interface_extruder.value;

    auto width = this->config.support_material_extrusion_width;
    if (width.value == 0) width = this->config.extrusion_width;

    if (role == frSupportMaterialInterface
        && this->config.support_material_interface_extrusion_width.value > 0)
        width = this->config.support_material_interface_extrusion_width;
    
    // We use a bogus layer_height because we use the same flow for all
    // support material layers.
    return Flow::new_from_config_width(
        role,
        width,
        this->_print->config.nozzle_diameter.get_at(extruder-1),
        this->config.layer_height.value,
        0
    );
}

void
PrintObject::generate_support_material() 
{
    //prereqs 
    this->slice();
    if (this->state.is_done(posSupportMaterial)) { return; }

    this->state.set_started(posSupportMaterial); 

    this->clear_support_layers();

    if ((!config.support_material
                && config.raft_layers == 0
                && config.support_material_enforce_layers == 0)
            || this->layers.size() < 2
       ) {
        this->state.set_done(posSupportMaterial);
        return;
    }
    if (_print->status_cb != nullptr)
        _print->status_cb(85, "Generating support material");

    this->_support_material()->generate(this);

    this->state.set_done(posSupportMaterial);

    std::stringstream stats {""};

    if (_print->status_cb != nullptr)
        _print->status_cb(85, stats.str().c_str());

}


void 
PrintObject::discover_horizontal_shells()
{
    #ifdef SLIC3R_DEBUG
    std::cout << "==> DISCOVERING HORIZONTAL SHELLS" << std::endl;
    #endif
    
    for (size_t region_id = 0U; region_id < _print->regions.size(); ++region_id) {
        for (size_t i = 0; i < this->layer_count(); ++i) {
            auto* layerm = this->get_layer(i)->get_region(region_id);
            const auto& region_config = layerm->region()->config;

            if (region_config.solid_infill_every_layers() > 0 && region_config.fill_density() > 0
                && (i % region_config.solid_infill_every_layers()) == 0) {
                const auto type = region_config.fill_density() == 100 ? (stInternal | stSolid) : (stInternal | stBridge);
                for (auto* s : layerm->fill_surfaces.filter_by_type(stInternal))
                    s->surface_type = type;
            }
            this->_discover_external_horizontal_shells(layerm, i, region_id);
        }
    }
}

void
PrintObject::_discover_external_horizontal_shells(LayerRegion* layerm, const size_t& i, const size_t& region_id)
{
    const auto& region_config = layerm->region()->config;
    for (auto& type : { stTop, stBottom, (stBottom | stBridge) }) {
        // find slices of current type for current layer
        // use slices instead of fill_surfaces because they also include the perimeter area
        // which needs to be propagated in shells; we need to grow slices like we did for
        // fill_surfaces though.  Using both ungrown slices and grown fill_surfaces will
        // not work in some situations, as there won't be any grown region in the perimeter 
        // area (this was seen in a model where the top layer had one extra perimeter, thus
        // its fill_surfaces were thinner than the lower layer's infill), however it's the best
        // solution so far. Growing the external slices by EXTERNAL_INFILL_MARGIN will put
        // too much solid infill inside nearly-vertical slopes.
      
        Polygons solid; 
        polygons_append(solid, to_polygons(layerm->slices.filter_by_type(type)));
        polygons_append(solid, to_polygons(layerm->fill_surfaces.filter_by_type(type)));
        if (solid.empty()) continue;
        
        #ifdef SLIC3R_DEBUG
        std::cout << "Layer " << i << " has " << (type == stTop ? "top" : "bottom") << " surfaces" << std::endl;
        #endif
        
        size_t solid_layers = type == stTop
            ? region_config.top_solid_layers()
            : region_config.bottom_solid_layers();
        solid_layers = min(solid_layers, this->layers.size());

        if (region_config.min_top_bottom_shell_thickness() > 0) {
            auto current_shell_thickness = static_cast<coordf_t>(solid_layers) * this->get_layer(i)->height;
            const auto min_shell_thickness = region_config.min_top_bottom_shell_thickness();
            Slic3r::Log::debug("vertical_shell_thickness") << "Initial shell thickness for layer " << i << " " 
                                                           << current_shell_thickness << " "
                                                           << "Minimum: " << min_shell_thickness << "\n";
            while (std::abs(min_shell_thickness - current_shell_thickness) > Slic3r::Geometry::epsilon && current_shell_thickness < min_shell_thickness) {
                solid_layers++;
                current_shell_thickness = static_cast<coordf_t>(solid_layers) * this->get_layer(i)->height;
                Slic3r::Log::debug("vertical_shell_thickness") << "Solid layer count: "
                                                               << solid_layers << "; "
                                                               << "current_shell_thickness: "
                                                               << current_shell_thickness
                                                               << "\n";
                if (solid_layers > this->layers.size()) {
                    throw std::runtime_error("Infinite loop when determining vertical shell thickness");
                }
            }
        }
        _discover_neighbor_horizontal_shells(layerm, i, region_id, type, solid, solid_layers);
    }
}

void
PrintObject::_discover_neighbor_horizontal_shells(LayerRegion* layerm, const size_t& i, const size_t& region_id, const SurfaceType& type, Polygons& solid, const size_t& solid_layers)
{
    const auto& region_config = layerm->region()->config;

    for (int n = ((type & stTop) != 0 ? i-1 : i+1); std::abs(n-int(i)) < solid_layers; ((type & stTop) != 0 ? n-- : n++)) {
        if (n < 0 || static_cast<size_t>(n) >= this->layer_count()) continue;

        LayerRegion* neighbor_layerm { this->get_layer(n)->get_region(region_id) };
        // make a copy so we can use them even after clearing the original collection
        SurfaceCollection neighbor_fill_surfaces{ neighbor_layerm->fill_surfaces };
        
        // find intersection between neighbor and current layer's surfaces
        // intersections have contours and holes
        Polygons new_internal_solid = intersection(
            solid,
            to_polygons(neighbor_fill_surfaces.filter_by_type({stInternal, (stInternal | stSolid)})),
            true
        );
        if (new_internal_solid.empty()) {
            // No internal solid needed on this layer. In order to decide whether to continue
            // searching on the next neighbor (thus enforcing the configured number of solid
            // layers, use different strategies according to configured infill density:
            if (region_config.fill_density == 0) {
                // If user expects the object to be void (for example a hollow sloping vase),
                // don't continue the search. In this case, we only generate the external solid
                // shell if the object would otherwise show a hole (gap between perimeters of 
                // the two layers), and internal solid shells are a subset of the shells found 
                // on each previous layer.
                return;
            } else {
                // If we have internal infill, we can generate internal solid shells freely.
                continue;
            }
        }

        if (region_config.fill_density == 0) {
            // if we're printing a hollow object we discard any solid shell thinner
            // than a perimeter width, since it's probably just crossing a sloping wall
            // and it's not wanted in a hollow print even if it would make sense when
            // obeying the solid shell count option strictly (DWIM!)
            const auto margin = neighbor_layerm->flow(frExternalPerimeter).scaled_width();
            const auto too_narrow = diff(
                new_internal_solid,
                offset2(new_internal_solid, -margin, +margin, CLIPPER_OFFSET_SCALE, ClipperLib::jtMiter, 5),
                true
            ); 
            if (!too_narrow.empty()) 
                new_internal_solid = solid = diff(new_internal_solid, too_narrow);
        }

        // make sure the new internal solid is wide enough, as it might get collapsed
        // when spacing is added in Slic3r::Fill
        {
            // require at least this size
            const auto margin = 3 * layerm->flow(frSolidInfill).scaled_width();

            // we use a higher miterLimit here to handle areas with acute angles
            // in those cases, the default miterLimit would cut the corner and we'd
            // get a triangle in $too_narrow; if we grow it below then the shell
            // would have a different shape from the external surface and we'd still
            // have the same angle, so the next shell would be grown even more and so on.
            const auto too_narrow = diff(
                new_internal_solid,
                offset2(new_internal_solid, -margin, +margin, CLIPPER_OFFSET_SCALE, ClipperLib::jtMiter, 5),
                true
            );

            if (!too_narrow.empty()) {
                // grow the collapsing parts and add the extra area to  the neighbor layer 
                // as well as to our original surfaces so that we support this 
                // additional area in the next shell too

                // make sure our grown surfaces don't exceed the fill area
                Polygons tmp;
                for (auto& s : neighbor_fill_surfaces)
                    if (s.is_internal() && !s.is_bridge())
                        append_to(tmp, (Polygons)s);
                const auto grown = intersection(
                    offset(too_narrow, +margin),
                    // Discard bridges as they are grown for anchoring and we can't
                    // remove such anchors. (This may happen when a bridge is being 
                    // anchored onto a wall where little space remains after the bridge
                    // is grown, and that little space is an internal solid shell so 
                    // it triggers this too_narrow logic.)
                    tmp
                );
                append_to(new_internal_solid, grown);
                solid = new_internal_solid;
            }
        }
        
        // internal-solid are the union of the existing internal-solid surfaces
        // and new ones
        Polygons tmp { to_polygons(neighbor_fill_surfaces.filter_by_type(stInternal | stSolid)) };
        polygons_append(tmp, new_internal_solid);
        const ExPolygons internal_solid = union_ex(tmp);

        // subtract intersections from layer surfaces to get resulting internal surfaces
        tmp = to_polygons(neighbor_fill_surfaces.filter_by_type(stInternal));
        const ExPolygons internal = diff_ex(tmp, to_polygons(internal_solid), 1);

        // assign resulting internal surfaces to layer
        neighbor_layerm->fill_surfaces.clear();
        neighbor_layerm->fill_surfaces.append(internal, stInternal);

        // assign new internal-solid surfaces to layer
        neighbor_layerm->fill_surfaces.append(internal_solid, (stInternal | stSolid));

        // assign top and bottom surfaces to layer
        SurfaceCollection tmp_coll;
        for (const Surface& s : neighbor_fill_surfaces.surfaces)
            if (s.is_top() || s.is_bottom())
                tmp_coll.append(s);
        
        for (auto s : tmp_coll.group()) {
            Polygons tmp;
            append_to(tmp, to_polygons(internal_solid));
            append_to(tmp, to_polygons(internal));
            
            const auto solid_surfaces = diff_ex(to_polygons(s), tmp, true);
            neighbor_layerm->fill_surfaces.append(solid_surfaces, *s.front());
        }
    }
}

// Idempotence of this method is guaranteed by the fact that we don't remove things from
// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
void
PrintObject::clip_fill_surfaces()
{
    if (! this->config.infill_only_where_needed.value ||
        ! std::any_of(this->print()->regions.begin(), this->print()->regions.end(), 
            [](const PrintRegion *region) { return region->config.fill_density > 0; }))
        return;

    // We only want infill under ceilings; this is almost like an
    // internal support material.
    // Proceed top-down, skipping the bottom layer.
    Polygons upper_internal;
    for (int layer_id = int(this->layers.size()) - 1; layer_id > 0; --layer_id) {
        const Layer *layer = this->layers[layer_id];
        Layer *lower_layer = this->layers[layer_id - 1];
        
        // Detect things that we need to support.
        // Solid surfaces to be supported.
        Polygons overhangs;
        for (const LayerRegion *layerm : layer->regions) {
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                Polygons polygons = to_polygons(surface.expolygon);
                if (surface.is_solid())
                    polygons_append(overhangs, polygons);
                //polygons_append(fill_surfaces, std::move(polygons));
            }
        }
        
        // We also need to support perimeters when there's at least one full unsupported loop
        {
            // Get perimeters area as the difference between slices and fill_surfaces
            Polygons fill_surfaces;
            for (const LayerRegion *layerm : layer->regions)
                polygons_append(fill_surfaces, (Polygons)layerm->fill_surfaces);
            Polygons perimeters = diff(layer->slices, fill_surfaces);
            
            // Only consider the area that is not supported by lower perimeters
            Polygons lower_layer_fill_surfaces;
            for (const LayerRegion *layerm : lower_layer->regions)
                polygons_append(lower_layer_fill_surfaces, (Polygons)layerm->fill_surfaces);
            perimeters = intersection(perimeters, lower_layer_fill_surfaces, true);
            
            // Only consider perimeter areas that are at least one extrusion width thick.
            //FIXME Offset2 eats out from both sides, while the perimeters are create outside in.
            //Should the pw not be half of the current value?
            float pw = FLT_MAX;
            for (const LayerRegion *layerm : layer->regions)
                pw = std::min<float>(pw, layerm->flow(frPerimeter).scaled_width());
            perimeters = offset2(perimeters, -pw, +pw);
            
            // Append such thick perimeters to the areas that need support
            polygons_append(overhangs, perimeters);
        }
        
        // Find new internal infill.
        {
            polygons_append(overhangs, std::move(upper_internal));
            
            // get our current internal fill boundaries
            Polygons lower_layer_internal_surfaces;
            for (const auto* layerm : lower_layer->regions)
                polygons_append(lower_layer_internal_surfaces, to_polygons(
                    layerm->fill_surfaces.filter_by_type({ stInternal, (stInternal | stVoid) })
                ));
            upper_internal = intersection(overhangs, lower_layer_internal_surfaces);
        }
        
        // Apply new internal infill to regions.
        for (auto* layerm : lower_layer->regions) {
            if (layerm->region()->config.fill_density.value == 0)
                continue;
            
            Polygons internal{ to_polygons(layerm->fill_surfaces.filter_by_type({ stInternal, (stInternal | stVoid) })) };            
            layerm->fill_surfaces.remove_types({ stInternal, (stInternal | stVoid) });
            layerm->fill_surfaces.append(intersection_ex(internal, upper_internal, true), stInternal);
            layerm->fill_surfaces.append(diff_ex        (internal, upper_internal, true), (stInternal | stVoid));
            
            // If there are voids it means that our internal infill is not adjacent to
            // perimeters. In this case it would be nice to add a loop around infill to
            // make it more robust and nicer. TODO.
        }
    }
}

// Simplify the sliced model, if "resolution" configuration parameter > 0.
// The simplification is problematic, because it simplifies the slices independent from each other,
// which makes the simplified discretization visible on the object surface.
void
PrintObject::_simplify_slices(double distance)
{
    for (auto* layer : this->layers) {
        layer->slices.simplify(distance);
        for (auto* layerm : layer->regions)
            layerm->slices.simplify(distance);
    }
}

}
