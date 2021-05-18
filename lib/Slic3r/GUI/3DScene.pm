package Slic3r::GUI::3DScene::Base;
use strict;
use warnings;
use Wx::Event qw(EVT_PAINT EVT_SIZE EVT_ERASE_BACKGROUND EVT_IDLE EVT_MOUSEWHEEL EVT_MOUSE_EVENTS);
# must load OpenGL *before* Wx::GLCanvas

use OpenGL qw(:glconstants :glfunctions :glufunctions :gluconstants);
use base qw(Wx::GLCanvas Class::Accessor);
use Math::Trig qw(asin);
use List::Util qw(reduce min max first);
use Slic3r::Geometry qw(X Y Z MIN MAX triangle_normal normalize deg2rad tan scale unscale scaled_epsilon);
use Slic3r::Geometry::Clipper qw(offset_ex intersection_pl);
use Wx::GLCanvas qw(:all);

__PACKAGE__->mk_accessors( qw(_quat _dirty init
                              enable_picking
                              enable_face_select
                              enable_moving
                              on_viewport_changed
                              on_hover
                              on_select
                              on_double_click
                              on_right_click
                              on_move
                              volumes
                              _sphi _stheta
                              cutting_plane_axis
                              cutting_plane_z
                              cut_lines_vertices
                              bed_shape
                              bed_triangles
                              bed_grid_lines
                              background
                              origin
                              _mouse_pos
                              _hover_volume_idx
                              _drag_volume_idx
                              _drag_start_pos
                              _drag_start_xy
                              _dragged
                              _camera_target
                              _zoom
                              ) );

use constant TRACKBALLSIZE  => 0.8;
use constant TURNTABLE_MODE => 1;
use constant GROUND_Z       => -0.02;
use constant PI             => 3.1415927;

# Constant to determine if Vertex Buffer objects are used to draw
# bed grid and the cut plane for object separation.
use constant HAS_VBO        => eval { glGenBuffersARB_p(0); GL_ARRAY_BUFFER_ARB; 1 };

# phi / theta angles to orient the camera.
use constant VIEW_TOP        => [0.0,0.0];
use constant VIEW_BOTTOM     => [0.0,180.0];
use constant VIEW_LEFT       => [90.0,90.0];
use constant VIEW_RIGHT      => [-90.0,90.0];
use constant VIEW_FRONT      => [0.0,90.0];
use constant VIEW_BACK       => [180.0,90.0];
use constant VIEW_DIAGONAL   => [45.0,45.0];

# Color Scheme
use Slic3r::GUI::ColorScheme;

use constant GIMBAL_LOCK_THETA_MAX => 170;

# make OpenGL::Array thread-safe
{
    no warnings 'redefine';
    *OpenGL::Array::CLONE_SKIP = sub { 1 };
}

sub new {
    my ($class, $parent) = @_;

    if ( ( defined $Slic3r::GUI::Settings->{_}{colorscheme} ) && ( Slic3r::GUI::ColorScheme->can($Slic3r::GUI::Settings->{_}{colorscheme}) ) ) {
        my $myGetSchemeName = \&{"Slic3r::GUI::ColorScheme::$Slic3r::GUI::Settings->{_}{colorscheme}"};
        $myGetSchemeName->();
    } else {
        Slic3r::GUI::ColorScheme->getDefault();
    }
    
    # We can only enable multi sample anti aliasing with wxWidgets 3.0.3 and with a hacked Wx::GLCanvas,
    # which exports some new WX_GL_XXX constants, namely WX_GL_SAMPLE_BUFFERS and WX_GL_SAMPLES.
    my $can_multisample =
        Wx::wxVERSION >= 3.000003 &&
        defined Wx::GLCanvas->can('WX_GL_SAMPLE_BUFFERS') &&
        defined Wx::GLCanvas->can('WX_GL_SAMPLES');
    my $attrib = [WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_DEPTH_SIZE, 24];
    if ($can_multisample) {
        # Request a window with multi sampled anti aliasing. This is a new feature in Wx 3.0.3 (backported from 3.1.0).
        # Use eval to avoid compilation, if the subs WX_GL_SAMPLE_BUFFERS and WX_GL_SAMPLES are missing.
        eval 'push(@$attrib, (WX_GL_SAMPLE_BUFFERS, 1, WX_GL_SAMPLES, 4));';
    }
    # wxWidgets expect the attrib list to be ended by zero.
    push(@$attrib, 0);

    # we request a depth buffer explicitly because it looks like it's not created by 
    # default on Linux, causing transparency issues
    my $self = $class->SUPER::new($parent, -1, Wx::wxDefaultPosition, Wx::wxDefaultSize, 0, "", $attrib);

    if (Wx::wxVERSION >= 3.000003) {
        # Wx 3.0.3 contains an ugly hack to support some advanced OpenGL attributes through the attribute list.
        # The attribute list is transferred between the wxGLCanvas and wxGLContext constructors using a single static array s_wglContextAttribs.
        # Immediately force creation of the OpenGL context to consume the static variable s_wglContextAttribs.
        $self->GetContext();
    }

    $self->{can_multisample} = $can_multisample;
    $self->background(1);
    $self->_quat((0, 0, 0, 1));
    $self->_stheta(45);
    $self->_sphi(45);
    $self->_zoom(1);
    
    # 3D point in model space
    $self->_camera_target(Slic3r::Pointf3->new(0,0,0));
    
    $self->reset_objects;
    
    EVT_PAINT($self, sub {
        my $dc = Wx::PaintDC->new($self);
        $self->Render($dc);
    });
    EVT_SIZE($self, sub { $self->_dirty(1) });
    EVT_IDLE($self, sub {
        return unless $self->_dirty;
        return if !$self->IsShownOnScreen;
        $self->Resize( $self->GetSizeWH );
        $self->Refresh;
    });
    EVT_MOUSEWHEEL($self, sub {
        my ($self, $e) = @_;
        
        # Calculate the zoom delta and apply it to the current zoom factor
        my $zoom = $e->GetWheelRotation() / $e->GetWheelDelta();
        if ($Slic3r::GUI::Settings->{_}{invert_zoom}) {
            $zoom *= -1;
        }
        $zoom = max(min($zoom, 4), -4);
        $zoom /= 10;
        $self->_zoom($self->_zoom / (1-$zoom));
        
        # In order to zoom around the mouse point we need to translate
        # the camera target
        my $size = Slic3r::Pointf->new($self->GetSizeWH);
        my $pos = Slic3r::Pointf->new($e->GetX, $size->y - $e->GetY); #-
        $self->_camera_target->translate(
            # ($pos - $size/2) represents the vector from the viewport center
            # to the mouse point. By multiplying it by $zoom we get the new,
            # transformed, length of such vector.
            # Since we want that point to stay fixed, we move our camera target
            # in the opposite direction by the delta of the length of such vector
            # ($zoom - 1). We then scale everything by 1/$self->_zoom since 
            # $self->_camera_target is expressed in terms of model units.
            -($pos->x - $size->x/2) * ($zoom) / $self->_zoom,
            -($pos->y - $size->y/2) * ($zoom) / $self->_zoom,
            0,
        ) if 0;
        $self->on_viewport_changed->() if $self->on_viewport_changed;
        $self->_dirty(1);
        $self->Refresh;
    });
    EVT_MOUSE_EVENTS($self, \&mouse_event);

    
    return $self;
}

