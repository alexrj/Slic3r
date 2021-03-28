#include <catch.hpp>
#include "test_data.hpp" // get access to init_print, etc
#include "GCodeReader.hpp"
#include "Config.hpp"
#include "Geometry.hpp"
#include <regex>

using namespace Slic3r::Test;
using namespace Slic3r;

/// Helper method to find the tool used for the brim (always the first extrusion)
int get_brim_tool(std::stringstream& gcode, Slic3r::GCodeReader& parser) {
    int brim_tool = -1;
    std::regex tool_regex("^T(\\d+)");
    std::smatch m;
    int tool = -1;

    parser.parse_stream(gcode, [&tool, &brim_tool, &m, tool_regex] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
        {
            // if the command is a T command, set the the current tool
            if (std::regex_match(line.cmd, m, tool_regex)) {
                tool = std::stoi(m[1].str());
            } else if (line.cmd == "G1" && line.extruding() && line.dist_XY() > 0 && brim_tool < 0) {
                brim_tool = tool;
                }
        });

    return brim_tool;
}


TEST_CASE("Skirt height is honored") {
    auto config {Config::new_from_defaults()};
    config->set("skirts", 1);
    config->set("skirt_height", 5);
    config->set("perimeters", 0);
    config->set("support_material_speed", 99);

    // avoid altering speeds unexpectedly
    config->set("cooling", false);
    config->set("first_layer_speed", "100%");
    auto support_speed = config->get<ConfigOptionFloat>("support_material_speed") * MM_PER_MIN;

    std::map<double, bool> layers_with_skirt;
    auto gcode {std::stringstream("")};
    gcode.clear();
    auto parser {Slic3r::GCodeReader()};
    Slic3r::Model model;

    SECTION("printing a single object") {
        auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
        Slic3r::Test::gcode(gcode, print);
    }

    SECTION("printing multiple objects") {
        auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, model, config)};
        Slic3r::Test::gcode(gcode, print);
    }
    parser.parse_stream(gcode, [&layers_with_skirt, &support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
    {
        if (line.extruding() && self.F == Approx(support_speed)) {
            layers_with_skirt[self.Z] = 1;
        }
    });

    REQUIRE(layers_with_skirt.size() == (size_t)config->getInt("skirt_height"));
}

