package Slic3r::GUI::Plater::ObjectCutDialog;
use strict;
use warnings;
use utf8;

use Slic3r::Geometry qw(PI X);
use Wx qw(wxTheApp :dialog :id :misc :sizer wxTAB_TRAVERSAL);
use Wx::Event qw(EVT_CLOSE EVT_BUTTON);
use base 'Wx::Dialog';

sub new {
    my $class = shift;
    my ($parent, %params) = @_;
    my $self = $class->SUPER::new($parent, -1, $params{object}->name, wxDefaultPosition, [500,500], wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    $self->{model_object_idx} = $params{model_object_idx};
    $self->{model_object} = $params{model_object};
    $self->{new_model_objects} = [];
    
    # cut options
    $self->{cut_options} = {
        z               => 0,
        keep_upper      => 1,
        keep_lower      => 1,
        rotate_lower    => 1,
        preview         => 1,
    };
    
    my $optgroup;
    $optgroup = $self->{optgroup} = Slic3r::GUI::OptionsGroup->new(
        parent      => $self,
        title       => 'Cut',
        on_change   => sub {
            my ($opt_id) = @_;
            
            $self->{cut_options}{$opt_id} = $optgroup->get_value($opt_id);
            wxTheApp->CallAfter(sub {
                $self->_update;
            });
        },
        label_width  => 120,
    );
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'z',
        type        => 'slider',
        label       => 'Z',
        default     => $self->{cut_options}{z},
        min         => 0,
        max         => $self->{model_object}->bounding_box->size->z,
        full_width  => 1,
    ));
    {
        my $line = Slic3r::GUI::OptionsGroup::Line->new(
            label => 'Keep',
        );
        $line->append_option(Slic3r::GUI::OptionsGroup::Option->new(
            opt_id  => 'keep_upper',
            type    => 'bool',
            label   => 'Upper part',
            default => $self->{cut_options}{keep_upper},
        ));
        $line->append_option(Slic3r::GUI::OptionsGroup::Option->new(
            opt_id  => 'keep_lower',
            type    => 'bool',
            label   => 'Lower part',
            default => $self->{cut_options}{keep_lower},
        ));
        $optgroup->append_line($line);
    }
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'rotate_lower',
        label       => 'Rotate lower part upwards',
        type        => 'bool',
        tooltip     => 'If enabled, the lower part will be rotated by 180° so that the flat cut surface lies on the print bed.',
        default     => $self->{cut_options}{rotate_lower},
    ));
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'preview',
        label       => 'Show preview',
        type        => 'bool',
        tooltip     => 'If enabled, object will be cut in real time.',
        default     => $self->{cut_options}{preview},
    ));
    {
        my $cut_button_sizer = Wx::BoxSizer->new(wxVERTICAL);
        $self->{btn_cut} = Wx::Button->new($self, -1, "Perform cut", wxDefaultPosition, wxDefaultSize);
        $cut_button_sizer->Add($self->{btn_cut}, 0, wxALIGN_RIGHT | wxALL, 10);
        $optgroup->append_line(Slic3r::GUI::OptionsGroup::Line->new(
            sizer => $cut_button_sizer,
        ));
    }
    
    # left pane with tree
    my $left_sizer = Wx::BoxSizer->new(wxVERTICAL);
    $left_sizer->Add($optgroup->sizer, 0, wxEXPAND | wxBOTTOM | wxLEFT | wxRIGHT, 10);
    
    # right pane with preview canvas
    my $canvas;
    if ($Slic3r::GUI::have_OpenGL) {
        $canvas = $self->{canvas} = Slic3r::GUI::3DScene->new($self);
        $canvas->load_object($self->{model_object}, undef, [0]);
        $canvas->set_auto_bed_shape;
        $canvas->SetSize([500,500]);
        $canvas->SetMinSize($canvas->GetSize);
        $canvas->zoom_to_volumes;
    }
    
    $self->{sizer} = Wx::BoxSizer->new(wxHORIZONTAL);
    $self->{sizer}->Add($left_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 10);
    $self->{sizer}->Add($canvas, 1, wxEXPAND | wxALL, 0) if $canvas;
    
    $self->SetSizer($self->{sizer});
    $self->SetMinSize($self->GetSize);
    $self->{sizer}->SetSizeHints($self);
    
    EVT_BUTTON($self, $self->{btn_cut}, sub {
        if ($self->{new_model_objects}{lower}) {
            if ($self->{cut_options}{rotate_lower}) {
                $self->{new_model_objects}{lower}->rotate(PI, X);
                $self->{new_model_objects}{lower}->center_around_origin;  # align to Z = 0
            }
        }
        if ($self->{new_model_objects}{upper}) {
            $self->{new_model_objects}{upper}->center_around_origin;  # align to Z = 0
        }
        
        $self->EndModal(wxID_OK);
        $self->Close;
    });
    
    $self->_update;
    
    return $self;
}