sub mouse_event {
    my ($self, $e) = @_;
    
    my $pos = Slic3r::Pointf->new($e->GetPositionXY);
    if ($e->Entering && &Wx::wxMSW) {
        # wxMSW needs focus in order to catch mouse wheel events
        $self->SetFocus;
    } elsif ($e->LeftDClick) {
        $self->on_double_click->()
            if $self->on_double_click;
    } elsif ($e->MiddleDClick) {
        if (@{$self->volumes}) {
            $self->zoom_to_volumes;
        } else {
            $self->zoom_to_bed;
        }
        $self->_dirty(1);
        $self->Refresh;
    } elsif (($e->LeftDown || $e->RightDown) && not $e->ShiftDown) {
        # If user pressed left or right button we first check whether this happened
        # on a volume or not.
        my $volume_idx = $self->_hover_volume_idx // -1;
        
        # select volume in this 3D canvas
        if ($self->enable_picking) {
            $self->deselect_volumes;
            $self->select_volume($volume_idx);
            
            if ($volume_idx != -1) {
                my $group_id = $self->volumes->[$volume_idx]->select_group_id;
                my @volumes;
                if ($group_id != -1) {
                    $self->select_volume($_)
                        for grep $self->volumes->[$_]->select_group_id == $group_id,
                        0..$#{$self->volumes};
                }
            }
            
            $self->Refresh;
        }
        
        # propagate event through callback
        $self->on_select->($volume_idx)
            if $self->on_select;
        
        if ($volume_idx != -1) {
            if ($e->LeftDown && $self->enable_moving) {
                $self->_drag_volume_idx($volume_idx);
                $self->_drag_start_pos($self->mouse_to_3d(@$pos));
            } elsif ($e->RightDown) {
                # if right clicking on volume, propagate event through callback
                $self->on_right_click->($e->GetPosition)
                    if $self->on_right_click;
            }
        }
    } elsif ($e->Dragging && $e->LeftIsDown && defined($self->_drag_volume_idx)) {
        # get new position at the same Z of the initial click point
        my $mouse_ray = $self->mouse_ray($e->GetX, $e->GetY);
        my $cur_pos = $mouse_ray->intersect_plane($self->_drag_start_pos->z);
        
        # calculate the translation vector
        my $vector = $self->_drag_start_pos->vector_to($cur_pos);
        
        # get volume being dragged
        my $volume = $self->volumes->[$self->_drag_volume_idx];
        
        # get all volumes belonging to the same group, if any
        my @volumes;
        if ($volume->drag_group_id == -1) {
            @volumes = ($volume);
        } else {
            @volumes = grep $_->drag_group_id == $volume->drag_group_id, @{$self->volumes};
        }
        
        # apply new temporary volume origin and ignore Z
        $_->origin->translate($vector->x, $vector->y, 0) for @volumes; #,,
        $self->_drag_start_pos($cur_pos);
        $self->_dragged(1);
        $self->Refresh;
    } elsif ($e->Dragging) {
        if ($e->AltDown) {
            # Move the camera center on the Z axis based on mouse Y axis movement
            if (defined $self->_drag_start_pos) {
                my $orig = $self->_drag_start_pos;
                $self->_camera_target->translate(0, 0, $pos->y - $orig->y);
                $self->on_viewport_changed->() if $self->on_viewport_changed;
                $self->Refresh;
            }
            $self->_drag_start_pos($pos);
        } elsif ($e->LeftIsDown) {
            # if dragging over blank area with left button, rotate
            if (defined $self->_drag_start_pos) {
                my $orig = $self->_drag_start_pos;
                if (TURNTABLE_MODE) {
                    $self->_sphi($self->_sphi + ($pos->x - $orig->x) * TRACKBALLSIZE);
                    $self->_stheta($self->_stheta - ($pos->y - $orig->y) * TRACKBALLSIZE);        #-
                    $self->_stheta(GIMBAL_LOCK_THETA_MAX) if $self->_stheta > GIMBAL_LOCK_THETA_MAX;
                    $self->_stheta(0) if $self->_stheta < 0;
                } else {
                    my $size = $self->GetClientSize;
                    my @quat = trackball(
                        $orig->x / ($size->width / 2) - 1,
                        1 - $orig->y / ($size->height / 2),       #/
                        $pos->x / ($size->width / 2) - 1,
                        1 - $pos->y / ($size->height / 2),        #/
                    );
                    $self->_quat(mulquats($self->_quat, \@quat));
                }
                $self->on_viewport_changed->() if $self->on_viewport_changed;
                $self->Refresh;
            }
            $self->_drag_start_pos($pos);
        } elsif ($e->MiddleIsDown || $e->RightIsDown) {
            # if dragging over blank area with right button, translate
            
            if (defined $self->_drag_start_xy) {
                # get point in model space at Z = 0
                my $cur_pos = $self->mouse_ray($e->GetX, $e->GetY)->intersect_plane(0);
                my $orig    = $self->mouse_ray(@{$self->_drag_start_xy})->intersect_plane(0);
                $self->_camera_target->translate(
                    @{$orig->vector_to($cur_pos)->negative},
                );
                $self->on_viewport_changed->() if $self->on_viewport_changed;
                $self->Refresh;
            }
            $self->_drag_start_xy($pos);
        }
    } elsif ($e->LeftUp || $e->MiddleUp || $e->RightUp) {
        if ($self->on_move && defined($self->_drag_volume_idx) && $self->_dragged) {
            # get all volumes belonging to the same group, if any
            my @volume_idxs;
            my $group_id = $self->volumes->[$self->_drag_volume_idx]->drag_group_id;
            if ($group_id == -1) {
                @volume_idxs = ($self->_drag_volume_idx);
            } else {
                @volume_idxs = grep $self->volumes->[$_]->drag_group_id == $group_id,
                    0..$#{$self->volumes};
            }
            $self->on_move->(@volume_idxs);
        }
        $self->_drag_volume_idx(undef);
        $self->_drag_start_pos(undef);
        $self->_drag_start_xy(undef);
        $self->_dragged(undef);
    } elsif ($e->Moving) {
        $self->_mouse_pos($pos);
        $self->Refresh if $self->enable_picking;
    } else {
        $e->Skip();
    }
}

sub reset_objects {
    my ($self) = @_;
    
    $self->volumes([]);
    $self->_dirty(1);
}

sub set_viewport_from_scene {
    my ($self, $scene) = @_;
    
    $self->_sphi($scene->_sphi);
    $self->_stheta($scene->_stheta);
    $self->_camera_target($scene->_camera_target);
    $self->_zoom($scene->_zoom);
    $self->_quat($scene->_quat);
    $self->_dirty(1);
}

sub zoom{
    my ($self, $direction) = @_;
    if( $direction eq 'in'){
        $self->_zoom($self->_zoom / (1-0.3));
    }
    elsif($direction eq 'out'){
        $self->_zoom($self->_zoom / (1+0.3));
    }
    $self->on_viewport_changed->() if $self->on_viewport_changed;
    $self->_dirty(1);
    $self->Refresh;
}

# Set the camera to a default orientation,
# zoom to volumes.
sub select_view {
    my ($self, $direction) = @_;
    my $dirvec;
    if (ref($direction)) {
        $dirvec = $direction;
    } else {
        if ($direction eq 'top') {
            $dirvec = VIEW_TOP;
        } elsif ($direction eq 'bottom') {
            $dirvec = VIEW_BOTTOM;
        } elsif ($direction eq 'left') {
            $dirvec = VIEW_LEFT;
        } elsif ($direction eq 'right') {
            $dirvec = VIEW_RIGHT;
        } elsif ($direction eq 'front') {
            $dirvec = VIEW_FRONT;
        } elsif ($direction eq 'back') {
            $dirvec = VIEW_BACK;
        } elsif ($direction eq 'diagonal') {
            $dirvec = VIEW_DIAGONAL;
        }
    }
    $self->_sphi($dirvec->[0]);
    $self->_stheta($dirvec->[1]);
    
    # Avoid gimbal lock.
    $self->_stheta(GIMBAL_LOCK_THETA_MAX) if $self->_stheta > GIMBAL_LOCK_THETA_MAX;
    $self->_stheta(0) if $self->_stheta < 0;
    
    # View everything.
    $self->volumes_bounding_box->defined
        ? $self->zoom_to_volumes
        : $self->zoom_to_bed;
    
    $self->on_viewport_changed->() if $self->on_viewport_changed;
    $self->_dirty(1);
    $self->Refresh;
}

sub zoom_to_bounding_box {
    my ($self, $bb) = @_;
    
    # calculate the zoom factor needed to adjust viewport to
    # bounding box
    my $max_size = max(@{$bb->size}) * 1.05;
    my $min_viewport_size = min($self->GetSizeWH);
    if ($max_size != 0) {
        # only re-zoom if we have a valid bounding box, avoid a divide by 0 error.
        $self->_zoom($min_viewport_size / $max_size);
    
        # center view around bounding box center
        $self->_camera_target($bb->center);
    
        $self->on_viewport_changed->() if $self->on_viewport_changed;
    }
}

sub zoom_to_bed {
    my ($self) = @_;
    
    if ($self->bed_shape) {
        $self->zoom_to_bounding_box($self->bed_bounding_box);
    }
}

sub zoom_to_volume {
    my ($self, $volume_idx) = @_;
    
    my $volume = $self->volumes->[$volume_idx];
    my $bb = $volume->transformed_bounding_box;
    $self->zoom_to_bounding_box($bb);
}

sub zoom_to_volumes {
    my ($self) = @_;
    $self->zoom_to_bounding_box($self->volumes_bounding_box);
}

