#ifndef slic3r_BridgeDetector_hpp_
#define slic3r_BridgeDetector_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "ExPolygonCollection.hpp"
#include <string>

namespace Slic3r {

class BridgeDetector {
public:
    /// The non-grown hole.
    ExPolygon expolygon;
    /// Lower slices, all regions.
    ExPolygonCollection lower_slices;
    /// Scaled extrusion width of the infill.
    coord_t extrusion_width;
    /// Angle resolution for the brute force search of the best bridging angle.
    double resolution;
    /// The final optimal angle.
    double angle;
    
    BridgeDetector(const ExPolygon &_expolygon, const ExPolygonCollection &_lower_slices, coord_t _extrusion_width);
    bool detect_angle();
    Polygons coverage(double angle = -1, bool precise = false) const;
    
    /// Return the bridge edges that are not currently supported but would permit use of the supplied
    /// bridge angle if it was supported.
    Polylines unsupported_edges(double angle = -1) const;
    
private:
    /// Open lines representing the supporting edges.
    Polylines _edges;
    /// Closed polygons representing the supporting areas.
    ExPolygons _anchors;
    
    class BridgeDirection {
        public:
        BridgeDirection(double a = -1.) : angle(a), coverage(0.), max_length(0.) {}
        /// the best direction is the one causing most lines to be bridged (thus most coverage)
        bool operator<(const BridgeDirection &other) const {
            // Initial sort by coverage only - comparator must obey strict weak ordering
            return this->coverage > other.coverage;
        };
        double angle;
        double coverage;
        double max_length;
    };
};

}

#endif
