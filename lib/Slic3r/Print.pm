# The slicing work horse.
# Extends C++ class Slic3r::Print
package Slic3r::Print;
use strict;
use warnings;

use File::Basename qw(basename fileparse);
use File::Spec;
use List::Util qw(min max first sum);
use Slic3r::ExtrusionLoop ':roles';
use Slic3r::ExtrusionPath ':roles';
use Slic3r::Flow ':roles';
use Slic3r::Geometry qw(X Y Z X1 Y1 X2 Y2 MIN MAX PI scale unscale convex_hull);
use Slic3r::Geometry::Clipper qw(diff_ex union_ex intersection_ex intersection offset
    offset2 union union_pt_chained JT_ROUND JT_SQUARE diff_pl);
use Slic3r::Print::State ':steps';
use Slic3r::Surface qw(S_TYPE_BOTTOM);

our $status_cb;

sub set_status_cb {
    my ($class, $cb) = @_;
    $status_cb = $cb;
}

sub status_cb {
    return $status_cb // sub {};
}

# this value is not supposed to be compared with $layer->id
# since they have different semantics
sub total_layer_count {
    my $self = shift;
    return max(map $_->total_layer_count, @{$self->objects});
}

sub size {
    my $self = shift;
    return $self->bounding_box->size;
}

sub process {
    my ($self) = @_;
    
    ### No need to call this as we call it as part of prepare_infill()
    ### until we fix the idempotency issue.
    ###$self->status_cb->(20, "Generating perimeters");
    ###$_->make_perimeters for @{$self->objects};
    
    $self->status_cb->(70, "Infilling layers");
    $_->infill for @{$self->objects};
    
    $_->generate_support_material for @{$self->objects};
    $self->make_skirt;
    $self->make_brim;  # must come after make_skirt
    
    # time to make some statistics
    if (0) {
        eval "use Devel::Size";
        print  "MEMORY USAGE:\n";
        printf "  meshes        = %.1fMb\n", List::Util::sum(map Devel::Size::total_size($_->meshes), @{$self->objects})/1024/1024;
        printf "  layer slices  = %.1fMb\n", List::Util::sum(map Devel::Size::total_size($_->slices), map @{$_->layers}, @{$self->objects})/1024/1024;
        printf "  region slices = %.1fMb\n", List::Util::sum(map Devel::Size::total_size($_->slices), map @{$_->regions}, map @{$_->layers}, @{$self->objects})/1024/1024;
        printf "  perimeters    = %.1fMb\n", List::Util::sum(map Devel::Size::total_size($_->perimeters), map @{$_->regions}, map @{$_->layers}, @{$self->objects})/1024/1024;
        printf "  fills         = %.1fMb\n", List::Util::sum(map Devel::Size::total_size($_->fills), map @{$_->regions}, map @{$_->layers}, @{$self->objects})/1024/1024;
        printf "  print object  = %.1fMb\n", Devel::Size::total_size($self)/1024/1024;
    }
    if (0) {
        eval "use Slic3r::Test::SectionCut";
        Slic3r::Test::SectionCut->new(print => $self)->export_svg("section_cut.svg");
    }
}

sub escaped_split {
    my ($line) = @_;

    # Free up three characters for temporary replacement
    $line =~ s/%/%%/g;
    $line =~ s/#/##/g;
    $line =~ s/\?/\?\?/g;

    # replace escaped !'s
    $line =~ s/\!\!/%#\?/g;
    
    # split on non-escaped whitespace
    my @split = split /(?<=[^\!])\s+/, $line, -1;

    for my $part (@split) {
      # replace escaped whitespace with the whitespace
      $part =~ s/\!(\s+)/$1/g;

      # resub temp symbols
      $part =~ s/%#\?/\!/g;
      $part =~ s/%%/%/g;
      $part =~ s/##/#/g;
      $part =~ s/\?\?/\?/g;
    }

    return @split;
}