sub volumes_bounding_box {
    my ($self) = @_;
    
    my $bb = Slic3r::Geometry::BoundingBoxf3->new;
    $bb->merge($_->transformed_bounding_box) for @{$self->volumes};
    return $bb;
}

sub bed_bounding_box {
    my ($self) = @_;
    
    my $bb = Slic3r::Geometry::BoundingBoxf3->new;
    if ($self->bed_shape) {
        $bb->merge_point(Slic3r::Pointf3->new(@$_, 0)) for @{$self->bed_shape};
    }
    return $bb;
}

sub max_bounding_box {
    my ($self) = @_;
    
    my $bb = $self->bed_bounding_box;
    $bb->merge($self->volumes_bounding_box);
    return $bb;
}

sub set_auto_bed_shape {
    my ($self, $bed_shape) = @_;
    
    # draw a default square bed around object center
    my $max_size = max(@{ $self->volumes_bounding_box->size });
    my $center = $self->volumes_bounding_box->center;
    $self->set_bed_shape([
        [ $center->x - $max_size, $center->y - $max_size ],  #--
        [ $center->x + $max_size, $center->y - $max_size ],  #--
        [ $center->x + $max_size, $center->y + $max_size ],  #++
        [ $center->x - $max_size, $center->y + $max_size ],  #++
    ]);
    $self->origin(Slic3r::Pointf->new(@$center[X,Y]));
}

sub set_bed_shape {
    my ($self, $bed_shape) = @_;
    
    $self->bed_shape($bed_shape);
    
    # triangulate bed
    my $expolygon = Slic3r::ExPolygon->new([ map [map scale($_), @$_], @$bed_shape ]);
    my $bed_bb = $expolygon->bounding_box;
    
    {
        my @points = ();
        foreach my $triangle (@{ $expolygon->triangulate }) {
            push @points, map {+ unscale($_->x), unscale($_->y), GROUND_Z } @$triangle;  #))
        }
        $self->bed_triangles(OpenGL::Array->new_list(GL_FLOAT, @points));
    }
    
    {
        my @polylines = ();
        for (my $x = $bed_bb->x_min; $x <= $bed_bb->x_max; $x += scale 10) {
            push @polylines, Slic3r::Polyline->new([$x,$bed_bb->y_min], [$x,$bed_bb->y_max]);
        }
        for (my $y = $bed_bb->y_min; $y <= $bed_bb->y_max; $y += scale 10) {
            push @polylines, Slic3r::Polyline->new([$bed_bb->x_min,$y], [$bed_bb->x_max,$y]);
        }
        # clip with a slightly grown expolygon because our lines lay on the contours and
        # may get erroneously clipped
        my @lines = map Slic3r::Line->new(@$_[0,-1]),
            @{intersection_pl(\@polylines, [ @{$expolygon->offset(+scaled_epsilon)} ])};
        
        # append bed contours
        push @lines, map @{$_->lines}, @$expolygon;
        
        my @points = ();
        foreach my $line (@lines) {
            push @points, map {+ unscale($_->x), unscale($_->y), GROUND_Z } @$line;  #))
        }
        $self->bed_grid_lines(OpenGL::Array->new_list(GL_FLOAT, @points));
    }
    
    $self->origin(Slic3r::Pointf->new(0,0));
}

sub deselect_volumes {
    my ($self) = @_;
    $_->selected(0) for @{$self->volumes};
    $_->selected_face(-1) for @{$self->volumes};
}

sub select_volume {
    my ($self, $volume_idx) = @_;
    
    if ($volume_idx != -1) {
        $self->volumes->[$volume_idx]->selected(1);
        $self->volumes->[$volume_idx]->selected_face($self->volumes->[$volume_idx]->hover_face);
    }
}

sub SetCuttingPlane {
    my ($self, $axis, $z, $expolygons) = @_;
    
    $self->cutting_plane_axis($axis);
    $self->cutting_plane_z($z);
    
    # grow slices in order to display them better
    $expolygons = offset_ex([ map @$_, @$expolygons ], scale 0.1);
    
    my $bb = $self->volumes_bounding_box;
    
    my @verts = ();
    foreach my $line (map @{$_->lines}, map @$_, @$expolygons) {
        if ($axis == X) {
            push @verts, (
                $bb->x_min + $z, unscale($line->a->x), unscale($line->a->y),  #))
                $bb->x_min + $z, unscale($line->b->x), unscale($line->b->y),  #))
            );
        } elsif ($axis == Y) {
            push @verts, (
                unscale($line->a->y), $bb->y_min + $z, unscale($line->a->x),  #))
                unscale($line->b->y), $bb->y_min + $z, unscale($line->b->x),  #))
            );
        } else {
            push @verts, (
                unscale($line->a->x), unscale($line->a->y), $z,  #))
                unscale($line->b->x), unscale($line->b->y), $z,  #))
            );
        }
        
    }
    $self->cut_lines_vertices(OpenGL::Array->new_list(GL_FLOAT, @verts));
}

