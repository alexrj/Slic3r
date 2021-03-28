#include <catch.hpp>
#include "test_data.hpp"
#include "Fill/Fill.hpp"
#include "Print.hpp"
#include "Geometry.hpp"
#include "Flow.hpp"

using namespace Slic3r;
using namespace Slic3r::Geometry;

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle = 0, double density = 1.0);

TEST_CASE("Fill: adjusted solid distance") {
    Print print;
    int surface_width {250};

    int distance {Slic3r::Flow::solid_spacing(surface_width, 47)};

    REQUIRE(distance == Approx(50));
    REQUIRE(surface_width % distance == 0);
}

TEST_CASE("Fill: Pattern Path Length") {
    auto filler {Slic3r::Fill::new_from_type("rectilinear")};
    filler->angle = -(PI)/2.0;
    filler->min_spacing = 5;
    filler->dont_adjust = true;
    filler->endpoints_overlap = false;
    filler->density = filler->min_spacing / 50.0;

    auto test {[filler] (const ExPolygon& poly) -> Polylines {
        auto surface {Slic3r::Surface(stTop, poly)};
        return filler->fill_surface(surface);
    }};


    SECTION("Square") {
        Points test_set;
        test_set.reserve(4);
        Pointfs points {Pointf(0,0), Pointf(100,0), Pointf(100,100), Pointf(0,100)};
        for (size_t i = 0; i < 4; ++i) {
            std::transform(points.cbegin()+i, points.cend(),   std::back_inserter(test_set), [] (const Pointf& a) -> Point { return Point::new_scale(a); } ); 
            std::transform(points.cbegin(), points.cbegin()+i, std::back_inserter(test_set), [] (const Pointf& a) -> Point { return Point::new_scale(a); } ); 
            Polylines paths {test(Slic3r::ExPolygon(test_set))};
            REQUIRE(paths.size() == 1); // one continuous path

            // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
            // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
            // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
            REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length

            test_set.clear();
        }
    }
    SECTION("Diamond with endpoints on grid") {
        Pointfs points {Pointf(0,0), Pointf(100,0), Pointf(150,50), Pointf(100,100), Pointf(0,100), Pointf(-50,50)};
        Points test_set;
        test_set.reserve(6);
        std::transform(points.cbegin(), points.cend(),   std::back_inserter(test_set), [] (const Pointf& a) -> Point { return Point::new_scale(a); } ); 
        Polylines paths {test(Slic3r::ExPolygon(test_set))};
        REQUIRE(paths.size() == 1); // one continuous path
    }

    SECTION("Square with hole") {
        Pointfs square { Pointf(0,0), Pointf(100,0), Pointf(100,100), Pointf(0,100)};
        Pointfs hole {Pointf(25,25), Pointf(75,25), Pointf(75,75), Pointf(25,75) };
        std::reverse(hole.begin(), hole.end());

        Points test_hole;
        Points test_square;

        std::transform(square.cbegin(), square.cend(), std::back_inserter(test_square), [] (const Pointf& a) -> Point { return Point::new_scale(a); } ); 
        std::transform(hole.cbegin(), hole.cend(), std::back_inserter(test_hole), [] (const Pointf& a) -> Point { return Point::new_scale(a); } ); 

        for (double angle : {-(PI/2.0), -(PI/4.0), -(PI), PI/2.0, PI}) {
            for (double spacing : {25.0, 5.0, 7.5, 8.5}) {
                filler->density = filler->min_spacing / spacing;
                filler->angle = angle;
                ExPolygon e(test_square, test_hole);
                Polylines paths {test(e)};
                REQUIRE((paths.size() >= 2 && paths.size() <= 3));
                // paths don't cross hole
                REQUIRE(diff_pl(paths, offset(e, +SCALED_EPSILON*10)).size() == 0);
            }
        }
    }
    SECTION("Regression: Missing infill segments in some rare circumstances") {
        filler->angle = (PI/4.0);
        filler->dont_adjust = false;
        filler->min_spacing = 0.654498;
        filler->endpoints_overlap = unscale(359974);
        filler->density = 1;
        filler->layer_id = 66;
        filler->z = 20.15;

        Points points {Point(25771516,14142125),Point(14142138,25771515),Point(2512749,14142131),Point(14142125,2512749)};
        Polylines paths {test(Slic3r::ExPolygon(points))};
        REQUIRE(paths.size() == 1); // one continuous path

        // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
        // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
        // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
        REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length
    }

    SECTION("Rotated Square") {
        Points square { Point::new_scale(0,0), Point::new_scale(50,0), Point::new_scale(50,50), Point::new_scale(0,50)};
        ExPolygon expolygon(square);
        auto filler {Slic3r::Fill::new_from_type("rectilinear")};
        filler->bounding_box = expolygon.bounding_box();
        filler->angle = 0;
        
        auto surface {Surface(stTop, expolygon)};
        auto flow {Slic3r::Flow(0.69, 0.4, 0.50)};

        filler->min_spacing = flow.spacing();
        filler->density = 1.0;

        for (auto angle : { 0.0, 45.0}) {
            surface.expolygon.rotate(angle, Point(0,0));
            auto paths {filler->fill_surface(surface)};
            REQUIRE(paths.size() == 1);
        }
    }
    SECTION("Solid surface fill") {
        Points points {
            Point::new_scale(6883102, 9598327.01296997),
            Point::new_scale(6883102, 20327272.01297),
            Point::new_scale(3116896, 20327272.01297),
            Point::new_scale(3116896, 9598327.01296997) 
        };
        ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        for (size_t i = 0; i <= 20; ++i)
        {
            expolygon.scale(1.05);
            REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        }
    }
    SECTION("Solid surface fill") {
        Points points {
                Point(59515297,5422499),Point(59531249,5578697),Point(59695801,6123186),
                Point(59965713,6630228),Point(60328214,7070685),Point(60773285,7434379),
                Point(61274561,7702115),Point(61819378,7866770),Point(62390306,7924789),
                Point(62958700,7866744),Point(63503012,7702244),Point(64007365,7434357),
                Point(64449960,7070398),Point(64809327,6634999),Point(65082143,6123325),
                Point(65245005,5584454),Point(65266967,5422499),Point(66267307,5422499),
                Point(66269190,8310081),Point(66275379,17810072),Point(66277259,20697500),
                Point(65267237,20697500),Point(65245004,20533538),Point(65082082,19994444),
                Point(64811462,19488579),Point(64450624,19048208),Point(64012101,18686514),
                Point(63503122,18415781),Point(62959151,18251378),Point(62453416,18198442),
                Point(62390147,18197355),Point(62200087,18200576),Point(61813519,18252990),
                Point(61274433,18415918),Point(60768598,18686517),Point(60327567,19047892),
                Point(59963609,19493297),Point(59695865,19994587),Point(59531222,20539379),
                Point(59515153,20697500),Point(58502480,20697500),Point(58502480,5422499)
        };
        ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55, PI/2.0) == true);
    }
    SECTION("Solid surface fill") {
        Points points {
            Point::new_scale(0,0),Point::new_scale(98,0),Point::new_scale(98,10), Point::new_scale(0,10)
        };
        ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.5, 45.0, 0.99) == true);
    }

}

