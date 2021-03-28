#include <catch.hpp>
#include <string>
#include "test_data.hpp"
#include "libslic3r.h"

using namespace Slic3r::Test;
using namespace std::literals;

SCENARIO("PrintObject: Perimeter generation") {
    GIVEN("20mm cube and default config") {
        auto config {Slic3r::Config::new_from_defaults()};
        TestMesh m { TestMesh::cube_20x20x20 };
        Slic3r::Model model;
        auto event_counter {0U};
        std::string stage;
        int value {0};
        auto callback {[&event_counter, &stage, &value] (int a, const char* b) { stage = std::string(b); event_counter++; value = a; }};
        config->set("fill_density", 0);

        WHEN("make_perimeters() is called")  {
            auto print {Slic3r::Test::init_print({m}, model, config)};
            const auto& object = *(print->objects.at(0));
            print->objects[0]->make_perimeters();
            THEN("67 layers exist in the model") {
                REQUIRE(object.layers.size() == 67);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for(auto* layer : object.layers) {
                    REQUIRE(layer->regions[0]->perimeters.size() == 1);
                }
            }
            THEN("Every layer in region 0 has 3 paths in its perimeters list.") {
                for(auto* layer : object.layers) {
                    REQUIRE(layer->regions[0]->perimeters.items_count() == 3);
                }
            }
        }
    }
}

SCENARIO("Print: Skirt generation") {
    GIVEN("20mm cube and default config") {
        auto config {Slic3r::Config::new_from_defaults()};
        TestMesh m { TestMesh::cube_20x20x20 };
        Slic3r::Model model;
        auto event_counter {0U};
        std::string stage;
        int value {0};
        config->set("skirt_height", 1);
        config->set("skirt_distance", 1);
        WHEN("Skirts is set to 2 loops")  {
            config->set("skirts", 2);
            auto print {Slic3r::Test::init_print({m}, model, config)};
            print->make_skirt();
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print->skirt.items_count() == 2);
                REQUIRE(print->skirt.flatten().entities.size() == 2);
            }
        }
    }
}

void test_is_solid_infill(shared_Print p, size_t obj_id, size_t layer_id ) {
    const PrintObject& obj { *(p->objects.at(obj_id)) };
    const Layer& layer { *(obj.get_layer(layer_id)) };

    // iterate over all of the regions in the layer
    for (const LayerRegion* reg : layer.regions) {
        // for each region, iterate over the fill surfaces
        for (const Surface& s : reg->fill_surfaces) {
            CHECK(s.is_solid());
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        auto config {Slic3r::Config::new_from_defaults()};
        TestMesh m { TestMesh::cube_20x20x20 };
        config->set("top_solid_layers", 2);
        config->set("bottom_solid_layers", 1);
        config->set("layer_height", 0.5); // get a known number of layers
        config->set("first_layer_height", 0.5);
        Slic3r::Model model;
        auto event_counter {0U};
        std::string stage;
        auto print {Slic3r::Test::init_print({m}, model, config)};
        print->process();
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
        test_is_solid_infill(print, 0, 0); // should be solid
        test_is_solid_infill(print, 0, 39); // should be solid
        test_is_solid_infill(print, 0, 38); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
            print->regions[0]->config.top_solid_layers = 3;
            print->objects[0]->invalidate_step(posPrepareInfill);
            print->process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(print, 0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(print, 0, 39);
                test_is_solid_infill(print, 0, 38);
                test_is_solid_infill(print, 0, 37);
            }
        }
    }

}

SCENARIO("Print: Brim generation") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        auto config {Slic3r::Config::new_from_defaults()};
        TestMesh m { TestMesh::cube_20x20x20 };
        Slic3r::Model model;
        auto event_counter {0U};
        std::string stage;
        int value {0};
        config->set("first_layer_extrusion_width", 1);
        WHEN("Brim is set to 3mm")  {
            config->set("brim_width", 3);
            auto print {Slic3r::Test::init_print({m}, model, config)};
            print->make_brim();
            THEN("Brim Extrusion collection has 4 loops in it") {
                REQUIRE(print->brim.items_count() == 4);
            }
        }
        WHEN("Brim is set to 6mm")  {
            config->set("brim_width", 6);
            auto print {Slic3r::Test::init_print({m}, model, config)};
            print->make_brim();
            THEN("Brim Extrusion collection has 7 loops in it") {
                REQUIRE(print->brim.items_count() == 7);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
            config->set("brim_width", 6);
            config->set("first_layer_extrusion_width", 0.5);
            auto print {Slic3r::Test::init_print({m}, model, config)};
            print->make_brim();
            THEN("Brim Extrusion collection has 13 loops in it") {
                REQUIRE(print->brim.items_count() == 13);
            }
        }
    }
}