# Given an axis and angle, compute quaternion.
sub axis_to_quat {
    my ($ax, $phi) = @_;
    
    my $lena = sqrt(reduce { $a + $b } (map { $_ * $_ } @$ax));
    my @q = map { $_ * (1 / $lena) } @$ax;
    @q = map { $_ * sin($phi / 2.0) } @q;
    $q[$#q + 1] = cos($phi / 2.0);
    return @q;
}

# Project a point on the virtual trackball. 
# If it is inside the sphere, map it to the sphere, if it outside map it
# to a hyperbola.
sub project_to_sphere {
    my ($r, $x, $y) = @_;
    
    my $d = sqrt($x * $x + $y * $y);
    if ($d < $r * 0.70710678118654752440) {     # Inside sphere
        return sqrt($r * $r - $d * $d);
    } else {                                    # On hyperbola
        my $t = $r / 1.41421356237309504880;
        return $t * $t / $d;
    }
}

sub cross {
    my ($v1, $v2) = @_;
  
    return (@$v1[1] * @$v2[2] - @$v1[2] * @$v2[1],
            @$v1[2] * @$v2[0] - @$v1[0] * @$v2[2],
            @$v1[0] * @$v2[1] - @$v1[1] * @$v2[0]);
}

# Simulate a track-ball. Project the points onto the virtual trackball, 
# then figure out the axis of rotation, which is the cross product of 
# P1 P2 and O P1 (O is the center of the ball, 0,0,0) Note: This is a 
# deformed trackball-- is a trackball in the center, but is deformed 
# into a hyperbolic sheet of rotation away from the center. 
# It is assumed that the arguments to this routine are in the range 
# (-1.0 ... 1.0).
sub trackball {
    my ($p1x, $p1y, $p2x, $p2y) = @_;
    
    if ($p1x == $p2x && $p1y == $p2y) {
        # zero rotation
        return (0.0, 0.0, 0.0, 1.0);
    }
    
    # First, figure out z-coordinates for projection of P1 and P2 to
    # deformed sphere
    my @p1 = ($p1x, $p1y, project_to_sphere(TRACKBALLSIZE, $p1x, $p1y));
    my @p2 = ($p2x, $p2y, project_to_sphere(TRACKBALLSIZE, $p2x, $p2y));
    
    # axis of rotation (cross product of P1 and P2)
    my @a = cross(\@p2, \@p1);

    # Figure out how much to rotate around that axis.
    my @d = map { $_ * $_ } (map { $p1[$_] - $p2[$_] } 0 .. $#p1);
    my $t = sqrt(reduce { $a + $b } @d) / (2.0 * TRACKBALLSIZE);
    
    # Avoid problems with out-of-control values...
    $t = 1.0 if ($t > 1.0);
    $t = -1.0 if ($t < -1.0);
    my $phi = 2.0 * asin($t);

    return axis_to_quat(\@a, $phi);
}

# Build a rotation matrix, given a quaternion rotation.
sub quat_to_rotmatrix {
    my ($q) = @_;
  
    my @m = ();
  
    $m[0] = 1.0 - 2.0 * (@$q[1] * @$q[1] + @$q[2] * @$q[2]);
    $m[1] = 2.0 * (@$q[0] * @$q[1] - @$q[2] * @$q[3]);
    $m[2] = 2.0 * (@$q[2] * @$q[0] + @$q[1] * @$q[3]);
    $m[3] = 0.0;

    $m[4] = 2.0 * (@$q[0] * @$q[1] + @$q[2] * @$q[3]);
    $m[5] = 1.0 - 2.0 * (@$q[2] * @$q[2] + @$q[0] * @$q[0]);
    $m[6] = 2.0 * (@$q[1] * @$q[2] - @$q[0] * @$q[3]);
    $m[7] = 0.0;

    $m[8] = 2.0 * (@$q[2] * @$q[0] - @$q[1] * @$q[3]);
    $m[9] = 2.0 * (@$q[1] * @$q[2] + @$q[0] * @$q[3]);
    $m[10] = 1.0 - 2.0 * (@$q[1] * @$q[1] + @$q[0] * @$q[0]);
    $m[11] = 0.0;

    $m[12] = 0.0;
    $m[13] = 0.0;
    $m[14] = 0.0;
    $m[15] = 1.0;
  
    return @m;
}

sub mulquats {
    my ($q1, $rq) = @_;
  
    return (@$q1[3] * @$rq[0] + @$q1[0] * @$rq[3] + @$q1[1] * @$rq[2] - @$q1[2] * @$rq[1],
            @$q1[3] * @$rq[1] + @$q1[1] * @$rq[3] + @$q1[2] * @$rq[0] - @$q1[0] * @$rq[2],
            @$q1[3] * @$rq[2] + @$q1[2] * @$rq[3] + @$q1[0] * @$rq[1] - @$q1[1] * @$rq[0],
            @$q1[3] * @$rq[3] - @$q1[0] * @$rq[0] - @$q1[1] * @$rq[1] - @$q1[2] * @$rq[2])
}

sub mouse_to_3d {
    my ($self, $x, $y, $z) = @_;

    my @viewport    = glGetIntegerv_p(GL_VIEWPORT);             # 4 items
    my @mview       = glGetDoublev_p(GL_MODELVIEW_MATRIX);      # 16 items
    my @proj        = glGetDoublev_p(GL_PROJECTION_MATRIX);     # 16 items
    
    $y = $viewport[3] - $y;
    $z //= glReadPixels_p($x, $y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT);
    my @projected = gluUnProject_p($x, $y, $z, @mview, @proj, @viewport);
    return Slic3r::Pointf3->new(@projected);
}

sub mouse_ray {
    my ($self, $x, $y) = @_;
    
    return Slic3r::Linef3->new(
        $self->mouse_to_3d($x, $y, 0),
        $self->mouse_to_3d($x, $y, 1),
    );
}

sub GetContext {
    my ($self) = @_;
    
    if (Wx::wxVERSION >= 2.009) {
        return $self->{context} ||= Wx::GLContext->new($self);
    } else {
        return $self->SUPER::GetContext;
    }
}
 
sub SetCurrent {
    my ($self, $context) = @_;
    
    if (Wx::wxVERSION >= 2.009) {
        return $self->SUPER::SetCurrent($context);
    } else {
        return $self->SUPER::SetCurrent;
    }
}

sub Resize {
    my ($self, $x, $y) = @_;
 
    return unless $self->GetContext;
    $self->_dirty(0);
    
    $self->SetCurrent($self->GetContext);
    glViewport(0, 0, $x, $y);
 
    $x /= $self->_zoom;
    $y /= $self->_zoom;
    
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    my $depth = 10 * max(@{ $self->max_bounding_box->size });
    glOrtho(
        -$x/2, $x/2, -$y/2, $y/2,
        -$depth, 2*$depth,
    );
    
    glMatrixMode(GL_MODELVIEW);
}
 
sub InitGL {
    my $self = shift;
 
    return if $self->init;
    return unless $self->GetContext;
    $self->init(1);
    
    glClearColor(0, 0, 0, 1);
    glColor3f(1, 0, 0);
    glEnable(GL_DEPTH_TEST);
    glClearDepth(1.0);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    # Set antialiasing/multisampling
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_POLYGON_SMOOTH);
    glEnable(GL_MULTISAMPLE) if ($self->{can_multisample});
    
    # ambient lighting
    glLightModelfv_p(GL_LIGHT_MODEL_AMBIENT, 0.1, 0.1, 0.1, 1);
    
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    
    # light from camera
    glLightfv_p(GL_LIGHT1, GL_POSITION, 1, 0, 1, 0);
    glLightfv_p(GL_LIGHT1, GL_SPECULAR, 0.8, 0.8, 0.8, 1);
    glLightfv_p(GL_LIGHT1, GL_DIFFUSE,  0.4, 0.4, 0.4, 1);
    
    # Enables Smooth Color Shading; try GL_FLAT for (lack of) fun. Default: GL_SMOOTH
    glShadeModel(GL_SMOOTH);
    
    glMaterialfv_p(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, 0.3, 0.3, 0.3, 1);
    glMaterialfv_p(GL_FRONT_AND_BACK, GL_SPECULAR, 1, 1, 1, 1);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 50);
    glMaterialfv_p(GL_FRONT_AND_BACK, GL_EMISSION, 0.1, 0.1, 0.1, 0.9);
    
    # A handy trick -- have surface material mirror the color.
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_MULTISAMPLE) if ($self->{can_multisample});
}
 