sub _update {
    my ($self) = @_;
    
    {
        # scale Z down to original size since we're using the transformed mesh for 3D preview
        # and cut dialog but ModelObject::cut() needs Z without any instance transformation
        my $z = $self->{cut_options}{z} / $self->{model_object}->instances->[0]->scaling_factor;
        
        {
            my ($new_model) = $self->{model_object}->cut($z);
            my ($upper_object, $lower_object) = @{$new_model->objects};
            $self->{new_model} = $new_model;
            $self->{new_model_objects} = {};
            if ($self->{cut_options}{keep_upper} && $upper_object->volumes_count > 0) {
                $self->{new_model_objects}{upper} = $upper_object;
            }
            if ($self->{cut_options}{keep_lower} && $lower_object->volumes_count > 0) {
                $self->{new_model_objects}{lower} = $lower_object;
            }
        }
        
        # update canvas
        if ($self->{canvas}) {
            # get volumes to render
            my @objects = ();
            if ($self->{cut_options}{preview}) {
                push @objects, values %{$self->{new_model_objects}};
            } else {
                push @objects, $self->{model_object};
            }
        
            # get section contour
            my @expolygons = ();
            foreach my $volume (@{$self->{model_object}->volumes}) {
                next if !$volume->mesh;
                next if $volume->modifier;
                my $expp = $volume->mesh->slice([ $z + $volume->mesh->bounding_box->z_min ])->[0];
                push @expolygons, @$expp;
            }
            foreach my $expolygon (@expolygons) {
                $self->{model_object}->instances->[0]->transform_polygon($_)
                    for @$expolygon;
                $expolygon->translate(map Slic3r::Geometry::scale($_), @{ $self->{model_object}->instances->[0]->offset });
            }
            
            $self->{canvas}->reset_objects;
            $self->{canvas}->load_object($_, undef, [0]) for @objects;
            $self->{canvas}->SetCuttingPlane(
                $self->{cut_options}{z},
                [@expolygons],
            );
            $self->{canvas}->Render;
        }
    }
    
    # update controls
    {
        my $z = $self->{cut_options}{z};
        my $optgroup = $self->{optgroup};
        $optgroup->get_field('keep_upper')->toggle(my $have_upper = abs($z - $optgroup->get_option('z')->max) > 0.1);
        $optgroup->get_field('keep_lower')->toggle(my $have_lower = $z > 0.1);
        $optgroup->get_field('rotate_lower')->toggle($z > 0 && $self->{cut_options}{keep_lower});
        $optgroup->get_field('preview')->toggle($self->{cut_options}{keep_upper} != $self->{cut_options}{keep_lower});
    
        # update cut button
        if (($self->{cut_options}{keep_upper} && $have_upper)
            || ($self->{cut_options}{keep_lower} && $have_lower)) {
            $self->{btn_cut}->Enable;
        } else {
            $self->{btn_cut}->Disable;
        }
    }
}

sub NewModelObjects {
    my ($self) = @_;
    return values %{ $self->{new_model_objects} };
}

1;
