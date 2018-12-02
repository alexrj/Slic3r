#include "PrintConfig.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>

namespace Slic3r {

PrintConfigDef::PrintConfigDef()
{
    ConfigOptionDef external_fill_pattern;
    external_fill_pattern.type = coEnum;
    external_fill_pattern.enum_keys_map = ConfigOptionEnum<InfillPattern>::get_enum_values();
    external_fill_pattern.enum_values.push_back("rectilinear");
    external_fill_pattern.enum_values.push_back("concentric");
    external_fill_pattern.enum_values.push_back("hilbertcurve");
    external_fill_pattern.enum_values.push_back("archimedeanchords");
    external_fill_pattern.enum_values.push_back("octagramspiral");
    external_fill_pattern.enum_labels.push_back(__TRANS("Rectilinear"));
    external_fill_pattern.enum_labels.push_back(__TRANS("Concentric"));
    external_fill_pattern.enum_labels.push_back(__TRANS("Hilbert Curve"));
    external_fill_pattern.enum_labels.push_back(__TRANS("Archimedean Chords"));
    external_fill_pattern.enum_labels.push_back(__TRANS("Octagram Spiral"));
    
    ConfigOptionDef* def;

    def = this->add("adaptive_slicing", coBool);
    def->label = "Use adaptive slicing";
    def->category = "Layers and Perimeters";
    def->tooltip = "Automatically determine layer heights by the objects topology instead of using the static value.";
    def->cli = "adaptive-slicing!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("adaptive_slicing_quality", coPercent);
    def->label = __TRANS("Adaptive quality");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Controls the quality / printing time tradeoff for adaptive layer generation. 0 -> fastest printing with max layer height, 100 -> highest quality, min layer height");
    def->sidetext = "%";
    def->cli = "adaptive-slicing-quality=f";
    def->min = 0;
    def->max = 100;
    def->gui_type = "slider";
    def->width = 200;
    def->default_value = new ConfigOptionPercent(75);

    def = this->add("avoid_crossing_perimeters", coBool);
    def->label = __TRANS("Avoid crossing perimeters");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Optimize travel moves in order to minimize the crossing of perimeters. This is mostly useful with Bowden extruders which suffer from oozing. This feature slows down both the print and the G-code generation.");
    def->cli = "avoid-crossing-perimeters!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("bed_shape", coPoints);
    def->label = __TRANS("Bed shape");
    def->tooltip = __TRANS("Shape of the print bed.");
    {
        ConfigOptionPoints* opt = new ConfigOptionPoints();
        opt->values.push_back(Pointf(0,0));
        opt->values.push_back(Pointf(200,0));
        opt->values.push_back(Pointf(200,200));
        opt->values.push_back(Pointf(0,200));
        def->default_value = opt;
    }
    def->cli = "bed-shape=s";

    def = this->add("has_heatbed", coBool);
    def->label = __TRANS("Has heated bed");
    def->tooltip = __TRANS("Unselecting this will suppress automatic generation of bed heating gcode.");
    def->cli = "has_heatbed!";
    def->default_value = new ConfigOptionBool(true);
    
    def = this->add("bed_temperature", coInt);
    def->label = __TRANS("Other layers");
    def->tooltip = __TRANS("Bed temperature for layers after the first one.");
    def->cli = "bed-temperature=i";
    def->full_label = __TRANS("Bed temperature");
    def->min = 0;
    def->max = 300;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("before_layer_gcode", coString);
    def->label = __TRANS("Before layer change G-code");
    def->tooltip = __TRANS("This custom code is inserted at every layer change, right before the Z move. Note that you can use placeholder variables for all Slic3r settings as well as [layer_num], [layer_z] and [current_retraction].");
    def->cli = "before-layer-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 50;
    def->default_value = new ConfigOptionString("");

    def = this->add("between_objects_gcode", coString);
    def->label = __TRANS("Between objects G-code");
    def->tooltip = __TRANS("This code is inserted between objects when using sequential printing. By default extruder and bed temperature are reset using non-wait command; however if M104, M109, M140 or M190 are detected in this custom code, Slic3r will not add temperature commands. Note that you can use placeholder variables for all Slic3r settings, so you can put a \"M109 S[first_layer_temperature]\" command wherever you want.");
    def->cli = "between-objects-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    def->default_value = new ConfigOptionString("");

    def = this->add("bottom_infill_pattern", external_fill_pattern);
    def->label = __TRANS("Bottom");
    def->full_label = __TRANS("Bottom infill pattern");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Infill pattern for bottom layers. This only affects the external visible layer, and not its adjacent solid shells.");
    def->cli = "bottom-infill-pattern=s";
    def->default_value = new ConfigOptionEnum<InfillPattern>(ipRectilinear);

    def = this->add("bottom_solid_layers", coInt);
    def->label = __TRANS("Bottom");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Number of solid layers to generate on bottom surfaces.");
    def->cli = "bottom-solid-layers=i";
    def->full_label = __TRANS("Bottom solid layers");
    def->min = 0;
    def->default_value = new ConfigOptionInt(3);

    def = this->add("bridge_acceleration", coFloat);
    def->label = __TRANS("Bridge");
    def->category = __TRANS("Speed > Acceleration");
    def->tooltip = __TRANS("This is the acceleration your printer will use for bridges. Set zero to disable acceleration control for bridges.");
    def->sidetext = "mm/s²";
    def->cli = "bridge-acceleration=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("bridge_fan_speed", coInt);
    def->label = __TRANS("Bridges fan speed");
    def->tooltip = __TRANS("This fan speed is enforced during all bridges and overhangs.");
    def->sidetext = "%";
    def->cli = "bridge-fan-speed=i";
    def->min = 0;
    def->max = 100;
    def->default_value = new ConfigOptionInt(100);

    def = this->add("bridge_flow_ratio", coFloat);
    def->label = __TRANS("Bridge flow ratio");
    def->category = __TRANS("Advanced");
    def->tooltip = __TRANS("This factor affects the amount of plastic for bridging. You can decrease it slightly to pull the extrudates and prevent sagging, although default settings are usually good and you should experiment with cooling (use a fan) before tweaking this.");
    def->cli = "bridge-flow-ratio=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(1);

    def = this->add("bridge_speed", coFloat);
    def->label = __TRANS("Bridges");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for printing bridges.");
    def->sidetext = "mm/s";
    def->cli = "bridge-speed=f";
    def->aliases.push_back("bridge_feed_rate");
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloat(60);

    def = this->add("brim_connections_width", coFloat);
    def->label = __TRANS("Brim connections width");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("If set to a positive value, straight connections will be built on the first layer between adjacent objects.");
    def->sidetext = "mm";
    def->cli = "brim-connections-width=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("brim_width", coFloat);
    def->label = __TRANS("Exterior brim width");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("Horizontal width of the brim that will be printed around each object on the first layer.");
    def->sidetext = "mm";
    def->cli = "brim-width=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("compatible_printers", coStrings);
    def->label = __TRANS("Compatible printers");
    def->default_value = new ConfigOptionStrings();

    def = this->add("complete_objects", coBool);
    def->label = __TRANS("Complete individual objects");
    def->category = __TRANS("Advanced");
    def->tooltip = __TRANS("When printing multiple objects or copies, this feature will complete each object before moving onto next one (and starting it from its bottom layer). This feature is useful to avoid the risk of ruined prints. Slic3r should warn and prevent you from extruder collisions, but beware.");
    def->cli = "complete-objects!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("cooling", coBool);
    def->label = __TRANS("Enable auto cooling");
    def->tooltip = __TRANS("This flag enables the automatic cooling logic that adjusts print speed and fan speed according to layer printing time.");
    def->cli = "cooling!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("default_acceleration", coFloat);
    def->label = __TRANS("Default");
    def->category = __TRANS("Speed > Acceleration");
    def->tooltip = __TRANS("This is the acceleration your printer will be reset to after the role-specific acceleration values are used (perimeter/infill). Set zero to prevent resetting acceleration at all.");
    def->sidetext = "mm/s²";
    def->cli = "default-acceleration=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("disable_fan_first_layers", coInt);
    def->label = __TRANS("Disable fan for the first");
    def->tooltip = __TRANS("This disables the fan completely for the first N layers to aid in the adhesion of media to the bed.");
    def->sidetext = __TRANS("layers");
    def->cli = "disable-fan-first-layers=i";
    def->min = 0;
    def->max = 1000;
    def->default_value = new ConfigOptionInt(3);

    def = this->add("dont_support_bridges", coBool);
    def->label = __TRANS("Don't support bridges");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Experimental option for preventing support material from being generated under bridged areas.");
    def->cli = "dont-support-bridges!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("duplicate_distance", coFloat);
    def->label = __TRANS("Distance between copies");
    def->tooltip = __TRANS("Distance used for the auto-arrange feature of the plater.");
    def->sidetext = "mm";
    def->cli = "duplicate-distance=f";
    def->aliases.push_back("multiply_distance");
    def->min = 0;
    def->default_value = new ConfigOptionFloat(6);

    def = this->add("end_gcode", coString);
    def->label = __TRANS("End G-code");
    def->tooltip = __TRANS("This end procedure is inserted at the end of the output file. Note that you can use placeholder variables for all Slic3r settings. If Slic3r detects M104, M109, M140 or M190 in your custom codes, such commands will not be prepended automatically so you're free to customize the order of heating commands and other custom actions.");
    def->cli = "end-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    def->default_value = new ConfigOptionString("M104 S0 ; turn off temperature\nG28 X0  ; home X axis\nM84     ; disable motors\n");

    def = this->add("end_filament_gcode", coStrings);
    def->label = __TRANS("End G-code");
    def->tooltip = __TRANS("This end procedure is inserted at the end of the output file, before the printer end gcode. Note that you can use placeholder variables for all Slic3r settings. If you have multiple extruders, the gcode is processed in extruder order. If Slic3r detects M104, M109, M140 or M190 in your custom codes, such commands will not be prepended automatically so you're free to customize the order of heating commands and other custom actions.");
    def->cli = "end-filament-gcode=s@";
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    {
        ConfigOptionStrings* opt = new ConfigOptionStrings();
        opt->values.push_back("; Filament-specific end gcode \n;END gcode for filament\n");
        def->default_value = opt;
    }

    def = this->add("external_fill_pattern", external_fill_pattern);
    def->label = __TRANS("Top/bottom fill pattern");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Fill pattern for top/bottom infill. This only affects the external visible layer, and not its adjacent solid shells.");
    def->cli = "external-fill-pattern|external-infill-pattern|solid-fill-pattern=s";
    def->aliases.push_back("solid_fill_pattern");
    def->shortcut.push_back("top_infill_pattern");
    def->shortcut.push_back("bottom_infill_pattern");
    
    def = this->add("external_perimeter_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("↳ external");
    def->full_label = __TRANS("External perimeters extrusion width");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for external perimeters. If auto is chosen, a value will be used that maximizes accuracy of the external visible surfaces. If expressed as percentage (for example 200%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "external-perimeter-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("external_perimeter_speed", coFloatOrPercent);
    def->label = __TRANS("↳ external");
    def->full_label = __TRANS("External perimeters speed");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("This separate setting will affect the speed of external perimeters (the visible ones). If expressed as percentage (for example: 80%) it will be calculated on the perimeters speed setting above.");
    def->sidetext = "mm/s or %";
    def->cli = "external-perimeter-speed=s";
    def->ratio_over = "perimeter_speed";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(50, true);

    def = this->add("external_perimeters_first", coBool);
    def->label = __TRANS("External perimeters first");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Print contour perimeters from the outermost one to the innermost one instead of the default inverse order.");
    def->cli = "external-perimeters-first!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("extra_perimeters", coBool);
    def->label = __TRANS("Extra perimeters if needed");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Add more perimeters when needed for avoiding gaps in sloping walls.");
    def->cli = "extra-perimeters!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("extruder", coInt);
    def->gui_type = "i_enum_open";
    def->label = __TRANS("Extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use (unless more specific extruder settings are specified).");
    def->cli = "extruder=i";
    def->min = 0;  // 0 = inherit defaults
    def->enum_labels.push_back("default");  // override label for item 0
    def->enum_labels.push_back("1");
    def->enum_labels.push_back("2");
    def->enum_labels.push_back("3");
    def->enum_labels.push_back("4");

    def = this->add("extruder_clearance_height", coFloat);
    def->label = __TRANS("Height");
    def->tooltip = __TRANS("Set this to the vertical distance between your nozzle tip and (usually) the X carriage rods. In other words, this is the height of the clearance cylinder around your extruder, and it represents the maximum depth the extruder can peek before colliding with other printed objects.");
    def->sidetext = "mm";
    def->cli = "extruder-clearance-height=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(20);

    def = this->add("extruder_clearance_radius", coFloat);
    def->label = __TRANS("Radius");
    def->tooltip = __TRANS("Set this to the clearance radius around your extruder. If the extruder is not centered, choose the largest value for safety. This setting is used to check for collisions and to display the graphical preview in the plater.");
    def->sidetext = "mm";
    def->cli = "extruder-clearance-radius=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(20);

    def = this->add("extruder_offset", coPoints);
    def->label = __TRANS("Extruder offset");
    def->tooltip = __TRANS("If your firmware doesn't handle the extruder displacement you need the G-code to take it into account. This option lets you specify the displacement of each extruder with respect to the first one. It expects positive coordinates (they will be subtracted from the XY coordinate).");
    def->sidetext = "mm";
    def->cli = "extruder-offset=s@";
    {
        ConfigOptionPoints* opt = new ConfigOptionPoints();
        opt->values.push_back(Pointf(0,0));
        def->default_value = opt;
    }

    def = this->add("extrusion_axis", coString);
    def->label = __TRANS("Extrusion axis");
    def->tooltip = __TRANS("Use this option to set the axis letter associated to your printer's extruder (usually E but some printers use A).");
    def->cli = "extrusion-axis=s";
    def->default_value = new ConfigOptionString("E");

    def = this->add("extrusion_multiplier", coFloats);
    def->label = __TRANS("Extrusion multiplier");
    def->tooltip = __TRANS("This factor changes the amount of flow proportionally. You may need to tweak this setting to get nice surface finish and correct single wall widths. Usual values are between 0.9 and 1.1. If you think you need to change this more, check filament diameter and your firmware E steps.");
    def->cli = "extrusion-multiplier=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(1);
        def->default_value = opt;
    }
    
    def = this->add("extrusion_width", coFloatOrPercent);
    def->label = __TRANS("Default extrusion width");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width. If expressed as percentage (for example: 230%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("fan_always_on", coBool);
    def->label = __TRANS("Keep fan always on");
    def->tooltip = __TRANS("If this is enabled, fan will never be disabled and will be kept running at least at its minimum speed. Useful for PLA, harmful for ABS.");
    def->cli = "fan-always-on!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("fan_below_layer_time", coInt);
    def->label = __TRANS("Enable fan if layer print time is below");
    def->tooltip = __TRANS("If layer print time is estimated below this number of seconds, fan will be enabled and its speed will be calculated by interpolating the minimum and maximum speeds.");
    def->sidetext = "approximate seconds";
    def->cli = "fan-below-layer-time=i";
    def->width = 60;
    def->min = 0;
    def->max = 1000;
    def->default_value = new ConfigOptionInt(60);

    def = this->add("filament_colour", coStrings);
    def->label = __TRANS("Color");
    def->tooltip = __TRANS("This is only used in the Slic3r interface as a visual help.");
    def->cli = "filament-color=s@";
    def->gui_type = "color";
    {
        ConfigOptionStrings* opt = new ConfigOptionStrings();
        opt->values.push_back("#FFFFFF");
        def->default_value = opt;
    }

    def = this->add("filament_notes", coStrings);
    def->label = __TRANS("Filament notes");
    def->tooltip = __TRANS("You can put your notes regarding the filament here.");
    def->cli = "filament-notes=s@";
    def->multiline = true;
    def->full_width = true;
    def->height = 130;
    {
        ConfigOptionStrings* opt = new ConfigOptionStrings();
        opt->values.push_back("");
        def->default_value = opt;
    }

    def = this->add("filament_max_volumetric_speed", coFloats);
    def->label = __TRANS("Max volumetric speed");
    def->tooltip = __TRANS("Maximum volumetric speed allowed for this filament. Limits the maximum volumetric speed of a print to the minimum of print and filament volumetric speed. Set to zero for no limit.");
    def->sidetext = "mm³/s";
    def->cli = "filament-max-volumetric-speed=f@";
    def->min = 0;
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0.f);
        def->default_value = opt;
    }

    def = this->add("filament_diameter", coFloats);
    def->label = __TRANS("Diameter");
    def->tooltip = __TRANS("Enter your filament diameter here. Good precision is required, so use a caliper and do multiple measurements along the filament, then compute the average.");
    def->sidetext = "mm";
    def->cli = "filament-diameter=f@";
    def->min = 0;
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(3);
        def->default_value = opt;
    }