sub Render {
    my ($self, $dc) = @_;
    
    # prevent calling SetCurrent() when window is not shown yet
    return unless $self->IsShownOnScreen;
    return unless my $context = $self->GetContext;
    $self->SetCurrent($context);
    $self->InitGL;
    
    if($SOLID_BACKGROUNDCOLOR == 1){
        glClearColor(@BACKGROUND_COLOR, 0);
    } else {
        glClearColor(1, 1, 1, 1);
    }
    
    glClearDepth(1);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    if (TURNTABLE_MODE) {
        glRotatef(-$self->_stheta, 1, 0, 0); # pitch
        glRotatef($self->_sphi, 0, 0, 1);    # yaw
    } else {
        my @rotmat = quat_to_rotmatrix($self->quat);
        glMultMatrixd_p(@rotmat[0..15]);
    }
    glTranslatef(@{ $self->_camera_target->negative });
    
    # light from above
    glLightfv_p(GL_LIGHT0, GL_POSITION, -0.5, -0.5, 1, 0);
    glLightfv_p(GL_LIGHT0, GL_SPECULAR, 0.2, 0.2, 0.2, 1);
    glLightfv_p(GL_LIGHT0, GL_DIFFUSE,  0.5, 0.5, 0.5, 1);
    
    if ($self->enable_picking) {
        glDisable(GL_MULTISAMPLE) if ($self->{can_multisample});
        glDisable(GL_LIGHTING);
        $self->draw_volumes(1);
        glFlush();
        glFinish();
        
        if (my $pos = $self->_mouse_pos) {
            my $col = [ glReadPixels_p($pos->x, $self->GetSize->GetHeight - $pos->y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE) ];
            my $volume_idx = $col->[0] + $col->[1]*256 + $col->[2]*256*256;
            $self->_hover_volume_idx(undef);
            $_->hover(0) for @{$self->volumes};
            $_->hover_face(-1) for @{$self->volumes};
            if ($volume_idx <= $#{$self->volumes}) {
                $self->_hover_volume_idx($volume_idx);
                
                $self->volumes->[$volume_idx]->hover(1);
                my $group_id = $self->volumes->[$volume_idx]->select_group_id;
                if ($group_id != -1) {
                    $_->hover(1) for grep { $_->select_group_id == $group_id } @{$self->volumes};
                }
                
                $self->on_hover->($volume_idx) if $self->on_hover;

                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glFlush();
                glFinish();
                if($self->enable_face_select){
                    $self->draw_volumes(2, $volume_idx);
                    my $color = [ glReadPixels_p($pos->x, $self->GetSize->GetHeight - $pos->y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE) ];
                    my $face_idx = $color->[0] + $color->[1]*256 + $color->[2]*256*256;
                    $self->volumes->[$volume_idx]->hover_face($face_idx);
                }
            }
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glFlush();
        glFinish();
        glEnable(GL_LIGHTING);
    }
    
    # draw fixed background
    if ($SOLID_BACKGROUNDCOLOR == 0 && $self->background) {
        glDisable(GL_LIGHTING);
        glPushMatrix();
        glLoadIdentity();
        
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        
        glBegin(GL_QUADS);
        glColor3f( @BOTTOM_COLOR ); # bottom color
        glVertex2f(-1.0,-1.0);
        glVertex2f(1,-1.0);
        glColor3f( @TOP_COLOR ); # top color
        glVertex2f(1, 1);
        glVertex2f(-1.0, 1);
        glEnd();
        glPopMatrix();
        
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glEnable(GL_LIGHTING);
    }
    
    # draw ground and axes
    glDisable(GL_LIGHTING);
    
    # draw ground
    my $ground_z = GROUND_Z;
    if ($self->bed_triangles) {
        glDisable(GL_DEPTH_TEST);
        
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        glEnableClientState(GL_VERTEX_ARRAY);
        my $triangle_vertex;
        if (HAS_VBO) {
            ($triangle_vertex) =
                glGenBuffersARB_p(1);
            $self->bed_triangles->bind($triangle_vertex);
            glBufferDataARB_p(GL_ARRAY_BUFFER_ARB, $self->bed_triangles, GL_STATIC_DRAW_ARB);
            glVertexPointer_c(3, GL_FLOAT, 0, 0);
        } else {
            # fall back on old behavior
            glVertexPointer_c(3, GL_FLOAT, 0, $self->bed_triangles->ptr());
        }
        glColor4f( @GROUND_COLOR );
        glNormal3d(0,0,1);
        glDrawArrays(GL_TRIANGLES, 0, $self->bed_triangles->elements / 3);
        glDisableClientState(GL_VERTEX_ARRAY);
        
        # we need depth test for grid, otherwise it would disappear when looking
        # the object from below
        glEnable(GL_DEPTH_TEST);
    
        # draw grid
        glLineWidth(2);
        glEnableClientState(GL_VERTEX_ARRAY);
        my $grid_vertex;
        if (HAS_VBO) {
            ($grid_vertex) =
                glGenBuffersARB_p(1);
            $self->bed_grid_lines->bind($grid_vertex);
            glBufferDataARB_p(GL_ARRAY_BUFFER_ARB, $self->bed_grid_lines, GL_STATIC_DRAW_ARB);
            glVertexPointer_c(3, GL_FLOAT, 0, 0);
        } else {
            # fall back on old behavior
            glVertexPointer_c(3, GL_FLOAT, 0, $self->bed_grid_lines->ptr());
        }
        glColor4f( @GRID_COLOR );
        glNormal3d(0,0,1);
        glDrawArrays(GL_LINES, 0, $self->bed_grid_lines->elements / 3);
        glDisableClientState(GL_VERTEX_ARRAY);
        
        glDisable(GL_BLEND);
        if (HAS_VBO) { 
            # Turn off buffer objects to let the rest of the draw code work.
            glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
            glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
            glDeleteBuffersARB_p($grid_vertex);
            glDeleteBuffersARB_p($triangle_vertex);
        }
    }
    
    my $volumes_bb = $self->volumes_bounding_box;
    
    {
        # draw axes
        # disable depth testing so that axes are not covered by ground
        glDisable(GL_DEPTH_TEST);
        my $origin = $self->origin;
        my $axis_len = max(
            max(@{ $self->bed_bounding_box->size }),
            1.2 * max(@{ $volumes_bb->size }),
        );
        glLineWidth(2);
        glBegin(GL_LINES);
        # draw line for x axis
        glColor3f(1, 0, 0);
        glVertex3f(@$origin, $ground_z);
        glVertex3f($origin->x + $axis_len, $origin->y, $ground_z);  #,,
        # draw line for y axis
        glColor3f(0, 1, 0);
        glVertex3f(@$origin, $ground_z);
        glVertex3f($origin->x, $origin->y + $axis_len, $ground_z);  #++
        glEnd();
        # draw line for Z axis
        # (re-enable depth test so that axis is correctly shown when objects are behind it)
        glEnable(GL_DEPTH_TEST);
        glBegin(GL_LINES);
        glColor3f(0, 0, 1);
        glVertex3f(@$origin, $ground_z);
        glVertex3f(@$origin, $ground_z+$axis_len);
        glEnd();
    }
    
    glEnable(GL_LIGHTING);
    
    # draw objects
    $self->draw_volumes;
    
    # draw cutting plane
    if (defined $self->cutting_plane_z) {
        my $plane_z = $self->cutting_plane_z;
        my $bb = $volumes_bb;
        glDisable(GL_CULL_FACE);
        glDisable(GL_LIGHTING);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        glColor4f( @COLOR_CUTPLANE );
        if ($self->cutting_plane_axis == X) {
            glVertex3f($bb->x_min+$plane_z, $bb->y_min-20, $bb->z_min-20);
            glVertex3f($bb->x_min+$plane_z, $bb->y_max+20, $bb->z_min-20);
            glVertex3f($bb->x_min+$plane_z, $bb->y_max+20, $bb->z_max+20);
            glVertex3f($bb->x_min+$plane_z, $bb->y_min-20, $bb->z_max+20);
        } elsif ($self->cutting_plane_axis == Y) {
            glVertex3f($bb->x_min-20, $bb->y_min+$plane_z, $bb->z_min-20);
            glVertex3f($bb->x_max+20, $bb->y_min+$plane_z, $bb->z_min-20);
            glVertex3f($bb->x_max+20, $bb->y_min+$plane_z, $bb->z_max+20);
            glVertex3f($bb->x_min-20, $bb->y_min+$plane_z, $bb->z_max+20);
        } elsif ($self->cutting_plane_axis == Z) {
            glVertex3f($bb->x_min-20, $bb->y_min-20, $bb->z_min+$plane_z);
            glVertex3f($bb->x_max+20, $bb->y_min-20, $bb->z_min+$plane_z);
            glVertex3f($bb->x_max+20, $bb->y_max+20, $bb->z_min+$plane_z);
            glVertex3f($bb->x_min-20, $bb->y_max+20, $bb->z_min+$plane_z);
        }
        glEnd();
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
    }
    
    if (defined $self->_drag_start_pos || defined $self->_drag_start_xy) {
        $self->draw_center_of_rotation($self->_camera_target->x, $self->_camera_target->y, $self->_camera_target->z);
    }

    glFlush();
 
    $self->SwapBuffers();
    
    # Calling glFinish has a performance penalty, but it seems to fix some OpenGL driver hang-up with extremely large scenes.
    glFinish();
}

sub draw_axes {
    my ($self, $x, $y, $z, $length, $width, $allways_visible) = @_;
    if ($allways_visible) {
        glDisable(GL_DEPTH_TEST);
    } else {
        glEnable(GL_DEPTH_TEST);
    }
    glLineWidth($width);
    glBegin(GL_LINES);
    # draw line for x axis
    glColor3f(1, 0, 0);
    glVertex3f($x, $y, $z);
    glVertex3f($x + $length, $y, $z);
    # draw line for y axis
    glColor3f(0, 1, 0);
    glVertex3f($x, $y, $z);
    glVertex3f($x, $y + $length, $z);
    # draw line for Z axis
    glColor3f(0, 0, 1);
    glVertex3f($x, $y, $z);
    glVertex3f($x, $y, $z + $length);
    glEnd();
}

sub draw_center_of_rotation {
    my ($self, $x, $y, $z) = @_;
    
    $self->draw_axes($x, $y, $z, 10, 1, 1);
    $self->draw_axes($x, $y, $z, 10, 4, 0);
}

sub draw_volumes {
    my ($self, $fakecolor, $volume_to_render) = @_;
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    
    my $upper = $volume_to_render // $#{$self->volumes};
    my $lower = $volume_to_render // 0;  
    foreach my $volume_idx ($lower..$upper) {
        my $volume = $self->volumes->[$volume_idx];
        glPushMatrix();
        glTranslatef(@{$volume->origin});
        my @baseColor;
        if ($fakecolor) {
            my $r = ($volume_idx & 0x000000FF) >>  0;
            my $g = ($volume_idx & 0x0000FF00) >>  8;
            my $b = ($volume_idx & 0x00FF0000) >> 16;
            @baseColor = ($r/255.0, $g/255.0, $b/255.0, 1);
        } elsif ($self->enable_face_select){
            @baseColor = @{ $volume->color };
        } elsif ($volume->selected) {
            @baseColor = @SELECTED_COLOR;
            push(@baseColor, $volume->color->[3]);
        } elsif ($volume->hover) {
            @baseColor = @HOVER_COLOR;
            push(@baseColor, $volume->color->[3]);
        } else {
            @baseColor = @{ $volume->color };
        }
        
        my @sorted_z = ();
        my ($min_z, $max_z);
        if ($volume->range && $volume->offsets) {
            @sorted_z = sort { $a <=> $b } keys %{$volume->offsets};
            
            ($min_z, $max_z) = @{$volume->range};
            $min_z = first { $_ >= $min_z } @sorted_z;
            $max_z = first { $_ > $max_z }  @sorted_z;
        }
        
        glCullFace(GL_BACK);
        if ($volume->qverts) {
            my ($min_offset, $max_offset);
            if (defined $min_z) {
                $min_offset = $volume->offsets->{$min_z}->[0];
            }
            if (defined $max_z) {
                $max_offset = $volume->offsets->{$max_z}->[0];
            }
            $min_offset //= 0;
            $max_offset //= $volume->qverts->size;
            
            glColor4f(@baseColor);
            glVertexPointer_c(3, GL_FLOAT, 0, $volume->qverts->verts_ptr);
            glNormalPointer_c(GL_FLOAT, 0, $volume->qverts->norms_ptr);
            glDrawArrays(GL_QUADS, $min_offset / 3, ($max_offset-$min_offset) / 3);
        }
        
        if ($volume->tverts) {
            my ($min_offset, $max_offset);
            if (defined $min_z) {
                $min_offset = $volume->offsets->{$min_z}->[1];
            }
            if (defined $max_z) {
                $max_offset = $volume->offsets->{$max_z}->[1];
            }
            $min_offset //= 0;
            $max_offset //= $volume->tverts->size;
            if($fakecolor && $fakecolor == 2){
                our @_cached_colors;
                our $_cached_ptr;
                my $pLen = @_cached_colors;
                my $toAdd = max($max_offset/3*4 - $pLen, 0);
                if($toAdd){
                    # TODO: move this into CPP to reduce memory consumption and decrease time when new models are added
                    my @colors = (@baseColor)x($toAdd);
                    for my $i (0 .. ($#colors/12)){
                        my $r = (($i+($pLen/12)) & 0x000000FF) >>  0;
                        my $g = (($i+($pLen/12)) & 0x0000FF00) >>  8;
                        my $b = (($i+($pLen/12)) & 0x00FF0000) >> 16;
                        $colors[$i*4*3 + 0] = $r / 255.0;
                        $colors[$i*4*3 + 1] = $g / 255.0;
                        $colors[$i*4*3 + 2] = $b / 255.0;
                        $colors[$i*4*3 + 3] = 1.0;
                        $colors[$i*4*3 + 4] = $r / 255.0;
                        $colors[$i*4*3 + 5] = $g / 255.0;
                        $colors[$i*4*3 + 6] = $b / 255.0;
                        $colors[$i*4*3 + 7] = 1.0;
                        $colors[$i*4*3 + 8] = $r / 255.0;
                        $colors[$i*4*3 + 9] = $g / 255.0;
                        $colors[$i*4*3 + 10] = $b / 255.0;
                        $colors[$i*4*3 + 11] = 1.0;
                    }
                    push(@_cached_colors, @colors);
                    $_cached_ptr = OpenGL::Array->new_list(GL_FLOAT,@_cached_colors);
                }
                glEnableClientState(GL_COLOR_ARRAY);
                glColorPointer_c(4, GL_FLOAT, 0,  $_cached_ptr->ptr());
                glVertexPointer_c(3, GL_FLOAT, 0, $volume->tverts->verts_ptr);
                glNormalPointer_c(GL_FLOAT, 0, $volume->tverts->norms_ptr);
                glDrawArrays(GL_TRIANGLES, $min_offset / 3, ($max_offset-$min_offset) / 3);
                glDisableClientState(GL_COLOR_ARRAY);
            } else {
                glVertexPointer_c(3, GL_FLOAT, 0, $volume->tverts->verts_ptr);
                glNormalPointer_c(GL_FLOAT, 0, $volume->tverts->norms_ptr);
                if ( (not $fakecolor) && $volume->selected && $volume->selected_face != -1 && $volume->selected_face <= $max_offset/3){
                    my $i = $volume->selected_face;
                    glColor4f(@SELECTED_COLOR,$volume->color->[3]);
                    glDrawArrays(GL_TRIANGLES, $i*3, 3);
                }
                if ( (not $fakecolor) && $volume->hover && $volume->hover_face != -1 && $volume->hover_face <= $max_offset/3){
                    my $i = $volume->hover_face;
                    glColor4f(@HOVER_COLOR,$volume->color->[3]);
                    glDrawArrays(GL_TRIANGLES, $i*3, 3);
                }
                glColor4f(@baseColor);
                glDrawArrays(GL_TRIANGLES, $min_offset / 3, ($max_offset-$min_offset) / 3);
            }
        }
        
        glPopMatrix();
    }
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisable(GL_BLEND);

    my $cut_vertex;
    if (defined $self->cutting_plane_z) {
        if (HAS_VBO) {
            # Use Vertex Buffer Object for cutting plane (previous method crashes on modern POGL). 
            ($cut_vertex) = glGenBuffersARB_p(1);
            $self->cut_lines_vertices->bind($cut_vertex);
            glBufferDataARB_p(GL_ARRAY_BUFFER_ARB, $self->cut_lines_vertices, GL_STATIC_DRAW_ARB);
            glVertexPointer_c(3, GL_FLOAT, 0, 0);
        } else {
            # Use legacy method.
            glVertexPointer_c(3, GL_FLOAT, 0, $self->cut_lines_vertices->ptr());
        }
        glLineWidth(2);
        glColor3f(0, 0, 0);
        glDrawArrays(GL_LINES, 0, $self->cut_lines_vertices->elements / 3);

        if (HAS_VBO) { 
            # Turn off buffer objects to let the rest of the draw code work.
            glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
            glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
            glDeleteBuffersARB_p($cut_vertex);
        }

    }
    glDisableClientState(GL_VERTEX_ARRAY);
}

sub calculate_normal {
    my $self = shift;
    my ($volume_idx) = @_;
    return undef if $volume_idx == -1;
    my $volume = $self->volumes->[$volume_idx];
    my $max_offset = $volume->tverts->size; # For now just assume this TODO: add the other checks
    
    if ($volume->selected && $volume->selected_face != -1 && $volume->selected_face <= $max_offset/3){
        my $i = $volume->selected_face;
        my $p1 = $volume->tverts->get_point($i*3);
        my $p2 = $volume->tverts->get_point($i*3+1);
        my $p3 = $volume->tverts->get_point($i*3+2);
        my $v1 = $p1->vector_to($p2);
        my $v2 = $p1->vector_to($p3);
        
        # Calculate the cross product
        my $x =  $v1->y() * $v2->z() - $v1->z() * $v2->y();
        my $y =  $v1->z() * $v2->x() - $v1->x() * $v2->z();
        my $z =  $v1->x() * $v2->y() - $v1->y() * $v2->x();
        
        # Normalize it
        my $d = sqrt($x*$x + $y*$y + $z*$z);
        return Slic3r::Pointf3->new($x/$d,$y/$d,$z/$d);
    }
    
    return undef;
}

package Slic3r::GUI::3DScene::Volume;
use Moo;

has 'bounding_box'      => (is => 'ro', required => 1);
has 'origin'            => (is => 'rw', default => sub { Slic3r::Pointf3->new(0,0,0) });
has 'color'             => (is => 'ro', required => 1);
has 'select_group_id'   => (is => 'rw', default => sub { -1 });
has 'drag_group_id'     => (is => 'rw', default => sub { -1 });
has 'selected'          => (is => 'rw', default => sub { 0 });
has 'selected_face'     => (is => 'rw', default => sub { -1 });
has 'hover'             => (is => 'rw', default => sub { 0 });
has 'hover_face'        => (is => 'rw', default => sub { -1 });
has 'range'             => (is => 'rw');

# geometric data
has 'qverts'            => (is => 'rw');  # GLVertexArray object
has 'tverts'            => (is => 'rw');  # GLVertexArray object
has 'mesh'              => (is => 'rw');  # only required for cut contours
has 'offsets'           => (is => 'rw');  # [ z => [ qverts_idx, tverts_idx ] ]

sub transformed_bounding_box {
    my ($self) = @_;
    
    my $bb = $self->bounding_box;
    $bb->translate(@{$self->origin});
    return $bb;
}

package Slic3r::GUI::3DScene;
use base qw(Slic3r::GUI::3DScene::Base);

use OpenGL qw(:glconstants :gluconstants :glufunctions);
use List::Util qw(first min max);
use Slic3r::Geometry qw(scale unscale epsilon);
use Slic3r::Print::State ':steps';
use Slic3r::GUI::ColorScheme;

__PACKAGE__->mk_accessors(qw(
    colors
    color_by
    color_toolpaths_by
    select_by
    drag_by
    volumes_by_object
    _objects_by_volumes
));

sub default_colors { [@COLOR_PARTS], [@COLOR_INFILL], [@COLOR_SUPPORT], [@COLOR_UNKNOWN] }

sub new {
    my $class = shift;
    
    my $self = $class->SUPER::new(@_);
    
    if ( ( defined $Slic3r::GUI::Settings->{_}{colorscheme} ) && ( Slic3r::GUI::ColorScheme->can($Slic3r::GUI::Settings->{_}{colorscheme}) ) ) {
        my $myGetSchemeName = \&{"Slic3r::GUI::ColorScheme::$Slic3r::GUI::Settings->{_}{colorscheme}"};
        $myGetSchemeName->();
    } else {
        Slic3r::GUI::ColorScheme->getDefault();
    }
    
    $self->colors([ $self->default_colors ]);
    $self->color_by('volume');      # object | volume
    $self->color_toolpaths_by('role'); # role | extruder
    $self->select_by('object');     # object | volume | instance
    $self->drag_by('instance');     # object | instance
    $self->volumes_by_object({});   # obj_idx => [ volume_idx, volume_idx ... ]
    $self->_objects_by_volumes({}); # volume_idx => [ obj_idx, instance_idx ]
    
    return $self;
}

sub load_object {
    my ($self, $model, $obj_idx, $instance_idxs) = @_;
    
    my $model_object;
    if ($model->isa('Slic3r::Model::Object')) {
        $model_object = $model;
        $model = $model_object->model;
        $obj_idx = 0;
    } else {
        $model_object = $model->get_object($obj_idx);
    }
    
    $instance_idxs ||= [0..$#{$model_object->instances}];
    
    my @volumes_idx = ();
    foreach my $volume_idx (0..$#{$model_object->volumes}) {
        my $volume = $model_object->volumes->[$volume_idx];
        foreach my $instance_idx (@$instance_idxs) {
            my $instance = $model_object->instances->[$instance_idx];
            my $mesh = $volume->get_transformed_mesh($instance);
            
            my $color_idx;
            if ($self->color_by eq 'volume') {
                $color_idx = $volume_idx;
            } elsif ($self->color_by eq 'object') {
                $color_idx = $obj_idx;
            }
        
            my $color = [ @{$self->colors->[ $color_idx % scalar(@{$self->colors}) ]} ];
            $color->[3] = $volume->modifier ? 0.5 : 1;
            push @{$self->volumes}, my $v = Slic3r::GUI::3DScene::Volume->new(
                bounding_box    => $mesh->bounding_box,
                color           => $color,
            );
            if ($self->select_by eq 'object') {
                $v->select_group_id($obj_idx*1000000);
            } elsif ($self->select_by eq 'volume') {
                $v->select_group_id($obj_idx*1000000 + $volume_idx*1000);
            } elsif ($self->select_by eq 'instance') {
                $v->select_group_id($obj_idx*1000000 + $volume_idx*1000 + $instance_idx);
            }
            if ($self->drag_by eq 'object') {
                $v->drag_group_id($obj_idx*1000);
            } elsif ($self->drag_by eq 'instance') {
                $v->drag_group_id($obj_idx*1000 + $instance_idx);
            }
            push @volumes_idx, my $scene_volume_idx = $#{$self->volumes};
            $self->_objects_by_volumes->{$scene_volume_idx} = [ $obj_idx, $volume_idx, $instance_idx ];
            
            my $verts = Slic3r::GUI::_3DScene::GLVertexArray->new;
            $verts->load_mesh($mesh);
            $v->tverts($verts);
        }
    }
    
    $self->volumes_by_object->{$obj_idx} = [@volumes_idx];
    return @volumes_idx;
}

sub load_print_object_slices {
    my ($self, $object) = @_;
    
    my @verts = ();
    my @norms = ();
    my @quad_verts = ();
    my @quad_norms = ();
    foreach my $layer (@{$object->layers}) {
        my $gap = 0;
        my $top_z = $layer->print_z;
        my $bottom_z = $layer->print_z - $layer->height + $gap;
    
        foreach my $copy (@{ $object->_shifted_copies }) {
            {
                my @expolygons = map $_->clone, @{$layer->slices};
                $_->translate(@$copy) for @expolygons;
                $self->_expolygons_to_verts(\@expolygons, $layer->print_z, \@verts, \@norms);
            }
            foreach my $slice (@{$layer->slices}) {
                foreach my $polygon (@$slice) {
                    foreach my $line (@{$polygon->lines}) {
                        $line->translate(@$copy);
                        
                        push @quad_norms, (0,0,-1), (0,0,-1);
                        push @quad_verts, (map unscale($_), @{$line->a}), $bottom_z;
                        push @quad_verts, (map unscale($_), @{$line->b}), $bottom_z;
                        push @quad_norms, (0,0,1), (0,0,1);
                        push @quad_verts, (map unscale($_), @{$line->b}), $top_z;
                        push @quad_verts, (map unscale($_), @{$line->a}), $top_z;
                        
                        # We'll use this for the middle normal when using 4 quads:
                        #my $xy_normal = $line->normal;
                        #$_xynormal->scale(1/$line->length);
                    }
                }
            }
        }
    }
    
    my $obb = $object->bounding_box;
    my $bb = Slic3r::Geometry::BoundingBoxf3->new;
    $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$obb->min_point}, 0));
    $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$obb->max_point}, $object->size->z));
    
    push @{$self->volumes}, my $v = Slic3r::GUI::3DScene::Volume->new(
        bounding_box    => $bb,
        color           => $self->colors->[0],
        verts           => OpenGL::Array->new_list(GL_FLOAT, @verts),
        norms           => OpenGL::Array->new_list(GL_FLOAT, @norms),
        quad_verts      => OpenGL::Array->new_list(GL_FLOAT, @quad_verts),
        quad_norms      => OpenGL::Array->new_list(GL_FLOAT, @quad_norms),
    );
}

