#include <catch.hpp>
#include "test_data.hpp" // get access to init_print, etc
#include "Config.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExtrusionEntity.hpp"
#include "GCodeReader.hpp"
#include "Point.hpp"
#include <cstdlib>

Point random_point(float LO=-50, float HI=50) {
    float x = LO + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(HI-LO)));
    float y = LO + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(HI-LO)));
    return Point(x, y);
}

// build a sample extrusion entity collection with random start and end points.
ExtrusionPath random_path(size_t length = 20, float LO=-50, float HI=50) {
    ExtrusionPath t {erPerimeter, 1.0, 1.0, 1.0};
    for (size_t j = 0; j < length; j++) {
        t.polyline.append(random_point(LO, HI));
    }
    return t;
}

ExtrusionPaths random_paths(size_t count=10, size_t length=20, float LO=-50, float HI=50) {
    ExtrusionPaths p;
    for (size_t i = 0; i < count; i++)
        p.push_back(random_path(length, LO, HI));
    return p;
}

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("ExtrusionEntityCollection: Polygon flattening") {
    srand(0xDEADBEEF); // consistent seed for test reproducibility.

    // Generate one specific random path set and save it for later comparison
    ExtrusionPaths nosort_path_set{ random_paths() };

    ExtrusionEntityCollection sub_nosort;
    sub_nosort.append(nosort_path_set);
    sub_nosort.no_sort = true;

    ExtrusionEntityCollection sub_sort;
    sub_sort.no_sort = false;
    sub_sort.append(random_paths());


    GIVEN("A Extrusion Entity Collection with a child that has one child that is marked as no-sort") {
        ExtrusionEntityCollection sample;
        ExtrusionEntityCollection output;

        sample.append(sub_sort);
        sample.append(sub_nosort);
        sample.append(sub_sort);
        WHEN("The EEC is flattened with default options (preserve_order=false)") {
            sample.flatten(&output);
            THEN("The output EEC contains no Extrusion Entity Collections") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection(); }) == 0);
            }
        }
        WHEN("The EEC is flattened with preservation (preserve_order=true)") {
            sample.flatten(&output, true);
            THEN("The output EECs contains one EEC.") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection(); }) == 1);
            }
            AND_THEN("The ordered EEC contains the same order of elements than the original") {
                // find the entity in the collection
                for (auto e : output.entities) {
                    if (!e->is_collection()) continue;
                    ExtrusionEntityCollection* temp = dynamic_cast<ExtrusionEntityCollection*>(e);
                    // check each Extrusion path against nosort_path_set to see if the first and last match the same
                    CHECK(nosort_path_set.size() == temp->size());
                    for (size_t i = 0; i < nosort_path_set.size(); i++) {
                        CHECK(temp->entities[i]->first_point() == nosort_path_set[i].first_point());
                        CHECK(temp->entities[i]->last_point() == nosort_path_set[i].last_point());
                    }
                }
            }
        }
    }
}

SCENARIO("ExtrusionEntityCollection: no sort") {
    auto config{ Config::new_from_defaults() };
    config->set("gcode_comments", true);
    config->set("skirts", "0");
    
    Slic3r::Model model;
    shared_Print print{ Slic3r::Test::init_print({ TestMesh::cube_20x20x20 }, model, config) };

    std::map<double, bool> layers_with_skirt;
    std::map<double, bool> layers_with_brim;

    print->process();
    //replace extrusion from sliceing by manual ones
    print->objects[0]->clear_layers();
    Layer* customL_layer = print->objects[0]->add_layer(0, 0.2, 0.2, 0.1);
    LayerRegion* custom_region = customL_layer->add_region(print->regions[0]);

    ExtrusionPath path_peri(ExtrusionRole::erPerimeter);
    path_peri.polyline.append(Point{ 0,0 });
    path_peri.polyline.append(Point{ scale_(1),scale_(0) });
    ExtrusionPath path_fill1(ExtrusionRole::erInternalInfill);
    path_fill1.polyline.append(Point{ scale_(1),scale_(0) });
    path_fill1.polyline.append(Point{ scale_(2),scale_(0) });
    ExtrusionPath path_fill2(ExtrusionRole::erInternalInfill);
    path_fill2.polyline.append(Point{ scale_(2),scale_(0) });
    path_fill2.polyline.append(Point{ scale_(3),scale_(0) });
    ExtrusionEntityCollection coll_fill;
    coll_fill.append(path_fill2);
    coll_fill.append(path_fill1);
    ExtrusionEntityCollection coll_peri;
    coll_peri.append(path_peri);


    WHEN("sort") {
        custom_region->fills.append(coll_fill);
        custom_region->perimeters.append(coll_peri);
        coll_fill.no_sort = false;
        std::stringstream gcode;
        Slic3r::Test::gcode(gcode, print);
        auto parser{ Slic3r::GCodeReader() };
        std::vector<float> extrude_x;
        parser.parse_stream(gcode, [](Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
        {
            line.extruding();
            if (self.Z == Approx(0.3) || line.new_Z() == Approx(0.3)) {
                if (line.extruding() && self.F == Approx(12)) {
                }
            }
        });
        parser.parse_stream(gcode, [&extrude_x, &config](Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
        {
            line.extruding();
            std::cout << "line.comment='" << line.comment << "'\n";
            if (line.comment == " infill" || line.comment == " perimeter" || line.comment == " move to first infill point") {
                extrude_x.push_back(line.new_X());
            }
        });
        REQUIRE(extrude_x.size()==3);
        REQUIRE(extrude_x[0] == 91);
        REQUIRE(extrude_x[1] == 92);
        REQUIRE(extrude_x[2] == 93);
    }


    WHEN("no sort") {
        coll_fill.no_sort = true;
        custom_region->fills.append(coll_fill);
        custom_region->perimeters.append(coll_peri);
        std::stringstream gcode;
        Slic3r::Test::gcode(gcode, print);
        auto parser{ Slic3r::GCodeReader() };
        std::vector<float> extrude_x;
        parser.parse_stream(gcode, [&extrude_x, &config](Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
        {
            if (line.comment == " infill" || line.comment == " perimeter" || line.comment == " move to first infill point") {
                extrude_x.push_back(line.new_X());
            }
        });
        REQUIRE(extrude_x.size() == 5);
        REQUIRE(extrude_x[0] == 91);
        REQUIRE(extrude_x[1] == 92);
        REQUIRE(extrude_x[2] == 93);
        REQUIRE(extrude_x[3] == 91);
        REQUIRE(extrude_x[4] == 92);
    }
}
