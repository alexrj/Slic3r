#include "Geometry.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Line.hpp"
#include "Log.hpp"
#include "PolylineCollection.hpp"
#include "clipper.hpp"
#include <algorithm>
#include <numeric>
#include <cassert>
#include <cmath>
#include <list>
#include <map>
#include <set>
#include <utility>
#include <stack>
#include <vector>

#ifdef SLIC3R_DEBUG
#include "SVG.hpp"
#endif

#ifdef SLIC3R_DEBUG
namespace boost { namespace polygon {

// The following code for the visualization of the boost Voronoi diagram is based on:
//
// Boost.Polygon library voronoi_graphic_utils.hpp header file
//          Copyright Andrii Sydorchuk 2010-2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
template <typename CT>
class voronoi_visual_utils {
 public:
  // Discretize parabolic Voronoi edge.
  // Parabolic Voronoi edges are always formed by one point and one segment
  // from the initial input set.
  //
  // Args:
  //   point: input point.
  //   segment: input segment.
  //   max_dist: maximum discretization distance.
  //   discretization: point discretization of the given Voronoi edge.
  //
  // Template arguments:
  //   InCT: coordinate type of the input geometries (usually integer).
  //   Point: point type, should model point concept.
  //   Segment: segment type, should model segment concept.
  //
  // Important:
  //   discretization should contain both edge endpoints initially.
  template <class InCT1, class InCT2,
            template<class> class Point,
            template<class> class Segment>
  static
  typename enable_if<
    typename gtl_and<
      typename gtl_if<
        typename is_point_concept<
          typename geometry_concept< Point<InCT1> >::type
        >::type
      >::type,
      typename gtl_if<
        typename is_segment_concept<
          typename geometry_concept< Segment<InCT2> >::type
        >::type
      >::type
    >::type,
    void
  >::type discretize(
      const Point<InCT1>& point,
      const Segment<InCT2>& segment,
      const CT max_dist,
      std::vector< Point<CT> >* discretization) {
    // Apply the linear transformation to move start point of the segment to
    // the point with coordinates (0, 0) and the direction of the segment to
    // coincide the positive direction of the x-axis.
    CT segm_vec_x = cast(x(high(segment))) - cast(x(low(segment)));
    CT segm_vec_y = cast(y(high(segment))) - cast(y(low(segment)));
    CT sqr_segment_length = segm_vec_x * segm_vec_x + segm_vec_y * segm_vec_y;

    // Compute x-coordinates of the endpoints of the edge
    // in the transformed space.
    CT projection_start = sqr_segment_length *
        get_point_projection((*discretization)[0], segment);
    CT projection_end = sqr_segment_length *
        get_point_projection((*discretization)[1], segment);

    // Compute parabola parameters in the transformed space.
    // Parabola has next representation:
    // f(x) = ((x-rot_x)^2 + rot_y^2) / (2.0*rot_y).
    CT point_vec_x = cast(x(point)) - cast(x(low(segment)));
    CT point_vec_y = cast(y(point)) - cast(y(low(segment)));
    CT rot_x = segm_vec_x * point_vec_x + segm_vec_y * point_vec_y;
    CT rot_y = segm_vec_x * point_vec_y - segm_vec_y * point_vec_x;

    // Save the last point.
    Point<CT> last_point = (*discretization)[1];
    discretization->pop_back();

    // Use stack to avoid recursion.
    std::stack<CT> point_stack;
    point_stack.push(projection_end);
    CT cur_x = projection_start;
    CT cur_y = parabola_y(cur_x, rot_x, rot_y);

    // Adjust max_dist parameter in the transformed space.
    const CT max_dist_transformed = max_dist * max_dist * sqr_segment_length;
    while (!point_stack.empty()) {
      CT new_x = point_stack.top();
      CT new_y = parabola_y(new_x, rot_x, rot_y);

      // Compute coordinates of the point of the parabola that is
      // furthest from the current line segment.
      CT mid_x = (new_y - cur_y) / (new_x - cur_x) * rot_y + rot_x;
      CT mid_y = parabola_y(mid_x, rot_x, rot_y);

      // Compute maximum distance between the given parabolic arc
      // and line segment that discretize it.
      CT dist = (new_y - cur_y) * (mid_x - cur_x) -
          (new_x - cur_x) * (mid_y - cur_y);
      dist = dist * dist / ((new_y - cur_y) * (new_y - cur_y) +
          (new_x - cur_x) * (new_x - cur_x));
      if (dist <= max_dist_transformed) {
        // Distance between parabola and line segment is less than max_dist.
        point_stack.pop();
        CT inter_x = (segm_vec_x * new_x - segm_vec_y * new_y) /
            sqr_segment_length + cast(x(low(segment)));
        CT inter_y = (segm_vec_x * new_y + segm_vec_y * new_x) /
            sqr_segment_length + cast(y(low(segment)));
        discretization->push_back(Point<CT>(inter_x, inter_y));
        cur_x = new_x;
        cur_y = new_y;
      } else {
        point_stack.push(mid_x);
      }
    }

    // Update last point.
    discretization->back() = last_point;
  }