sub load_print_toolpaths {
    my ($self, $print) = @_;
    
    return if !$print->step_done(STEP_SKIRT);
    return if !$print->step_done(STEP_BRIM);
    return if !$print->has_skirt
        && $print->config->brim_width == 0
        && $print->config->interior_brim_width == 0
        && $print->config->brim_connections_width == 0;
    
    my $bb = Slic3r::Geometry::BoundingBoxf3->new;
    {
        my $pbb = $print->bounding_box;
        $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$pbb->min_point}));
        $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$pbb->max_point}));
    }
    
    my $color_by_extruder = $self->color_toolpaths_by eq 'extruder';

    if ($print->brim_count > 0) {  # hack out brim display because of experimental brim.
        for (my $id = 0; $id < $print->brim_count; $id++) {
            my $color = $color_by_extruder
                ? $self->colors->[ ($print->brim_extruder-1) % @{$self->colors} ]
                : $self->colors->[2];

            push @{$self->volumes}, my $volume = Slic3r::GUI::3DScene::Volume->new(
                bounding_box    => $bb,
                color           => $color,
                qverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
                tverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
                offsets         => {},  # print_z => [ qverts, tverts ]
            );

            my $top_z = $print->get_object(0)->get_layer($id)->print_z;
            $volume->offsets->{$top_z} = [0, 0];
            $self->_extrusionentity_to_verts($print->brim_layer($id), $top_z, Slic3r::Point->new(0,0),
                $volume->qverts, $volume->tverts);
        }
    }
    if (!$print->skirt->empty) {
        # TODO: it's a bit difficult to predict skirt extruders with the current skirt logic.
        # We need to rewrite it anyway.
        my $color = +($self->default_colors)[0];
        push @{$self->volumes}, my $volume = Slic3r::GUI::3DScene::Volume->new(
            bounding_box    => $bb,
            color           => $color,
            qverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
            tverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
            offsets         => {},  # print_z => [ qverts, tverts ]
        );
    
        my $skirt_height = 0;  # number of layers
        if ($print->has_infinite_skirt) {
            $skirt_height = $print->total_layer_count;
        } else {
            $skirt_height = min($print->config->skirt_height, $print->total_layer_count);
        }
        $skirt_height ||= 1 if $print->config->brim_width > 0 || $print->config->interior_brim_width;
    
        # get first $skirt_height layers (maybe this should be moved to a PrintObject method?)
        my $object0 = $print->get_object(0);
        my @layers = ();
        push @layers, map $object0->get_layer($_-1), 1..min($skirt_height, $object0->layer_count);
        push @layers, map $object0->get_support_layer($_-1), 1..min($skirt_height, $object0->support_layer_count);
        @layers = sort { $a->print_z <=> $b->print_z } @layers;
        @layers = @layers[0..($skirt_height-1)];
    
        foreach my $i (0..($skirt_height-1)) {
            my $top_z = $layers[$i]->print_z;
            $volume->offsets->{$top_z} = [$volume->qverts->size, $volume->tverts->size];
        
            $self->_extrusionentity_to_verts($print->skirt, $top_z, Slic3r::Point->new(0,0),
                $volume->qverts, $volume->tverts);
        }
    }
}

