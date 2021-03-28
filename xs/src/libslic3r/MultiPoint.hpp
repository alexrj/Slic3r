#ifndef slic3r_MultiPoint_hpp_
#define slic3r_MultiPoint_hpp_

#include "libslic3r.h"
#include <algorithm>
#include <vector>
#include "Line.hpp"
#include "Point.hpp"

namespace Slic3r {

class BoundingBox;

class MultiPoint
{
    public:
    Points points;
    
    operator Points() const;
    void scale(double factor);
    void translate(double x, double y);
    void translate(const Point &vector);
    void rotate(double angle);
    void rotate(double angle, const Point &center);
    void reverse();
    Point first_point() const;
    virtual Point last_point() const = 0;
    virtual Lines lines() const = 0;
    double length() const;
    bool is_valid() const { return this->points.size() >= 2; }

    int find_point(const Point &point) const;
    bool has_boundary_point(const Point &point) const;
    /// return the index of the closest point in this polygon in relation with "point"
    /// \param point the point to compare with. 
    /// \return the index of the closest point in the points vector, or max(size_t) if points is empty. 
    size_t closest_point_index(const Point &point) const {
        size_t idx = -1;
        if (! this->points.empty()) {
            idx = 0;
            double dist_min = this->points.front().distance_to_sq(point);
            for (size_t i = 1; i < this->points.size(); ++ i) {
                double d = this->points[i].distance_to_sq(point);
                if (d < dist_min) {
                    dist_min = d;
                    idx = i;
                }
            }
        }
        return idx;
    }
    const Point* closest_point(const Point &point) const { return this->points.empty() ? nullptr : &this->points[this->closest_point_index(point)]; }
    BoundingBox bounding_box() const;
    
    // Return true if there are exact duplicates.
    bool has_duplicate_points() const;
    
    // Remove exact duplicates, return true if any duplicate has been removed.
    bool remove_duplicate_points();
    
    void append(const Point &point);
    void append(const Points &points);
    void append(const Points::const_iterator &begin, const Points::const_iterator &end);
    bool intersection(const Line& line, Point* intersection) const;
    bool first_intersection(const Line& line, Point* intersection) const;
    std::string dump_perl() const;
    
    static Points _douglas_peucker(const Points &points, const double tolerance);
    
    protected:
    MultiPoint() {};
    explicit MultiPoint(const Points &_points): points(_points) {};
    ~MultiPoint() = default;
};

} // namespace Slic3r

#endif
