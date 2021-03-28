#ifndef slic3r_ExPolygon_hpp_
#define slic3r_ExPolygon_hpp_

#include "libslic3r.h"
#include "BoundingBox.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include <ostream>
#include <vector>

namespace Slic3r {

class ExPolygon;
typedef std::vector<ExPolygon> ExPolygons;

class ExPolygon
{
    public:
    Polygon contour;
    Polygons holes;
    ExPolygon() {};
    explicit ExPolygon(const Polygon &_contour) : contour(_contour) {};
    explicit ExPolygon(const Points &_contour) : contour(Polygon(_contour)) {};
    /// Constructor to build a single holed 
    explicit ExPolygon(const Points &_contour, const Points &_hole) : contour(Polygon(_contour)), holes(Polygons(Polygon(_hole))) { };
    operator Points() const;
    operator Polygons() const;
    void scale(double factor);
    void translate(double x, double y);
    void rotate(double angle);
    void rotate(double angle, const Point &center);
    double area() const;
    bool is_valid() const;
    bool contains(const Line &line) const;
    bool contains(const Polyline &polyline) const;
    bool contains(const Point &point) const;
    bool contains_b(const Point &point) const;
    bool has_boundary_point(const Point &point) const;
    BoundingBox bounding_box() const { return this->contour.bounding_box(); };
    /// removes collinear points within SCALED_EPSILON tolerance
    void remove_colinear_points();
    void remove_vertical_collinear_points(coord_t tolerance);
    void simplify_p(double tolerance, Polygons* polygons) const;
    Polygons simplify_p(double tolerance) const;
    ExPolygons simplify(double tolerance) const;
    void simplify(double tolerance, ExPolygons* expolygons) const;
    void remove_point_too_near(const coord_t tolerance);
    ///TODO: remove that function. It's kept only because of perl tests.
    void medial_axis(double max_width, double min_width, Polylines* polylines) const;
    void medial_axis(const ExPolygon &bounds, double max_width, double min_width, ThickPolylines* polylines) const;
    void get_trapezoids(Polygons* polygons) const;
    void get_trapezoids(Polygons* polygons, double angle) const;
    void get_trapezoids2(Polygons* polygons) const;
    void get_trapezoids2(Polygons* polygons, double angle) const;
    void triangulate(Polygons* polygons) const;
    void triangulate_pp(Polygons* polygons) const;
    void triangulate_p2t(Polygons* polygons) const;
    Lines lines() const;
    std::string dump_perl() const;
};

// Count a number of polygons stored inside the vector of expolygons.
// Useful for allocating space for polygons when converting expolygons to polygons.
inline size_t number_polygons(const ExPolygons &expolys)
{
    size_t n_polygons = 0;
    for (ExPolygons::const_iterator it = expolys.begin(); it != expolys.end(); ++ it)
        n_polygons += it->holes.size() + 1;
    return n_polygons;
}

inline ExPolygons
operator+(ExPolygons src1, const ExPolygons &src2) {
    append_to(src1, src2);
    return src1;
};

std::ostream& operator <<(std::ostream &s, const ExPolygons &expolygons);


inline void 
polygons_append(Polygons &dst, const ExPolygon &src) 
{ 
    dst.reserve(dst.size() + src.holes.size() + 1);
    dst.push_back(src.contour);
    dst.insert(dst.end(), src.holes.begin(), src.holes.end());
}

inline void 
polygons_append(Polygons &dst, const ExPolygons &src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back(it->contour);
        dst.insert(dst.end(), it->holes.begin(), it->holes.end());
    }
}

inline void 
polygons_append(Polygons &dst, ExPolygon &&src)
{ 
    dst.reserve(dst.size() + src.holes.size() + 1);
    dst.push_back(std::move(src.contour));
    std::move(std::begin(src.holes), std::end(src.holes), std::back_inserter(dst));
    src.holes.clear();
}

inline void 
polygons_append(Polygons &dst, ExPolygons &&src)
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (ExPolygons::iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back(std::move(it->contour));
        std::move(std::begin(it->holes), std::end(it->holes), std::back_inserter(dst));
        it->holes.clear();
    }
}

inline void 
expolygons_append(ExPolygons &dst, const ExPolygons &src) 
{ 
    dst.insert(dst.end(), src.begin(), src.end());
}

inline void 
expolygons_append(ExPolygons &dst, ExPolygons &&src)
{ 
    if (dst.empty()) {
        dst = std::move(src);
    } else {
        std::move(std::begin(src), std::end(src), std::back_inserter(dst));
        src.clear();
    }
}

