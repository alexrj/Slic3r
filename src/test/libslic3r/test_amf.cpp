#include <catch.hpp>
#include <test_options.hpp>
#include "IO.hpp"

using namespace Slic3r;
using namespace std::literals::string_literals;

SCENARIO("Reading deflated AMF files") {
    GIVEN("Compressed AMF file of a 20mm cube") {
        Model model;
        WHEN("file is read") {
            bool result_code = IO::AMF::read(std::string(testfile_dir) + "test_amf/20mmbox_deflated.amf"s, &model);
            THEN("Does not return false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains a single ModelObject.") {
                REQUIRE(model.objects.size() == 1);
            }
        }
        WHEN("single file is read with some subdirectories") {
            bool result_code = IO::AMF::read(std::string(testfile_dir) + "test_amf/20mmbox_deflated-in_directories.amf"s, &model);
            THEN("Read returns false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains no ModelObjects.") {
                REQUIRE(model.objects.size() == 1);
            }
        }
        WHEN("file is read with unsupported file structure (multiple files)") {
            bool result_code = IO::AMF::read(std::string(testfile_dir) + "test_amf/20mmbox_deflated-mult_files.amf"s, &model);
            THEN("Read returns false.") {
                REQUIRE(result_code == false);
            }
            THEN("Model object contains no ModelObjects.") {
                REQUIRE(model.objects.size() == 0);
            }
        }
    }
    GIVEN("Uncompressed AMF file of a 20mm cube") {
        Model model;
        WHEN("file is read") {
            bool result_code = IO::AMF::read(std::string(testfile_dir) + "test_amf/20mmbox.amf"s, &model);
            THEN("Does not return false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains a single ModelObject.") {
                REQUIRE(model.objects.size() == 1);
            }
        }
        WHEN("nonexistant file is read") {
            bool result_code = IO::AMF::read(std::string(testfile_dir) + "test_amf/20mmbox-doesnotexist.amf"s, &model);
            THEN("Read returns false.") {
                REQUIRE(result_code == false);
            }
            THEN("Model object contains no ModelObject.") {
                REQUIRE(model.objects.size() == 0);
            }
        }
    }
}