    def = this->add("filament_density", coFloats);
    def->label = __TRANS("Density");
    def->tooltip = __TRANS("Enter your filament density here. This is only for statistical information. A decent way is to weigh a known length of filament and compute the ratio of the length to volume. Better is to calculate the volume directly through displacement.");
    def->sidetext = "g/cm³";
    def->cli = "filament-density=f@";
    def->min = 0;
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("filament_cost", coFloats);
    def->label = __TRANS("Cost");
    def->tooltip = __TRANS("Enter your filament cost per kg here. This is only for statistical information.");
    def->sidetext = __TRANS("money/kg");
    def->cli = "filament-cost=f@";
    def->min = 0;
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }
    
    def = this->add("filament_settings_id", coString);
    def->default_value = new ConfigOptionString("");
    
    def = this->add("fill_angle", coFloat);
    def->label = __TRANS("Fill angle");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Default base angle for infill orientation. Cross-hatching will be applied to this. Bridges will be infilled using the best direction Slic3r can detect, so this setting does not affect them.");
    def->sidetext = "°";
    def->cli = "fill-angle=i";
    def->min = 0;
    def->max = 359;
    def->default_value = new ConfigOptionFloat(45);

    def = this->add("fill_density", coPercent);
    def->gui_type = "f_enum_open";
    def->gui_flags = "show_value";
    def->label = __TRANS("Fill density");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Density of internal infill, expressed in the range 0% - 100%.");
    def->sidetext = "%";
    def->cli = "fill-density=s";
    def->min = 0;
    def->max = 100;
    def->enum_values.push_back("0");
    def->enum_values.push_back("5");
    def->enum_values.push_back("10");
    def->enum_values.push_back("15");
    def->enum_values.push_back("20");
    def->enum_values.push_back("25");
    def->enum_values.push_back("30");
    def->enum_values.push_back("40");
    def->enum_values.push_back("50");
    def->enum_values.push_back("60");
    def->enum_values.push_back("70");
    def->enum_values.push_back("80");
    def->enum_values.push_back("90");
    def->enum_values.push_back("100");
    def->enum_labels.push_back("0%");
    def->enum_labels.push_back("5%");
    def->enum_labels.push_back("10%");
    def->enum_labels.push_back("15%");
    def->enum_labels.push_back("20%");
    def->enum_labels.push_back("25%");
    def->enum_labels.push_back("30%");
    def->enum_labels.push_back("40%");
    def->enum_labels.push_back("50%");
    def->enum_labels.push_back("60%");
    def->enum_labels.push_back("70%");
    def->enum_labels.push_back("80%");
    def->enum_labels.push_back("90%");
    def->enum_labels.push_back("100%");
    def->default_value = new ConfigOptionPercent(20);

    def = this->add("fill_gaps", coBool);
    def->label = __TRANS("Fill gaps");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("If this is enabled, gaps will be filled with single passes. Enable this for better quality, disable it for shorter printing times.");
    def->cli = "fill-gaps!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("fill_pattern", coEnum);
    def->label = __TRANS("Fill pattern");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Fill pattern for general low-density infill.");
    def->cli = "fill-pattern=s";
    def->enum_keys_map = ConfigOptionEnum<InfillPattern>::get_enum_values();
    def->enum_values.push_back("rectilinear");
    def->enum_values.push_back("alignedrectilinear");
    def->enum_values.push_back("grid");
    def->enum_values.push_back("triangles");
    def->enum_values.push_back("stars");
    def->enum_values.push_back("cubic");
    def->enum_values.push_back("concentric");
    def->enum_values.push_back("honeycomb");
    def->enum_values.push_back("3dhoneycomb");
    def->enum_values.push_back("gyroid");
    def->enum_values.push_back("hilbertcurve");
    def->enum_values.push_back("archimedeanchords");
    def->enum_values.push_back("octagramspiral");
    def->enum_labels.push_back(__TRANS("Rectilinear"));
    def->enum_labels.push_back(__TRANS("Aligned Rectilinear"));
    def->enum_labels.push_back(__TRANS("Grid"));
    def->enum_labels.push_back(__TRANS("Triangles"));
    def->enum_labels.push_back(__TRANS("Stars"));
    def->enum_labels.push_back(__TRANS("Cubic"));
    def->enum_labels.push_back(__TRANS("Concentric"));
    def->enum_labels.push_back(__TRANS("Honeycomb"));
    def->enum_labels.push_back(__TRANS("3D Honeycomb"));
    def->enum_labels.push_back(__TRANS("Gyroid"));
    def->enum_labels.push_back(__TRANS("Hilbert Curve"));
    def->enum_labels.push_back(__TRANS("Archimedean Chords"));
    def->enum_labels.push_back(__TRANS("Octagram Spiral"));
    def->default_value = new ConfigOptionEnum<InfillPattern>(ipStars);

    def = this->add("first_layer_acceleration", coFloat);
    def->label = __TRANS("First layer");
    def->category = __TRANS("Speed > Acceleration");
    def->tooltip = __TRANS("This is the acceleration your printer will use for first layer. Set zero to disable acceleration control for first layer.");
    def->sidetext = "mm/s²";
    def->cli = "first-layer-acceleration=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("first_layer_bed_temperature", coInt);
    def->label = __TRANS("First layer");
    def->tooltip = __TRANS("Heated build plate temperature for the first layer. Set this to zero to disable bed temperature control commands in the output.");
    def->cli = "first-layer-bed-temperature=i";
    def->max = 0;
    def->max = 300;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("first_layer_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("First layer");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for first layer. You can use this to force fatter extrudates for better adhesion. If expressed as percentage (for example 120%) it will be computed over first layer height.");
    def->sidetext = "mm or %";
    def->cli = "first-layer-extrusion-width=s";
    def->ratio_over = "first_layer_height";
    def->min = 0;
    def->enum_values.push_back("200%");
    def->enum_labels.push_back("default");
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(200, true);

    def = this->add("first_layer_height", coFloatOrPercent);
    def->label = __TRANS("First layer height");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("When printing with very low layer heights, you might still want to print a thicker bottom layer to improve adhesion and tolerance for non perfect build plates. This can be expressed as an absolute value or as a percentage (for example: 150%) over the default layer height.");
    def->sidetext = "mm or %";
    def->cli = "first-layer-height=s";
    def->ratio_over = "layer_height";
    def->default_value = new ConfigOptionFloatOrPercent(0.35, false);

    def = this->add("first_layer_speed", coFloatOrPercent);
    def->label = __TRANS("First layer speed");
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("If expressed as absolute value in mm/s, this speed will be applied to all the print moves of the first layer, regardless of their type. If expressed as a percentage (for example: 40%) it will scale the default speeds.");
    def->sidetext = "mm/s or %";
    def->cli = "first-layer-speed=s";
    def->min = 0;
    def->default_value = new ConfigOptionFloatOrPercent(30, false);

    def = this->add("first_layer_temperature", coInts);
    def->label = __TRANS("First layer");
    def->tooltip = __TRANS("Extruder temperature for first layer. If you want to control temperature manually during print, set this to zero to disable temperature control commands in the output file.");
    def->cli = "first-layer-temperature=i@";
    def->min = 0;
    def->max = 500;
    {
        ConfigOptionInts* opt = new ConfigOptionInts();
        opt->values.push_back(200);
        def->default_value = opt;
    }
    
    def = this->add("gap_fill_speed", coFloatOrPercent);
    def->label = __TRANS("↳ gaps");
    def->full_label = "Gap fill speed";
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for filling gaps. Since these are usually single lines you might want to use a low speed for better sticking. If expressed as percentage (for example: 80%) it will be calculated on the infill speed setting above.");
    def->sidetext = "mm/s or %";
    def->cli = "gap-fill-speed=s";
    def->ratio_over = "infill_speed";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(20, false);

    def = this->add("gcode_arcs", coBool);
    def->label = __TRANS("Use native G-code arcs");
    def->tooltip = __TRANS("This experimental feature tries to detect arcs from segments and generates G2/G3 arc commands instead of multiple straight G1 commands.");
    def->cli = "gcode-arcs!";
    def->default_value = new ConfigOptionBool(0);

    def = this->add("gcode_comments", coBool);
    def->label = __TRANS("Verbose G-code");
    def->tooltip = __TRANS("Enable this to get a commented G-code file, with each line explained by a descriptive text. If you print from SD card, the additional weight of the file could make your firmware slow down.");
    def->cli = "gcode-comments!";
    def->default_value = new ConfigOptionBool(0);

    def = this->add("gcode_flavor", coEnum);
    def->label = __TRANS("G-code flavor");
    def->tooltip = __TRANS("Some G/M-code commands, including temperature control and others, are not universal. Set this option to your printer's firmware to get a compatible output. The \"No extrusion\" flavor prevents Slic3r from exporting any extrusion value at all.");
    def->cli = "gcode-flavor=s";
    def->enum_keys_map = ConfigOptionEnum<GCodeFlavor>::get_enum_values();
    def->enum_values.push_back("reprap");
    def->enum_values.push_back("repetier");
    def->enum_values.push_back("teacup");
    def->enum_values.push_back("makerware");
    def->enum_values.push_back("sailfish");
    def->enum_values.push_back("mach3");
    def->enum_values.push_back("machinekit");
    def->enum_values.push_back("smoothie");
    def->enum_values.push_back("no-extrusion");
    def->enum_labels.push_back("RepRap (Marlin/Sprinter)");
    def->enum_labels.push_back("Repetier");
    def->enum_labels.push_back("Teacup");
    def->enum_labels.push_back("MakerWare (MakerBot)");
    def->enum_labels.push_back("Sailfish (MakerBot)");
    def->enum_labels.push_back("Mach3/LinuxCNC");
    def->enum_labels.push_back("Machinekit");
    def->enum_labels.push_back("Smoothieware");
    def->enum_labels.push_back("No extrusion");
    def->default_value = new ConfigOptionEnum<GCodeFlavor>(gcfRepRap);

    def = this->add("host_type", coEnum);
    def->label = "Host type";
    def->tooltip = "Select Octoprint or Duet to connect to your machine via LAN";
    def->cli = "host-type=s";
    def->enum_keys_map = ConfigOptionEnum<HostType>::get_enum_values();
    def->enum_values.push_back("octoprint");
    def->enum_values.push_back("duet");
    def->enum_labels.push_back("Octoprint");
    def->enum_labels.push_back("Duet");
    def->default_value = new ConfigOptionEnum<HostType>(htOctoprint);
    
    def = this->add("infill_acceleration", coFloat);
    def->label = __TRANS("Infill");
    def->category = __TRANS("Speed > Acceleration");
    def->tooltip = __TRANS("This is the acceleration your printer will use for infill. Set zero to disable acceleration control for infill.");
    def->sidetext = "mm/s²";
    def->cli = "infill-acceleration=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("infill_every_layers", coInt);
    def->label = __TRANS("Combine infill every");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("This feature allows to combine infill and speed up your print by extruding thicker infill layers while preserving thin perimeters, thus accuracy.");
    def->sidetext = "layers";
    def->cli = "infill-every-layers=i";
    def->full_label = "Combine infill every n layers";
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("infill_extruder", coInt);
    def->label = __TRANS("Infill extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use when printing infill.");
    def->cli = "infill-extruder=i";
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("infill_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("Infill");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for infill. You may want to use fatter extrudates to speed up the infill and make your parts stronger. If expressed as percentage (for example 90%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "infill-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("infill_first", coBool);
    def->label = __TRANS("Infill before perimeters");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("This option will switch the print order of perimeters and infill, making the latter first.");
    def->cli = "infill-first!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("infill_only_where_needed", coBool);
    def->label = __TRANS("Only infill where needed");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("This option will limit infill to the areas actually needed for supporting ceilings (it will act as internal support material). If enabled, slows down the G-code generation due to the multiple checks involved.");
    def->cli = "infill-only-where-needed!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("infill_overlap", coFloatOrPercent);
    def->label = __TRANS("Infill/perimeters overlap");
    def->category = __TRANS("Advanced");
    def->tooltip = __TRANS("This setting applies an additional overlap between infill and perimeters for better bonding. Theoretically this shouldn't be needed, but backlash might cause gaps. If expressed as percentage (example: 15%) it is calculated over perimeter extrusion width.");
    def->sidetext = "mm or %";
    def->cli = "infill-overlap=s";
    def->ratio_over = "perimeter_extrusion_width";
    def->default_value = new ConfigOptionFloatOrPercent(55, true);

    def = this->add("infill_speed", coFloat);
    def->label = __TRANS("Infill");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for printing the internal fill.");
    def->sidetext = "mm/s";
    def->cli = "infill-speed=f";
    def->aliases.push_back("print_feed_rate");
    def->aliases.push_back("infill_feed_rate");
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloat(80);

    def = this->add("interior_brim_width", coFloat);
    def->label = __TRANS("Interior brim width");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("Horizontal width of the brim that will be printed inside object holes on the first layer.");
    def->sidetext = "mm";
    def->cli = "interior-brim-width=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("interface_shells", coBool);
    def->label = __TRANS("Interface shells");
    def->tooltip = __TRANS("Force the generation of solid shells between adjacent materials/volumes. Useful for multi-extruder prints with translucent materials or manual soluble support material.");
    def->cli = "interface-shells!";
    def->category = __TRANS("Layers and Perimeters");
    def->default_value = new ConfigOptionBool(false);

    def = this->add("label_printed_objects", coBool);
    def->label = "Label Prints with Object ID";
    def->tooltip = "Enable this to add comments in the G-Code that label print moves with what object they belong. Can be used with Octoprint CancelObject plugin.";
    def->cli = "label-printed-objects!";
    def->default_value = new ConfigOptionBool(0);

    def = this->add("layer_gcode", coString);
    def->label = __TRANS("After layer change G-code");
    def->tooltip = __TRANS("This custom code is inserted at every layer change, right after the Z move and before the extruder moves to the first layer point. Note that you can use placeholder variables for all Slic3r settings as well as [layer_num], [layer_z] and [current_retraction].");
    def->cli = "after-layer-gcode|layer-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 50;
    def->default_value = new ConfigOptionString("");

    def = this->add("layer_height", coFloat);
    def->label = __TRANS("Layer height");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("This setting controls the height (and thus the total number) of the slices/layers. Thinner layers give better accuracy but take more time to print.");
    def->sidetext = "mm";
    def->cli = "layer-height=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0.3);

    def = this->add("match_horizontal_surfaces", coBool);
    def->label = "Match horizontal surfaces";
    def->tooltip = "Try to match horizontal surfaces during the slicing process. Matching is not guaranteed, very small surfaces and multiple surfaces with low vertical distance might cause bad results.";
    def->cli = "match-horizontal-surfaces!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("max_fan_speed", coInt);
    def->label = __TRANS("Max");
    def->tooltip = __TRANS("This setting represents the maximum speed of your fan.");
    def->sidetext = "%";
    def->cli = "max-fan-speed=i";
    def->min = 0;
    def->max = 100;
    def->default_value = new ConfigOptionInt(100);

    def = this->add("max_layer_height", coFloats);
	def->label = "Max";
	def->tooltip = "This is the highest printable layer height for this extruder and limits the resolution for adaptive slicing. Typical values are slightly smaller than nozzle_diameter.";
	def->sidetext = "mm";
	def->cli = "max-layer-height=f@";
	def->min = 0;
	{
		ConfigOptionFloats* opt = new ConfigOptionFloats();
		opt->values.push_back(0.3);
		def->default_value = opt;
	}

    def = this->add("max_print_speed", coFloat);
    def->label = __TRANS("Max print speed");
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("When setting other speed settings to 0 Slic3r will autocalculate the optimal speed in order to keep constant extruder pressure. This experimental setting is used to set the highest print speed you want to allow.");
    def->sidetext = "mm/s";
    def->cli = "max-print-speed=f";
    def->min = 1;
    def->default_value = new ConfigOptionFloat(80);

    def = this->add("max_volumetric_speed", coFloat);
    def->label = __TRANS("Max volumetric speed");
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("If set to a non-zero value, extrusion will be limited to this volumetric speed. You may want to set it to your extruder maximum. As a hint, you can read calculated volumetric speeds in the comments of any G-code file you export from Slic3r.");
    def->sidetext = "mm³/s";
    def->cli = "max-volumetric-speed=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("min_fan_speed", coInt);
    def->label = __TRANS("Min");
    def->tooltip = __TRANS("This setting represents the minimum PWM your fan needs to work.");
    def->sidetext = "%";
    def->cli = "min-fan-speed=i";
    def->min = 0;
    def->max = 100;
    def->default_value = new ConfigOptionInt(35);


    def = this->add("min_shell_thickness", coFloat);
    def->label = "Minimum shell thickness";
    def->category = "Layers and Perimeters";
    def->sidetext = "mm";
    def->tooltip = "Alternative method of configuring perimeters and top/bottom layers. If this is above 0 extra perimeters and solid layers will be generated when necessary";
    def->cli = "min-shell-thickness=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("min_top_bottom_shell_thickness", coFloat);
    def->label = "Minimum shell thickness";
    def->category = "Layers and Perimeters";
    def->sidetext = "mm";
    def->tooltip = "Alternative method of configuring top/bottom layers. If this is above 0 extra solid layers will be generated when necessary";
    def->cli = "min-vertical-shell-thickness=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("min_layer_height", coFloats);
    def->label = "Min";
    def->tooltip = "This is the lowest printable layer height for this extruder and limits the resolution for adaptive slicing. Typical values are 0.1 or 0.05.";
    def->sidetext = "mm";
    def->cli = "min-layer-height=f@";
    def->min = 0;
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0.15);
        def->default_value = opt;
    }

    def = this->add("min_print_speed", coFloat);
    def->label = __TRANS("Min print speed");
    def->tooltip = __TRANS("Slic3r will not scale speed down below this speed.");
    def->sidetext = "mm/s";
    def->cli = "min-print-speed=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(10);

    def = this->add("min_skirt_length", coFloat);
    def->label = __TRANS("Minimum extrusion length");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("Generate no less than the number of skirt loops required to consume the specified amount of filament on the bottom layer. For multi-extruder machines, this minimum applies to each extruder.");
    def->sidetext = "mm";
    def->cli = "min-skirt-length=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("notes", coString);
    def->label = __TRANS("Configuration notes");
    def->tooltip = __TRANS("You can put here your personal notes. This text will be added to the G-code header comments.");
    def->cli = "notes=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 130;
    def->default_value = new ConfigOptionString("");

    def = this->add("nozzle_diameter", coFloats);
    def->label = __TRANS("Nozzle diameter");
    def->tooltip = __TRANS("This is the diameter of your extruder nozzle (for example: 0.5, 0.35 etc.)");
    def->sidetext = "mm";
    def->cli = "nozzle-diameter=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0.5);
        def->default_value = opt;
    }

    def = this->add("octoprint_apikey", coString);
    def->label = __TRANS("API Key");
    def->tooltip = __TRANS("Slic3r can upload G-code files to OctoPrint. This field should contain the API Key required for authentication.");
    def->cli = "octoprint-apikey=s";
    def->default_value = new ConfigOptionString("");

    def = this->add("print_host", coString);
    def->label = __TRANS("Host or IP");
    def->tooltip = __TRANS("Slic3r can upload G-code files to an Octoprint/Duet server. This field should contain the hostname or IP address of the server instance.");
    def->cli = "octoprint-host=s";
    def->default_value = new ConfigOptionString("");

    def = this->add("only_retract_when_crossing_perimeters", coBool);
    def->label = __TRANS("Only retract when crossing perimeters");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Disables retraction when the travel path does not exceed the upper layer's perimeters (and thus any ooze will be probably invisible).");
    def->cli = "only-retract-when-crossing-perimeters!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("ooze_prevention", coBool);
    def->label = __TRANS("Enable");
    def->full_label = "Ooze Prevention";
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("During multi-extruder prints, this option will drop the temperature of the inactive extruders to prevent oozing. It will enable a tall skirt automatically and move extruders outside such skirt when changing temperatures.");
    def->cli = "ooze-prevention!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("output_filename_format", coString);
    def->label = __TRANS("Output filename format");
    def->tooltip = __TRANS("You can use all configuration options as variables inside this template. For example: [layer_height], [fill_density] etc. You can also use [timestamp], [year], [month], [day], [hour], [minute], [second], [version], [input_filename], [input_filename_base].");
    def->cli = "output-filename-format=s";
    def->full_width = true;
    def->default_value = new ConfigOptionString("[input_filename_base].gcode");

    def = this->add("overhangs", coBool);
    def->label = __TRANS("Detect bridging perimeters");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Experimental option to adjust flow for overhangs (bridge flow will be used), to apply bridge speed to them and enable fan.");
    def->cli = "overhangs|detect-bridging-perimeters!";
    def->default_value = new ConfigOptionBool(true);
    
    def = this->add("shortcuts", coStrings);
    def->label = __TRANS("Shortcuts");
    def->aliases.push_back("overridable");
    {
        ConfigOptionStrings* opt = new ConfigOptionStrings();
        opt->values.push_back("support_material");
        def->default_value = opt;
    }

    def = this->add("perimeter_acceleration", coFloat);
    def->label = __TRANS("Perimeters");
    def->category = __TRANS("Speed > Acceleration");
    def->tooltip = __TRANS("This is the acceleration your printer will use for perimeters. A high value like 9000 usually gives good results if your hardware is up to the job. Set zero to disable acceleration control for perimeters.");
    def->sidetext = "mm/s²";
    def->cli = "perimeter-acceleration=f";
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("perimeter_extruder", coInt);
    def->label = __TRANS("Perimeter extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use when printing perimeters and brim. First extruder is 1.");
    def->cli = "perimeter-extruder=i";
    def->aliases.push_back("perimeters_extruder");
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("perimeter_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("Perimeters");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for perimeters. You may want to use thinner extrudates to get more accurate surfaces. If expressed as percentage (for example 200%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "perimeter-extrusion-width=s";
    def->aliases.push_back("perimeters_extrusion_width");
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("perimeter_speed", coFloat);
    def->label = __TRANS("Perimeters");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for perimeters (contours, aka vertical shells).");
    def->sidetext = "mm/s";
    def->cli = "perimeter-speed=f";
    def->aliases.push_back("perimeter_feed_rate");
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloat(60);

    def = this->add("perimeters", coInt);
    def->label = __TRANS("Perimeters");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("This option sets the number of perimeters to generate for each layer. Note that Slic3r may increase this number automatically when it detects sloping surfaces which benefit from a higher number of perimeters if the Extra Perimeters option is enabled.");
    def->sidetext = "(minimum)";
    def->cli = "perimeters=i";
    def->aliases.push_back("perimeter_offsets");
    def->min = 0;
    def->default_value = new ConfigOptionInt(3);

    def = this->add("post_process", coStrings);
    def->label = __TRANS("Post-processing scripts");
    def->tooltip = __TRANS("If you want to process the output G-code through custom scripts, just list their absolute paths here. Separate multiple scripts on individual lines. Scripts will be passed the absolute path to the G-code file as the first argument, and they can access the Slic3r config settings by reading environment variables.");
    def->cli = "post-process=s@";
    def->multiline = true;
    def->full_width = true;
    def->height = 60;
    def->default_value = new ConfigOptionStrings();

    def = this->add("printer_notes", coString);
    def->label = __TRANS("Printer notes");
    def->tooltip = __TRANS("You can put your notes regarding the printer here. This text will be added to the G-code header comments.");
    def->cli = "printer-notes=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 130;
    def->default_value = new ConfigOptionString("");

    def = this->add("print_settings_id", coString);
    def->default_value = new ConfigOptionString("");
    
    def = this->add("printer_settings_id", coString);
    def->default_value = new ConfigOptionString("");

    def = this->add("pressure_advance", coFloat);
    def->label = __TRANS("Pressure advance");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("When set to a non-zero value, this experimental option enables pressure regulation. It's the K constant for the advance algorithm that pushes more or less filament upon speed changes. It's useful for Bowden-tube extruders. Reasonable values are in range 0-10.");
    def->cli = "pressure-advance=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("raft_layers", coInt);
    def->label = __TRANS("Raft layers");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("The object will be raised by this number of layers, and support material will be generated under it.");
    def->sidetext = __TRANS("layers");
    def->cli = "raft-layers=i";
    def->min = 0;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("regions_overlap", coFloat);
    def->label = __TRANS("Regions/extruders overlap");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("This setting applies an additional overlap between regions printed with distinct extruders or distinct settings. This shouldn't be needed under normal circumstances.");
    def->sidetext = "mm";
    def->cli = "regions-overlap=s";
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("raft_offset", coFloat);
    def->label = __TRANS("Raft offset");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Horizontal margin between object base layer and raft contour.");
    def->sidetext = "mm";
    def->cli = "raft-offset=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(4);

    def = this->add("resolution", coFloat);
    def->label = __TRANS("Resolution (deprecated)");
    def->tooltip = __TRANS("Minimum detail resolution, used to simplify the input file for speeding up the slicing job and reducing memory usage. High-resolution models often carry more detail than printers can render. Set to zero to disable any simplification and use full resolution from input.");
    def->sidetext = "mm";
    def->cli = "resolution=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("retract_before_travel", coFloats);
    def->label = __TRANS("Minimum travel after retraction");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("Retraction is not triggered when travel moves are shorter than this length.");
    def->sidetext = "mm";
    def->cli = "retract-before-travel=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(2);
        def->default_value = opt;
    }
    
    def = this->add("retract_layer_change", coBools);
    def->label = __TRANS("Retract on layer change");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("This flag enforces a retraction whenever a Z move is done.");
    def->cli = "retract-layer-change!";
    {
        ConfigOptionBools* opt = new ConfigOptionBools();
        opt->values.push_back(false);
        def->default_value = opt;
    }

    def = this->add("retract_length", coFloats);
    def->label = __TRANS("Length");
    def->category = __TRANS("Retraction");
    def->full_label = "Retraction Length";
    def->tooltip = __TRANS("When retraction is triggered, filament is pulled back by the specified amount (the length is measured on raw filament, before it enters the extruder).");
    def->sidetext = "mm (zero to disable)";
    def->cli = "retract-length=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(2);
        def->default_value = opt;
    }

    def = this->add("retract_length_toolchange", coFloats);
    def->label = __TRANS("Length");
    def->full_label = "Retraction Length (Toolchange)";
    def->tooltip = __TRANS("When retraction is triggered before changing tool, filament is pulled back by the specified amount (the length is measured on raw filament, before it enters the extruder).");
    def->sidetext = __TRANS("mm (zero to disable)");
    def->cli = "retract-length-toolchange=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(10);
        def->default_value = opt;
    }

    def = this->add("retract_lift", coFloats);
    def->label = __TRANS("Lift Z");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("If you set this to a positive value, Z is quickly raised every time a retraction is triggered. When using multiple extruders, only the setting for the first extruder will be considered.");
    def->sidetext = "mm";
    def->cli = "retract-lift=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("retract_lift_above", coFloats);
    def->label = __TRANS("Above Z");
    def->full_label = __TRANS("Only lift Z above");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("If you set this to a positive value, Z lift will only take place above the specified absolute Z. You can tune this setting for skipping lift on the first layers.");
    def->sidetext = "mm";
    def->cli = "retract-lift-above=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("retract_lift_below", coFloats);
    def->label = __TRANS("Below Z");
    def->full_label = __TRANS("Only lift Z below");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("If you set this to a positive value, Z lift will only take place below the specified absolute Z. You can tune this setting for limiting lift to the first layers.");
    def->sidetext = "mm";
    def->cli = "retract-lift-below=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("retract_restart_extra", coFloats);
    def->label = __TRANS("Extra length on restart");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("When the retraction is compensated after the travel move, the extruder will push this additional amount of filament. This setting is rarely needed.");
    def->sidetext = "mm";
    def->cli = "retract-restart-extra=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("retract_restart_extra_toolchange", coFloats);
    def->label = __TRANS("Extra length on restart");
    def->tooltip = __TRANS("When the retraction is compensated after changing tool, the extruder will push this additional amount of filament.");
    def->sidetext = "mm";
    def->cli = "retract-restart-extra-toolchange=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(0);
        def->default_value = opt;
    }

    def = this->add("retract_speed", coFloats);
    def->label = __TRANS("Speed");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("The speed for retractions (it only applies to the extruder motor). If you use the Firmware Retraction option, please note this value still affects the auto-speed pressure regulator.");
    def->sidetext = "mm/s";
    def->cli = "retract-speed=f@";
    {
        ConfigOptionFloats* opt = new ConfigOptionFloats();
        opt->values.push_back(40);
        def->default_value = opt;
    }

    def = this->add("seam_position", coEnum);
    def->label = __TRANS("Seam position");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Position of perimeters starting points.");
    def->cli = "seam-position=s";
    def->enum_keys_map = ConfigOptionEnum<SeamPosition>::get_enum_values();
    def->enum_values.push_back("random");
    def->enum_values.push_back("nearest");
    def->enum_values.push_back("aligned");
    def->enum_values.push_back("rear");
    def->enum_labels.push_back("Random");
    def->enum_labels.push_back("Nearest");
    def->enum_labels.push_back("Aligned");
    def->enum_labels.push_back("Rear");
    def->default_value = new ConfigOptionEnum<SeamPosition>(spAligned);

    def = this->add("serial_port", coString);
    def->gui_type = "select_open";
    def->label = "";
    def->full_label = __TRANS("Serial port");
    def->tooltip = __TRANS("USB/serial port for printer connection.");
    def->cli = "serial-port=s";
    def->width = 200;
    def->default_value = new ConfigOptionString("");

    def = this->add("serial_speed", coInt);
    def->gui_type = "i_enum_open";
    def->label = __TRANS("Speed");
    def->full_label = "Serial port speed";
    def->tooltip = __TRANS("Speed (baud) of USB/serial port for printer connection.");
    def->cli = "serial-speed=i";
    def->min = 1;
    def->max = 500000;
    def->enum_values.push_back("57600");
    def->enum_values.push_back("115200");
    def->enum_values.push_back("250000");
    def->default_value = new ConfigOptionInt(250000);

    def = this->add("skirt_distance", coFloat);
    def->label = __TRANS("Distance from object");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("Distance between skirt and object(s). Set this to zero to attach the skirt to the object(s) and get a brim for better adhesion.");
    def->sidetext = "mm";
    def->cli = "skirt-distance=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(6);

    def = this->add("skirt_height", coInt);
    def->label = __TRANS("Skirt height");
    def->category = __TRANS("Skirt and brim");
    def->tooltip = __TRANS("Height of skirt expressed in layers. Set this to a tall value to use skirt as a shield against drafts.");
    def->sidetext = "layers";
    def->cli = "skirt-height=i";
    def->default_value = new ConfigOptionInt(1);

    def = this->add("skirts", coInt);
    def->label = __TRANS("Loops (minimum)");
    def->category = __TRANS("Skirt and brim");
    def->full_label = "Skirt Loops";
    def->tooltip = __TRANS("Number of loops for the skirt. If the Minimum Extrusion Length option is set, the number of loops might be greater than the one configured here. Set this to zero to disable skirt completely.");
    def->cli = "skirts=i";
    def->min = 0;
    def->default_value = new ConfigOptionInt(1);
    
    def = this->add("slowdown_below_layer_time", coInt);
    def->label = __TRANS("Slow down if layer print time is below");
    def->tooltip = __TRANS("If layer print time is estimated below this number of seconds, print moves speed will be scaled down to extend duration to this value.");
    def->sidetext = __TRANS("approximate seconds");
    def->cli = "slowdown-below-layer-time=i";
    def->width = 60;
    def->min = 0;
    def->max = 1000;
    def->default_value = new ConfigOptionInt(5);

    def = this->add("small_perimeter_speed", coFloatOrPercent);
    def->label = __TRANS("↳ small");
    def->full_label = __TRANS("Small perimeters speed");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("This separate setting will affect the speed of perimeters having radius <= 6.5mm (usually holes). If expressed as percentage (for example: 80%) it will be calculated on the perimeters speed setting above.");
    def->sidetext = "mm/s or %";
    def->cli = "small-perimeter-speed=s";
    def->ratio_over = "perimeter_speed";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(15, false);

    def = this->add("small_perimeter_length", coFloat);
    def->label = __TRANS("Small perimeter length");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("This setting controls the lenghtof the small perimeters");
    def->sidetext = "mm";
    def->cli = "small-perimeter-length=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(6.5*2*PI);

    def = this->add("solid_infill_below_area", coFloat);
    def->label = __TRANS("Solid infill threshold area");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Force solid infill for regions having a smaller area than the specified threshold.");
    def->sidetext = "mm²";
    def->cli = "solid-infill-below-area=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(70);

    def = this->add("solid_infill_extruder", coInt);
    def->label = __TRANS("Solid infill extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use when printing solid infill.");
    def->cli = "solid-infill-extruder=i";
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("solid_infill_every_layers", coInt);
    def->label = __TRANS("Solid infill every");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("This feature allows to force a solid layer every given number of layers. Zero to disable. You can set this to any value (for example 9999); Slic3r will automatically choose the maximum possible number of layers to combine according to nozzle diameter and layer height.");
    def->sidetext = __TRANS("layers");
    def->cli = "solid-infill-every-layers=i";
    def->min = 0;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("solid_infill_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("↳ solid");
    def->full_label = __TRANS("Solid infill extrusion width");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for infill for solid surfaces. If expressed as percentage (for example 90%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "solid-infill-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("solid_infill_speed", coFloatOrPercent);
    def->label = __TRANS("↳ solid");
    def->full_label = __TRANS("Solid infill speed");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for printing solid regions (top/bottom/internal horizontal shells). This can be expressed as a percentage (for example: 80%) over the default infill speed above.");
    def->sidetext = "mm/s or %";
    def->cli = "solid-infill-speed=s";
    def->ratio_over = "infill_speed";
    def->aliases.push_back("solid_infill_feed_rate");
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(20, false);

    def = this->add("solid_layers", coInt);
    def->label = __TRANS("Solid layers");
    def->tooltip = __TRANS("Number of solid layers to generate on top and bottom surfaces.");
    def->cli = "solid-layers=i";
    def->shortcut.push_back("top_solid_layers");
    def->shortcut.push_back("bottom_solid_layers");
    def->min = 0;

    def = this->add("spiral_vase", coBool);
    def->label = __TRANS("Spiral vase");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("This feature will raise Z gradually while printing a single-walled object in order to remove any visible seam. This option requires a single perimeter, no infill, no top solid layers and no support material. You can still set any number of bottom solid layers as well as skirt/brim loops. It won't work when printing more than an object.");
    def->cli = "spiral-vase!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("standby_temperature_delta", coInt);
    def->label = __TRANS("Temperature variation");
    def->full_label = __TRANS("Standby temperature delta");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("Temperature difference to be applied when an extruder is not active.  Enables a full-height \"sacrificial\" skirt on which the nozzles are periodically wiped.");
    def->sidetext = "∆°C";
    def->cli = "standby-temperature-delta=i";
    def->min = -500;
    def->max = 500;
    def->default_value = new ConfigOptionInt(-5);

    def = this->add("start_gcode", coString);
    def->label = __TRANS("Start G-code");
    def->tooltip = __TRANS("This start procedure is inserted at the beginning, after bed has reached the target temperature and extruder just started heating, and before extruder has finished heating. If Slic3r detects M104, M109, M140 or M190 in your custom codes, such commands will not be prepended automatically so you're free to customize the order of heating commands and other custom actions. Note that you can use placeholder variables for all Slic3r settings, so you can put a \"M109 S[first_layer_temperature]\" command wherever you want.");
    def->cli = "start-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    def->default_value = new ConfigOptionString("G28 ; home all axes\nG1 Z5 F5000 ; lift nozzle\n");

    def = this->add("start_filament_gcode", coStrings);
    def->label = __TRANS("Start G-code");
    def->tooltip = __TRANS("This start procedure is inserted at the beginning, after any printer start gcode. This is used to override settings for a specific filament. If Slic3r detects M104, M109, M140 or M190 in your custom codes, such commands will not be prepended automatically so you're free to customize the order of heating commands and other custom actions. Note that you can use placeholder variables for all Slic3r settings, so you can put a \"M109 S[first_layer_temperature]\" command wherever you want. If you have multiple extruders, the gcode is processed in extruder order.");
    def->cli = "start-filament-gcode=s@";
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    {
        ConfigOptionStrings* opt = new ConfigOptionStrings();
        opt->values.push_back("; Filament gcode\n");
        def->default_value = opt;
    }

    def = this->add("support_material", coBool);
    def->label = __TRANS("Generate support material");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Enable support material generation.");
    def->cli = "support-material!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("support_material_angle", coInt);
    def->label = __TRANS("Pattern angle");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Use this setting to rotate the support material pattern on the horizontal plane.");
    def->sidetext = "°";
    def->cli = "support-material-angle=i";
    def->min = 0;
    def->max = 359;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("support_material_buildplate_only", coBool);
    def->label = __TRANS("Support on build plate only");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Only create support if it lies on a build plate. Don't create support on a print.");
    def->cli = "support-material-buildplate-only!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("support_material_contact_distance", coFloat);
    def->gui_type = "f_enum_open";
    def->label = __TRANS("Contact Z distance");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("The vertical distance between object and support material interface. Setting this to 0 will also prevent Slic3r from using bridge flow and speed for the first object layer.");
    def->sidetext = "mm";
    def->cli = "support-material-contact-distance=f";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_values.push_back("0.2");
    def->enum_labels.push_back("0 (soluble)");
    def->enum_labels.push_back("0.2 (detachable)");
    def->default_value = new ConfigOptionFloat(0.2);

    def = this->add("support_material_max_layers", coInt);
    def->label = __TRANS("Max layer count for supports");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Disable support generation above this layer. Setting this to 0 will disable this feature.");
    def->sidetext = __TRANS("layers");
    def->cli = "support-material-max-layers=f";
    def->full_label = "Maximum layer count for support generation";
    def->min = 0;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("support_material_enforce_layers", coInt);
    def->label = __TRANS("Enforce support for the first");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Generate support material for the specified number of layers counting from bottom, regardless of whether normal support material is enabled or not and regardless of any angle threshold. This is useful for getting more adhesion of objects having a very thin or poor footprint on the build plate.");
    def->sidetext = "layers";
    def->cli = "support-material-enforce-layers=f";
    def->full_label = __TRANS("Enforce support for the first n layers");
    def->min = 0;
    def->default_value = new ConfigOptionInt(0);

    def = this->add("support_material_extruder", coInt);
    def->label = __TRANS("Support material/raft/skirt extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use when printing support material, raft and skirt.");
    def->cli = "support-material-extruder=i";
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("support_material_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("Support material");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for support material. If expressed as percentage (for example 90%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "support-material-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("support_material_interface_extruder", coInt);
    def->label = __TRANS("Support material/raft interface extruder");
    def->category = __TRANS("Extruders");
    def->tooltip = __TRANS("The extruder to use when printing support material interface. This affects raft too.");
    def->cli = "support-material-interface-extruder=i";
    def->min = 1;
    def->default_value = new ConfigOptionInt(1);

    def = this->add("support_material_interface_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("Support interface");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for support material interface. If expressed as percentage (for example 90%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "support-material-interface-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("support_material_interface_layers", coInt);
    def->label = __TRANS("Interface layers");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Number of interface layers to insert between the object(s) and support material.");
    def->sidetext = __TRANS("layers");
    def->cli = "support-material-interface-layers=i";
    def->min = 0;
    def->default_value = new ConfigOptionInt(3);

    def = this->add("support_material_interface_spacing", coFloat);
    def->label = __TRANS("Interface pattern spacing");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Spacing between interface lines. Set zero to get a solid interface.");
    def->sidetext = "mm";
    def->cli = "support-material-interface-spacing=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("support_material_interface_speed", coFloatOrPercent);
    def->label = __TRANS("↳ interface");
    def->category = __TRANS("Support material interface speed");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Speed for printing support material interface layers. If expressed as percentage (for example 50%) it will be calculated over support material speed.");
    def->sidetext = "mm/s or %";
    def->cli = "support-material-interface-speed=s";
    def->ratio_over = "support_material_speed";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(100, true);

    def = this->add("support_material_pattern", coEnum);
    def->label = __TRANS("Pattern");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Pattern used to generate support material.");
    def->cli = "support-material-pattern=s";
    def->enum_keys_map = ConfigOptionEnum<SupportMaterialPattern>::get_enum_values();
    def->enum_values.push_back(__TRANS("rectilinear"));
    def->enum_values.push_back(__TRANS("rectilinear-grid"));
    def->enum_values.push_back(__TRANS("honeycomb"));
    def->enum_values.push_back(__TRANS("pillars"));
    def->enum_labels.push_back(__TRANS("rectilinear"));
    def->enum_labels.push_back(__TRANS("rectilinear grid"));
    def->enum_labels.push_back(__TRANS("honeycomb"));
    def->enum_labels.push_back(__TRANS("pillars"));
    def->default_value = new ConfigOptionEnum<SupportMaterialPattern>(smpPillars);

    def = this->add("support_material_pillar_size", coFloat);
    def->label = __TRANS("Pillar size");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Size of the pillars in the pillar support pattern");
    def->sidetext = "mm";
    def->cli = "support-material-pillar-size=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(2.5);

    def = this->add("support_material_pillar_spacing", coFloat);
    def->label = __TRANS("Pillar spacing");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Spacing between pillars in the pillar support pattern");
    def->sidetext = "mm";
    def->cli = "support-material-pillar-spacing=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(10);

    def = this->add("support_material_spacing", coFloat);
    def->label = __TRANS("Pattern spacing");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Spacing between support material lines.");
    def->sidetext = "mm";
    def->cli = "support-material-spacing=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(2.5);




    def = this->add("support_material_speed", coFloat);
    def->label = __TRANS("Support material");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Speed for printing support material.");
    def->sidetext = "mm/s";
    def->cli = "support-material-speed=f";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloat(60);

    def = this->add("support_material_threshold", coFloatOrPercent);
    def->label = __TRANS("Overhang threshold");
    def->category = __TRANS("Support material");
    def->tooltip = __TRANS("Support material will not be generated for overhangs whose slope angle (90° = vertical) is above the given threshold. In other words, this value represent the most horizontal slope (measured from the horizontal plane) that you can print without support material. Set to a percentage to automatically detect based on some % of overhanging perimeter width instead (recommended).");
    def->sidetext = "° (or %)";
    def->cli = "support-material-threshold=s";
    def->min = 0;
    def->max = 300;
    def->default_value = new ConfigOptionFloatOrPercent(60, true);

    def = this->add("temperature", coInts);
    def->label = __TRANS("Other layers");
    def->tooltip = __TRANS("Extruder temperature for layers after the first one. Set this to zero to disable temperature control commands in the output.");
    def->cli = "temperature=i@";
    def->full_label = __TRANS("Temperature");
    def->max = 0;
    def->max = 500;
    {
        ConfigOptionInts* opt = new ConfigOptionInts();
        opt->values.push_back(200);
        def->default_value = opt;
    }
    
    def = this->add("thin_walls", coBool);
    def->label = __TRANS("Detect thin walls");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Detect single-width walls (parts where two extrusions don't fit and we need to collapse them into a single trace).");
    def->cli = "thin-walls!";
    def->default_value = new ConfigOptionBool(true);

    def = this->add("threads", coInt);
    def->label = __TRANS("Threads");
    def->tooltip = __TRANS("Threads are used to parallelize long-running tasks. Optimal threads number is slightly above the number of available cores/processors.");
    def->readonly = true;
    def->cli = "threads=i";
    def->min = 1;
    {
        unsigned int threads = boost::thread::hardware_concurrency();
        def->default_value = new ConfigOptionInt(threads > 0 ? threads : 2);
    }
    
    def = this->add("toolchange_gcode", coString);
    def->label = __TRANS("Tool change G-code");
    def->tooltip = __TRANS("This custom code is inserted right before every extruder change. Note that you can use placeholder variables for all Slic3r settings as well as [previous_extruder], [next_extruder], [previous_retraction] and [next_retraction].");
    def->cli = "toolchange-gcode=s";
    def->multiline = true;
    def->full_width = true;
    def->height = 50;
    def->default_value = new ConfigOptionString("");

    def = this->add("top_infill_extrusion_width", coFloatOrPercent);
    def->label = __TRANS("↳ top solid");
    def->full_label = __TRANS("Top solid infill extrusion width");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Extrusion Width");
    def->tooltip = __TRANS("Set this to a non-zero value to set a manual extrusion width for infill for top surfaces. You may want to use thinner extrudates to fill all narrow regions and get a smoother finish. If expressed as percentage (for example 90%) it will be computed over layer height.");
    def->sidetext = "mm or %";
    def->cli = "top-infill-extrusion-width=s";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("default");
    def->default_value = new ConfigOptionFloatOrPercent(0, false);

    def = this->add("top_infill_pattern", external_fill_pattern);
    def->label = __TRANS("Top");
    def->full_label = __TRANS("Top infill pattern");
    def->category = __TRANS("Infill");
    def->tooltip = __TRANS("Infill pattern for top layers. This only affects the external visible layer, and not its adjacent solid shells.");
    def->cli = "top-infill-pattern=s";
    def->default_value = new ConfigOptionEnum<InfillPattern>(ipRectilinear);

    def = this->add("top_solid_infill_speed", coFloatOrPercent);
    def->label = __TRANS("↳ top solid");
    def->full_label = __TRANS("Top solid infill speed");
    def->gui_type = "f_enum_open";
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for printing top solid layers (it only applies to the uppermost external layers and not to their internal solid layers). You may want to slow down this to get a nicer surface finish. This can be expressed as a percentage (for example: 80%) over the solid infill speed above.");
    def->sidetext = "mm/s or %";
    def->cli = "top-solid-infill-speed=s";
    def->ratio_over = "solid_infill_speed";
    def->min = 0;
    def->enum_values.push_back("0");
    def->enum_labels.push_back("auto");
    def->default_value = new ConfigOptionFloatOrPercent(15, false);

    def = this->add("top_solid_layers", coInt);
    def->label = __TRANS("Top");
    def->category = __TRANS("Layers and Perimeters");
    def->tooltip = __TRANS("Number of solid layers to generate on top surfaces.");
    def->cli = "top-solid-layers=i";
    def->full_label = "Top solid layers";
    def->min = 0;
    def->default_value = new ConfigOptionInt(3);

    def = this->add("travel_speed", coFloat);
    def->label = __TRANS("Travel");
    def->category = __TRANS("Speed");
    def->tooltip = __TRANS("Speed for travel moves (jumps between distant extrusion points).");
    def->sidetext = "mm/s";
    def->cli = "travel-speed=f";
    def->aliases.push_back("travel_feed_rate");
    def->min = 1;
    def->default_value = new ConfigOptionFloat(130);

    def = this->add("use_firmware_retraction", coBool);
    def->label = __TRANS("Use firmware retraction");
    def->tooltip = __TRANS("This experimental setting uses G10 and G11 commands to have the firmware handle the retraction. This is only supported in recent Marlin.");
    def->cli = "use-firmware-retraction!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("use_relative_e_distances", coBool);
    def->label = __TRANS("Use relative E distances");
    def->tooltip = __TRANS("If your firmware requires relative E values, check this, otherwise leave it unchecked. Most firmwares use absolute values.");
    def->cli = "use-relative-e-distances!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("use_set_and_wait_extruder", coBool);
    def->label = __TRANS("Use Set-and-Wait GCode (Extruder)");
    def->tooltip = __TRANS("If your firmware supports a set and wait gcode for temperature changes, use it for automatically inserted temperature gcode for all extruders. Does not affect custom gcode.");
    def->cli = "use-set-and-wait-extruder!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("use_set_and_wait_bed", coBool);
    def->label = __TRANS("Use Set-and-Wait GCode (Bed)");
    def->tooltip = __TRANS("If your firmware supports a set and wait gcode for temperature changes, use it for automatically inserted temperature gcode for the heatbed. Does not affect custom gcode.");
    def->cli = "use-set-and-wait-heatbed!";
    def->default_value = new ConfigOptionBool(false);


    def = this->add("use_volumetric_e", coBool);
    def->label = __TRANS("Use volumetric E");
    def->tooltip = __TRANS("This experimental setting uses outputs the E values in cubic millimeters instead of linear millimeters. If your firmware doesn't already know filament diameter(s), you can put commands like 'M200 D[filament_diameter_0] T0' in your start G-code in order to turn volumetric mode on and use the filament diameter associated to the filament selected in Slic3r. This is only supported in recent Marlin.");
    def->cli = "use-volumetric-e!";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("vibration_limit", coFloat);
    def->label = __TRANS("Vibration limit (deprecated)");
    def->tooltip = __TRANS("This experimental option will slow down those moves hitting the configured frequency limit. The purpose of limiting vibrations is to avoid mechanical resonance. Set zero to disable.");
    def->sidetext = "Hz";
    def->cli = "vibration-limit=f";
    def->min = 0;
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("wipe", coBools);
    def->label = __TRANS("Wipe while retracting");
    def->category = __TRANS("Retraction");
    def->tooltip = __TRANS("This flag will move the nozzle while retracting to minimize the possible blob on leaky extruders.");
    def->cli = "wipe!";
    {
        ConfigOptionBools* opt = new ConfigOptionBools();
        opt->values.push_back(false);
        def->default_value = opt;
    }

    def = this->add("xy_size_compensation", coFloat);
    def->label = __TRANS("XY Size Compensation");
    def->category = __TRANS("Advanced");
    def->tooltip = __TRANS("The object will be grown/shrunk in the XY plane by the configured value (negative = inwards, positive = outwards). This might be useful for fine-tuning hole sizes.");
    def->sidetext = "mm";
    def->cli = "xy-size-compensation=f";
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("z_offset", coFloat);
    def->label = __TRANS("Z offset");
    def->tooltip = __TRANS("This value will be added (or subtracted) from all the Z coordinates in the output G-code. It is used to compensate for bad Z endstop position: for example, if your endstop zero actually leaves the nozzle 0.3mm far from the print bed, set this to -0.3 (or fix your endstop).");
    def->sidetext = "mm";
    def->cli = "z-offset=f";
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("z_steps_per_mm", coFloat);
    def->label = __TRANS("Z full steps/mm");
    def->tooltip = __TRANS("Set this to the number of *full* steps (not microsteps) needed for moving the Z axis by 1mm; you can calculate this by dividing the number of microsteps configured in your firmware by the microstepping amount (8, 16, 32). Slic3r will round your configured layer height to the nearest multiple of that value in order to ensure the best accuracy. This is most useful for machines with imperial leadscrews or belt-driven Z or for unusual layer heights with metric leadscrews. If you still experience wobbling, try using 4 * full_step to achieve the same motor phase pattern for every layer (important for belt driven z-axis). Set to zero to disable this experimental feature.");
    def->cli = "z-steps-per-mm=f";
    def->default_value = new ConfigOptionFloat(0);

    def = this->add("sequential_print_priority", coInt);
    def->label = __TRANS("Sequential Printing Priority");
    def->category = __TRANS("Advanced");
    def->tooltip =__TRANS("Set this to alter object priority for sequential printing. Objects are first sorted by priority (smaller integers print first), then by height.");
    def->cli = "sequential-print-priority=i";
    def->default_value = new ConfigOptionInt(0);
}

const PrintConfigDef print_config_def;

void
DynamicPrintConfig::normalize() {
    if (this->has("extruder")) {
        int extruder = this->option("extruder")->getInt();
        this->erase("extruder");
        if (extruder != 0) {
            if (!this->has("infill_extruder"))
                this->option("infill_extruder", true)->setInt(extruder);
            if (!this->has("perimeter_extruder"))
                this->option("perimeter_extruder", true)->setInt(extruder);
            if (!this->has("support_material_extruder"))
                this->option("support_material_extruder", true)->setInt(extruder);
            if (!this->has("support_material_interface_extruder"))
                this->option("support_material_interface_extruder", true)->setInt(extruder);
        }
    }
    
    /*
    if (this->has("external_fill_pattern")) {
        InfillPattern p = this->opt<ConfigOptionEnum<InfillPattern> >("external_fill_pattern");
        this->erase("external_fill_pattern");
        if (!this->has("bottom_infill_pattern"))
            this->opt<ConfigOptionEnum<InfillPattern> >("bottom_infill_pattern", true)->value = p;
        if (!this->has("top_infill_pattern"))
            this->opt<ConfigOptionEnum<InfillPattern> >("top_infill_pattern", true)->value = p;
    }
    */
    
    if (!this->has("solid_infill_extruder") && this->has("infill_extruder"))
        this->option("solid_infill_extruder", true)->setInt(this->option("infill_extruder")->getInt());
    
    if (this->has("spiral_vase") && this->opt<ConfigOptionBool>("spiral_vase", true)->value) {
        {
            // this should be actually done only on the spiral layers instead of all
            ConfigOptionBools* opt = this->opt<ConfigOptionBools>("retract_layer_change", true);
            opt->values.assign(opt->values.size(), false);  // set all values to false
        }
        {
            this->opt<ConfigOptionInt>("perimeters", true)->value       = 1;
            this->opt<ConfigOptionInt>("top_solid_layers", true)->value = 0;
            this->opt<ConfigOptionPercent>("fill_density", true)->value  = 0;
        }
    }
}

double
PrintConfigBase::min_object_distance() const
{
    double extruder_clearance_radius = this->option("extruder_clearance_radius")->getFloat();
    double duplicate_distance = this->option("duplicate_distance")->getFloat();
    
    // min object distance is max(duplicate_distance, clearance_radius)
    return (this->option("complete_objects")->getBool() && extruder_clearance_radius > duplicate_distance)
        ? extruder_clearance_radius
        : duplicate_distance;
}

bool
PrintConfigBase::set_deserialize(t_config_option_key opt_key, std::string str, bool append)
{
    this->_handle_legacy(opt_key, str);
    if (opt_key.empty()) return true; // ignore option
    return ConfigBase::set_deserialize(opt_key, str, append);
}

void
PrintConfigBase::_handle_legacy(t_config_option_key &opt_key, std::string &value) const
{
    // handle legacy options
    if (opt_key == "extrusion_width_ratio" || opt_key == "bottom_layer_speed_ratio"
        || opt_key == "first_layer_height_ratio") {
        boost::replace_first(opt_key, "_ratio", "");
        if (opt_key == "bottom_layer_speed") opt_key = "first_layer_speed";
        try {
            float v = boost::lexical_cast<float>(value);
            if (v != 0) 
                value = boost::lexical_cast<std::string>(v*100) + "%";
        } catch (boost::bad_lexical_cast &) {
            value = "0";
        }
    } else if (opt_key == "gcode_flavor" && value == "makerbot") {
        value = "makerware";
    } else if (opt_key == "fill_density" && value.find("%") == std::string::npos) {
        try {
            // fill_density was turned into a percent value
            float v = boost::lexical_cast<float>(value);
            value = boost::lexical_cast<std::string>(v*100) + "%";
        } catch (boost::bad_lexical_cast &) {}
    } else if (opt_key == "randomize_start" && value == "1") {
        opt_key = "seam_position";
        value = "random";
    } else if (opt_key == "bed_size" && !value.empty()) {
        opt_key = "bed_shape";
        ConfigOptionPoint p;
        p.deserialize(value);
        std::ostringstream oss;
        oss << "0x0," << p.value.x << "x0," << p.value.x << "x" << p.value.y << ",0x" << p.value.y;
        value = oss.str();
    } else if (opt_key == "octoprint_host" && !value.empty()) {
        opt_key = "print_host";
    } else if ((opt_key == "perimeter_acceleration" && value == "25")
        || (opt_key == "infill_acceleration" && value == "50")) {
        /*  For historical reasons, the world's full of configs having these very low values;
            to avoid unexpected behavior we need to ignore them. Banning these two hard-coded
            values is a dirty hack and will need to be removed sometime in the future, but it
            will avoid lots of complaints for now. */
        value = "0";
    } else if (opt_key == "support_material_threshold" && value == "0") {
        // 0 used to be automatic threshold, but we introduced percent values so let's
        // transform it into the default value
        value = "60%";
    }
    
    // cemetery of old config settings
    if (opt_key == "duplicate_x" || opt_key == "duplicate_y" || opt_key == "multiply_x" 
        || opt_key == "multiply_y" || opt_key == "support_material_tool" 
        || opt_key == "acceleration" || opt_key == "adjust_overhang_flow" 
        || opt_key == "standby_temperature" || opt_key == "scale" || opt_key == "rotate" 
        || opt_key == "duplicate" || opt_key == "duplicate_grid" || opt_key == "rotate" 
        || opt_key == "scale"  || opt_key == "duplicate_grid" 
        || opt_key == "start_perimeters_at_concave_points" 
        || opt_key == "start_perimeters_at_non_overhang" || opt_key == "randomize_start" 
        || opt_key == "seal_position" || opt_key == "bed_size" || opt_key == "octoprint_host" 
        || opt_key == "print_center" || opt_key == "g0" || opt_key == "threads")
    {
        opt_key = "";
        return;
    }
    
    if (!this->def->has(opt_key)) {
        //printf("Unknown option %s\n", opt_key.c_str());
        opt_key = "";
        return;
    }
}

CLIActionsConfigDef::CLIActionsConfigDef()
{
    ConfigOptionDef* def;
    
    // Actions:
    def = this->add("export_obj", coBool);
    def->label = __TRANS("Export SVG");
    def->tooltip = __TRANS("Export the model(s) as OBJ.");
    def->cli = "export-obj";
    def->default_value = new ConfigOptionBool(false);
    
    def = this->add("export_pov", coBool);
    def->label = __TRANS("Export POV");
    def->tooltip = __TRANS("Export the model as POV-Ray definition.");
    def->cli = "export-pov";
    def->default_value = new ConfigOptionBool(false);
    
    def = this->add("export_svg", coBool);
    def->label = __TRANS("Export SVG");
    def->tooltip = __TRANS("Slice the model and export solid slices as SVG.");
    def->cli = "export-svg";
    def->default_value = new ConfigOptionBool(false);
    
    def = this->add("export_sla_svg", coBool);
    def->label = __TRANS("Export SVG for SLA");
    def->tooltip = __TRANS("Slice the model and export SLA printing layers as SVG.");
    def->cli = "export-sla-svg|sla";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("export_3mf", coBool);
    def->label = __TRANS("Export 3MF");
    def->tooltip = __TRANS("Export the model(s) as 3MF.");
    def->cli = "export-3mf";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("export_amf", coBool);
    def->label = __TRANS("Export AMF");
    def->tooltip = __TRANS("Export the model(s) as AMF.");
    def->cli = "export-amf";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("export_stl", coBool);
    def->label = __TRANS("Export STL");
    def->tooltip = __TRANS("Export the model(s) as STL.");
    def->cli = "export-stl";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("export_gcode", coBool);
    def->label = __TRANS("Export G-code");
    def->tooltip = __TRANS("Slice the model and export toolpaths as G-code.");
    def->cli = "export-gcode|gcode|g";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("help", coBool);
    def->label = __TRANS("Help");
    def->tooltip = __TRANS("Show this help.");
    def->cli = "help|h";
    def->default_value = new ConfigOptionBool(false);

    def = this->add("help_options", coBool);
    def->label = __TRANS("Help (options)");
    def->tooltip = __TRANS("Show the full list of print/G-code configuration options.");
    def->cli = "help-options";
    def->default_value = new ConfigOptionBool(false);
    
    def = this->add("info", coBool);
    def->label = __TRANS("Output Model Info");
    def->tooltip = __TRANS("Write information about the model to the console.");
    def->cli = "info";
    def->default_value = new ConfigOptionBool(false);
    
    def = this->add("save", coString);
    def->label = __TRANS("Save config file");
    def->tooltip = __TRANS("Save configuration to the specified file.");
    def->cli = "save";
    def->default_value = new ConfigOptionString();
}

CLITransformConfigDef::CLITransformConfigDef()
{
    ConfigOptionDef* def;
    
    // Transform options:
    def = this->add("align_xy", coPoint);
    def->label = __TRANS("Align XY");
    def->tooltip = __TRANS("Align the model to the given point.");
    def->cli = "align-xy";
    def->default_value = new ConfigOptionPoint(Pointf(100,100));
    
    def = this->add("cut", coFloat);
    def->label = __TRANS("Cut");
    def->tooltip = __TRANS("Cut model at the given Z.");
    def->cli = "cut";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("cut_grid", coFloat);
    def->label = __TRANS("Cut");
    def->tooltip = __TRANS("Cut model in the XY plane into tiles of the specified max size.");
    def->cli = "cut-grid";
    def->default_value = new ConfigOptionPoint();
    
    def = this->add("cut_x", coFloat);
    def->label = __TRANS("Cut");
    def->tooltip = __TRANS("Cut model at the given X.");
    def->cli = "cut-x";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("cut_y", coFloat);
    def->label = __TRANS("Cut");
    def->tooltip = __TRANS("Cut model at the given Y.");
    def->cli = "cut-y";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("center", coPoint);
    def->label = __TRANS("Center");
    def->tooltip = __TRANS("Center the print around the given center.");
    def->cli = "center";
    def->default_value = new ConfigOptionPoint(Pointf(100,100));

    def = this->add("dont_arrange", coBool);
    def->label = __TRANS("Don't arrange");
    def->tooltip = __TRANS("Do not rearrange the given models before merging and keep their original XY coordinates.");
    def->cli = "dont-arrange";
    
    def = this->add("duplicate", coInt);
    def->label = __TRANS("Duplicate");
    def->tooltip =__TRANS("Multiply copies by this factor.");
    def->cli = "duplicate=i";
    def->min = 1;
    
    def = this->add("duplicate_grid", coPoint);
    def->label = __TRANS("Duplicate by grid");
    def->tooltip = __TRANS("Multiply copies by creating a grid.");
    def->cli = "duplicate-grid";

    def = this->add("merge", coBool);
    def->label = __TRANS("Merge");
    def->tooltip = __TRANS("Arrange the supplied models in a plate and merge them in a single model in order to perform actions once.");
    def->cli = "merge|m";

    def = this->add("repair", coBool);
    def->label = __TRANS("Repair");
    def->tooltip = __TRANS("Try to repair any non-manifold meshes (this option is implicitly added whenever we need to slice the model to perform the requested action).");
    def->cli = "repair";
    
    def = this->add("rotate", coFloat);
    def->label = __TRANS("Rotate");
    def->tooltip = __TRANS("Rotation angle around the Z axis in degrees.");
    def->cli = "rotate";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("rotate_x", coFloat);
    def->label = __TRANS("Rotate around X");
    def->tooltip = __TRANS("Rotation angle around the X axis in degrees.");
    def->cli = "rotate-x";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("rotate_y", coFloat);
    def->label = __TRANS("Rotate around Y");
    def->tooltip = __TRANS("Rotation angle around the Y axis in degrees.");
    def->cli = "rotate-y";
    def->default_value = new ConfigOptionFloat(0);
    
    def = this->add("scale", coFloatOrPercent);
    def->label = __TRANS("Scale");
    def->tooltip = __TRANS("Scaling factor or percentage.");
    def->cli = "scale";
    def->default_value = new ConfigOptionFloatOrPercent(1, false);

    def = this->add("split", coBool);
    def->label = __TRANS("Split");
    def->tooltip = __TRANS("Detect unconnected parts in the given model(s) and split them into separate objects.");
    def->cli = "split";
    
    def = this->add("scale_to_fit", coPoint3);
    def->label = __TRANS("Scale to Fit");
    def->tooltip = __TRANS("Scale to fit the given volume.");
    def->cli = "scale-to-fit";
    def->default_value = new ConfigOptionPoint3(Pointf3(0,0,0));
}


CLIMiscConfigDef::CLIMiscConfigDef()
{
    ConfigOptionDef* def;
    
    def = this->add("ignore_nonexistent_config", coBool);
    def->label = __TRANS("Ignore non-existent config files");
    def->tooltip = __TRANS("Do not fail if a file supplied to --load does not exist.");
    def->cli = "ignore-nonexistent-config";
    
    def = this->add("load", coStrings);
    def->label = __TRANS("Load config file");
    def->tooltip = __TRANS("Load configuration from the specified file. It can be used more than once to load options from multiple files.");
    def->cli = "load";
    
    def = this->add("output", coString);
    def->label = __TRANS("Output File");
    def->tooltip = __TRANS("The file where the output will be written (if not specified, it will be based on the input file).");
    def->cli = "output|o";
    
    #ifdef USE_WX
    def = this->add("autosave", coString);
    def->label = __TRANS("Autosave");
    def->tooltip = __TRANS("Automatically export current configuration to the specified file.");
    def->cli = "autosave";
    
    def = this->add("datadir", coString);
    def->label = __TRANS("Data directory");
    def->tooltip = __TRANS("Load and store settings at the given directory. This is useful for maintaining different profiles or including configurations from a network storage.");
    def->cli = "datadir";
    #endif
}

const CLIActionsConfigDef    cli_actions_config_def;
const CLITransformConfigDef  cli_transform_config_def;
const CLIMiscConfigDef       cli_misc_config_def;

}
