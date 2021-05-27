#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "libslic3r.h"
#include <vector>
#include "ExPolygonCollection.hpp"
#include "Flow.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"
#include "ExtrusionEntityCollection.hpp"

namespace Slic3r {
	
	
struct PerimeterIntersectionPoint {
    size_t idx_children;
    Point child_best;
    Point outter_best;
    size_t idx_polyline_outter;
    coord_t distance;
};

// Hierarchy of perimeters.
class PerimeterGeneratorLoop {
public:
    // Polygon of this contour.
    Polygon polygon;
    // Is it a contour or a hole?
    // Contours are CCW oriented, holes are CW oriented.
    bool is_contour;
    // Depth in the hierarchy. External perimeter has depth = 0. An external perimeter could be both a contour and a hole.
    unsigned short depth;
    // Children contour, may be both CCW and CW oriented (outer contours or holes).
    std::vector<PerimeterGeneratorLoop> children;
    
    PerimeterGeneratorLoop(Polygon polygon, unsigned short depth)
        : polygon(polygon), is_contour(false), depth(depth)
        {};
    // External perimeter. It may be CCW or CW oriented (outer contour or hole contour).
    bool is_external() const { return this->depth == 0; }
    // An island, which may have holes, but it does not have another internal island.
    bool is_internal_contour() const;
};

typedef std::vector<PerimeterGeneratorLoop> PerimeterGeneratorLoops;

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection* slices;
    const ExPolygonCollection* lower_slices;
    double layer_height;
    int layer_id;
    Flow perimeter_flow;
    Flow ext_perimeter_flow;
    Flow overhang_flow;
    Flow solid_infill_flow;
    PrintRegionConfig* config;
    PrintObjectConfig* object_config;
    PrintConfig* print_config;
    // Outputs:
    ExtrusionEntityCollection* loops;
    ExtrusionEntityCollection* gap_fill;
    SurfaceCollection* fill_surfaces;
    
    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices, 
        double                      layer_height,
        Flow                        flow,
        PrintRegionConfig*          config,
        PrintObjectConfig*          object_config,
        PrintConfig*                print_config,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces)
        : slices(slices), lower_slices(NULL), layer_height(layer_height),
            layer_id(-1), perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            loops(loops), gap_fill(gap_fill), fill_surfaces(fill_surfaces),
            _ext_mm3_per_mm(-1), _mm3_per_mm(-1), _mm3_per_mm_overhang(-1)
        {};
    void process();
    
    private:
    double _ext_mm3_per_mm;
    double _mm3_per_mm;
    double _mm3_per_mm_overhang;
    Polygons _lower_slices_p;
    
    ExtrusionEntityCollection _traverse_loops(const PerimeterGeneratorLoops &loops,
        ThickPolylines &thin_walls) const;
    ExtrusionLoop _traverse_and_join_loops(const PerimeterGeneratorLoop &loop, 
        const PerimeterGeneratorLoops &childs, const Point entryPoint) const;
    ExtrusionLoop _extrude_and_cut_loop(const PerimeterGeneratorLoop &loop, 
        const Point entryPoint, const Line &direction = Line(Point(0,0),Point(0,0))) const;
    PerimeterIntersectionPoint _get_nearest_point(const PerimeterGeneratorLoops &children, 
        ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const;
    ExtrusionEntityCollection _variable_width
        (const ThickPolylines &polylines, ExtrusionRole role, Flow flow) const;
};

}

#endif
