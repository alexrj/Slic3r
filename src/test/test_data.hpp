#ifndef SLIC3R_TEST_DATA_HPP
#define SLIC3R_TEST_DATA_HPP
#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "Geometry.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "Config.hpp"

#include <unordered_map>

namespace Slic3r { namespace Test {

constexpr double MM_PER_MIN = 60.0;

/// Enumeration of test meshes
enum class TestMesh {
    A,
    L,
    V,
    _40x10,
    cube_20x20x20,
    sphere_50mm,
    bridge,
    bridge_with_hole,
    cube_with_concave_hole,
    cube_with_hole,
    gt2_teeth,
    ipadstand,
    overhang,
    pyramid,
    sloping_hole,
    slopy_cube,
    small_dorito,
    step,
    two_hollow_squares
};

// Necessary for <c++17
struct TestMeshHash {
    std::size_t operator()(TestMesh tm) const {
        return static_cast<std::size_t>(tm);
    }
};

/// Mesh enumeration to name mapping
extern const std::unordered_map<TestMesh, const char*, TestMeshHash> mesh_names;

/// Port of Slic3r::Test::mesh
/// Basic cubes/boxes should call TriangleMesh::make_cube() directly and rescale/translate it
TriangleMesh mesh(TestMesh m);

TriangleMesh mesh(TestMesh m, Pointf3 translate, Pointf3 scale = Pointf3(1.0, 1.0, 1.0));
TriangleMesh mesh(TestMesh m, Pointf3 translate, double scale = 1.0);

/// Templated function to see if two values are equivalent (+/- epsilon)
template <typename T>
bool _equiv(const T& a, const T& b) { return abs(a - b) < Slic3r::Geometry::epsilon; }

template <typename T>
bool _equiv(const T& a, const T& b, double epsilon) { return abs(a - b) < epsilon; }

//Slic3r::Model model(const std::string& model_name, TestMesh m, Pointf3 translate = Pointf3(0,0,0), Pointf3 scale = Pointf3(1.0,1.0,1.0));
//Slic3r::Model model(const std::string& model_name, TestMesh m, Pointf3 translate = Pointf3(0,0,0), double scale = 1.0);

Slic3r::Model model(const std::string& model_name, TriangleMesh&& _mesh);

shared_Print init_print(std::initializer_list<TestMesh> meshes, Slic3r::Model& model, config_ptr _config = Slic3r::Config::new_from_defaults(), bool comments = false, Slic3r::Pointf center = Slic3r::Pointf(100,100));
shared_Print init_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Model& model, config_ptr _config = Slic3r::Config::new_from_defaults(), bool comments = false, Slic3r::Pointf center = Slic3r::Pointf(100,100));

void gcode(std::stringstream& gcode, shared_Print print);

} } // namespace Slic3r::Test


#endif // SLIC3R_TEST_DATA_HPP