SCENARIO("Original Slic3r Skirt/Brim tests", "[!mayfail]") {
    auto parser {Slic3r::GCodeReader()};
    Slic3r::Model model;
    auto gcode {std::stringstream("")};
    gcode.clear();
    const double used_layer_height = 0.01;
    GIVEN("A default configuration") {
        auto config {Config::new_from_defaults()};
        config->set("support_material_speed", 99);
        config->set("first_layer_height", used_layer_height);
        config->set("gcode_comments", true);
        // avoid altering speeds unexpectedly
        config->set("cooling", false);
        config->set("first_layer_speed", "100%");
        // remove noise from top/solid layers
        config->set("top_solid_layers", 0);
        config->set("bottom_solid_layers", 1);

        WHEN("Brim width is set to 5") {
            config->set("perimeters", 0);
            config->set("skirts", 0);
            config->set("brim_width", 5);
            config->set("interior_brim_width", 0);
            config->set("brim_connections_width", 0);
            config->set("first_layer_extrusion_width", 0.10);
            config->set("extrusion_width", 0.10);
            config->set("nozzle_diameter", "0.05");
            THEN("Configuration is sane.") {
                REQUIRE(config->get<ConfigOptionFloatOrPercent>("first_layer_extrusion_width").get_abs_value(used_layer_height) > 0.05);
                REQUIRE(used_layer_height < 0.05);
            }
            THEN("Brim is generated") {
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                Slic3r::Test::gcode(gcode, print);
                bool brim_generated = false;
                auto support_speed = config->get<ConfigOptionFloat>("support_material_speed") * MM_PER_MIN;
                parser.parse_stream(gcode, [&brim_generated, support_speed, used_layer_height] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
                    {
                        if (self.Z == Approx(used_layer_height) || line.new_Z() == Approx(used_layer_height)) {
                            if (line.extruding() && self.F == Approx(support_speed)) {
                                brim_generated = true;
                            }
                        }
                    });
                REQUIRE(brim_generated);
            }
            THEN("Brim width is 5mm around the object.") {
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config, false, Slic3r::Pointf(0, 0))};
                Slic3r::Test::gcode(gcode, print);
                double max_x = 0, min_x = 0, max_y = 0, min_y = 0;
                auto support_speed = config->get<ConfigOptionFloat>("support_material_speed") * MM_PER_MIN;
                auto line_width = config->get<ConfigOptionFloatOrPercent>("first_layer_extrusion_width").get_abs_value(used_layer_height);
                auto brim = config->get<ConfigOptionFloat>("brim_width");
                parser.parse_stream(gcode, [&max_x, &max_y, &min_x, &min_y, line_width, support_speed, used_layer_height] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
                    {
                        if (self.Z == Approx(used_layer_height) || line.new_Z() == Approx(used_layer_height)) {
                            if (line.extruding() && self.F == Approx(support_speed)) {
                                max_x = std::max(double(self.X), max_x);
                                min_x = std::min(double(self.X), min_x);
                                max_y = std::max(double(self.Y), max_y);
                                min_y = std::min(double(self.Y), min_y);
                            }
                        }
                    });
                // match the centerline of our extrusion min/max, should be very close to +/-15mm
                REQUIRE(max_x == Approx(15.0-(line_width / 2.0)).epsilon(line_width * 0.01));
                REQUIRE(max_y == Approx(15.0-(line_width / 2.0)).epsilon(line_width * 0.01));
                REQUIRE(min_x == Approx(-15.0+(line_width / 2.0)).epsilon(line_width * 0.01));
                REQUIRE(min_y == Approx(-15.0+(line_width / 2.0)).epsilon(line_width * 0.01));
            }

        }

        WHEN("Skirt area is smaller than the brim") {
            config->set("skirts", 1);
            config->set("brim_width", 10);
            auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
            THEN("Gcode generates") {
                Slic3r::Test::gcode(gcode, print);
                auto exported {gcode.str()};
                REQUIRE(exported.size() > 0);
            }
        }

        WHEN("Skirt height is 0 and skirts > 0") {
            config->set("skirts", 2);
            config->set("skirt_height", 0);

            auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
            THEN("Gcode generates") {
                Slic3r::Test::gcode(gcode, print);
                auto exported {gcode.str()};
                REQUIRE(exported.size() > 0);
            }
        }

        WHEN("Perimeter extruder = 2 and support extruders = 3") {
            config->set("skirts", 0);
            config->set("brim_width", 5);
            config->set("perimeter_extruder", 2);
            config->set("support_material_extruder", 3);
            THEN("Brim is printed with the extruder used for the perimeters of first object") {
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                Slic3r::Test::gcode(gcode, print);
                int tool = get_brim_tool(gcode, parser);
                REQUIRE(tool == config->getInt("perimeter_extruder") - 1);
            }
        }
        WHEN("Perimeter extruder = 2, support extruders = 3, raft is enabled") {
            config->set("skirts", 0);
            config->set("brim_width", 5);
            config->set("perimeter_extruder", 2);
            config->set("support_material_extruder", 3);
            config->set("raft_layers", 1);
            THEN("brim is printed with same extruder as skirt") {
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                Slic3r::Test::gcode(gcode, print);
                int tool = get_brim_tool(gcode, parser);
                REQUIRE(tool == config->getInt("support_material_extruder") - 1);
            }
        }
        WHEN("brim width to 1 with layer_width of 0.5") {
            config->set("skirts", 0);
            config->set("first_layer_extrusion_width", 0.5);
            config->set("brim_width", 1);
            config->set("brim_ears", false);
			
            THEN("2 brim lines") {
                Slic3r::Model model;
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                print->process();
                REQUIRE(print->brim.size() == 2);
            }
        }

        WHEN("brim ears on a square") {
            config->set("skirts", 0);
            config->set("first_layer_extrusion_width", 0.5);
            config->set("brim_width", 1);
            config->set("brim_ears", true);
            config->set("brim_ears_max_angle", 91);
			
            Slic3r::Model model;
            auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
            print->process();

            THEN("Four brim ears") {
                REQUIRE(print->brim.size() == 4);
            }
        }


        WHEN("brim ears on a square but with a too small max angle") {
            config->set("skirts", 0);
            config->set("first_layer_extrusion_width", 0.5);
            config->set("brim_width", 1);
            config->set("brim_ears", true);
            config->set("brim_ears_max_angle", 89);
			
            THEN("no brim") {
                Slic3r::Model model;
                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                print->process();
                REQUIRE(print->brim.size() == 0);
            }
        }

        WHEN("Object is plated with overhang support and a brim") {
            config->set("layer_height", 0.4);
            config->set("first_layer_height", 0.4);
            config->set("skirts", 1);
            config->set("skirt_distance", 0);
            config->set("support_material_speed", 99);
            config->set("perimeter_extruder", 1);
            config->set("support_material_extruder", 2);
            config->set("infill_extruder", 3); // ensure that a tool command gets emitted.
            config->set("cooling", false);                     // to prevent speeds to be altered
            config->set("first_layer_speed", "100%");      // to prevent speeds to be altered

            Slic3r::Model model;
            auto print {Slic3r::Test::init_print({TestMesh::overhang}, model, config)};
            print->process();

            // config->set("support_material", true);      // to prevent speeds to be altered

            THEN("skirt length is large enough to contain object with support") {
                CHECK(config->getBool("support_material")); // test is not valid if support material is off
                double skirt_length = 0.0;
                Points extrusion_points;
                int tool = -1;
                std::regex tool_regex("^T(\\d+)");
                std::smatch m;

                auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
                Slic3r::Test::gcode(gcode, print);

                auto support_speed = config->get<ConfigOptionFloat>("support_material_speed") * MM_PER_MIN;
                parser.parse_stream(gcode, [tool_regex, &m, config, &extrusion_points, &tool, &skirt_length, support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
                    {
                        std::cerr << line.cmd << "\n";
                        if (std::regex_match(line.cmd, m, tool_regex)) {
                            tool = std::stoi(m[1].str());
                        } else if (self.Z == Approx(config->get<ConfigOptionFloat>("first_layer_height").value)) {
                            // on first layer
                            if (line.extruding() && line.dist_XY() > 0) {
                                auto speed = ( self.F > 0 ?  self.F : line.new_F());
                                std::cerr << "Tool " << tool << "\n";
                                if (speed == Approx(support_speed) && tool == config->getInt("perimeter_extruder") - 1) {
                                    // Skirt uses first material extruder, support material speed.
                                    skirt_length += line.dist_XY();
                                } else {
                                    extrusion_points.push_back(Slic3r::Point::new_scale(line.new_X(), line.new_Y()));
                                }
                            }
                        }

                        if (self.Z == Approx(0.3) || line.new_Z() == Approx(0.3)) {
                            if (line.extruding() && self.F == Approx(support_speed)) {
                            }
                        }
                    });
                auto convex_hull = Slic3r::Geometry::convex_hull(extrusion_points);
                auto hull_perimeter = unscale(convex_hull.split_at_first_point().length());
                REQUIRE(skirt_length > hull_perimeter);
            }
        }
        WHEN("Large minimum skirt length is used.") {
            config->set("min_skirt_length", 20);
            auto gcode {std::stringstream("")};
            Slic3r::Model model;
            auto print {Slic3r::Test::init_print({TestMesh::cube_20x20x20}, model, config)};
            THEN("Gcode generation doesn't crash") {
                Slic3r::Test::gcode(gcode, print);
                auto exported {gcode.str()};
                REQUIRE(exported.size() > 0);
            }
        }
    }
}