sub load_print_object_toolpaths {
    my ($self, $object) = @_;
    
    # order layers by print_z
    my @layers = sort { $a->print_z <=> $b->print_z }
        @{$object->layers}, @{$object->support_layers};
    
    # Bounding box of the object and its copies.
    my $bb = Slic3r::Geometry::BoundingBoxf3->new;
    {
        my $obb = $object->bounding_box;
        foreach my $copy (@{ $object->_shifted_copies }) {
            my $cbb = $obb->clone;
            $cbb->translate(@$copy);
            $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$cbb->min_point}, 0));
            $bb->merge_point(Slic3r::Pointf3->new_unscale(@{$cbb->max_point}, $object->size->z));
        }
    }
    
    my %volumes = ();  # color => Volume
    
    # Maximum size of an allocation block: 32MB / sizeof(float)
    my $alloc_size_max = 32 * 1048576 / 4;
    
    my $add = sub {
        my ($coll, $top_z, $copy, $color) = @_;
        
        my $volume = $volumes{$color};
        if (!$volume) {
            push @{$self->volumes}, $volumes{$color} = $volume = Slic3r::GUI::3DScene::Volume->new(
                bounding_box    => $bb,
                color           => $color,
                qverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
                tverts          => Slic3r::GUI::_3DScene::GLVertexArray->new,
                offsets         => {},  # print_z => [ qverts, tverts ]
            );
        }
        
        $volume->offsets->{$top_z} //= [ $volume->qverts->size, $volume->tverts->size ];
        $self->_extrusionentity_to_verts($coll, $top_z, $copy, $volume->qverts, $volume->tverts);
    };
    
    my $color_by_extruder = $self->color_toolpaths_by eq 'extruder';
    
    foreach my $layer (@layers) {
        my $top_z = $layer->print_z;
        foreach my $copy (@{ $object->_shifted_copies }) {
            foreach my $layerm (@{$layer->regions}) {
                if ($object->step_done(STEP_PERIMETERS)) {
                    my $color = $color_by_extruder
                        ? $self->colors->[ ($layerm->region->config->perimeter_extruder-1) % @{$self->colors} ]
                        : $self->colors->[0];
                    $add->($layerm->perimeters, $top_z, $copy, $color);
                }
                
                if ($object->step_done(STEP_INFILL)) {
                    my $color = $color_by_extruder
                        ? $self->colors->[ ($layerm->region->config->infill_extruder-1) % @{$self->colors} ]
                        : $self->colors->[1];
                    
                    if ($color_by_extruder && $layerm->region->config->infill_extruder != $layerm->region->config->solid_infill_extruder) {
                        # divide solid and non-solid infill
                        my $solid = Slic3r::ExtrusionPath::Collection->new;
                        my $non_solid = Slic3r::ExtrusionPath::Collection->new;
                        foreach my $fill (@{$layerm->fills}) {
                            if ($fill->[0]->is_solid_infill) {
                                $solid->append($fill);
                            } else {
                                $non_solid->append($fill);
                            }
                        }
                        $add->($non_solid, $top_z, $copy, $color);
                        $color = $self->colors->[ ($layerm->region->config->solid_infill_extruder-1) % @{$self->colors} ];
                        $add->($solid, $top_z, $copy, $color);
                    } else {
                        $add->($layerm->fills, $top_z, $copy, $color);
                    }
                }
            }
            
            if ($layer->isa('Slic3r::Layer::Support') && $object->step_done(STEP_SUPPORTMATERIAL)) {
                {
                    my $color = $color_by_extruder
                        ? $self->colors->[ ($layer->object->config->support_material_extruder-1) % @{$self->colors} ]
                        : $self->colors->[2];
                    $add->($layer->support_fills, $top_z, $copy, $color);
                }
                {
                    my $color = ($color_by_extruder)
                        ? $self->colors->[ ($layer->object->config->support_material_interface_extruder-1) % @{$self->colors} ]
                        : $self->colors->[2];
                    $add->($layer->support_interface_fills, $top_z, $copy, $color);
                }
            }
        }
        
        foreach my $color (keys %volumes) {
            my $volume = $volumes{$color};
            if ($volume->qverts->size() > $alloc_size_max || $volume->tverts->size() > $alloc_size_max) {
                # stop appending to this volume; create a new one next time
                delete $volumes{$color};
            }
        }
    }
}