 private:
  // Compute y(x) = ((x - a) * (x - a) + b * b) / (2 * b).
  static CT parabola_y(CT x, CT a, CT b) {
    return ((x - a) * (x - a) + b * b) / (b + b);
  }

  // Get normalized length of the distance between:
  //   1) point projection onto the segment
  //   2) start point of the segment
  // Return this length divided by the segment length. This is made to avoid
  // sqrt computation during transformation from the initial space to the
  // transformed one and vice versa. The assumption is made that projection of
  // the point lies between the start-point and endpoint of the segment.
  template <class InCT,
            template<class> class Point,
            template<class> class Segment>
  static
  typename enable_if<
    typename gtl_and<
      typename gtl_if<
        typename is_point_concept<
          typename geometry_concept< Point<int> >::type
        >::type
      >::type,
      typename gtl_if<
        typename is_segment_concept<
          typename geometry_concept< Segment<long> >::type
        >::type
      >::type
    >::type,
    CT
  >::type get_point_projection(
      const Point<CT>& point, const Segment<InCT>& segment) {
    CT segment_vec_x = cast(x(high(segment))) - cast(x(low(segment)));
    CT segment_vec_y = cast(y(high(segment))) - cast(y(low(segment)));
    CT point_vec_x = x(point) - cast(x(low(segment)));
    CT point_vec_y = y(point) - cast(y(low(segment)));
    CT sqr_segment_length =
        segment_vec_x * segment_vec_x + segment_vec_y * segment_vec_y;
    CT vec_dot = segment_vec_x * point_vec_x + segment_vec_y * point_vec_y;
    return vec_dot / sqr_segment_length;
  }

  template <typename InCT>
  static CT cast(const InCT& value) {
    return static_cast<CT>(value);
  }
};

} } // namespace boost::polygon
#endif

using namespace boost::polygon;  // provides also high() and low()