inline Polygons 
to_polygons(const ExPolygon &src)
{
    Polygons polygons;
    polygons.reserve(src.holes.size() + 1);
    polygons.push_back(src.contour);
    polygons.insert(polygons.end(), src.holes.begin(), src.holes.end());
    return polygons;
}

inline Polygons 
to_polygons(const ExPolygons &src)
{
    Polygons polygons;
    polygons.reserve(number_polygons(src));
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++it) {
        polygons.push_back(it->contour);
        polygons.insert(polygons.end(), it->holes.begin(), it->holes.end());
    }
    return polygons;
}

inline Polygons 
to_polygons(ExPolygon &&src)
{
    Polygons polygons;
    polygons.reserve(src.holes.size() + 1);
    polygons.push_back(std::move(src.contour));
    std::move(std::begin(src.holes), std::end(src.holes), std::back_inserter(polygons));
    src.holes.clear();
    return polygons;
}

inline Polygons 
to_polygons(ExPolygons &&src)
{
    Polygons polygons;
    polygons.reserve(number_polygons(src));
    for (ExPolygons::iterator it = src.begin(); it != src.end(); ++it) {
        polygons.push_back(std::move(it->contour));
        std::move(std::begin(it->holes), std::end(it->holes), std::back_inserter(polygons));
        it->holes.clear();
    }
    return polygons;
}

} // namespace Slic3r

// start Boost
#include <boost/polygon/polygon.hpp>
namespace boost { namespace polygon {
    template <>
        struct polygon_traits<ExPolygon> {
        typedef coord_t coordinate_type;
        typedef Points::const_iterator iterator_type;
        typedef Point point_type;

        // Get the begin iterator
        static inline iterator_type begin_points(const ExPolygon& t) {
            return t.contour.points.begin();
        }

        // Get the end iterator
        static inline iterator_type end_points(const ExPolygon& t) {
            return t.contour.points.end();
        }

        // Get the number of sides of the polygon
        static inline std::size_t size(const ExPolygon& t) {
            return t.contour.points.size();
        }

        // Get the winding direction of the polygon
        static inline winding_direction winding(const ExPolygon& t) {
            return unknown_winding;
        }
    };

    template <>
    struct polygon_mutable_traits<ExPolygon> {
        //expects stl style iterators
        template <typename iT>
        static inline ExPolygon& set_points(ExPolygon& expolygon, iT input_begin, iT input_end) {
            expolygon.contour.points.assign(input_begin, input_end);
            // skip last point since Boost will set last point = first point
            expolygon.contour.points.pop_back();
            return expolygon;
        }
    };
    
    
    template <>
    struct geometry_concept<ExPolygon> { typedef polygon_with_holes_concept type; };

    template <>
    struct polygon_with_holes_traits<ExPolygon> {
        typedef Polygons::const_iterator iterator_holes_type;
        typedef Polygon hole_type;
        static inline iterator_holes_type begin_holes(const ExPolygon& t) {
            return t.holes.begin();
        }
        static inline iterator_holes_type end_holes(const ExPolygon& t) {
            return t.holes.end();
        }
        static inline unsigned int size_holes(const ExPolygon& t) {
            return t.holes.size();
        }
    };

    template <>
    struct polygon_with_holes_mutable_traits<ExPolygon> {
         template <typename iT>
         static inline ExPolygon& set_holes(ExPolygon& t, iT inputBegin, iT inputEnd) {
              t.holes.assign(inputBegin, inputEnd);
              return t;
         }
    };
    
    //first we register CPolygonSet as a polygon set
    template <>
    struct geometry_concept<ExPolygons> { typedef polygon_set_concept type; };

    //next we map to the concept through traits
    template <>
    struct polygon_set_traits<ExPolygons> {
        typedef coord_t coordinate_type;
        typedef ExPolygons::const_iterator iterator_type;
        typedef ExPolygons operator_arg_type;

        static inline iterator_type begin(const ExPolygons& polygon_set) {
            return polygon_set.begin();
        }

        static inline iterator_type end(const ExPolygons& polygon_set) {
            return polygon_set.end();
        }

        //don't worry about these, just return false from them
        static inline bool clean(const ExPolygons& polygon_set) { return false; }
        static inline bool sorted(const ExPolygons& polygon_set) { return false; }
    };

    template <>
    struct polygon_set_mutable_traits<ExPolygons> {
        template <typename input_iterator_type>
        static inline void set(ExPolygons& expolygons, input_iterator_type input_begin, input_iterator_type input_end) {
            expolygons.assign(input_begin, input_end);
        }
    };
} }
// end Boost

#endif
