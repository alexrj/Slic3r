#include "PerimeterGenerator.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "MedialAxis.hpp"
#include <cmath>
#include <cassert>

namespace Slic3r {

void
PerimeterGenerator::process()
{
    // other perimeters
    this->_mm3_per_mm           = this->perimeter_flow.mm3_per_mm();
    coord_t pwidth              = this->perimeter_flow.scaled_width();
    coord_t pspacing            = this->perimeter_flow.scaled_spacing();
    
    // external perimeters
    this->_ext_mm3_per_mm       = this->ext_perimeter_flow.mm3_per_mm();
    coord_t ext_pwidth          = this->ext_perimeter_flow.scaled_width();
    coord_t ext_pspacing        = this->ext_perimeter_flow.scaled_spacing();
    coord_t ext_pspacing2       = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);
    
    // overhang perimeters
    this->_mm3_per_mm_overhang  = this->overhang_flow.mm3_per_mm();
    
    // solid infill
    coord_t ispacing            = this->solid_infill_flow.scaled_spacing();
    
    //nozzle diameter
    const double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder-1);
    
    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the ext_pspacing calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_pspacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on 
    // internal flow which is unrelated.
    coord_t min_spacing         = pspacing      * (1 - INSET_OVERLAP_TOLERANCE);
    coord_t ext_min_spacing     = ext_pspacing  * (1 - INSET_OVERLAP_TOLERANCE);

    // minimum shell thickness
    coord_t min_shell_thickness = scale_(this->config->min_shell_thickness);
    
    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != NULL && this->config->overhangs) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by half the nozzle diameter used 
        // in the current layer
        this->_lower_slices_p = offset(*this->lower_slices, scale_(+nozzle_diameter/2));
    }
    
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    for (Surfaces::const_iterator surface = this->slices->surfaces.begin();
        surface != this->slices->surfaces.end(); ++surface) {
        // detect how many perimeters must be generated for this island
        int loops = this->config->perimeters + surface->extra_perimeters;

        // If the user has defined a minimum shell thickness compute the number of loops needed to satisfy
        if (min_shell_thickness > 0) {
            int min_loops = 1;

            min_loops += ceil(((float)min_shell_thickness-ext_pwidth)/pwidth);

            if (loops < min_loops)
                loops = min_loops;
        }

        const int loop_number = loops-1;  // 0-indexed loops
        

        Polygons gaps;
        
        Polygons last = surface->expolygon.simplify_p(SCALED_RESOLUTION);
        if (loop_number >= 0) {  // no loops = -1
            
            std::vector<PerimeterGeneratorLoops> contours(loop_number+1);    // depth => loops
            std::vector<PerimeterGeneratorLoops> holes(loop_number+1);       // depth => loops
            ThickPolylines thin_walls;
            
            // we loop one time more than needed in order to find gaps after the last perimeter was applied
            for (int i = 0; i <= loop_number+1; ++i) {  // outer loop is 0
                Polygons offsets;
                if (i == 0) {
                    // compute next onion, without taking care of thin_walls : destroy too thin areas.
                    if (!this->config->thin_walls)
                        offsets = offset(last, -(float)(ext_pwidth / 2));


                    // look for thin walls
                    if (this->config->thin_walls) {
                        // the minimum thickness of a single loop is:
                        // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                        //here, we shrink & grow by ext_min_spacing to remove areas where the current loop can't be extruded 
                        offsets = offset2(
                            last,
                            -(ext_pwidth/2 + ext_min_spacing/2 - 1),
                            +(ext_min_spacing/2 - 1));

                        // detect edge case where a curve can be split in multiple small chunks.
                        Polygons no_thin_onion = offset(last, -(float)(ext_pwidth / 2));
                        float div = 2;
                        while (no_thin_onion.size()>0 && offsets.size() > no_thin_onion.size() && no_thin_onion.size() + offsets.size() > 3) {
                            div += 0.5;
                            //use a sightly smaller offset2 spacing to try to improve the split, but with a little bit of over-extrusion
                            Polygons next_onion_secondTry = offset2(
                                last,
                                -(float)(ext_pwidth / 2 + ext_min_spacing / div - 1),
                                +(float)(ext_min_spacing / div - 1));
                            if (offsets.size() >  next_onion_secondTry.size() * 1.1) {
                                offsets = next_onion_secondTry;
                            }
                            if (div > 3) break;
                        }
                        
                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = scale_(this->config->thin_walls_min_width.get_abs_value(this->ext_perimeter_flow.nozzle_diameter));
                        
                        Polygons no_thin_zone = offset(offsets, ext_pwidth/2, CLIPPER_OFFSET_SCALE, jtSquare, 3);
                        // medial axis requires non-overlapping geometry
                        Polygons thin_zones = diff(last, no_thin_zone, true);
                        //don't use offset2_ex, because we don't want to merge the zones that have been separated.
                        //a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
                        ExPolygons half_thins = offset_ex(thin_zones, (float)(-min_width / 2));
                        //simplify them
                        for (ExPolygon &half_thin : half_thins) {
                            half_thin.remove_point_too_near(SCALED_RESOLUTION);
                        }
                        //we push the bits removed and put them into what we will use as our anchor
                        if (half_thins.size() > 0) {
                            no_thin_zone = diff(last, to_polygons(offset_ex(half_thins, (float)(min_width / 2) - SCALED_EPSILON)), true);
                        }
                        // compute a bit of overlap to anchor thin walls inside the print.
                        for (ExPolygon &half_thin : half_thins) {
                            //growing back the polygon
                            ExPolygons thin = offset_ex(half_thin, (float)(min_width / 2));
                            assert(thin.size() <= 1);
                            if (thin.empty()) continue;
                            double overlap = (coord_t)scale_(this->config->thin_walls_overlap.get_abs_value(this->ext_perimeter_flow.nozzle_diameter));
                            ExPolygons anchor = intersection_ex(
                                to_polygons(offset_ex(half_thin, (float)(min_width / 2 + overlap), CLIPPER_OFFSET_SCALE, jtSquare, 3)), 
                                no_thin_zone, true);
                            ExPolygons bounds = _clipper_ex(ClipperLib::ctUnion, to_polygons(thin), to_polygons(anchor), true);
                            for (ExPolygon &bound : bounds) {
                                if (!intersection_ex(thin[0], bound).empty()) {
                                    //be sure it's not too small to extrude reliably
                                    thin[0].remove_point_too_near(SCALED_RESOLUTION);
                                    if (thin[0].area() > min_width*(ext_pwidth + ext_pspacing2)) {
                                        bound.remove_point_too_near(SCALED_RESOLUTION);
                                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop (*1.1 because of circles approx.)
                                        Slic3r::MedialAxis ma{ thin[0], (ext_pwidth + ext_pspacing2)*1.1, min_width, coord_t(this->layer_height) };
                                        ma.use_bounds(bound)
                                            .use_min_real_width((coord_t)scale_(this->ext_perimeter_flow.nozzle_diameter))
                                            .use_tapers(overlap)
                                            .build(thin_walls);
                                    }
                                    break;
                                }
                            }
                        }
                        #ifdef DEBUG
                        printf("  %zu thin walls detected\n", thin_walls.size());
                        #endif
                        
                        /*
                        if (false) {
                            require "Slic3r/SVG.pm";
                            Slic3r::SVG::output(
                                "medial_axis.svg",
                                no_arrows       => 1,
                                #expolygons      => \@expp,
                                polylines       => \@thin_walls,
                            );
                        }
                        */
                    }
                } else {
                    //FIXME Is this offset correct if the line width of the inner perimeters differs
                    // from the line width of the infill?
                    coord_t distance = (i == 1) ? ext_pspacing2 : pspacing;
                    
                    if (this->config->thin_walls) {
                        // This path will ensure, that the perimeters do not overfill, as in 
                        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                        // excessively, creating gaps, which then need to be filled in by the not very 
                        // reliable gap fill algorithm.
                        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                        // the original.
                        offsets = offset2(
                            last,
                            -(distance + min_spacing/2 - 1),
                            +(min_spacing/2 - 1)
                        );
                    } else {
                        // If "detect thin walls" is not enabled, this paths will be entered, which 
                        // leads to overflows, as in prusa3d/Slic3r GH #32
                        offsets = offset(
                            last,
                            -distance
                        );
                    }
                    
                    // look for gaps
                    if (this->config->fill_gaps && this->config->fill_density.value > 0) {
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        Polygons diff_pp = diff(
                            offset(last, -0.5*distance),
                            offset(offsets, +0.5*distance + 10)  // safety offset
                        );
                        gaps.insert(gaps.end(), diff_pp.begin(), diff_pp.end());
                    }
                }
                
                if (offsets.empty()) break;
                if (i > loop_number) break; // we were only looking for gaps this time
                
                last = offsets;
                for (Polygons::const_iterator polygon = offsets.begin(); polygon != offsets.end(); ++polygon) {
                    PerimeterGeneratorLoop loop(*polygon, i);
                    loop.is_contour = polygon->is_counter_clockwise();
                    if (loop.is_contour) {
                        contours[i].push_back(loop);
                    } else {
                        holes[i].push_back(loop);
                    }
                }
            }
            
            // nest loops: holes first
            for (int d = 0; d <= loop_number; ++d) {
                PerimeterGeneratorLoops &holes_d = holes[d];
                
                // loop through all holes having depth == d
                for (int i = 0; i < (int)holes_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = holes_d[i];
                    
                    // find the hole loop that contains this one, if any
                    for (int t = d+1; t <= loop_number; ++t) {
                        for (int j = 0; j < (int)holes[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = holes[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    
                    // if no hole contains this hole, find the contour loop that contains it
                    for (int t = loop_number; t >= 0; --t) {
                        for (int j = 0; j < (int)contours[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    NEXT_LOOP: ;
                }
            }
        
            // nest contour loops
            for (int d = loop_number; d >= 1; --d) {
                PerimeterGeneratorLoops &contours_d = contours[d];
                
                // loop through all contours having depth == d
                for (int i = 0; i < (int)contours_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = contours_d[i];
                
                    // find the contour loop that contains it
                    for (int t = d-1; t >= 0; --t) {
                        for (size_t j = 0; j < contours[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                contours_d.erase(contours_d.begin() + i);
                                --i;
                                goto NEXT_CONTOUR;
                            }
                        }
                    }
                    
                    NEXT_CONTOUR: ;
                }
            }
        
            // at this point, all loops should be in contours[0]
            
            ExtrusionEntityCollection entities = this->_traverse_loops(contours.front(), thin_walls);
            
            // if brim will be printed, reverse the order of perimeters so that
            // we continue inwards after having finished the brim
            // TODO: add test for perimeter order
            if (this->config->external_perimeters_first
                || (this->layer_id == 0 && this->print_config->brim_width.value > 0))
                    entities.reverse();
            
            // append perimeters for this slice as a collection
            if (!entities.empty())
                this->loops->append(entities);
        }
        
        // fill gaps
        if (!gaps.empty()) {
            /*
            SVG svg("gaps.svg");
            svg.draw(union_ex(gaps));
            svg.Close();
            */
            
            // collapse 
            double min = 0.2*pwidth * (1 - INSET_OVERLAP_TOLERANCE);
            //be sure we don't gapfill where the perimeters are already touching each other (negative spacing).
            min = std::max(min, double(Flow::new_from_spacing(EPSILON, nozzle_diameter, this->layer_height, false).scaled_width()));
            double max = 2*pspacing;
            ExPolygons gaps_ex = diff_ex(
                offset2(gaps, -min/2, +min/2),
                offset2(gaps, -max/2, +max/2),
                true
            );
            
            ThickPolylines polylines;
            for (const ExPolygon &ex : gaps_ex) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (ex.area() > min*max) {
                    MedialAxis{ex, coord_t(max),coord_t(min), coord_t(this->layer_height)}.build(polylines);
                }
            }
            if (!polylines.empty()) {
                ExtrusionEntityCollection gap_fill = discretize_variable_width(polylines, erGapFill, this->solid_infill_flow);
                
                this->gap_fill->append(gap_fill.entities);
            
                /*  Make sure we don't infill narrow parts that are already gap-filled
                    (we only consider this surface's gaps to reduce the diff() complexity).
                    Growing actual extrusions ensures that gaps not filled by medial axis
                    are not subtracted from fill surfaces (they might be too short gaps
                    that medial axis skips but infill might join with other infill regions
                    and use zigzag).  */
                //FIXME Vojtech: This grows by a rounded extrusion width, not by line spacing,
                // therefore it may cover the area, but no the volume.
                last = diff(last, gap_fill.grow());
            }
        }
        
        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset = 0;
        if (loop_number == 0) {
            // one loop
            inset += ext_pspacing2/2;
        } else if (loop_number > 0) {
            // two or more loops
            inset += pspacing/2;
        }
        
        {
            ExPolygons expp = union_ex(last);
            
            // simplify infill contours according to resolution
            Polygons pp;
            for (ExPolygons::const_iterator ex = expp.begin(); ex != expp.end(); ++ex)
                ex->simplify_p(SCALED_RESOLUTION, &pp);
            
            // collapse too narrow infill areas
            coord_t min_perimeter_infill_spacing = ispacing * (1 - INSET_OVERLAP_TOLERANCE);
            expp = offset2_ex(
                pp,
                -inset -min_perimeter_infill_spacing/2,
                +min_perimeter_infill_spacing/2
            );
            
            // append infill areas to fill_surfaces
            this->fill_surfaces->append(expp, stInternal);  // use a bogus surface type
        }
    }
}

ExtrusionEntityCollection
PerimeterGenerator::_traverse_loops(const PerimeterGeneratorLoops &loops,
    ThickPolylines &thin_walls) const
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntityCollection coll;
    for (PerimeterGeneratorLoops::const_iterator loop = loops.begin();
        loop != loops.end(); ++loop) {
        bool is_external = loop->is_external();
        
        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop->is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }
        
        // detect overhanging/bridging perimeters
        ExtrusionPaths paths;
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance.value == 0)) {
            // get non-overhang paths by intersecting this loop with the grown lower slices
            {
                const Polylines polylines = intersection_pl(loop->polygon, this->_lower_slices_p);
                for (const Polyline &polyline : polylines) {
                    ExtrusionPath path(role);
                    path.polyline   = polyline;
                    path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm;
                    path.width      = is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width;
                    path.height     = this->layer_height;
                    paths.push_back(path);
                }
            }
            
            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            {
                const Polylines polylines = diff_pl(loop->polygon, this->_lower_slices_p);
                for (const Polyline &polyline : polylines) {
                    ExtrusionPath path(erOverhangPerimeter);
                    path.polyline   = polyline;
                    path.mm3_per_mm = this->_mm3_per_mm_overhang;
                    path.width      = this->overhang_flow.width;
                    path.height     = this->overhang_flow.height;
                    paths.push_back(path);
                }
            }
            
            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = ExtrusionEntityCollection(paths).chained_path();
        } else {
            ExtrusionPath path(role);
            path.polyline   = loop->polygon.split_at_first_point();
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm;
            path.width      = is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width;
            path.height     = this->layer_height;
            paths.push_back(path);
        }
        
        coll.append(ExtrusionLoop(paths, loop_role));
    }
    
    // append thin walls to the nearest-neighbor search (only for first iteration)
    if (!thin_walls.empty()) {
        ExtrusionEntityCollection tw = discretize_variable_width(thin_walls, erExternalPerimeter, this->ext_perimeter_flow);
        
        coll.append(tw.entities);
        thin_walls.clear();
    }
    
    // sort entities into a new collection using a nearest-neighbor search,
    // preserving the original indices which are useful for detecting thin walls
    ExtrusionEntityCollection sorted_coll;
    coll.chained_path(&sorted_coll, false, &sorted_coll.orig_indices);
    
    // traverse children and build the final collection
    ExtrusionEntityCollection entities;
    for (std::vector<size_t>::const_iterator idx = sorted_coll.orig_indices.begin();
        idx != sorted_coll.orig_indices.end();
        ++idx) {
        
        if (*idx >= loops.size()) {
            // this is a thin wall
            // let's get it from the sorted collection as it might have been reversed
            size_t i = idx - sorted_coll.orig_indices.begin();
            entities.append(*sorted_coll.entities[i]);
        } else {
            const PerimeterGeneratorLoop &loop = loops[*idx];
            ExtrusionLoop eloop = *dynamic_cast<ExtrusionLoop*>(coll.entities[*idx]);
            
            ExtrusionEntityCollection children = this->_traverse_loops(loop.children, thin_walls);
            if (loop.is_contour) {
                eloop.make_counter_clockwise();
                entities.append(children.entities);
                entities.append(eloop);
            } else {
                eloop.make_clockwise();
                entities.append(eloop);
                entities.append(children.entities);
            }
        }
    }
    return entities;
}

bool
PerimeterGeneratorLoop::is_internal_contour() const
{
    if (this->is_contour) {
        // an internal contour is a contour containing no other contours
        for (std::vector<PerimeterGeneratorLoop>::const_iterator loop = this->children.begin();
            loop != this->children.end(); ++loop) {
            if (loop->is_contour) {
                return false;
            }
        }
        return true;
    }
    return false;
}

}