namespace Slic3r { namespace Geometry {

static bool
sort_points (Point a, Point b)
{
    return (a.x < b.x) || (a.x == b.x && a.y < b.y);
}

/* This implementation is based on Andrew's monotone chain 2D convex hull algorithm */
Polygon
convex_hull(Points points)
{
    assert(points.size() >= 3);
    // sort input points
    std::sort(points.begin(), points.end(), sort_points);
    
    int n = points.size(), k = 0;
    Polygon hull;
    hull.points.resize(2*n);

    // Build lower hull
    for (int i = 0; i < n; i++) {
        while (k >= 2 && points[i].ccw(hull.points[k-2], hull.points[k-1]) <= 0) k--;
        hull.points[k++] = points[i];
    }

    // Build upper hull
    for (int i = n-2, t = k+1; i >= 0; i--) {
        while (k >= t && points[i].ccw(hull.points[k-2], hull.points[k-1]) <= 0) k--;
        hull.points[k++] = points[i];
    }

    hull.points.resize(k);
    
    assert( hull.points.front().coincides_with(hull.points.back()) );
    hull.points.pop_back();
    
    return hull;
}

Polygon
convex_hull(const Polygons &polygons)
{
    Points pp;
    for (Polygons::const_iterator p = polygons.begin(); p != polygons.end(); ++p) {
        pp.insert(pp.end(), p->points.begin(), p->points.end());
    }
    return convex_hull(pp);
}

/* accepts an arrayref of points and returns a list of indices
   according to a nearest-neighbor walk */
void
chained_path(const Points &points, std::vector<Points::size_type> &retval, Point start_near)
{
    PointConstPtrs my_points;
    std::map<const Point*,Points::size_type> indices;
    my_points.reserve(points.size());
    for (Points::const_iterator it = points.begin(); it != points.end(); ++it) {
        my_points.push_back(&*it);
        indices[&*it] = it - points.begin();
    }
    
    retval.reserve(points.size());
    while (!my_points.empty()) {
        Points::size_type idx = start_near.nearest_point_index(my_points);
        start_near = *my_points[idx];
        retval.push_back(indices[ my_points[idx] ]);
        my_points.erase(my_points.begin() + idx);
    }
}

void
chained_path(const Points &points, std::vector<Points::size_type> &retval)
{
    if (points.empty()) return;  // can't call front() on empty vector
    chained_path(points, retval, points.front());
}

/* retval and items must be different containers */
template<class T>
void
chained_path_items(Points &points, T &items, T &retval)
{
    std::vector<Points::size_type> indices;
    chained_path(points, indices);
    for (std::vector<Points::size_type>::const_iterator it = indices.begin(); it != indices.end(); ++it)
        retval.push_back(items[*it]);
}
template void chained_path_items(Points &points, ClipperLib::PolyNodes &items, ClipperLib::PolyNodes &retval);

bool
directions_parallel(double angle1, double angle2, double max_diff)
{
    double diff = fabs(angle1 - angle2);
    max_diff += EPSILON;
    return diff < max_diff || fabs(diff - PI) < max_diff;
}

template<class T>
bool
contains(const std::vector<T> &vector, const Point &point)
{
    for (const T &it : vector)
        if (it.contains(point))
            return true;
    
    return false;
}
template bool contains(const Polygons &vector, const Point &point);
template bool contains(const ExPolygons &vector, const Point &point);

template<class T>
double
area(const std::vector<T> &vector)
{
    double area = 0;
    for (const T &it : vector)
        area += it.area();
    
    return area;
}
template double area(const Polygons &vector);

double
rad2deg(double angle)
{
    return angle / PI * 180.0;
}

double
rad2deg_dir(double angle)
{
    angle = (angle < PI) ? (-angle + PI/2.0) : (angle + PI/2.0);
    if (angle < 0) angle += PI;
    return rad2deg(angle);
}

double
deg2rad(double angle)
{
    return PI * angle / 180.0;
}

double
linint(double value, double oldmin, double oldmax, double newmin, double newmax)
{
    return (value - oldmin) * (newmax - newmin) / (oldmax - oldmin) + newmin;
}

Point
circle_taubin_newton(const Points& input, size_t cycles)
{
    return circle_taubin_newton(input.cbegin(), input.cend(), cycles);
}

Point
circle_taubin_newton(const Points::const_iterator& input_begin, const Points::const_iterator& input_end, size_t cycles)
{
    Pointfs tmp;
    tmp.reserve(std::distance(input_begin, input_end));
    std::transform(input_begin, input_end, std::back_inserter(tmp), [] (const Point& in) {return Pointf::new_unscale(in); } );
    return Point::new_scale(circle_taubin_newton(tmp.cbegin(), tmp.end(), cycles));
}

Pointf
circle_taubin_newton(const Pointfs& input, size_t cycles)
{
    return circle_taubin_newton(input.cbegin(), input.cend(), cycles);
}


/// Adapted from work in "Circular and Linear Regression: Fitting circles and lines by least squares", pg 126
/// Returns a point corresponding to the center of a circle for which all of the points from input_begin to input_end
/// lie on.
Pointf
circle_taubin_newton(const Pointfs::const_iterator& input_begin, const Pointfs::const_iterator& input_end, size_t cycles)
{
    // calculate the centroid of the data set
    const Pointf sum = std::accumulate(input_begin, input_end, Pointf(0,0));
    const size_t n = std::distance(input_begin, input_end);
    const double n_flt = static_cast<double>(n);
    const Pointf centroid { sum / n_flt };

    // Compute the normalized moments of the data set.
    double Mxx = 0, Myy = 0, Mxy = 0, Mxz = 0, Myz = 0, Mzz = 0;
    for (auto it = input_begin; it < input_end; ++it) {
        // center/normalize the data.
        double Xi {it->x - centroid.x};
        double Yi {it->y - centroid.y};
        double Zi {Xi*Xi + Yi*Yi};
        Mxy += (Xi*Yi);
        Mxx += (Xi*Xi);
        Myy += (Yi*Yi);
        Mxz += (Xi*Zi);
        Myz += (Yi*Zi);
        Mzz += (Zi*Zi);
    }

    // divide by number of points to get the moments
    Mxx /= n_flt;
    Myy /= n_flt;
    Mxy /= n_flt;
    Mxz /= n_flt;
    Myz /= n_flt;
    Mzz /= n_flt;

    // Compute the coefficients of the characteristic polynomial for the circle
    // eq 5.60
    const double Mz {Mxx + Myy}; // xx + yy = z
    const double Cov_xy {Mxx*Myy - Mxy*Mxy}; // this shows up a couple times so cache it here.
    const double C3 {4.0*Mz};
    const double C2 {-3.0*(Mz*Mz) - Mzz};
    const double C1 {Mz*(Mzz - (Mz*Mz)) + 4.0*Mz*Cov_xy - (Mxz*Mxz) - (Myz*Myz)};
    const double C0 {(Mxz*Mxz)*Myy + (Myz*Myz)*Mxx - 2.0*Mxz*Myz*Mxy - Cov_xy*(Mzz - (Mz*Mz))};

    const double C22 = {C2 + C2};
    const double C33 = {C3 + C3 + C3};

    // solve the characteristic polynomial with Newton's method.
    double xnew = 0.0;
    double ynew = 1e20;

    for (size_t i = 0; i < cycles; ++i) {
        const double yold {ynew};
        ynew = C0 + xnew * (C1 + xnew*(C2 + xnew * C3));
        if (std::abs(ynew) > std::abs(yold)) {
            Slic3r::Log::error("Geometry") << "Fit is going in the wrong direction.\n";
            return Pointf(std::nan(""), std::nan(""));
        }
        const double Dy {C1 + xnew*(C22 + xnew*C33)};

        const double xold {xnew};
        xnew = xold - (ynew / Dy);

        if (std::abs((xnew-xold) / xnew) < 1e-12) i = cycles; // converged, we're done here

        if (xnew < 0) {
            // reset, we went negative
            xnew = 0.0;
        }
    }
    
    // compute the determinant and the circle's parameters now that we've solved.
    double DET = xnew*xnew - xnew*Mz + Cov_xy;

    Pointf center {Pointf(Mxz * (Myy - xnew) - Myz * Mxy, Myz * (Mxx - xnew) - Mxz*Mxy)};
    center = center / DET / 2;

    return center + centroid;
    
}

/*
== Perl implementations for methods tested in geometry.t but not translated.  ==
== The first three are unreachable in the current perl code and the fourth is ==
== only called from ArcFitting which currently dies before reaching the call. ==
sub point_in_segment {
    my ($point, $line) = @_;

    my ($x, $y) = @$point;
    my $line_p = $line->pp;
    my @line_x = sort { $a <=> $b } $line_p->[A][X], $line_p->[B][X];
    my @line_y = sort { $a <=> $b } $line_p->[A][Y], $line_p->[B][Y];

    # check whether the point is in the segment bounding box
    return 0 unless $x >= ($line_x[0] - epsilon) && $x <= ($line_x[1] + epsilon)
        && $y >= ($line_y[0] - epsilon) && $y <= ($line_y[1] + epsilon);

    # if line is vertical, check whether point's X is the same as the line
    if ($line_p->[A][X] == $line_p->[B][X]) {
        return abs($x - $line_p->[A][X]) < epsilon ? 1 : 0;
    }

    # calculate the Y in line at X of the point
    my $y3 = $line_p->[A][Y] + ($line_p->[B][Y] - $line_p->[A][Y])
        * ($x - $line_p->[A][X]) / ($line_p->[B][X] - $line_p->[A][X]);
    return abs($y3 - $y) < epsilon ? 1 : 0;
}


# given a $polygon, return the (first) segment having $point
sub polygon_segment_having_point {
    my ($polygon, $point) = @_;

    foreach my $line (@{ $polygon->lines }) {
        return $line if point_in_segment($point, $line);
    }
    return undef;
}


# polygon must be simple (non complex) and ccw
sub polygon_is_convex {
    my ($points) = @_;
    for (my $i = 0; $i <= $#$points; $i++) {
        my $angle = angle3points($points->[$i-1], $points->[$i-2], $points->[$i]);
        return 0 if $angle < PI;
    }
    return 1;
}

# this assumes a CCW rotation from $p2 to $p3 around $p1
sub angle3points {
    my ($p1, $p2, $p3) = @_;
    # p1 is the center

    my $angle = atan2($p2->[X] - $p1->[X], $p2->[Y] - $p1->[Y])
              - atan2($p3->[X] - $p1->[X], $p3->[Y] - $p1->[Y]);

    # we only want to return only positive angles
    return $angle <= 0 ? $angle + 2*PI() : $angle;
}
*/


class ArrangeItem {
    public:
    Pointf pos;
    size_t index_x, index_y;
    coordf_t dist;
};
class ArrangeItemIndex {
    public:
    coordf_t index;
    ArrangeItem item;
    ArrangeItemIndex(coordf_t _index, ArrangeItem _item) : index(_index), item(_item) {};
};

bool
arrange(size_t total_parts, const Pointf &part_size, coordf_t dist, const BoundingBoxf* bb, Pointfs &positions)
{
    positions.clear();

    Pointf part = part_size;

    // use actual part size (the largest) plus separation distance (half on each side) in spacing algorithm
    part.x += dist;
    part.y += dist;
    
    Pointf area;
    if (bb != NULL && bb->defined) {
        area = bb->size();
    } else {
        // bogus area size, large enough not to trigger the error below
        area.x = part.x * total_parts;
        area.y = part.y * total_parts;
    }
    
    // this is how many cells we have available into which to put parts
    size_t cellw = floor((area.x + dist) / part.x);
    size_t cellh = floor((area.y + dist) / part.y);
    if (total_parts > (cellw * cellh))
        return false;
    
    // total space used by cells
    Pointf cells(cellw * part.x, cellh * part.y);
    
    // bounding box of total space used by cells
    BoundingBoxf cells_bb;
    cells_bb.merge(Pointf(0,0)); // min
    cells_bb.merge(cells);  // max
    
    // center bounding box to area
    cells_bb.translate(
        (area.x - cells.x) / 2,
        (area.y - cells.y) / 2
    );
    
    // list of cells, sorted by distance from center
    std::vector<ArrangeItemIndex> cellsorder;
    
    // work out distance for all cells, sort into list
    for (size_t i = 0; i <= cellw-1; ++i) {
        for (size_t j = 0; j <= cellh-1; ++j) {
            coordf_t cx = linint(i + 0.5, 0, cellw, cells_bb.min.x, cells_bb.max.x);
            coordf_t cy = linint(j + 0.5, 0, cellh, cells_bb.min.y, cells_bb.max.y);
            
            coordf_t xd = fabs((area.x / 2) - cx);
            coordf_t yd = fabs((area.y / 2) - cy);
            
            ArrangeItem c;
            c.pos.x = cx;
            c.pos.y = cy;
            c.index_x = i;
            c.index_y = j;
            c.dist = xd * xd + yd * yd - fabs((cellw / 2) - (i + 0.5));
            
            // binary insertion sort
            {
                coordf_t index = c.dist;
                size_t low = 0;
                size_t high = cellsorder.size();
                while (low < high) {
                    size_t mid = (low + ((high - low) / 2)) | 0;
                    coordf_t midval = cellsorder[mid].index;
                    
                    if (midval < index) {
                        low = mid + 1;
                    } else if (midval > index) {
                        high = mid;
                    } else {
                        cellsorder.insert(cellsorder.begin() + mid, ArrangeItemIndex(index, c));
                        goto ENDSORT;
                    }
                }
                cellsorder.insert(cellsorder.begin() + low, ArrangeItemIndex(index, c));
            }
            ENDSORT: ;
        }
    }
    
    // the extents of cells actually used by objects
    coordf_t lx = 0;
    coordf_t ty = 0;
    coordf_t rx = 0;
    coordf_t by = 0;

    // now find cells actually used by objects, map out the extents so we can position correctly
    for (size_t i = 1; i <= total_parts; ++i) {
        ArrangeItemIndex c = cellsorder[i - 1];
        coordf_t cx = c.item.index_x;
        coordf_t cy = c.item.index_y;
        if (i == 1) {
            lx = rx = cx;
            ty = by = cy;
        } else {
            if (cx > rx) rx = cx;
            if (cx < lx) lx = cx;
            if (cy > by) by = cy;
            if (cy < ty) ty = cy;
        }
    }
    // now we actually place objects into cells, positioned such that the left and bottom borders are at 0
    for (size_t i = 1; i <= total_parts; ++i) {
        ArrangeItemIndex c = cellsorder.front();
        cellsorder.erase(cellsorder.begin());
        coordf_t cx = c.item.index_x - lx;
        coordf_t cy = c.item.index_y - ty;
        
        positions.push_back(Pointf(cx * part.x, cy * part.y));
    }
    
    if (bb != NULL && bb->defined) {
        for (Pointfs::iterator p = positions.begin(); p != positions.end(); ++p) {
            p->x += bb->min.x;
            p->y += bb->min.y;
        }
    }
    
    return true;
}

    
} }
