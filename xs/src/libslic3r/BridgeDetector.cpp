#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include <algorithm>

namespace Slic3r {

BridgeDetector::BridgeDetector(const ExPolygon &_expolygon, const ExPolygonCollection &_lower_slices,
    coord_t _extrusion_width)
    : expolygon(_expolygon), extrusion_width(_extrusion_width),
        resolution(PI/36.0), angle(-1)
{
    /*  outset our bridge by an arbitrary amount; we'll use this outer margin
        for detecting anchors */
    Polygons grown = offset(this->expolygon, this->extrusion_width);
    
    // remove narrow gaps from lower slices
    // (this is only needed as long as we use clipped test lines for angle detection
    // and we check their endpoints: when endpoint fall in the gap we'd get false
    // negatives)
    this->lower_slices.expolygons = offset2_ex(
        _lower_slices,
        +this->extrusion_width/2,
        -this->extrusion_width/2
    );
    
    // detect what edges lie on lower slices by turning bridge contour and holes
    // into polylines and then clipping them with each lower slice's contour
    this->_edges = intersection_pl(grown, this->lower_slices.contours());
    
    #ifdef SLIC3R_DEBUG
    printf("  bridge has %zu support(s)\n", this->_edges.size());
    #endif
    
    // detect anchors as intersection between our bridge expolygon and the lower slices
    // safety offset required to avoid Clipper from detecting empty intersection while Boost actually found some edges
    this->_anchors = intersection_ex(grown, this->lower_slices, true);
    
    #if 0
    {
        SVG svg("bridge.svg");
        svg.draw(this->expolygon);
        svg.draw(this->lower_slices, "red");
        svg.draw(this->_anchors, "yellow");
        svg.draw(this->_edges, "black", scale_(0.2));
        svg.Close();
        
        std::cout << "expolygon: " << this->expolygon.dump_perl() << std::endl;
        for (const ExPolygon &e : this->lower_slices.expolygons)
            std::cout << "lower: " << e.dump_perl() << std::endl;
    }
    #endif
}

bool
BridgeDetector::detect_angle()
{
    // Do nothing if the bridging region is completely in the air
    // and there are no anchors available at the layer below.
    if (this->_edges.empty() || this->_anchors.empty()) return false;
    
    /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
        we'll use this one to clip our test lines and be sure that their endpoints
        are inside the anchors and not on their contours leading to false negatives. */
    const Polygons clip_area = offset(this->expolygon, +this->extrusion_width/2);
    
    /*  we'll now try several directions using a rudimentary visibility check:
        bridge in several directions and then sum the length of lines having both
        endpoints within anchors */
    
    // generate the list of candidate angles
    std::vector<BridgeDirection> candidates;
    {
        // we test angles according to configured resolution
        std::vector<double> angles;
        for (int i = 0; i <= PI/this->resolution; ++i)
            angles.push_back(i * this->resolution);
    
        // we also test angles of each bridge contour
        for (const Polygon &p : (Polygons)this->expolygon)
            for (const Line &line : p.lines())
                angles.push_back(line.direction());
    
        /*  we also test angles of each open supporting edge
            (this finds the optimal angle for C-shaped supports) */
        for (const Polyline &edge : this->_edges) {
            if (edge.first_point().coincides_with(edge.last_point())) continue;
            angles.push_back(Line(edge.first_point(), edge.last_point()).direction());
        }
    
        // remove duplicates
        constexpr double min_resolution = PI/180.0;  // 1 degree
        std::sort(angles.begin(), angles.end());
        for (size_t i = 1; i < angles.size(); ++i) {
            if (Slic3r::Geometry::directions_parallel(angles[i], angles[i-1], min_resolution)) {
                angles.erase(angles.begin() + i);
                --i;
            }
        }
        /*  compare first value with last one and remove the greatest one (PI) 
            in case they are parallel (PI, 0) */
        if (Slic3r::Geometry::directions_parallel(angles.front(), angles.back(), min_resolution))
            angles.pop_back();
        
        for (auto angle : angles)
            candidates.push_back(BridgeDirection(angle));
    }
    
    const coord_t line_increment = this->extrusion_width;
    bool have_coverage = false;
    for (BridgeDirection &candidate : candidates) {
        Polygons my_clip_area = clip_area;
        ExPolygons my_anchors = this->_anchors;
        
        // rotate everything - the center point doesn't matter
        for (Polygon &p : my_clip_area)
            p.rotate(-candidate.angle, Point(0,0));
        for (ExPolygon &e : my_anchors)
            e.rotate(-candidate.angle, Point(0,0));
    
        // generate lines in this direction
        BoundingBox bb;
        for (const ExPolygon &e : my_anchors)
            bb.merge(e.bounding_box());
        
        Lines lines;
        for (coord_t y = bb.min.y; y <= bb.max.y; y += line_increment)
            lines.push_back(Line(Point(bb.min.x, y), Point(bb.max.x, y)));
        
        const Lines clipped_lines = intersection_ln(lines, my_clip_area);
        
        for (const Line &line : clipped_lines) {
            // skip any line not having both endpoints within anchors
            if (!Slic3r::Geometry::contains(my_anchors, line.a)
                || !Slic3r::Geometry::contains(my_anchors, line.b))
                continue;
            
            candidate.max_length = std::max(candidate.max_length, line.length());
            // Calculate coverage as actual covered area, because length of centerlines
            // is not accurate enough when such lines are slightly skewed and not parallel
            // to the sides; calculating area will compute them as triangles.
            // TODO: use a faster algorithm for computing covered area by using a sweep line
            // instead of intersecting many lines.
            candidate.coverage += Slic3r::Geometry::area(intersection(
                my_clip_area,
                offset((Polyline)line, +this->extrusion_width/2)
            ));
        }
        if (candidate.coverage > 0) have_coverage = true;
        
        #if 0
        std::cout << "angle = "  << Slic3r::Geometry::rad2deg(candidate.angle)
            << "; coverage = "   << candidate.coverage
            << "; max_length = " << candidate.max_length
            << std::endl;
        #endif
    }
    
    // if no direction produced coverage, then there's no bridge direction
    if (!have_coverage) return false;
    
    // sort directions by coverage - most coverage first
    std::sort(candidates.begin(), candidates.end());
    
    // if any other direction is within extrusion width of coverage, prefer it if shorter
    // TODO: There are two options here - within width of the angle with most coverage, or within width of the currently preferred?
    size_t i_best = 0;
    for (size_t i = 1; i < candidates.size() && candidates[i_best].coverage - candidates[i].coverage < this->extrusion_width; ++ i)
        if (candidates[i].max_length < candidates[i_best].max_length)
            i_best = i;
    
    this->angle = candidates[i_best].angle;
    
    if (this->angle >= PI) this->angle -= PI;
    
    #ifdef SLIC3R_DEBUG
    printf("  Optimal infill angle is %d degrees\n", (int)Slic3r::Geometry::rad2deg(this->angle));
    #endif
    
    return true;
}

Polygons
BridgeDetector::coverage(double angle, bool precise) const
{
    if (angle == -1) angle = this->angle;
    if (angle == -1) return Polygons();
	
    // Clone our expolygon and rotate it so that we work with vertical lines.
    ExPolygon expolygon = this->expolygon;
    expolygon.rotate(PI/2.0 - angle, Point(0,0));
    
    /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
        we'll use this one to generate our trapezoids and be sure that their vertices
        are inside the anchors and not on their contours leading to false negatives. */
    ExPolygons grown = offset_ex(expolygon, this->extrusion_width/2.0);
    
    // Compute trapezoids according to a vertical orientation
    Polygons trapezoids;
    for (const ExPolygon &e : grown)
        e.get_trapezoids2(&trapezoids, PI/2.0);
    
    // get anchors, convert them to Polygons and rotate them too
    Polygons anchors;
    for (const ExPolygon &anchor : this->_anchors) {
        Polygons pp = anchor;
        for (Polygon &p : pp)
            p.rotate(PI/2.0 - angle, Point(0,0));
        append_to(anchors, pp);
    }
    
    Polygons covered;
    for (Polygon &trapezoid : trapezoids) {
        
		size_t n_supported = 0;
		if (!precise) {
			// not nice, we need a more robust non-numeric check
			for (const Line &supported_line : intersection_ln(trapezoid.lines(), anchors))
				if (supported_line.length() >= this->extrusion_width)
					++n_supported;
		} else {
			Polygons intersects = intersection(trapezoid, anchors);
			n_supported = intersects.size();

			if (n_supported >= 2) {
				// trim it to not allow to go outside of the intersections
				BoundingBox center_bound = intersects[0].bounding_box();
				coord_t min_y = center_bound.center().y, max_y = center_bound.center().y;
				for (const Polygon &poly_bound : intersects) {
					center_bound = poly_bound.bounding_box();
					if (min_y > center_bound.center().y) min_y = center_bound.center().y;
					if (max_y < center_bound.center().y) max_y = center_bound.center().y;
				}
				coord_t min_x = trapezoid[0].x, max_x = trapezoid[0].x;
				for (const Point &p : trapezoid.points) {
					if (min_x > p.x) min_x = p.x;
					if (max_x < p.x) max_x = p.x;
				}
				//add what get_trapezoids3 has removed (+EPSILON)
				min_x -= (this->extrusion_width / 4 + 1);
				max_x += (this->extrusion_width / 4 + 1);
				coord_t mid_x = (min_x + max_x) / 2;
				for (Point &p : trapezoid.points) {
					if (p.y < min_y) p.y = min_y;
					if (p.y > max_y) p.y = max_y;
					if (p.x > min_x && p.x < mid_x) p.x = min_x;
					if (p.x < max_x && p.x > mid_x) p.x = max_x;
				}
			}
		}

		if (n_supported >= 2) covered.push_back(trapezoid);        
    }
    
    // merge trapezoids and rotate them back
    covered = union_(covered);
    for (Polygon &p : covered)
        p.rotate(-(PI/2.0 - angle), Point(0,0));
    
    // intersect trapezoids with actual bridge area to remove extra margins
    // and append it to result
    return intersection(covered, this->expolygon);
    
    /*
    if (0) {
        my @lines = map @{$_->lines}, @$trapezoids;
        $_->rotate(-(PI/2 - $angle), [0,0]) for @lines;
        
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output(
            "coverage_" . rad2deg($angle) . ".svg",
            expolygons          => [$self->expolygon],
            green_expolygons    => $self->_anchors,
            red_expolygons      => $coverage,
            lines               => \@lines,
        );
    }
    */
}

///  This method returns the bridge edges (as polylines) that are not supported
///    but would allow the entire bridge area to be bridged with detected angle
///    if supported too
Polylines
BridgeDetector::unsupported_edges(double angle) const
{
    if (angle == -1) angle = this->angle;
    if (angle == -1) return Polylines();
    
    // get bridge edges (both contour and holes)
    Polylines bridge_edges;
    for (const Polygon &p : (Polygons)this->expolygon)
        bridge_edges.push_back(p.split_at_first_point());
    
    // get unsupported edges
    Polylines _unsupported = diff_pl(
        bridge_edges,
        offset(this->lower_slices, +this->extrusion_width)
    );
    
    /*  Split into individual segments and filter out edges parallel to the bridging angle
        TODO: angle tolerance should probably be based on segment length and flow width,
        so that we build supports whenever there's a chance that at least one or two bridge
        extrusions would be anchored within such length (i.e. a slightly non-parallel bridging
        direction might still benefit from anchors if long enough)
        double angle_tolerance = PI / 180.0 * 5.0; */
    Polylines unsupported;
    for (const Polyline &polyline : _unsupported)
        for (const Line &line : polyline.lines())
            if (!Slic3r::Geometry::directions_parallel(line.direction(), angle))
                unsupported.push_back(line);
    return unsupported;
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output(
            "unsupported_" . rad2deg($angle) . ".svg",
            expolygons          => [$self->expolygon],
            green_expolygons    => $self->_anchors,
            red_expolygons      => union_ex($grown_lower),
            no_arrows           => 1,
            polylines           => \@bridge_edges,
            red_polylines       => $unsupported,
        );
    }
    */
}

}
