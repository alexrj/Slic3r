#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Print.hpp"
#include "Log.hpp"

namespace Slic3r {

/// Initialises upper_layer, lower_layer to NULL
/// Initialises slicing_errors to false
Layer::Layer(size_t id, PrintObject *object, coordf_t height, coordf_t print_z,
        coordf_t slice_z)
:   upper_layer(NULL),
    lower_layer(NULL),
    regions(),
    slicing_errors(false),
    slice_z(slice_z),
    print_z(print_z),
    height(height),
    slices(),
    _id(id),
    _object(object)
{
}

/// Removes references to self and clears regions
Layer::~Layer()
{
    // remove references to self
    if (NULL != this->upper_layer) {
        this->upper_layer->lower_layer = NULL;
    }

    if (NULL != this->lower_layer) {
        this->lower_layer->upper_layer = NULL;
    }

    this->clear_regions();
}

/// Getter for this->_id
size_t
Layer::id() const
{
    return this->_id;
}

/// Setter for this->_id
void
Layer::set_id(size_t id)
{
    this->_id = id;
}

/// Getter for this->regions.size()
size_t
Layer::region_count() const
{
    return this->regions.size();
}

/// Deletes all regions using this->delete_region()
void
Layer::clear_regions()
{
    for (int i = this->regions.size()-1; i >= 0; --i)
        this->delete_region(i);
}

/// Creates a LayerRegion from a PrintRegion and adds it to this->regions
LayerRegion*
Layer::add_region(PrintRegion* print_region)
{
    LayerRegion* region = new LayerRegion(this, print_region);
    this->regions.push_back(region);
    return region;
}

/// Deletes an individual region
void
Layer::delete_region(int idx)
{
    LayerRegionPtrs::iterator i = this->regions.begin() + idx;
    LayerRegion* item = *i;
    this->regions.erase(i);
    delete item;
}

/// Merge all regions' slices to get islands
//TODO: is this right?
void
Layer::make_slices()
{
    ExPolygons slices;
    if (this->regions.size() == 1) {
        // optimization: if we only have one region, take its slices
        slices = this->regions.front()->slices;
    } else {
        Polygons slices_p;
        FOREACH_LAYERREGION(this, layerm) {
            Polygons region_slices_p = (*layerm)->slices;
            slices_p.insert(slices_p.end(), region_slices_p.begin(), region_slices_p.end());
        }
        slices = union_ex(slices_p);
    }
    
    this->slices.expolygons.clear();
    this->slices.expolygons.reserve(slices.size());
    
    // prepare ordering points
    // While it's more computationally expensive, we use centroid()
    // instead of first_point() because it's [much more] deterministic
    // and preserves ordering across similar layers.
    Points ordering_points;
    ordering_points.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        ordering_points.push_back(ex.contour.centroid());
    
    // sort slices
    std::vector<Points::size_type> order;
    Slic3r::Geometry::chained_path(ordering_points, order);
    
    // populate slices vector
    for (const Points::size_type &o : order)
        this->slices.expolygons.push_back(slices[o]);
}

/// Iterates over all of the LayerRegion and invokes LayerRegion->merge_slices()
void
Layer::merge_slices()
{
    FOREACH_LAYERREGION(this, layerm) {
        (*layerm)->merge_slices();
    }
}

/// Uses LayerRegion->slices.any_internal_contains(item)
template <class T>
bool
Layer::any_internal_region_slice_contains(const T &item) const
{
    FOREACH_LAYERREGION(this, layerm) {
        if ((*layerm)->slices.any_internal_contains(item)) return true;
    }
    return false;
}
template bool Layer::any_internal_region_slice_contains<Polyline>(const Polyline &item) const;

/// Uses LayerRegion->slices.any_bottom_contains(item)
template <class T>
bool
Layer::any_bottom_region_slice_contains(const T &item) const
{
    FOREACH_LAYERREGION(this, layerm) {
        if ((*layerm)->slices.any_bottom_contains(item)) return true;
    }
    return false;
}
template bool Layer::any_bottom_region_slice_contains<Polyline>(const Polyline &item) const;

/// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
/// The resulting fill surface is split back among the originating regions.
void
Layer::make_perimeters()
{
    #ifdef SLIC3R_DEBUG
    printf("Making perimeters for layer %zu\n", this->id());
    #endif
    
    // keep track of regions whose perimeters we have already generated
    std::set<size_t> done;
    
    FOREACH_LAYERREGION(this, layerm) {
        size_t region_id = layerm - this->regions.begin();
        if (done.find(region_id) != done.end()) continue;
        done.insert(region_id);
        const PrintRegionConfig &config = (*layerm)->region()->config;
        
        // find compatible regions
        LayerRegionPtrs layerms;
        layerms.push_back(*layerm);
        for (LayerRegionPtrs::const_iterator it = layerm + 1; it != this->regions.end(); ++it) {
            LayerRegion* other_layerm = *it;
            const PrintRegionConfig &other_config = other_layerm->region()->config;
            
            if (config.perimeter_extruder   == other_config.perimeter_extruder
                && config.perimeters        == other_config.perimeters
                && config.perimeter_speed   == other_config.perimeter_speed
                && config.gap_fill_speed    == other_config.gap_fill_speed
                && config.overhangs         == other_config.overhangs
                && config.serialize("perimeter_extrusion_width").compare(other_config.serialize("perimeter_extrusion_width")) == 0
                && config.thin_walls        == other_config.thin_walls
                && config.external_perimeters_first == other_config.external_perimeters_first
                && config.perimeter_loop    == other_config.perimeter_loop) {
                layerms.push_back(other_layerm);
                done.insert(it - this->regions.begin());
            }
        }
        
        if (layerms.size() == 1) {  // optimization
            (*layerm)->fill_surfaces.surfaces.clear();
            (*layerm)->make_perimeters((*layerm)->slices, &(*layerm)->fill_surfaces);
        } else {
            // group slices (surfaces) according to number of extra perimeters
            std::map<unsigned short,Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
            for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
                for (Surfaces::iterator s = (*l)->slices.surfaces.begin(); s != (*l)->slices.surfaces.end(); ++s) {
                    slices[s->extra_perimeters].push_back(*s);
                }
            }
            
            // merge the surfaces assigned to each group
            SurfaceCollection new_slices;
            for (const auto &it : slices) {
                ExPolygons expp = union_ex(it.second, true);
                for (ExPolygon &ex : expp) {
                    Surface s = it.second.front();  // clone type and extra_perimeters
                    s.expolygon = ex;
                    new_slices.surfaces.push_back(s);
                }
            }
            
            // make perimeters
            SurfaceCollection fill_surfaces;
            (*layerm)->make_perimeters(new_slices, &fill_surfaces);
            
            // assign fill_surfaces to each layer
            if (!fill_surfaces.surfaces.empty()) {
                for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
                    ExPolygons expp = intersection_ex(
                        (Polygons) fill_surfaces,
                        (Polygons) (*l)->slices
                    );
                    (*l)->fill_surfaces.surfaces.clear();
                    
                    for (ExPolygons::iterator ex = expp.begin(); ex != expp.end(); ++ex) {
                        Surface s = fill_surfaces.surfaces.front();  // clone type and extra_perimeters
                        s.expolygon = *ex;
                        (*l)->fill_surfaces.surfaces.push_back(s);
                    }
                }
            }
        }
    }
}