/* 

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([4,0], [10,0], [15,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([15,0], [10,0], [4,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

for my $pattern (qw(rectilinear honeycomb hilbertcurve concentric)) {
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('fill_pattern', $pattern);
    $config->set('external_fill_pattern', $pattern);
    $config->set('perimeters', 1);
    $config->set('skirts', 0);
    $config->set('fill_density', 20);
    $config->set('layer_height', 0.05);
    $config->set('perimeter_extruder', 1);
    $config->set('infill_extruder', 2);
    my $print = Slic3r::Test::init_print('20mm_cube', config => $config, scale => 2);
    ok my $gcode = Slic3r::Test::gcode($print), "successful $pattern infill generation";
    my $tool = undef;
    my @perimeter_points = my @infill_points = ();
    Slic3r::GCode::Reader->new->parse($gcode, sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->perimeter_extruder-1) {
            push @perimeter_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            } elsif ($tool == $config->infill_extruder-1) {
            push @infill_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            }
            }
            });
    my $convex_hull = convex_hull(\@perimeter_points);
    ok !(defined first { !$convex_hull->contains_point($_) } @infill_points), "infill does not exceed perimeters ($pattern)";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('infill_only_where_needed', 1);
    $config->set('bottom_solid_layers', 0);
    $config->set('infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.5);
    $config->set('fill_density', 40);
    $config->set('cooling', 0);                 # for preventing speeds from being altered
        $config->set('first_layer_speed', '100%');  # for preventing speeds from being altered

        my $test = sub {
            my $print = Slic3r::Test::init_print('pyramid', config => $config);

            my $tool = undef;
            my @infill_extrusions = ();  # array of polylines
                Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
                        my ($self, $cmd, $args, $info) = @_;

                        if ($cmd =~ /^T(\d+)/) {
                        $tool = $1;
                        } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
                        if ($tool == $config->infill_extruder-1) {
                        push @infill_extrusions, Slic3r::Line->new_scale(
                                [ $self->X, $self->Y ],
                                [ $info->{new_X}, $info->{new_Y} ],
                                );
                        }
                        }
                        });
            return 0 if !@infill_extrusions;  # prevent calling convex_hull() with no points

                my $convex_hull = convex_hull([ map $_->pp, map @$_, @infill_extrusions ]);
            return unscale unscale sum(map $_->area, @{offset([$convex_hull], scale(+$config->infill_extrusion_width/2))});
        };

    my $tolerance = 5;  # mm^2

        $config->set('solid_infill_below_area', 0);
    ok $test->() < $tolerance,
       'no infill is generated when using infill_only_where_needed on a pyramid';

    $config->set('solid_infill_below_area', 70);
    ok abs($test->() - $config->solid_infill_below_area) < $tolerance,
       'infill is only generated under the forced solid shells';
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 1);
    $config->set('fill_density', 0);
    $config->set('top_solid_layers', 0);
    $config->set('bottom_solid_layers', 0);
    $config->set('solid_infill_below_area', 20000000);
    $config->set('solid_infill_every_layers', 2);
    $config->set('perimeter_speed', 99);
    $config->set('external_perimeter_speed', 99);
    $config->set('cooling', 0);
    $config->set('first_layer_speed', '100%');

    my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %layers_with_extrusion = ();
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd eq 'G1' && $info->{dist_XY} > 0 && $info->{extruding}) {
            if (($args->{F} // $self->F) != $config->perimeter_speed*60) {
            $layers_with_extrusion{$self->Z} = ($args->{F} // $self->F);
            }
            }
            });

    ok !%layers_with_extrusion,
       "solid_infill_below_area and solid_infill_every_layers are ignored when fill_density is 0";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 3);
    $config->set('fill_density', 0);
    $config->set('layer_height', 0.2);
    $config->set('first_layer_height', 0.2);
    $config->set('nozzle_diameter', [0.35]);
    $config->set('infill_extruder', 2);
    $config->set('solid_infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.52);
    $config->set('solid_infill_extrusion_width', 0.52);
    $config->set('first_layer_extrusion_width', 0);

    my $print = Slic3r::Test::init_print('A', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->infill_extruder-1) {
            my $z = 1 * $self->Z;
            $infill{$z} ||= [];
            push @{$infill{$z}}, Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            }
            }
            });
    my $grow_d = scale($config->infill_extrusion_width)/2;
    my $layer0_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.2} } ]);
    my $layer1_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.4} } ]);
    my $diff = diff($layer0_infill, $layer1_infill);
    $diff = offset2_ex($diff, -$grow_d, +$grow_d);
    $diff = [ grep { $_->area > 2*(($grow_d*2)**2) } @$diff ];
    is scalar(@$diff), 0, 'no missing parts in solid shell when fill_density is 0';
}

{
    # GH: #2697
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('perimeter_extrusion_width', 0.72);
    $config->set('top_infill_extrusion_width', 0.1);
    $config->set('infill_extruder', 2);         # in order to distinguish infill
        $config->set('solid_infill_extruder', 2);   # in order to distinguish infill

        my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my %other  = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            my $z = 1 * $self->Z;
            my $line = Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            if ($tool == $config->infill_extruder-1) {
            $infill{$z} //= [];
            push @{$infill{$z}}, $line;
            } else {
            $other{$z} //= [];
            push @{$other{$z}}, $line;
            }
            }
            });
    my $top_z = max(keys %infill);
    my $top_infill_grow_d = scale($config->top_infill_extrusion_width)/2;
    my $top_infill = union([ map @{$_->grow($top_infill_grow_d)}, @{ $infill{$top_z} } ]);
    my $perimeters_grow_d = scale($config->perimeter_extrusion_width)/2;
    my $perimeters = union([ map @{$_->grow($perimeters_grow_d)}, @{ $other{$top_z} } ]);
    my $covered = union_ex([ @$top_infill, @$perimeters ]);
    my @holes = map @{$_->holes}, @$covered;
    ok sum(map unscale unscale $_->area*-1, @holes) < 1, 'no gaps between top solid infill and perimeters';
}
*/

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle, double density) {
    auto* filler {Slic3r::Fill::new_from_type("rectilinear")};
    filler->bounding_box = expolygon.bounding_box();
    filler->angle = angle;
    filler->dont_adjust = false;

    Surface surface(stBottom, expolygon);
    Flow flow(flow_spacing, 0.4, flow_spacing);

    filler->min_spacing = flow.spacing();
    filler->density = density;

    Polylines paths {filler->fill_surface(surface)};

    // check whether any part was left uncovered
    Polygons grown_paths;
    grown_paths.reserve(paths.size());

// figure out what is actually going on here re: data types
    std::for_each(paths.begin(), paths.end(), [filler, &grown_paths] (const Polyline& p) { 
        polygons_append(grown_paths, p.grow(scale_(filler->spacing() / 2.0)));
    });
    
    ExPolygons uncovered = diff_ex(expolygon, grown_paths, true);

    // ignore very small dots
    const auto scaled_flow_spacing { std::pow(scale_(flow_spacing), 2) };
    auto iter {std::remove_if(uncovered.begin(), uncovered.end(), [scaled_flow_spacing] (const ExPolygon& poly) {
        return poly.area() > scaled_flow_spacing;
    }) };
    uncovered.erase(iter, uncovered.end());

    return uncovered.size() == 0; // solid surface is fully filled

}
