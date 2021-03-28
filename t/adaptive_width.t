use Test::More;
use strict;
use warnings;

plan tests => 32;

BEGIN {
    use FindBin;
    use lib "$FindBin::Bin/../lib";
    use local::lib "$FindBin::Bin/../local-lib";
}

use List::Util qw(first);
use Slic3r;
use Slic3r::Geometry qw(X Y scale epsilon);
use Slic3r::Surface ':types';

sub scale_points (@) { map [scale $_->[X], scale $_->[Y]], @_ }

{
    my $test = sub {
        my ($expolygon, $fw, $expected, $descr) = @_;
        
        my $flow = Slic3r::Flow->new(
            nozzle_diameter => 0.5,
            height          => 0.4,
            width           => $fw,
        );
        my $slices = Slic3r::Surface::Collection->new;
        $slices->append(Slic3r::Surface->new(
            surface_type    => S_TYPE_INTERNAL,
            expolygon       => $expolygon,
        ));
        my $config          = Slic3r::Config::Full->new;
        $config->set('thin_walls_overlap',0);
        my $loops           = Slic3r::ExtrusionPath::Collection->new;
        my $gap_fill        = Slic3r::ExtrusionPath::Collection->new;
        my $fill_surfaces   = Slic3r::Surface::Collection->new;
        my $pg = Slic3r::Layer::PerimeterGenerator->new(
            $slices, $flow->height, $flow,
            $config, $config, $config,
            $loops, $gap_fill, $fill_surfaces,
        );
        $pg->process;
        
        $loops = $loops->flatten;
        my @single = grep $_->isa('Slic3r::ExtrusionPath'), @$loops;
        my @loops  = grep $_->isa('Slic3r::ExtrusionLoop'), @$loops;
        
        if (0) {
            require "Slic3r/SVG.pm";
            Slic3r::SVG::output(
                "output.svg",
                expolygons => [$expolygon],
                polylines => [ map $_->isa('Slic3r::ExtrusionPath') ? $_->polyline : $_->polygon->split_at_first_point, @$loops ],
                red_polylines => [ map $_->polyline, @$gap_fill ],
            );
        }
        
        is scalar(@single),  $expected->{single} // 0, "expected number of single lines ($descr)";
        is scalar(@loops),   $expected->{loops}  // 0, "expected number of loops ($descr)";
        is scalar(@$gap_fill), $expected->{gaps} // 0, "expected number of gap fills ($descr)";
        
        if ($expected->{single}) {
            ok abs($loops->[0]->width - $expected->{width}) < epsilon, "single line covers the full width ($descr)";
        }
        if ($expected->{loops}) {
            my $loop_width = $loops[0][0]->width;
            my $gap_fill_width = @$gap_fill ? $gap_fill->[0]->width : 0;
            ok $loop_width * $expected->{loops} * 2 + $gap_fill_width > $expected->{width} - epsilon,
                "loop total width + gap fill covers the full width ($descr)";
        }
    };
    
    my $fw = 0.7;
    my $test_rect = sub {
        my ($width, $expected) = @_;
        
        my $e = Slic3r::ExPolygon->new([ scale_points [0,0], [100,0], [100,$width], [0,$width] ]);
        $expected->{width} = $width;
        $test->($e, $fw, $expected, $width);
    };
    $test_rect->($fw * 1,   { single => 1, gaps => 0 });
    $test_rect->($fw * 1.3, { single => 1, gaps => 0 });
    $test_rect->($fw * 1.5, { single => 1, gaps => 0 });
    $test_rect->($fw * 2,   { loops => 1,  gaps => 0 });
    $test_rect->($fw * 2.5, { loops => 1,  gaps => 1 });
    $test_rect->($fw * 3,   { loops => 1,  gaps => 1 });
    $test_rect->($fw * 3.5, { loops => 2,  gaps => 0 });
    $test_rect->($fw * 4,   { loops => 2,  gaps => 1 });
}

__END__