sub set_toolpaths_range {
    my ($self, $min_z, $max_z) = @_;
    
    foreach my $volume (@{$self->volumes}) {
        $volume->range([ $min_z, $max_z ]);
    }
}

sub _expolygons_to_verts {
    my ($self, $expolygons, $z, $verts, $norms) = @_;
    
    my $tess = gluNewTess();
    gluTessCallback($tess, GLU_TESS_BEGIN,     'DEFAULT');
    gluTessCallback($tess, GLU_TESS_END,       'DEFAULT');
    gluTessCallback($tess, GLU_TESS_VERTEX, sub {
        my ($x, $y, $z) = @_;
        push @$verts, $x, $y, $z;
        push @$norms, (0,0,1), (0,0,1), (0,0,1);
    });
    gluTessCallback($tess, GLU_TESS_COMBINE,   'DEFAULT');
    gluTessCallback($tess, GLU_TESS_ERROR,     'DEFAULT');
    gluTessCallback($tess, GLU_TESS_EDGE_FLAG, 'DEFAULT');
    
    foreach my $expolygon (@$expolygons) {
        gluTessBeginPolygon($tess);
        foreach my $polygon (@$expolygon) {
            gluTessBeginContour($tess);
            gluTessVertex_p($tess, (map unscale($_), @$_), $z) for @$polygon;
            gluTessEndContour($tess);
        }
        gluTessEndPolygon($tess);
    }
    
    gluDeleteTess($tess);
}

# Fill in the $qverts and $tverts with quads and triangles
# for the extrusion $entity.
sub _extrusionentity_to_verts {
    my ($self, $entity, $top_z, $copy, $qverts, $tverts) = @_;
    
    my ($lines, $widths, $heights, $closed);
    if ($entity->isa('Slic3r::ExtrusionPath::Collection')) {
        $self->_extrusionentity_to_verts($_, $top_z, $copy, $qverts, $tverts)
            for @$entity;
        return;
    } elsif ($entity->isa('Slic3r::ExtrusionPath')) {
        my $polyline = $entity->polyline->clone;
        $polyline->remove_duplicate_points;
        $polyline->translate(@$copy);
        $lines = $polyline->lines;
        $widths = [ map $entity->width, 0..$#$lines ];
        $heights = [ map $entity->height, 0..$#$lines ];
        $closed = 0;
    } else {
        $lines   = [];
        $widths  = [];
        $heights = [];
        $closed  = 1;
        foreach my $path (@$entity) {
            my $polyline = $path->polyline->clone;
            $polyline->remove_duplicate_points;
            $polyline->translate(@$copy);
            my $path_lines = $polyline->lines;
            push @$lines, @$path_lines;
            push @$widths, map $path->width, 0..$#$path_lines;
            push @$heights, map $path->height, 0..$#$path_lines;
        }
    }
    
    # Calling the C++ implementation Slic3r::_3DScene::_extrusionentity_to_verts_do()
    # This adds new vertices to the $qverts and $tverts.
    Slic3r::GUI::_3DScene::_extrusionentity_to_verts_do($lines, $widths, $heights,
        $closed, 
        # Top height of the extrusion.
        $top_z, 
        # $copy is not used here.
        $copy,
        # GLVertexArray object: C++ class maintaining an std::vector<float> for coords and normals.
        $qverts,
        $tverts);
}

sub object_idx {
    my ($self, $volume_idx) = @_;
    return $self->_objects_by_volumes->{$volume_idx}[0];
}

sub volume_idx {
    my ($self, $volume_idx) = @_;
    return $self->_objects_by_volumes->{$volume_idx}[1];
}

sub instance_idx {
    my ($self, $volume_idx) = @_;
    return $self->_objects_by_volumes->{$volume_idx}[2];
}

1;
