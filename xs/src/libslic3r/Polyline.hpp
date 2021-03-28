#ifndef slic3r_Polyline_hpp_
#define slic3r_Polyline_hpp_

#include "libslic3r.h"
#include "Line.hpp"
#include "MultiPoint.hpp"
#include "Polygon.hpp"
#include <string>
#include <vector>
#include "clipper.hpp"

namespace Slic3r {

class Polyline;
class ThickPolyline;
typedef std::vector<Polyline> Polylines;
typedef std::vector<ThickPolyline> ThickPolylines;

class Polyline : public MultiPoint {
    public:
    operator Polylines() const;
    operator Line() const;
    Point last_point() const;
    Point leftmost_point() const;
    virtual Lines lines() const;
    void clip_end(double distance);
    void clip_start(double distance);
    void extend_end(double distance);
    void extend_start(double distance);
    Points equally_spaced_points(double distance) const;
    void simplify(double tolerance);
    template <class T> void simplify_by_visibility(const T &area);
    void split_at(const Point &point, Polyline* p1, Polyline* p2) const;
    bool is_straight() const;
    std::string wkt() const;
    Polygons grow(double delta, double scale = CLIPPER_OFFSET_SCALE, ClipperLib::JoinType joinType = ClipperLib::jtSquare, double miterLimit = 3.0) const;
};


/// ThickPolyline : a polyline with a width for each point
/// This class has a vector of coordf_t, it must be the same size as points.
/// it's used to store the size of the line at this point.
/// Also, the endpoint let us know if the front() and back() of the polyline 
/// join something or is a dead-end.
class ThickPolyline : public Polyline {
    public:
    /// width size must be == point size
    std::vector<coordf_t> width;
    /// if true => it's an endpoint, if false it join an other ThickPolyline. first is at front(), second is at back()
    std::pair<bool,bool> endpoints;
    ThickPolyline() : endpoints(std::make_pair(false, false)) {};
    ThickLines thicklines() const;
    void reverse();
};

/// concatenate poylines if possible and refresh the endpoints
void concatThickPolylines(ThickPolylines &polylines);

inline Polylines
to_polylines(const Polygons &polygons)
{
    Polylines pp;
    for (Polygons::const_iterator it = polygons.begin(); it != polygons.end(); ++it)
        pp.push_back((Polyline)*it);
    return pp;
}

inline Polylines
to_polylines(const Lines &lines)
{
    Polylines pp;
    for (Lines::const_iterator it = lines.begin(); it != lines.end(); ++it)
        pp.push_back((Polyline)*it);
    return pp;
}

}

#endif