sub export_gcode {
    my $self = shift;
    my %params = @_;
    
    # prerequisites
    $self->process;
    
    # output everything to a G-code file
    my $output_file = $self->output_filepath($params{output_file} // '');
    $self->status_cb->(90, "Exporting G-code" . ($output_file ? " to $output_file" : ""));
    
    {
        # open output gcode file if we weren't supplied a file-handle
        my ($fh, $tempfile);
        if ($params{output_fh}) {
            $fh = $params{output_fh};
        } else {
            $tempfile = "$output_file.tmp";
            Slic3r::open(\$fh, ">", $tempfile)
                or die "Failed to open $tempfile for writing\n";
    
            # enable UTF-8 output since user might have entered Unicode characters in fields like notes
            binmode $fh, ':utf8';
        }

        Slic3r::Print::GCode->new(
            print   => $self,
            fh      => $fh,
        )->export;

        # close our gcode file
        close $fh;
        if ($tempfile) {
            my $renamed = 0;
            for my $i (1..5) {
                last if $renamed = rename Slic3r::encode_path($tempfile), Slic3r::encode_path($output_file);
                # Wait for 1/4 seconds and try to rename once again.
                select(undef, undef, undef, 0.25);
            }
            Slic3r::debugf "Failed to remove the output G-code file from $tempfile to $output_file. Is $tempfile locked?\n"
                if !$renamed;
        }
    }
    
    # run post-processing scripts
    if (@{$self->config->post_process}) {
        $self->status_cb->(95, "Running post-processing scripts");
        $self->config->setenv;
        for my $script (@{$self->config->post_process}) {
            Slic3r::debugf "  '%s' '%s'\n", $script, $output_file;
            my @parsed_script = escaped_split $script;
            my $executable = shift @parsed_script ;
            push @parsed_script, $output_file;
            # -x doesn't return true on Windows except for .exe files
            if (($^O eq 'MSWin32') ? !(-e $executable) : !(-x $executable)) {
                die "The configured post-processing script is not executable: check permissions or escape whitespace/exclamation points. ($executable) \n";
            }
            system($executable, @parsed_script);
        }
    }
}

# Export SVG slices for the offline SLA printing.
sub export_svg {
    my $self = shift;
    my %params = @_;
    
    $_->slice for @{$self->objects};
    
    my $fh = $params{output_fh};
    if (!$fh) {
        my $output_file = $self->output_filepath($params{output_file});
        $output_file =~ s/\.gcode$/.svg/i;
        Slic3r::open(\$fh, ">", $output_file) or die "Failed to open $output_file for writing\n";
        print "Exporting to $output_file..." unless $params{quiet};
    }
    
    my $print_bb = $self->bounding_box;
    my $print_size = $print_bb->size;
    print $fh sprintf <<"EOF", unscale($print_size->[X]), unscale($print_size->[Y]);
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.0//EN" "http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd">
<svg width="%s" height="%s" xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:slic3r="http://slic3r.org/namespaces/slic3r">
  <!-- 
  Generated using Slic3r $Slic3r::VERSION
  http://slic3r.org/
   -->
EOF
    
    my $print_polygon = sub {
        my ($polygon, $type) = @_;
        printf $fh qq{    <polygon slic3r:type="%s" points="%s" style="fill: %s" />\n},
            $type, (join ' ', map { join ',', map unscale $_, @$_ } @$polygon),
            ($type eq 'contour' ? 'white' : 'black');
    };
    
    my @layers = sort { $a->print_z <=> $b->print_z }
        map { @{$_->layers}, @{$_->support_layers} }
        @{$self->objects};
    
    my $layer_id = -1;
    my @previous_layer_slices = ();
    for my $layer (@layers) {
        $layer_id++;
        if ($layer->slice_z == -1) {
            printf $fh qq{  <g id="layer%d">\n}, $layer_id;
        } else {
            printf $fh qq{  <g id="layer%d" slic3r:z="%s">\n}, $layer_id, unscale($layer->slice_z);
        }
        
        my @current_layer_slices = ();
        # sort slices so that the outermost ones come first
        my @slices = sort { $a->contour->contains_point($b->contour->first_point) ? 0 : 1 } @{$layer->slices};
        foreach my $copy (@{$layer->object->_shifted_copies}) {
            foreach my $slice (@slices) {
                my $expolygon = $slice->clone;
                $expolygon->translate(@$copy);
                $expolygon->translate(-$print_bb->x_min, -$print_bb->y_min);
                $print_polygon->($expolygon->contour, 'contour');
                $print_polygon->($_, 'hole') for @{$expolygon->holes};
                push @current_layer_slices, $expolygon;
            }
        }
        # generate support material
        if ($self->has_support_material && $layer->id > 0) {
            my (@supported_slices, @unsupported_slices) = ();
            foreach my $expolygon (@current_layer_slices) {
                my $intersection = intersection_ex(
                    [ map @$_, @previous_layer_slices ],
                    [ @$expolygon ],
                );
                @$intersection
                    ? push @supported_slices, $expolygon
                    : push @unsupported_slices, $expolygon;
            }
            my @supported_points = map @$_, @$_, @supported_slices;
            foreach my $expolygon (@unsupported_slices) {
                # look for the nearest point to this island among all
                # supported points
                my $contour = $expolygon->contour;
                my $support_point = $contour->first_point->nearest_point(\@supported_points)
                    or next;
                my $anchor_point = $support_point->nearest_point([ @$contour ]);
                printf $fh qq{    <line x1="%s" y1="%s" x2="%s" y2="%s" style="stroke-width: 2; stroke: white" />\n},
                    map @$_, $support_point, $anchor_point;
            }
        }
        print $fh qq{  </g>\n};
        @previous_layer_slices = @current_layer_slices;
    }
    
    print $fh "</svg>\n";
    close $fh;
    print "Done.\n" unless $params{quiet};
}

sub make_brim {
    my $self = shift;
    
    # prerequisites
    $_->make_perimeters for @{$self->objects};
    $_->infill for @{$self->objects};
    $_->generate_support_material for @{$self->objects};
    $self->make_skirt;
    
    $self->status_cb->(88, "Generating brim");
    for (my $i = 0; $i < $self->config->brim_layers; $i++) {
        $self->_make_brim($i);
    }
}

# Wrapper around the C++ Slic3r::Print::validate()
# to produce a Perl exception without a hang-up on some Strawberry perls.
sub validate
{
    my $self = shift;
    my $err = $self->_validate;
    die $err . "\n" if (defined($err) && $err ne '');
}

1;