/// Iterates over all of the LayerRegion and invokes LayerRegion->make_fill()
/// Asserts that the fills created are not NULL
void
Layer::make_fills()
{
    #ifdef SLIC3R_DEBUG
    Slic3r::Log::debug("Layer") << "Making fills for layer " 
                                << this->id() << "\n";
    #endif
    
    FOREACH_LAYERREGION(this, it_layerm) {
        (*it_layerm)->make_fill();
        
        #ifndef NDEBUG
        for (size_t i = 0; i < (*it_layerm)->fills.entities.size(); ++i)
            assert(dynamic_cast<ExtrusionEntityCollection*>((*it_layerm)->fills.entities[i]) != NULL);
        #endif
    }
}

/// Analyzes slices of a region (SurfaceCollection slices).
/// Each region slice (instance of Surface) is analyzed, whether it is supported or whether it is the top surface.
/// Initially all slices are of type S_TYPE_INTERNAL.
/// Slices are compared against the top / bottom slices and regions and classified to the following groups:
/// S_TYPE_TOP - Part of a region, which is not covered by any upper layer. This surface will be filled with a top solid infill.
/// S_TYPE_BOTTOM | S_TYPE_BRIDGE - Part of a region, which is not fully supported, but it hangs in the air, or it hangs loosely on a support or a raft.
/// S_TYPE_BOTTOM - Part of a region, which is not supported by the same region, but it is supported either by another region, or by a soluble interface layer.
/// S_TYPE_INTERNAL - Part of a region, which is supported by the same region type.
/// If a part of a region is of S_TYPE_BOTTOM and S_TYPE_TOP, the S_TYPE_BOTTOM wins.
void
Layer::detect_surfaces_type()
{
    PrintObject &object = *this->object();
    
    for (size_t region_id = 0; region_id < this->regions.size(); ++region_id) {
        LayerRegion &layerm = *this->regions[region_id];
        
        // comparison happens against the *full* slices (considering all regions)
        // unless internal shells are requested
    
        // We call layer->slices or layerm->slices on these neighbor layers
        // and we convert them into Polygons so we only care about their total
        // coverage. We only write to layerm->slices so we can read layer->slices safely.
        Layer* const &upper_layer = this->upper_layer;
        Layer* const &lower_layer = this->lower_layer;
    
        // collapse very narrow parts (using the safety offset in the diff is not enough)
        // TODO: this offset2 makes this method not idempotent (see #3764), so we should
        // move it to where we generate fill_surfaces instead and leave slices unaltered
        const float offs = layerm.flow(frExternalPerimeter).scaled_width() / 10.f;

        const Polygons layerm_slices_surfaces = layerm.slices;

        // find top surfaces (difference between current surfaces
        // of current layer and upper one)
        SurfaceCollection top;
        if (upper_layer != NULL) {
            Polygons upper_slices;
            if (object.config.interface_shells.value) {
                const LayerRegion* upper_layerm = upper_layer->get_region(region_id);
                boost::lock_guard<boost::mutex> l(upper_layerm->_slices_mutex);
                upper_slices = upper_layerm->slices;
            } else {
                upper_slices = upper_layer->slices;
            }
        
            top.append(
                offset2_ex(
                    diff(layerm_slices_surfaces, upper_slices, true),
                    -offs, offs
                ),
                stTop
            );
        } else {
            // if no upper layer, all surfaces of this one are solid
            // we clone surfaces because we're going to clear the slices collection
            top = layerm.slices;
            for (Surface &s : top.surfaces) s.surface_type = stTop;
        }
    
        // find bottom surfaces (difference between current surfaces
        // of current layer and lower one)
        SurfaceCollection bottom;
        if (lower_layer != NULL) {
            // If we have soluble support material, don't bridge. The overhang will be squished against a soluble layer separating
            // the support from the print.
            const SurfaceType surface_type_bottom =
                (object.config.support_material.value && object.config.support_material_contact_distance.value == 0)
                ? stBottom
                : (stBottom | stBridge);
        
            // Any surface lying on the void is a true bottom bridge (an overhang)
            bottom.append(
                offset2_ex(
                    diff(layerm_slices_surfaces, lower_layer->slices, true),
                    -offs, offs
                ),
                surface_type_bottom
            );
        
            // if user requested internal shells, we need to identify surfaces
            // lying on other slices not belonging to this region
            if (object.config.interface_shells) {
                // non-bridging bottom surfaces: any part of this layer lying
                // on something else, excluding those lying on our own region
                const LayerRegion* lower_layerm = lower_layer->get_region(region_id);
                boost::lock_guard<boost::mutex> l(lower_layerm->_slices_mutex);
                bottom.append(
                    offset2_ex(
                        diff(
                            intersection(layerm_slices_surfaces, lower_layer->slices), // supported
                            lower_layerm->slices,
                            true
                        ),
                        -offs, offs
                    ),
                    stBottom
                );
            }
        } else {
            // if no lower layer, all surfaces of this one are solid
            // we clone surfaces because we're going to clear the slices collection
            bottom = layerm.slices;
        
            // if we have raft layers, consider bottom layer as a bridge
            // just like any other bottom surface lying on the void
            const SurfaceType surface_type_bottom =
                (object.config.raft_layers.value > 0 && object.config.support_material_contact_distance.value > 0)
                ? (stBottom | stBridge)
                : stBottom;
            for (Surface &s : bottom.surfaces) s.surface_type = surface_type_bottom;
        }
    
        // now, if the object contained a thin membrane, we could have overlapping bottom
        // and top surfaces; let's do an intersection to discover them and consider them
        // as bottom surfaces (to allow for bridge detection)
        if (!top.empty() && !bottom.empty()) {
            const Polygons top_polygons = to_polygons(STDMOVE(top));
            top.clear();
            top.append(
                // TODO: maybe we don't need offset2?
                offset2_ex(diff(top_polygons, bottom, true), -offs, offs),
                stTop
            );
        }
    
        // save surfaces to layer
        {
            boost::lock_guard<boost::mutex> l(layerm._slices_mutex);
            layerm.slices.clear();
            layerm.slices.append(STDMOVE(top));
            layerm.slices.append(STDMOVE(bottom));
    
            // find internal surfaces (difference between top/bottom surfaces and others)
            {
                Polygons topbottom = top; append_to(topbottom, (Polygons)bottom);
    
                layerm.slices.append(
                    // TODO: maybe we don't need offset2?
                    offset2_ex(
                        diff(layerm_slices_surfaces, topbottom, true),
                        -offs, offs
                    ),
                    stInternal
                );
            }
        }
    
        #ifdef SLIC3R_DEBUG
        printf("  layer %zu has %zu bottom, %zu top and %zu internal surfaces\n",
            this->id(), bottom.size(), top.size(),
            layerm.slices.size()-bottom.size()-top.size());
        #endif
    
        {
            /*  Fill in layerm->fill_surfaces by trimming the layerm->slices by the cumulative layerm->fill_surfaces.
                Note: this method should be idempotent, but fill_surfaces gets modified
                in place. However we're now only using its boundaries (which are invariant)
                so we're safe. This guarantees idempotence of prepare_infill() also in case
                that combine_infill() turns some fill_surface into VOID surfaces.  */
            const Polygons fill_boundaries = layerm.fill_surfaces;
            layerm.fill_surfaces.clear();
            // No other instance of this function is writing to this layer, so we can read safely.
            for (const Surface &surface : layerm.slices.surfaces) {
                // No other instance of this function modifies fill_surfaces.
                layerm.fill_surfaces.append(
                    intersection_ex(surface, fill_boundaries),
                    surface.surface_type
                );
            }
        }
    }
}

///Iterates over all LayerRegions and invokes LayerRegion->process_external_surfaces
void
Layer::process_external_surfaces()
{
    for (LayerRegion* &layerm : this->regions)
        layerm->process_external_surfaces();
}

}
