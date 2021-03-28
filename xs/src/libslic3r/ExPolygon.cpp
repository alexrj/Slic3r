#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "MedialAxis.hpp"
#include "Geometry.hpp"
#include "Polygon.hpp"
#include "Line.hpp"
#include "ClipperUtils.hpp"
#include "polypartition.h"
#include "poly2tri/poly2tri.h"
#include <algorithm>
#include <cassert>
#include <list>

namespace Slic3r {

ExPolygon::operator Points() const
{
    Points points;
    Polygons pp = *this;
    for (Polygons::const_iterator poly = pp.begin(); poly != pp.end(); ++poly) {
        for (Points::const_iterator point = poly->points.begin(); point != poly->points.end(); ++point)
            points.push_back(*point);
    }
    return points;
}

ExPolygon::operator Polygons() const
{
    Polygons polygons;
    polygons.reserve(this->holes.size() + 1);
    polygons.push_back(this->contour);
    for (Polygons::const_iterator it = this->holes.begin(); it != this->holes.end(); ++it) {
        polygons.push_back(*it);
    }
    return polygons;
}

void
ExPolygon::scale(double factor)
{
    contour.scale(factor);
    for (Polygons::iterator it = holes.begin(); it != holes.end(); ++it) {
        (*it).scale(factor);
    }
}

void
ExPolygon::translate(double x, double y)
{
    contour.translate(x, y);
    for (Polygons::iterator it = holes.begin(); it != holes.end(); ++it) {
        (*it).translate(x, y);
    }
}

void
ExPolygon::rotate(double angle)
{
    contour.rotate(angle);
    for (Polygons::iterator it = holes.begin(); it != holes.end(); ++it) {
        (*it).rotate(angle);
    }
}

void
ExPolygon::rotate(double angle, const Point &center)
{
    contour.rotate(angle, center);
    for (Polygons::iterator it = holes.begin(); it != holes.end(); ++it) {
        (*it).rotate(angle, center);
    }
}

double
ExPolygon::area() const
{
    double a = this->contour.area();
    for (Polygons::const_iterator it = this->holes.begin(); it != this->holes.end(); ++it) {
        a -= -(*it).area();  // holes have negative area
    }
    return a;
}

bool
ExPolygon::is_valid() const
{
    if (!this->contour.is_valid() || !this->contour.is_counter_clockwise()) return false;
    for (Polygons::const_iterator it = this->holes.begin(); it != this->holes.end(); ++it) {
        if (!(*it).is_valid() || (*it).is_counter_clockwise()) return false;
    }
    return true;
}

bool
ExPolygon::contains(const Line &line) const
{
    return this->contains((Polyline)line);
}

bool
ExPolygon::contains(const Polyline &polyline) const
{
    return diff_pl((Polylines)polyline, *this).empty();
}

bool
ExPolygon::contains(const Point &point) const
{
    if (!this->contour.contains(point)) return false;
    for (Polygons::const_iterator it = this->holes.begin(); it != this->holes.end(); ++it) {
        if (it->contains(point)) return false;
    }
    return true;
}

// inclusive version of contains() that also checks whether point is on boundaries
bool
ExPolygon::contains_b(const Point &point) const
{
    return this->contains(point) || this->has_boundary_point(point);
}

bool
ExPolygon::has_boundary_point(const Point &point) const
{
    if (this->contour.has_boundary_point(point)) return true;
    for (Polygons::const_iterator h = this->holes.begin(); h != this->holes.end(); ++h) {
        if (h->has_boundary_point(point)) return true;
    }
    return false;
}

void
ExPolygon::remove_colinear_points()
{
    this->contour.remove_collinear_points();
    for (Polygon &p : this->holes)
        p.remove_collinear_points();
}

void
ExPolygon::remove_vertical_collinear_points(coord_t tolerance)
{
    this->contour.remove_vertical_collinear_points(tolerance);
    for (Polygon &p : this->holes)
        p.remove_vertical_collinear_points(tolerance);
}

void
ExPolygon::simplify_p(double tolerance, Polygons* polygons) const
{
    Polygons pp = this->simplify_p(tolerance);
    polygons->insert(polygons->end(), pp.begin(), pp.end());
}

Polygons
ExPolygon::simplify_p(double tolerance) const
{
    Polygons pp;
    pp.reserve(this->holes.size() + 1);
    
    // contour
    {
        Polygon p = this->contour;
        p.points.push_back(p.points.front());
        p.points = MultiPoint::_douglas_peucker(p.points, tolerance);
        p.points.pop_back();
        pp.push_back(p);
    }
    
    // holes
    for (Polygons::const_iterator it = this->holes.begin(); it != this->holes.end(); ++it) {
        Polygon p = *it;
        p.points.push_back(p.points.front());
        p.points = MultiPoint::_douglas_peucker(p.points, tolerance);
        p.points.pop_back();
        pp.push_back(p);
    }
    return simplify_polygons(pp);
}

ExPolygons
ExPolygon::simplify(double tolerance) const
{
    Polygons pp = this->simplify_p(tolerance);
    return union_ex(pp);
}

void
ExPolygon::simplify(double tolerance, ExPolygons* expolygons) const
{
    ExPolygons ep = this->simplify(tolerance);
    expolygons->insert(expolygons->end(), ep.begin(), ep.end());
}

/// remove point that are at SCALED_EPSILON * 2 distance.
//simplier than simplify
void
ExPolygon::remove_point_too_near(const coord_t tolerance) {
    size_t id = 1;
    while (id < this->contour.points.size() - 1) {
        double newdist = min(this->contour.points[id].distance_to(this->contour.points[id - 1])
            , this->contour.points[id].distance_to(this->contour.points[id + 1]));
        if (newdist < (double)tolerance) {
            this->contour.points.erase(this->contour.points.begin() + id);
            newdist = this->contour.points[id].distance_to(this->contour.points[id - 1]);
        }
        //go to next one
        //if you removed a point, it check if the next one isn't too near from the previous one.
        // if not, it byepass it.
        if (newdist > (double)tolerance) {
            ++id;
        }
    }
    if (this->contour.points.front().distance_to(this->contour.points.back()) < tolerance) {
        this->contour.points.erase(this->contour.points.end() -1);
    }
}

void
ExPolygon::medial_axis(double max_width, double min_width, Polylines* polylines) const
{
    ThickPolylines tp;
    MedialAxis{*this, coord_t(max_width), coord_t(min_width), coord_t(max_width / 2.0)}.build(tp);
    polylines->insert(polylines->end(), tp.begin(), tp.end());
}

void
ExPolygon::get_trapezoids(Polygons* polygons) const
{
    ExPolygons expp;
    expp.push_back(*this);
    boost::polygon::get_trapezoids(*polygons, expp);
}

void
ExPolygon::get_trapezoids(Polygons* polygons, double angle) const
{
    ExPolygon clone = *this;
    clone.rotate(PI/2 - angle, Point(0,0));
    clone.get_trapezoids(polygons);
    for (Polygons::iterator polygon = polygons->begin(); polygon != polygons->end(); ++polygon)
        polygon->rotate(-(PI/2 - angle), Point(0,0));
}

// This algorithm may return more trapezoids than necessary
// (i.e. it may break a single trapezoid in several because
// other parts of the object have x coordinates in the middle)
void
ExPolygon::get_trapezoids2(Polygons* polygons) const
{
    // get all points of this ExPolygon
    Points pp = *this;
    
    // build our bounding box
    BoundingBox bb(pp);
    
    // get all x coordinates
    std::vector<coord_t> xx;
    xx.reserve(pp.size());
    for (Points::const_iterator p = pp.begin(); p != pp.end(); ++p)
        xx.push_back(p->x);
    std::sort(xx.begin(), xx.end());
    
    // find trapezoids by looping from first to next-to-last coordinate
    for (std::vector<coord_t>::const_iterator x = xx.begin(); x != xx.end()-1; ++x) {
        coord_t next_x = *(x + 1);
        if (*x == next_x) continue;
        
        // build rectangle
        Polygon poly;
        poly.points.resize(4);
        poly[0].x = *x;
        poly[0].y = bb.min.y;
        poly[1].x = next_x;
        poly[1].y = bb.min.y;
        poly[2].x = next_x;
        poly[2].y = bb.max.y;
        poly[3].x = *x;
        poly[3].y = bb.max.y;
        
        // intersect with this expolygon
        Polygons trapezoids = intersection(poly, *this);
        
        // append results to return value
        polygons->insert(polygons->end(), trapezoids.begin(), trapezoids.end());
    }
}

void
ExPolygon::get_trapezoids2(Polygons* polygons, double angle) const
{
    ExPolygon clone = *this;
    clone.rotate(PI/2 - angle, Point(0,0));
    clone.get_trapezoids2(polygons);
    for (Polygons::iterator polygon = polygons->begin(); polygon != polygons->end(); ++polygon)
        polygon->rotate(-(PI/2 - angle), Point(0,0));
}

// While this triangulates successfully, it's NOT a constrained triangulation
// as it will create more vertices on the boundaries than the ones supplied.
void
ExPolygon::triangulate(Polygons* polygons) const
{
    // first make trapezoids
    Polygons trapezoids;
    this->get_trapezoids2(&trapezoids);
    
    // then triangulate each trapezoid
    for (Polygons::iterator polygon = trapezoids.begin(); polygon != trapezoids.end(); ++polygon)
        polygon->triangulate_convex(polygons);
}

void
ExPolygon::triangulate_pp(Polygons* polygons) const
{
    // convert polygons
    std::list<TPPLPoly> input;
    
    ExPolygons expp = simplify_polygons_ex(*this, true);
    
    for (ExPolygons::const_iterator ex = expp.begin(); ex != expp.end(); ++ex) {
        // contour
        {
            TPPLPoly p;
            p.Init(ex->contour.points.size());
            //printf("%zu\n0\n", ex->contour.points.size());
            for (Points::const_iterator point = ex->contour.points.begin(); point != ex->contour.points.end(); ++point) {
                p[ point-ex->contour.points.begin() ].x = point->x;
                p[ point-ex->contour.points.begin() ].y = point->y;
                //printf("%ld %ld\n", point->x, point->y);
            }
            p.SetHole(false);
            input.push_back(p);
        }
    
        // holes
        for (Polygons::const_iterator hole = ex->holes.begin(); hole != ex->holes.end(); ++hole) {
            TPPLPoly p;
            p.Init(hole->points.size());
            //printf("%zu\n1\n", hole->points.size());
            for (Points::const_iterator point = hole->points.begin(); point != hole->points.end(); ++point) {
                p[ point-hole->points.begin() ].x = point->x;
                p[ point-hole->points.begin() ].y = point->y;
                //printf("%ld %ld\n", point->x, point->y);
            }
            p.SetHole(true);
            input.push_back(p);
        }
    }
    
    // perform triangulation
    std::list<TPPLPoly> output;
    int res = TPPLPartition().Triangulate_MONO(&input, &output);
    if (res != 1) CONFESS("Triangulation failed");
    
    // convert output polygons
    for (std::list<TPPLPoly>::iterator poly = output.begin(); poly != output.end(); ++poly) {
        long num_points = poly->GetNumPoints();
        Polygon p;
        p.points.resize(num_points);
        for (long i = 0; i < num_points; ++i) {
            p.points[i].x = (*poly)[i].x;
            p.points[i].y = (*poly)[i].y;
        }
        polygons->push_back(p);
    }
}

void
ExPolygon::triangulate_p2t(Polygons* polygons) const
{
    for (const ExPolygon &ex : simplify_polygons_ex(*this, true)) {
        // contour
        std::vector<p2t::Point*> ContourPoints;
        
        Polygon contour = ex.contour;
        contour.remove_duplicate_points();
        for (const Point &point : contour.points) {
            // We should delete each p2t::Point object
            ContourPoints.push_back(new p2t::Point(point.x, point.y));
        }
        p2t::CDT cdt(ContourPoints);

        // holes
        for (Polygon hole : ex.holes) {
            hole.remove_duplicate_points();
            std::vector<p2t::Point*> points;
            Point prev = hole.points.back();
            for (Point &point : hole.points) {
                // Shrink large polygons by reducing each coordinate by 1 in the
                // general direction of the last point as we wind around
                // This normally wouldn't work in every case, but our upscaled polygons
                // have little chance to create new duplicate points with this method.
                // For information on why this was needed, see:
                //    https://code.google.com/p/poly2tri/issues/detail?id=90
                //    https://github.com/raptor/clip2tri
                (point.x - prev.x) > 0 ? point.x-- : point.x++;
                (point.y - prev.y) > 0 ? point.y-- : point.y++;
                prev = point;
                
                // will be destructed in SweepContext::~SweepContext
                points.push_back(new p2t::Point(point.x, point.y));
            }
            cdt.AddHole(points);
        }
        
        // perform triangulation
        cdt.Triangulate();
        std::vector<p2t::Triangle*> triangles = cdt.GetTriangles();
        
        for (p2t::Triangle* triangle : triangles) {
            Polygon p;
            for (int i = 0; i <= 2; ++i) {
                p2t::Point* point = triangle->GetPoint(i);
                p.points.push_back(Point(point->x, point->y));
            }
            polygons->push_back(p);
        }
        
        for (p2t::Point* it : ContourPoints)
            delete it;
    }
}

Lines
ExPolygon::lines() const
{
    Lines lines = this->contour.lines();
    for (Polygons::const_iterator h = this->holes.begin(); h != this->holes.end(); ++h) {
        Lines hole_lines = h->lines();
        lines.insert(lines.end(), hole_lines.begin(), hole_lines.end());
    }
    return lines;
}

std::string
ExPolygon::dump_perl() const
{
    std::ostringstream ret;
    ret << "[" << this->contour.dump_perl();
    for (Polygons::const_iterator h = this->holes.begin(); h != this->holes.end(); ++h)
        ret << "," << h->dump_perl();
    ret << "]";
    return ret.str();
}

std::ostream&
operator <<(std::ostream &s, const ExPolygons &expolygons)
{
    for (const ExPolygon &e : expolygons)
        s << e.dump_perl() << std::endl;
    return s;
}

}
