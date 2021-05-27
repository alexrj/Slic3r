#include "PerimeterGenerator.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <cmath>
#include <cassert>

namespace Slic3r {

PerimeterGeneratorLoops get_all_Childs(PerimeterGeneratorLoop loop) {
	PerimeterGeneratorLoops ret;
	for (PerimeterGeneratorLoop &child : loop.children) {
		ret.push_back(child);
		PerimeterGeneratorLoops vals = get_all_Childs(child);
		ret.insert(ret.end(), vals.begin(), vals.end());
	}
	return ret;
}

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
        double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder-1);
        
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
                    // the minimum thickness of a single loop is:
                    // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                    if (this->config->thin_walls) {
                        offsets = offset2(
                            last,
                            -(ext_pwidth/2 + ext_min_spacing/2 - 1),
                            +(ext_min_spacing/2 - 1)
                        );
                    } else {
                        offsets = offset(last, -ext_pwidth/2);
                    }
                    
                    // look for thin walls
                    if (this->config->thin_walls) {
                        Polygons no_thin_zone = offset(offsets, +ext_pwidth/2);
                        Polygons diffpp = diff(
                            last,
                            no_thin_zone,
                            true  // medial axis requires non-overlapping geometry
                        );
                        
                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = scale_(this->ext_perimeter_flow.nozzle_diameter / 3);
                        ExPolygons expp = offset2_ex(diffpp, -min_width/2, +min_width/2);
						
                         // compute a bit of overlap to anchor thin walls inside the print.
                        ExPolygons anchor = intersection_ex(to_polygons(offset_ex(expp, (float)(ext_pwidth / 2))), no_thin_zone, true);
                        
                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop
                        for (ExPolygons::const_iterator ex = expp.begin(); ex != expp.end(); ++ex) {
                            ExPolygons bounds = _clipper_ex(ClipperLib::ctUnion, (Polygons)*ex, to_polygons(anchor), true);
							//search our bound
                            for (ExPolygon &bound : bounds) {
                                if (!intersection_ex(*ex, bound).empty()) {
                                    // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop
                                    ex->medial_axis(bound, ext_pwidth + ext_pspacing2, min_width, &thin_walls);
                                    continue;
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
        
            // at this point, all loops should be in contours[0] (= contours.front() )
            ExtrusionEntityCollection entities;
            if (config->perimeter_loop.value) {
                //onlyone_perimter = >fusion all perimeterLoops
                for (PerimeterGeneratorLoop &loop : contours.front()) {
                    ExtrusionLoop extr_loop = this->_traverse_and_join_loops(loop, get_all_Childs(loop), loop.polygon.points.front());
                    //ExtrusionLoop extr_loop = this->_traverse_and_join_loops_old(loop, loop.polygon.points.front(), true);
                    extr_loop.paths.back().polyline.points.push_back(extr_loop.paths.front().polyline.points.front());
                    entities.append(extr_loop);
                }

                // append thin walls
                if (!thin_walls.empty()) {
                    ExtrusionEntityCollection tw = this->_variable_width
                        (thin_walls, erExternalPerimeter, this->ext_perimeter_flow);

                    entities.append(tw.entities);
                    thin_walls.clear();
                }
            } else {
                entities = this->_traverse_loops(contours.front(), thin_walls);
            }
            
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
            double max = 2*pspacing;
            ExPolygons gaps_ex = diff_ex(
                offset2(gaps, -min/2, +min/2),
                offset2(gaps, -max/2, +max/2),
                true
            );
            
            ThickPolylines polylines;
            for (ExPolygons::const_iterator ex = gaps_ex.begin(); ex != gaps_ex.end(); ++ex)
                ex->medial_axis(*ex, max, min, &polylines);
            
            if (!polylines.empty()) {
                ExtrusionEntityCollection gap_fill = this->_variable_width(polylines, 
                    erGapFill, this->solid_infill_flow);
                
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
        ExtrusionEntityCollection tw = this->_variable_width
            (thin_walls, erExternalPerimeter, this->ext_perimeter_flow);
        
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


PerimeterIntersectionPoint
PerimeterGenerator::_get_nearest_point(const PerimeterGeneratorLoops &children, ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const {
    //find best points of intersections
    PerimeterIntersectionPoint intersect;
    intersect.distance = 0x7FFFFFFF; // ! assumption on intersect type & max value
    intersect.idx_polyline_outter = -1;
    intersect.idx_children = -1;
    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            if ((myPolylines.paths[idx_poly].role == erExternalPerimeter || child.is_external() )
                && this->object_config->seam_position.value != SeamPosition::spRandom) {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = child.polygon.points[child.polygon.closest_point_index(p)];
                    const double dist = nearest_p.distance_to(p);
                    //Try to find a point in the far side, aligning them
                    if (dist + dist_cut / 20 < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter <0 || p.y > intersect.outter_best.y)
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            } else {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = child.polygon.points[child.polygon.closest_point_index(p)];
                    const double dist = nearest_p.distance_to(p);
                    if (dist + SCALED_EPSILON < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter<0 || p.y < intersect.outter_best.y)
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            //second, try to check from one of my points
            //don't check the last point, as it's used to go outter, can't use it to go inner.
            for (size_t idx_point = 1; idx_point < myPolylines.paths[idx_poly].polyline.points.size()-1; idx_point++) {
                const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                Point nearest_p = child.polygon.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = p;
                    intersect.child_best = nearest_p;
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            //if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            //lastly, try to check from one of his points
            for (size_t idx_point = 0; idx_point < child.polygon.points.size(); idx_point++) {
                const Point &p = child.polygon.points[idx_point];
                Point nearest_p = myPolylines.paths[idx_poly].polyline.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = nearest_p;
                    intersect.child_best = p;
                }
            }
        }
    }
    return intersect;
}

ExtrusionLoop
PerimeterGenerator::_extrude_and_cut_loop(const PerimeterGeneratorLoop &loop, const Point entry_point, const Line &direction) const
{

    bool need_to_reverse = false;
    Polyline initial_polyline;
    const coord_t dist_cut = (coord_t)scale_(this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1));


    if (loop.polygon.points.size() < 3) return ExtrusionLoop(elrDefault);
    if (loop.polygon.length() < dist_cut * 2) {
        ExtrusionLoop single_point(elrDefault);
        Polyline poly_point;
        poly_point.append(loop.polygon.centroid());
        single_point.paths.emplace_back(
            loop.is_external() ? erExternalPerimeter : erPerimeter,
            (double)(loop.is_external() ? this->_ext_mm3_per_mm : this->_mm3_per_mm),
            (float)(loop.is_external() ? this->ext_perimeter_flow.width : this->perimeter_flow.width),
            (float)(this->layer_height));
        single_point.paths.back().polyline = poly_point;
        return single_point;
    }
    const size_t idx_closest_from_entry_point = loop.polygon.closest_point_index(entry_point);
    if (loop.polygon.points[idx_closest_from_entry_point].distance_to(entry_point) > SCALED_EPSILON) {
        //create new Point
        //get first point
        size_t idx_before = -1;
        for (size_t idx_p_a = 0; idx_p_a < loop.polygon.points.size(); ++idx_p_a) {
            Line l(loop.polygon.points[idx_p_a], loop.polygon.points[(idx_p_a + 1 == loop.polygon.points.size()) ? 0 : (idx_p_a + 1)]);
            if (entry_point.distance_to(l) < SCALED_EPSILON) {
                idx_before = idx_p_a;
                break;
            }
        }
        if (idx_before == (size_t)-1) std::cout << "ERROR: _traverse_and_join_loops : idx_before can't be finded to create new point\n";
        initial_polyline = loop.polygon.split_at_index(idx_before);
        initial_polyline.points.push_back(entry_point);
        initial_polyline.points[0] = entry_point;
    } else {
        initial_polyline = loop.polygon.split_at_index(idx_closest_from_entry_point);
    }
   

    //std::vector<PerimeterPolylineNode> myPolylines;
    ExtrusionLoop my_loop;

    //overhang / notoverhang
    {
        bool is_external = loop.is_external();

        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }

        // detect overhanging/bridging perimeters
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance.value == 0)) {
            ExtrusionPaths paths;
            // get non-overhang paths by intersecting this loop with the grown lower slices
            for (Polyline &to_extrude : intersection_pl(initial_polyline, this->_lower_slices_p)) {
                ExtrusionPath path(role, 
                    is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm, 
                    is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width, 
                    this->layer_height);
                path.polyline = to_extrude;
                paths.push_back(path);
            }

            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            for (Polyline &to_extrude : diff_pl(initial_polyline, this->_lower_slices_p)) {
                ExtrusionPath path(erOverhangPerimeter, 
                    this->_mm3_per_mm_overhang, 
                    this->overhang_flow.width, 
                    this->overhang_flow.height);
                path.polyline = to_extrude;
                paths.push_back(path);
            }

            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = (ExtrusionPaths)ExtrusionEntityCollection(paths).chained_path();
             
            
            if (direction.length() > 0) {
                Polyline direction_polyline;
                for (ExtrusionPath &path : paths) {
                    direction_polyline.points.insert(direction_polyline.points.end(), path.polyline.points.begin(), path.polyline.points.end());
                }
                direction_polyline.clip_start(SCALED_RESOLUTION);
                direction_polyline.clip_end(SCALED_RESOLUTION);
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }
            if (need_to_reverse) {
                std::reverse(paths.begin(), paths.end());
            }
            //search for the first path
            size_t good_idx = 0; 
            for (size_t idx_path = 0; idx_path < paths.size(); idx_path++) {
                const ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) {
                    if (path.polyline.points.back().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                } else {
                    if (path.polyline.points.front().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                }
            }
            for (size_t idx_path = good_idx; idx_path < paths.size(); idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
            for (size_t idx_path = 0; idx_path < good_idx; idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
        } else {

            if (direction.length() > 0) {
                Polyline direction_polyline = initial_polyline;
                direction_polyline.clip_start(SCALED_RESOLUTION);
                direction_polyline.clip_end(SCALED_RESOLUTION);
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }

            ExtrusionPath path(role);
            path.polyline = initial_polyline;
            if (need_to_reverse) path.polyline.reverse();
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm;
            path.width = is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width;
            path.height = (float)(this->layer_height);
            my_loop.paths.emplace_back(path);
        }

    }
    
    return my_loop;
}

ExtrusionLoop
PerimeterGenerator::_traverse_and_join_loops(const PerimeterGeneratorLoop &loop, const PerimeterGeneratorLoops &children, const Point entry_point) const
{
    // other perimeters
    const coord_t perimeter_spacing = this->perimeter_flow.scaled_spacing();

    //// external perimeters
    const coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();

    //TODO change this->external_perimeter_flow.scaled_width() if it's the first one!
    const coord_t max_width_extrusion = this->perimeter_flow.scaled_width();
    ExtrusionLoop my_loop = _extrude_and_cut_loop(loop, entry_point);

    //iterate on each point ot find the best place to go into the child
    std::vector<PerimeterGeneratorLoop> childs = children;
    while (!childs.empty()) {
        PerimeterIntersectionPoint nearest = this->_get_nearest_point(childs, my_loop, this->perimeter_flow.scaled_width(), this->perimeter_flow.scaled_width()* 1.42);
        if (nearest.idx_children == (size_t)-1) {
            //return ExtrusionEntityCollection();
            break;
        } else {
            const PerimeterGeneratorLoop &child = childs[nearest.idx_children];
            //create new node with recursive ask for the inner perimeter & COPY of the points, ready to be cut
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths[nearest.idx_polyline_outter]);

            ExtrusionPath *outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            ExtrusionPath *outer_end = &my_loop.paths[nearest.idx_polyline_outter + 1];
            Line deletedSection;

            //cut our polyline
            //separate them
            size_t nearest_idx_outter = outer_start->polyline.closest_point_index(nearest.outter_best);
            if (outer_start->polyline.points[nearest_idx_outter].coincides_with_epsilon(nearest.outter_best)) {
                if (nearest_idx_outter < outer_start->polyline.points.size() - 1) {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + nearest_idx_outter + 1, outer_start->polyline.points.end());
                }
                if (nearest_idx_outter > 0) {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + nearest_idx_outter);
                }
            } else {
                //get first point
                size_t idx_before = -1;
                for (size_t idx_p_a = 0; idx_p_a < outer_start->polyline.points.size() - 1; ++idx_p_a) {
                    Line l(outer_start->polyline.points[idx_p_a], outer_start->polyline.points[idx_p_a + 1]);
                    if (nearest.outter_best.distance_to(l) < SCALED_EPSILON) {
                        idx_before = idx_p_a;
                        break;
                    }
                }
                if (idx_before == (size_t)-1) {
                    std::cout << "ERROR: idx_before can't be finded\n";
                    continue;
                }

                Points &my_polyline_points = outer_start->polyline.points;
                my_polyline_points.erase(my_polyline_points.begin() + idx_before + 1, my_polyline_points.end());
                my_polyline_points.push_back(nearest.outter_best);

                if (idx_before < outer_end->polyline.points.size()-1)
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + idx_before + 1);
                else
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin()+1, outer_end->polyline.points.end());
                outer_end->polyline.points.insert(outer_end->polyline.points.begin(), nearest.outter_best);
            }
            Polyline to_reduce = outer_start->polyline;
            if (to_reduce.points.size()>1) to_reduce.clip_end(SCALED_RESOLUTION);
            deletedSection.a = to_reduce.points.back();
            to_reduce = outer_end->polyline;
            if (to_reduce.points.size()>1) to_reduce.clip_start(SCALED_RESOLUTION);
            deletedSection.b = to_reduce.points.front();
            
            //get the inner loop to connect to us.
            ExtrusionLoop child_loop = _extrude_and_cut_loop(child, nearest.child_best, deletedSection);

            const coord_t inner_child_spacing = child.is_external() ? ext_perimeter_spacing : perimeter_spacing;
            const coord_t outer_start_spacing = scale_(outer_start->width - outer_start->height * (1. - 0.25 * PI));
            const coord_t outer_end_spacing = scale_(outer_end->width - outer_end->height * (1. - 0.25 * PI));

            //FIXME: if child_loop has no point or 1 point or not enough space !!!!!!!
            const size_t child_paths_size = child_loop.paths.size();
            if (child_paths_size == 0) continue;
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, child_loop.paths.begin(), child_loop.paths.end());
            
            //add paths into my_loop => need to re-get the refs
            outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
            ExtrusionPath *inner_start = &my_loop.paths[nearest.idx_polyline_outter+1];
            ExtrusionPath *inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
            //TRIM
            //choose trim direction
            if (outer_start->polyline.points.size() == 1 && outer_end->polyline.points.size() == 1) {
                //do nothing
            } else if (outer_start->polyline.points.size() == 1) {
                outer_end->polyline.clip_start(outer_end_spacing);
                if (inner_end->polyline.length() > inner_child_spacing)
                    inner_end->polyline.clip_end(inner_child_spacing);
                else
                    inner_end->polyline.clip_end(inner_end->polyline.length() / 2);
            } else if (outer_end->polyline.points.size() == 1) {
                outer_start->polyline.clip_end(outer_start_spacing);
                if (inner_start->polyline.length() > inner_child_spacing)
                    inner_start->polyline.clip_start(inner_child_spacing);
                else
                    inner_start->polyline.clip_start(inner_start->polyline.length()/2);
            } else {
                double length_poly_1 = outer_start->polyline.length();
                double length_poly_2 = outer_end->polyline.length();
                coord_t length_trim_1 = outer_start_spacing / 2;
                coord_t length_trim_2 = outer_end_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    outer_start->polyline.clip_end(length_trim_1);
                } else {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + 1, outer_start->polyline.points.end());
                }
                if (length_poly_2 > length_trim_2) {
                    outer_end->polyline.clip_start(length_trim_2);
                } else {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.end() - 1);
                }
                
                length_poly_1 = inner_start->polyline.length();
                length_poly_2 = inner_end->polyline.length();
                length_trim_1 = inner_child_spacing / 2;
                length_trim_2 = inner_child_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    inner_start->polyline.clip_start(length_trim_1);
                } else {
                    inner_start->polyline.points.erase(
                        inner_start->polyline.points.begin(), 
                        inner_start->polyline.points.end() - 1);
                }
                if (length_poly_2 > length_trim_2) {
                    inner_end->polyline.clip_end(length_trim_2);
                } else {
                    inner_end->polyline.points.erase(
                        inner_end->polyline.points.begin() + 1,
                        inner_end->polyline.points.end());
                }
            }

            //last check to see if we need a reverse
            {
                Line l1(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Line l2(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_inter(0, 0);
                bool is_interect = l1.intersection(l2, &p_inter);
                if (is_interect && p_inter.distance_to(l1) < SCALED_EPSILON && p_inter.distance_to(l2) < SCALED_EPSILON) {
                    //intersection! need to reverse!
                    std::reverse(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1);
                    for (size_t idx = nearest.idx_polyline_outter + 1; idx < nearest.idx_polyline_outter + child_paths_size + 1; idx++) {
                        my_loop.paths[idx].reverse();
                    }
                    outer_start = &my_loop.paths[nearest.idx_polyline_outter];
                    inner_start = &my_loop.paths[nearest.idx_polyline_outter + 1];
                    inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
                    outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
                }

            }

            //now add extrusionPAths to connect the two loops
            ExtrusionPaths travel_path_begin;
            ExtrusionPaths travel_path_end;
            double dist_travel = outer_start->polyline.points.back().distance_to(inner_start->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erNone, 0, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                //travel_path_begin[0].extruder_id = -1;
                //travel_path_begin[1].extruder_id = -1;
                //travel_path_begin[2].extruder_id = -1;
                Line line(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x = (coord_t)(p_dist_cut_extrude.x * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y = (coord_t)(p_dist_cut_extrude.y * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel = line.a;
                p_start_travel += p_dist_cut_extrude;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(p_start_travel);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel = line.b;
                p_end_travel -= p_dist_cut_extrude;
                travel_path_begin[2].polyline.append(p_end_travel);
                travel_path_begin[2].polyline.append(inner_start->polyline.points.front());
                //fake travel in the middle
                travel_path_begin[1].polyline.append(p_start_travel);
                travel_path_begin[1].polyline.append(p_end_travel);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm * flow_mult, (float)(outer_start->width * flow_mult), outer_start->height);
                //travel_path_begin[0].extruder_id = -1;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(inner_start->polyline.points.front());
            }
            dist_travel = inner_end->polyline.points.back().distance_to(outer_end->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erNone, 0, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                //travel_path_end[0].extruder_id = -1;
                //travel_path_end[1].extruder_id = -1;
                //travel_path_end[2].extruder_id = -1;
                Line line(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x = (coord_t)(p_dist_cut_extrude.x * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y = (coord_t)(p_dist_cut_extrude.y * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel_2 = line.a;
                p_start_travel_2 += p_dist_cut_extrude;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(p_start_travel_2);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel_2 = line.b;
                p_end_travel_2 -= p_dist_cut_extrude;
                travel_path_end[2].polyline.append(p_end_travel_2);
                travel_path_end[2].polyline.append(outer_end->polyline.points.front());
                //fake travel in the middle
                travel_path_end[1].polyline.append(p_start_travel_2);
                travel_path_end[1].polyline.append(p_end_travel_2);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm * flow_mult, (float)(outer_end->width * flow_mult), outer_end->height);
                //travel_path_end[0].extruder_id = -1;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(outer_end->polyline.points.front());
            }
            //add paths into my_loop => after that all ref are wrong!
            for (int i = travel_path_end.size() - 1; i >= 0; i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1, travel_path_end[i]);
            }
            for (int i = travel_path_begin.size() - 1; i >= 0; i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, travel_path_begin[i]);
            }
        }

        //update for next loop
        childs.erase(childs.begin() + nearest.idx_children);
    }

    return my_loop;
}

ExtrusionEntityCollection
PerimeterGenerator::_variable_width(const ThickPolylines &polylines, ExtrusionRole role, Flow flow) const
{
    // this value determines granularity of adaptive width, as G-code does not allow
    // variable extrusion within a single move; this value shall only affect the amount
    // of segments, and any pruning shall be performed before we apply this tolerance
    const double tolerance = scale_(0.05);
    
    ExtrusionEntityCollection coll;
    for (ThickPolylines::const_iterator p = polylines.begin(); p != polylines.end(); ++p) {
        ExtrusionPaths paths;
        ExtrusionPath path(role);
        ThickLines lines = p->thicklines();
        
        for (int i = 0; i < (int)lines.size(); ++i) {
            const ThickLine& line = lines[i];
            
            const coordf_t line_len = line.length();
            if (line_len < SCALED_EPSILON) continue;
            
            double thickness_delta = fabs(line.a_width - line.b_width);
            if (thickness_delta > tolerance) {
                const size_t segments = ceil(thickness_delta / tolerance);
                const coordf_t seg_len = line_len / segments;
                Points pp;
                std::vector<coordf_t> width;
                {
                    pp.push_back(line.a);
                    width.push_back(line.a_width);
                    for (size_t j = 1; j < segments; ++j) {
                        pp.push_back(line.point_at(j*seg_len));
                        
                        coordf_t w = line.a_width + (j*seg_len) * (line.b_width-line.a_width) / line_len;
                        width.push_back(w);
                        width.push_back(w);
                    }
                    pp.push_back(line.b);
                    width.push_back(line.b_width);
                    
                    assert(pp.size() == segments + 1);
                    assert(width.size() == segments*2);
                }
                
                // delete this line and insert new ones
                lines.erase(lines.begin() + i);
                for (size_t j = 0; j < segments; ++j) {
                    ThickLine new_line(pp[j], pp[j+1]);
                    new_line.a_width = width[2*j];
                    new_line.b_width = width[2*j+1];
                    lines.insert(lines.begin() + i + j, new_line);
                }
                
                --i;
                continue;
            }
            
            const double w = fmax(line.a_width, line.b_width);
            
            if (path.polyline.points.empty()) {
                flow.width = unscale(w);
                #ifdef SLIC3R_DEBUG
                printf("  filling %f gap\n", flow.width);
                #endif
                
                // make sure we don't include too thin segments which
                // may cause even slightly negative mm3_per_mm because of floating point math
                path.mm3_per_mm  = flow.mm3_per_mm();
                if (path.mm3_per_mm < EPSILON) continue;
                
                path.width       = flow.width;
                path.height      = flow.height;
                path.polyline.append(line.a);
                path.polyline.append(line.b);
            } else {
                thickness_delta = fabs(scale_(flow.width) - w);
                if (thickness_delta <= tolerance) {
                    // the width difference between this line and the current flow width is 
                    // within the accepted tolerance
                
                    path.polyline.append(line.b);
                } else {
                    // we need to initialize a new line
                    paths.push_back(path);
                    path = ExtrusionPath(role);
                    --i;
                }
            }
        }
        if (path.polyline.is_valid())
            paths.push_back(path);
        
        // append paths to collection
        if (!paths.empty()) {
            if (paths.front().first_point().coincides_with(paths.back().last_point())) {
                coll.append(ExtrusionLoop(paths));
            } else {
                coll.append(paths);
            }
        }
    }
    
    return coll;
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
