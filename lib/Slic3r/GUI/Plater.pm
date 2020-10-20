# The "Plater" tab. It contains the "3D", "2D", "Preview" and "Layers" subtabs.

package Slic3r::GUI::Plater::UndoOperation;
use strict;
use warnings;

sub new{
    my $class = shift;
    my $self = {
        type => shift,
        object_identifier => shift,
        attributes => shift,
    };
    bless ($self, $class);
    return $self;
}

package Slic3r::GUI::Plater;
use strict;
use warnings;
use utf8;

use File::Basename qw(basename dirname);
use List::Util qw(sum first max none any);
use Slic3r::Geometry qw(X Y Z MIN MAX scale unscale deg2rad rad2deg);
use Math::Trig qw(acos);
use LWP::UserAgent;
use threads::shared qw(shared_clone);
use Wx qw(:button :cursor :dialog :filedialog :keycode :icon :font :id :misc 
    :panel :sizer :toolbar :window wxTheApp :notebook :combobox);
use Wx::Event qw(EVT_BUTTON EVT_COMMAND EVT_KEY_DOWN EVT_MOUSE_EVENTS EVT_PAINT EVT_TOOL 
    EVT_CHOICE EVT_COMBOBOX EVT_TIMER EVT_NOTEBOOK_PAGE_CHANGED EVT_LEFT_UP EVT_CLOSE);
use base qw(Wx::Panel Class::Accessor);
use Slic3r::GUI::ColorScheme;

use Net::SSL;
$ENV{HTTPS_VERSION} = 3;
$ENV{PERL_LWP_SSL_VERIFY_HOSTNAME} = 0;

__PACKAGE__->mk_accessors(qw(presets));

use constant TB_ADD             => &Wx::NewId;
use constant TB_REMOVE          => &Wx::NewId;
use constant TB_RESET           => &Wx::NewId;
use constant TB_ARRANGE         => &Wx::NewId;
use constant TB_EXPORT_GCODE    => &Wx::NewId;
use constant TB_EXPORT_STL      => &Wx::NewId;
use constant TB_MORE    => &Wx::NewId;
use constant TB_FEWER   => &Wx::NewId;
use constant TB_X90CW   => &Wx::NewId;
use constant TB_X90CCW  => &Wx::NewId;
use constant TB_Y90CW   => &Wx::NewId;
use constant TB_Y90CCW  => &Wx::NewId;
use constant TB_Z90CW   => &Wx::NewId;
use constant TB_Z90CCW  => &Wx::NewId;
use constant TB_45CW    => &Wx::NewId;
use constant TB_45CCW   => &Wx::NewId;
use constant TB_ROTFACE => &Wx::NewId;
use constant TB_SCALE   => &Wx::NewId;
use constant TB_SPLIT   => &Wx::NewId;
use constant TB_CUT     => &Wx::NewId;
use constant TB_LAYERS  => &Wx::NewId;
use constant TB_SETTINGS => &Wx::NewId;

# package variables to avoid passing lexicals to threads
our $THUMBNAIL_DONE_EVENT    : shared = Wx::NewEventType;
our $PROGRESS_BAR_EVENT      : shared = Wx::NewEventType;
our $ERROR_EVENT             : shared = Wx::NewEventType;
our $EXPORT_COMPLETED_EVENT  : shared = Wx::NewEventType;
our $PROCESS_COMPLETED_EVENT : shared = Wx::NewEventType;

use constant FILAMENT_CHOOSERS_SPACING => 0;
use constant PROCESS_DELAY => 0.5 * 1000; # milliseconds

sub new {
    my $class = shift;
    my ($parent) = @_;
    my $self = $class->SUPER::new($parent, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    $self->{config} = Slic3r::Config->new_from_defaults(qw(
        bed_shape complete_objects extruder_clearance_radius skirts skirt_distance brim_width
        serial_port serial_speed host_type print_host octoprint_apikey shortcuts filament_colour
    ));
    $self->{model} = Slic3r::Model->new;
    $self->{print} = Slic3r::Print->new;
    $self->{processed} = 0;
    # List of Perl objects Slic3r::GUI::Plater::Object, representing a 2D preview of the platter.
    $self->{objects} = [];

    # Objects identifier used for undo/redo operations. It's a one time id assigned to each newly created object.
    $self->{object_identifier} = 0;

    # Stack of undo operations.
    $self->{undo_stack} = [];

    # Stack of redo operations.
    $self->{redo_stack} = [];

    # Preset dialogs
    $self->{'preset_dialogs'} = {};

    $self->{print}->set_status_cb(sub {
        my ($percent, $message) = @_;
        
        if ($Slic3r::have_threads) {
            Wx::PostEvent($self, Wx::PlThreadEvent->new(-1, $PROGRESS_BAR_EVENT, shared_clone([$percent, $message])));
        } else {
            $self->on_progress_event($percent, $message);
        }
    });
    
    # Initialize preview notebook
    $self->{preview_notebook} = Wx::Notebook->new($self, -1, wxDefaultPosition, [335,335], wxNB_BOTTOM);
    
    # Initialize handlers for canvases
    my $on_select_object = sub {
        my ($obj_idx) = @_;
        $self->select_object($obj_idx);
    };
    my $on_double_click = sub {
        $self->object_settings_dialog if $self->selected_object;
    };
    my $on_right_click = sub {
        my ($canvas, $click_pos) = @_;
        
        my ($obj_idx, $object) = $self->selected_object;
        return if !defined $obj_idx;
        
        my $menu = $self->object_menu;
        $canvas->PopupMenu($menu, $click_pos);
        $menu->Destroy;
    };
    my $on_instances_moved = sub {
        $self->on_model_change;
    };

    # Initialize 3D plater
    if ($Slic3r::GUI::have_OpenGL) {
        $self->{canvas3D} = Slic3r::GUI::Plater::3D->new($self->{preview_notebook}, $self->{objects}, $self->{model}, $self->{config});
        $self->{preview_notebook}->AddPage($self->{canvas3D}, '3D');
        $self->{canvas3D}->set_on_select_object($on_select_object);
        $self->{canvas3D}->set_on_double_click($on_double_click);
        $self->{canvas3D}->set_on_right_click(sub { $on_right_click->($self->{canvas3D}, @_); });
        $self->{canvas3D}->set_on_instances_moved($on_instances_moved);
        $self->{canvas3D}->on_viewport_changed(sub {
            $self->{preview3D}->canvas->set_viewport_from_scene($self->{canvas3D});
        });
    }
    
    # Initialize 2D preview canvas
    $self->{canvas} = Slic3r::GUI::Plater::2D->new($self->{preview_notebook}, wxDefaultSize, $self->{objects}, $self->{model}, $self->{config});
    $self->{preview_notebook}->AddPage($self->{canvas}, '2D');
    $self->{canvas}->on_select_object($on_select_object);
    $self->{canvas}->on_double_click($on_double_click);
    $self->{canvas}->on_right_click(sub { $on_right_click->($self->{canvas}, @_); });
    $self->{canvas}->on_instances_moved($on_instances_moved);
    
    # Initialize 3D toolpaths preview
    $self->{preview3D_page_idx} = -1;
    if ($Slic3r::GUI::have_OpenGL) {
        $self->{preview3D} = Slic3r::GUI::Plater::3DPreview->new($self->{preview_notebook}, $self->{print});
        $self->{preview3D}->canvas->on_viewport_changed(sub {
            $self->{canvas3D}->set_viewport_from_scene($self->{preview3D}->canvas);
        });
        $self->{preview_notebook}->AddPage($self->{preview3D}, 'Preview');
        $self->{preview3D_page_idx} = $self->{preview_notebook}->GetPageCount-1;
    }
    
    # Initialize toolpaths preview
    $self->{toolpaths2D_page_idx} = -1;
    if ($Slic3r::GUI::have_OpenGL) {
        $self->{toolpaths2D} = Slic3r::GUI::Plater::2DToolpaths->new($self->{preview_notebook}, $self->{print});
        $self->{preview_notebook}->AddPage($self->{toolpaths2D}, 'Layers');
        $self->{toolpaths2D_page_idx} = $self->{preview_notebook}->GetPageCount-1;
    }
    
    EVT_NOTEBOOK_PAGE_CHANGED($self, $self->{preview_notebook}, sub {
        wxTheApp->CallAfter(sub {
            my $sel = $self->{preview_notebook}->GetSelection;
            if ($sel == $self->{preview3D_page_idx} || $sel == $self->{toolpaths2D_page_idx}) {
                if (!$Slic3r::GUI::Settings->{_}{background_processing} && !$self->{processed}) {
                    $self->statusbar->SetCancelCallback(sub {
                        $self->stop_background_process;
                        $self->statusbar->SetStatusText("Slicing cancelled");
                        $self->{preview_notebook}->SetSelection(0);

                    });
                    $self->start_background_process;
                } else {
                    $self->{preview3D}->load_print
                        if $sel == $self->{preview3D_page_idx};
                }
            }
        });
    });
    
    # toolbar for object manipulation
    if (!&Wx::wxMSW) {
        Wx::ToolTip::Enable(1);
        $self->{htoolbar} = Wx::ToolBar->new($self, -1, wxDefaultPosition, wxDefaultSize, wxTB_HORIZONTAL | wxTB_TEXT | wxBORDER_SIMPLE | wxTAB_TRAVERSAL);
        $self->{htoolbar}->AddTool(TB_ADD, "Add…", Wx::Bitmap->new($Slic3r::var->("brick_add.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_REMOVE, "Delete", Wx::Bitmap->new($Slic3r::var->("brick_delete.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_RESET, "Delete All", Wx::Bitmap->new($Slic3r::var->("cross.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_ARRANGE, "Arrange", Wx::Bitmap->new($Slic3r::var->("bricks.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
        $self->{htoolbar}->AddTool(TB_MORE, "More", Wx::Bitmap->new($Slic3r::var->("add.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_FEWER, "Fewer", Wx::Bitmap->new($Slic3r::var->("delete.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
       
        if ($Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz' || $Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz-big') {
            $self->{htoolbar}->AddTool(TB_X90CCW, "90° X ccw", Wx::Bitmap->new($Slic3r::var->(@rotateX90ccwT), wxBITMAP_TYPE_PNG), '');
            $self->{htoolbar}->AddTool(TB_X90CW, "90° X cw", Wx::Bitmap->new($Slic3r::var->(@rotateX90cwT), wxBITMAP_TYPE_PNG), '');
            $self->{htoolbar}->AddTool(TB_Y90CCW, "90° Y ccw", Wx::Bitmap->new($Slic3r::var->(@rotateY90ccwT), wxBITMAP_TYPE_PNG), '');
            $self->{htoolbar}->AddTool(TB_Y90CW, "90° Y cw", Wx::Bitmap->new($Slic3r::var->(@rotateY90cwT), wxBITMAP_TYPE_PNG), '');
        }
        
        $self->{htoolbar}->AddTool(TB_45CCW, "45° ccw", Wx::Bitmap->new($Slic3r::var->(@rotateZ45ccwT), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_45CW, "45° cw", Wx::Bitmap->new($Slic3r::var->(@rotateZ45cwT), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_ROTFACE, "Rotate face", Wx::Bitmap->new($Slic3r::var->("rotate_face.png"), wxBITMAP_TYPE_PNG), '');
        
        $self->{htoolbar}->AddSeparator;

        $self->{htoolbar}->AddTool(TB_SCALE, "Scale…", Wx::Bitmap->new($Slic3r::var->("arrow_out.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_SPLIT, "Split", Wx::Bitmap->new($Slic3r::var->("shape_ungroup.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_CUT, "Cut…", Wx::Bitmap->new($Slic3r::var->("package.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
        $self->{htoolbar}->AddTool(TB_SETTINGS, "Settings…", Wx::Bitmap->new($Slic3r::var->("cog.png"), wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_LAYERS, "Layer heights…", Wx::Bitmap->new($Slic3r::var->("variable_layer_height.png"), wxBITMAP_TYPE_PNG), '');
    } else {
        my %tbar_buttons = (
            add             => "Add…",
            remove          => "Delete",
            reset           => "Delete All",
            arrange         => "Arrange",
            increase        => "",
            decrease        => "",
            rotateX90ccw    => "",
            rotateX90cw     => "",
            rotateY90ccw    => "",
            rotateY90cw     => "",
            rotateZ90ccw    => "",
            rotateZ90cw     => "",
            rotateZ45ccw    => "",
            rotateZ45cw     => "",
            rotateFace      => "",
            changescale     => "Scale…",
            split           => "Split",
            cut             => "Cut…",
            layers          => "Layer heights…",
            settings        => "Settings…",
        );
        my %tbar_buttonsToolTip = (
            add             => "Adds new Objects",
            remove          => "Delete Object",
            reset           => "Clears the Plate of all Objects",
            arrange         => "Automaitally arrange Objects",
            increase        => "Add another Copy",
            decrease        => "Remove a Copy",
            rotateX90ccw    => "Rotate around X by 90° counter clockwise",
            rotateX90cw     => "Rotate around X by 90° clockwise",
            rotateY90ccw    => "Rotate around Y by 90° counter clockwise",
            rotateY90cw     => "Rotate around Y by 90° clockwise",
            rotateZ90ccw    => "Rotate around Z by 90° counter clockwise",
            rotateZ90cw     => "Rotate around Z by 90° clockwise",
            rotateZ45ccw    => "Rotate around Z by 45° counter clockwise",
            rotateZ45cw     => "Rotate around Z by 45° clockwise",
            rotateFace      => "Rotate to Face",
            changescale     => "Change Scale of Object",
            split           => "Split Object",
            cut             => "Cut Object",
            settings        => "Settings, Parts, Modifiers and Layers",
        );
        $self->{btoolbar} = Wx::BoxSizer->new(wxHORIZONTAL);
        
        my @buttons = qw(add remove reset arrange increase decrease);
        if ($Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz' || $Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz-big') {
            push @buttons, qw(rotateX90ccw rotateX90cw rotateY90ccw rotateY90cw rotateZ45ccw rotateZ45cw);
        }
        push @buttons, qw(changescale split cut settings);
        for (@buttons) {
            $self->{"btn_$_"} = Wx::Button->new($self, -1, $tbar_buttons{$_}, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            $self->{btoolbar}->Add($self->{"btn_$_"});
            $self->{"btn_$_"}->SetToolTipString($tbar_buttonsToolTip{$_});
        }
    }

    # right pane buttons
    $self->{btn_export_gcode} = Wx::Button->new($self, -1, "Export G-code…", wxDefaultPosition, [-1, -1], wxBU_LEFT);
    $self->{btn_print} = Wx::Button->new($self, -1, "Print…", wxDefaultPosition, [-1, -1], wxBU_LEFT);
    $self->{btn_send_gcode} = Wx::Button->new($self, -1, "Send to printer", wxDefaultPosition, [-1, -1], wxBU_LEFT);
    $self->{btn_export_stl} = Wx::Button->new($self, -1, "Export STL…", wxDefaultPosition, [-1, -1], wxBU_LEFT);
    #$self->{btn_export_gcode}->SetFont($Slic3r::GUI::small_font);
    #$self->{btn_export_stl}->SetFont($Slic3r::GUI::small_font);
    $self->{btn_print}->Hide;
    $self->{btn_send_gcode}->Hide;
    
    if ($Slic3r::GUI::have_button_icons) {
        my %icons = (
            add            => "brick_add.png",
            remove         => "brick_delete.png",
            reset          => "cross.png",
            arrange        => "bricks.png",
            export_gcode   => "cog_go.png",
            print          => "arrow_up.png",
            send_gcode     => "arrow_up.png",
            export_stl     => "brick_go.png",
            
            increase       => "add.png",
            decrease       => "delete.png",
            

            rotateX90cw    => @rotateX90cwT,
            rotateX90ccw   => @rotateX90ccwT,
            rotateY90cw    => @rotateY90cwT,
            rotateY90ccw   => @rotateY90ccwT,
            rotateZ90cw    => @rotateZ90cwT,
            rotateZ90ccw   => @rotateZ90ccwT,
            rotateZ45cw    => @rotateZ45cwT,
            rotateZ45ccw   => @rotateZ45ccwT,
            
            rotateFace     => "rotate_face.png",
            changescale    => "arrow_out.png",
            split          => "shape_ungroup.png",
            cut            => "package.png",
            layers         => "variable_layer_height.png",
            settings       => "cog.png",

        );
        for (grep $self->{"btn_$_"}, keys %icons) {
            $self->{"btn_$_"}->SetBitmap(Wx::Bitmap->new($Slic3r::var->($icons{$_}), wxBITMAP_TYPE_PNG));
        }
    }
    $self->selection_changed(0);
    $self->object_list_changed;
    EVT_BUTTON($self, $self->{btn_export_gcode}, sub {
        $self->export_gcode;
    });
    EVT_BUTTON($self, $self->{btn_print}, sub {
        $self->{print_file} = $self->export_gcode(Wx::StandardPaths::Get->GetTempDir());
    });
    EVT_LEFT_UP($self->{btn_send_gcode}, sub {
        my (undef, $e) = @_;
        
        my $alt = $e->ShiftDown;
        wxTheApp->CallAfter(sub {
            $self->prepare_send($alt);
        });
    });
    EVT_BUTTON($self, $self->{btn_export_stl}, \&export_stl);
    
    if ($self->{htoolbar}) {
        EVT_TOOL($self, TB_ADD, sub { $self->add; });
        EVT_TOOL($self, TB_REMOVE, sub { $self->remove() }); # explicitly pass no argument to remove
        EVT_TOOL($self, TB_RESET, sub { $self->reset; });
        EVT_TOOL($self, TB_ARRANGE, sub { $self->arrange; });
        EVT_TOOL($self, TB_MORE, sub { $self->increase; });
        EVT_TOOL($self, TB_FEWER, sub { $self->decrease; });
        if ($Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz' || $Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz-big') {
            EVT_TOOL($self, TB_X90CW, sub { $_[0]->rotate(-90, X) });
            EVT_TOOL($self, TB_X90CCW, sub { $_[0]->rotate(90, X) });
            EVT_TOOL($self, TB_Y90CW, sub { $_[0]->rotate(-90, Y) });
            EVT_TOOL($self, TB_Y90CCW, sub { $_[0]->rotate(90, Y) });
            EVT_TOOL($self, TB_Z90CW, sub { $_[0]->rotate(-90, Z) });
            EVT_TOOL($self, TB_Z90CCW, sub { $_[0]->rotate(90, Z) });
        }
        EVT_TOOL($self, TB_45CW, sub { $_[0]->rotate(-45, Z) });
        EVT_TOOL($self, TB_45CCW, sub { $_[0]->rotate(45, Z) });
        EVT_TOOL($self, TB_ROTFACE, sub { $_[0]->rotate_face });

        EVT_TOOL($self, TB_SCALE, sub { $self->changescale(undef); });
        EVT_TOOL($self, TB_SPLIT, sub { $self->split_object; });
        EVT_TOOL($self, TB_CUT, sub { $_[0]->object_cut_dialog });
        EVT_TOOL($self, TB_LAYERS, sub { $_[0]->object_layers_dialog });
        EVT_TOOL($self, TB_SETTINGS, sub { $_[0]->object_settings_dialog });
    } else {
        EVT_BUTTON($self, $self->{btn_add}, sub { $self->add; });
        EVT_BUTTON($self, $self->{btn_remove}, sub { $self->remove() }); # explicitly pass no argument to remove
        EVT_BUTTON($self, $self->{btn_reset}, sub { $self->reset; });
        EVT_BUTTON($self, $self->{btn_arrange}, sub { $self->arrange; });
        EVT_BUTTON($self, $self->{btn_increase}, sub { $self->increase; });
        EVT_BUTTON($self, $self->{btn_decrease}, sub { $self->decrease; });

        if ($Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz' || $Slic3r::GUI::Settings->{_}{rotation_controls} eq 'xyz-big') {
            EVT_BUTTON($self, $self->{btn_rotateX90cw}, sub { $_[0]->rotate(-90, X) });
            EVT_BUTTON($self, $self->{btn_rotateX90ccw}, sub { $_[0]->rotate(90, X) });
            EVT_BUTTON($self, $self->{btn_rotateY90cw}, sub { $_[0]->rotate(-90, Y) });
            EVT_BUTTON($self, $self->{btn_rotateY90ccw}, sub { $_[0]->rotate(90, Y) });
            EVT_BUTTON($self, $self->{btn_rotateZ90cw}, sub { $_[0]->rotate(-90, Z) });
            EVT_BUTTON($self, $self->{btn_rotateZ90ccw}, sub { $_[0]->rotate(90, Z) });
        }
        EVT_BUTTON($self, $self->{btn_rotateZ45cw}, sub { $_[0]->rotate(-45, Z) });
        EVT_BUTTON($self, $self->{btn_rotateZ45ccw}, sub { $_[0]->rotate(45, Z) });
        EVT_BUTTON($self, $self->{btn_rotateFace}, sub { $_[0]->rotate_face });

        EVT_BUTTON($self, $self->{btn_rotate45cw}, sub { $_[0]->rotate(-45) });
        EVT_BUTTON($self, $self->{btn_rotate45ccw}, sub { $_[0]->rotate(45) });
        
        EVT_BUTTON($self, $self->{btn_changescale}, sub { $self->changescale(undef); });
        EVT_BUTTON($self, $self->{btn_split}, sub { $self->split_object; });
        EVT_BUTTON($self, $self->{btn_cut}, sub { $_[0]->object_cut_dialog });
        EVT_BUTTON($self, $self->{btn_layers}, sub { $_[0]->object_layers_dialog });
        EVT_BUTTON($self, $self->{btn_settings}, sub { $_[0]->object_settings_dialog });
    }
    
    $_->SetDropTarget(Slic3r::GUI::Plater::DropTarget->new($self))
        for grep defined($_),
            $self, $self->{canvas}, $self->{canvas3D}, $self->{preview3D};
    
    EVT_COMMAND($self, -1, $THUMBNAIL_DONE_EVENT, sub {
        my ($self, $event) = @_;
        my ($obj_idx) = @{$event->GetData};
        return if !$self->{objects}[$obj_idx];  # object was deleted before thumbnail generation completed
        
        $self->on_thumbnail_made($obj_idx);
    });
    
    EVT_COMMAND($self, -1, $PROGRESS_BAR_EVENT, sub {
        my ($self, $event) = @_;
        my ($percent, $message) = @{$event->GetData};
        $self->on_progress_event($percent, $message);
    });
    
    EVT_COMMAND($self, -1, $ERROR_EVENT, sub {
        my ($self, $event) = @_;
        Slic3r::GUI::show_error($self, @{$event->GetData});
    });
    
    EVT_COMMAND($self, -1, $EXPORT_COMPLETED_EVENT, sub {
        my ($self, $event) = @_;
        $self->on_export_completed($event->GetData);
    });
    
    EVT_COMMAND($self, -1, $PROCESS_COMPLETED_EVENT, sub {
        my ($self, $event) = @_;
        $self->on_process_completed($event->GetData);
    });
    
    if ($Slic3r::have_threads) {
        my $timer_id = Wx::NewId();
        $self->{apply_config_timer} = Wx::Timer->new($self, $timer_id);
        EVT_TIMER($self, $timer_id, sub {
            my ($self, $event) = @_;
            $self->async_apply_config;
        });
    }
    
    $self->{canvas}->update_bed_size;
    if ($self->{canvas3D}) {
        $self->{canvas3D}->update_bed_size;
        $self->{canvas3D}->zoom_to_bed;
    }
    if ($self->{preview3D}) {
        $self->{preview3D}->set_bed_shape($self->{config}->bed_shape);
    }
    
    {
        my $presets = $self->{presets_sizer} = Wx::FlexGridSizer->new(3, 3, 1, 2);
        $presets->AddGrowableCol(1, 1);
        $presets->SetFlexibleDirection(wxHORIZONTAL);
        my %group_labels = (
            print       => 'Print settings',
            filament    => 'Filament',
            printer     => 'Printer',
        );
        $self->{preset_choosers} = {};
        $self->{preset_choosers_names} = {};  # wxChoice* => []
        for my $group (qw(print filament printer)) {
            # label
            my $text = Wx::StaticText->new($self, -1, "$group_labels{$group}:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
            $text->SetFont($Slic3r::GUI::small_font);
            
            # dropdown control
            my $choice = Wx::BitmapComboBox->new($self, -1, "", wxDefaultPosition, wxDefaultSize, [], wxCB_READONLY);
            $self->{preset_choosers}{$group} = [$choice];
            # setup the listener
            EVT_COMBOBOX($choice, $choice, sub {
                my ($choice) = @_;
                wxTheApp->CallAfter(sub {
                    $self->_on_change_combobox($group, $choice);
                });
            });
            
            # settings button
            my $settings_btn = Wx::BitmapButton->new($self, -1, Wx::Bitmap->new($Slic3r::var->("cog.png"), wxBITMAP_TYPE_PNG), 
                wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            EVT_BUTTON($self, $settings_btn, sub {
                $self->show_preset_editor($group, 0);
            });
            
            $presets->Add($text, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            $presets->Add($choice, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND | wxBOTTOM, 0);
            $presets->Add($settings_btn, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND | wxLEFT, 3);
        }
        
        {
            my $o = $self->{settings_override_panel} = Slic3r::GUI::Plater::OverrideSettingsPanel->new($self,
                on_change => sub {
                    my ($opt_key) = @_;
                    
                    my ($preset) = $self->selected_presets('print');
                    $preset->load_config;
                    
                    # If this option is not in the override panel it means it was manually deleted,
                    # so let's restore the profile value.
                    if (!$self->{settings_override_config}->has($opt_key)) {
                        $preset->_dirty_config->set($opt_key, $preset->_config->get($opt_key));
                    } else {
                        # Apply the overrides to the current Print preset, potentially making it dirty
                        $preset->_dirty_config->apply($self->{settings_override_config});
                        
                        # If this is a configured shortcut (and not just a dirty option),
                        # save it now.
                        if (any { $_ eq $opt_key } @{$preset->dirty_config->shortcuts}) {
                            $preset->save([$opt_key]);
                        }
                    }
                    
                    $self->load_presets;
                    $self->config_changed;
                    
                    # Reload the open tab if any
                    if (my $print_tab = $self->GetFrame->{preset_editor_tabs}{print}) {
                        $print_tab->load_presets;
                        $print_tab->reload_preset;
                    }
                });
            $o->can_add(0);
            $o->can_delete(1);
            $o->set_opt_keys([ Slic3r::GUI::PresetEditor::Print->options ]);
            $self->{settings_override_config} = Slic3r::Config->new;
            $o->set_default_config($self->{settings_override_config});
            $o->set_config($self->{settings_override_config});
        }
        
        my $object_info_sizer;
        {
            my $box = Wx::StaticBox->new($self, -1, "Info");
            $object_info_sizer = Wx::StaticBoxSizer->new($box, wxVERTICAL);
            $object_info_sizer->SetMinSize([350,-1]);
            
            {
                my $sizer = Wx::BoxSizer->new(wxHORIZONTAL);
                $object_info_sizer->Add($sizer, 0, wxEXPAND | wxBOTTOM, 5);
                my $text = Wx::StaticText->new($self, -1, "Object:", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
                $text->SetFont($Slic3r::GUI::small_font);
                $sizer->Add($text, 0, wxALIGN_CENTER_VERTICAL);
                
                # We supply a bogus width to wxChoice (sizer will override it and stretch 
                # the control anyway), because if we leave the default (-1) it will stretch
                # too much according to the contents, and this is bad with long file names.
                $self->{object_info_choice} = Wx::Choice->new($self, -1, wxDefaultPosition, [100,-1], []);
                $self->{object_info_choice}->SetFont($Slic3r::GUI::small_font);
                $sizer->Add($self->{object_info_choice}, 1, wxALIGN_CENTER_VERTICAL);
                
                EVT_CHOICE($self, $self->{object_info_choice}, sub {
                    $self->select_object($self->{object_info_choice}->GetSelection);
                    $self->refresh_canvases;
                });
            }
            
            my $grid_sizer = Wx::FlexGridSizer->new(3, 4, 5, 5);
            $grid_sizer->SetFlexibleDirection(wxHORIZONTAL);
            $grid_sizer->AddGrowableCol(1, 1);
            $grid_sizer->AddGrowableCol(3, 1);
            $object_info_sizer->Add($grid_sizer, 0, wxEXPAND);
            
            my @info = (
                copies      => "Copies",
                size        => "Size",
                volume      => "Volume",
                facets      => "Facets",
                materials   => "Materials",
                manifold    => "Manifold",
            );
            while (my $field = shift @info) {
                my $label = shift @info;
                my $text = Wx::StaticText->new($self, -1, "$label:", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
                $text->SetFont($Slic3r::GUI::small_font);
                $grid_sizer->Add($text, 0);
                
                $self->{"object_info_$field"} = Wx::StaticText->new($self, -1, "", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
                $self->{"object_info_$field"}->SetFont($Slic3r::GUI::small_font);
                if ($field eq 'manifold') {
                    $self->{object_info_manifold_warning_icon} = Wx::StaticBitmap->new($self, -1, Wx::Bitmap->new($Slic3r::var->("error.png"), wxBITMAP_TYPE_PNG));
                    $self->{object_info_manifold_warning_icon}->Hide;
                    
                    my $h_sizer = Wx::BoxSizer->new(wxHORIZONTAL);
                    $h_sizer->Add($self->{object_info_manifold_warning_icon}, 0);
                    $h_sizer->Add($self->{"object_info_$field"}, 0);
                    $grid_sizer->Add($h_sizer, 0, wxEXPAND);
                } else {
                    $grid_sizer->Add($self->{"object_info_$field"}, 0);
                }
            }
        }

        my $print_info_sizer;
        {
            my $box = Wx::StaticBox->new($self, -1, "Print Summary");
            $print_info_sizer = Wx::StaticBoxSizer->new($box, wxVERTICAL);
            $print_info_sizer->SetMinSize([350,-1]);
            my $grid_sizer = Wx::FlexGridSizer->new(2, 2, 5, 5);
            $grid_sizer->SetFlexibleDirection(wxHORIZONTAL);
            $grid_sizer->AddGrowableCol(1, 1);
            $grid_sizer->AddGrowableCol(3, 1);
            $print_info_sizer->Add($grid_sizer, 0, wxEXPAND);
            my @info = (
                fil     => "Used Filament",
                cost    => "Cost",
                tim     => "Time",
            );
            while (my $field = shift @info) {
                my $label = shift @info;
                my $text = Wx::StaticText->new($self, -1, "$label:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
                $text->SetFont($Slic3r::GUI::small_font);
                $grid_sizer->Add($text, 0);
                
                $self->{"print_info_$field"} = Wx::StaticText->new($self, -1, "", wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
                $self->{"print_info_$field"}->SetFont($Slic3r::GUI::small_font);
                $grid_sizer->Add($self->{"print_info_$field"}, 0);
            }
            $self->{sliced_info_box} = $print_info_sizer;
            
        }
        
        my $buttons_sizer = Wx::BoxSizer->new(wxHORIZONTAL);
        $buttons_sizer->AddStretchSpacer(1);
        $buttons_sizer->Add($self->{btn_export_stl}, 0, wxALIGN_RIGHT, 0);
        $buttons_sizer->Add($self->{btn_print}, 0, wxALIGN_RIGHT, 0);
        $buttons_sizer->Add($self->{btn_send_gcode}, 0, wxALIGN_RIGHT, 0);
        $buttons_sizer->Add($self->{btn_export_gcode}, 0, wxALIGN_RIGHT, 0);
        
        $self->{right_sizer} = my $right_sizer = Wx::BoxSizer->new(wxVERTICAL);
        $right_sizer->Add($presets, 0, wxEXPAND | wxTOP, 10) if defined $presets;
        $right_sizer->Add($buttons_sizer, 0, wxEXPAND | wxBOTTOM, 5);
        $right_sizer->Add($self->{settings_override_panel}, 1, wxEXPAND, 5);
        $right_sizer->Add($object_info_sizer, 0, wxEXPAND, 0);
        $right_sizer->Add($print_info_sizer, 0, wxEXPAND, 0);
        $right_sizer->Hide($print_info_sizer);
        
        my $hsizer = Wx::BoxSizer->new(wxHORIZONTAL);
        $hsizer->Add($self->{preview_notebook}, 1, wxEXPAND | wxTOP, 1);
        $hsizer->Add($right_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 3);
        
        my $sizer = Wx::BoxSizer->new(wxVERTICAL);
        $sizer->Add($self->{htoolbar}, 0, wxEXPAND, 0) if $self->{htoolbar};
        $sizer->Add($self->{btoolbar}, 0, wxEXPAND, 0) if $self->{btoolbar};
        $sizer->Add($hsizer, 1, wxEXPAND, 0);
        
        $sizer->SetSizeHints($self);
        $self->SetSizer($sizer);
    }
    
    $self->load_presets;
    $self->_on_select_preset($_) for qw(printer filament print);
    
    return $self;
}

sub prompt_unsaved_changes {
    my ($self) = @_;
    
    foreach my $group (qw(printer filament print)) {
        foreach my $choice (@{$self->{preset_choosers}{$group}}) {
            my $pp = $self->{preset_choosers_names}{$choice};
            for my $i (0..$#$pp) {
                my $preset = first { $_->name eq $pp->[$i] } @{wxTheApp->presets->{$group}};
                if (!$preset->prompt_unsaved_changes($self)) {
                    # Restore the previous one
                    $choice->SetSelection($i);
                    return 0;
                }
            }
        }
    }
    return 1;
}

sub _on_change_combobox {
    my ($self, $group, $choice) = @_;
    
    if (0) {
        # This code is disabled because wxPerl doesn't provide GetCurrentSelection
        my $current_name = $self->{preset_choosers_names}{$choice}[$choice->GetCurrentSelection];
        my $current = first { $_->name eq $current_name } @{wxTheApp->presets->{$group}};
        if (!$current->prompt_unsaved_changes($self)) {
            # Restore the previous one
            $choice->SetSelection($choice->GetCurrentSelection);
            return;
        }
    } else {
        return 0 if !$self->prompt_unsaved_changes;
    }
    wxTheApp->CallAfter(sub {
        $self->_on_select_preset($group);
        
        # This will remove the "(modified)" mark from any dirty preset handled here.
        $self->load_presets;
    });
}

sub _on_select_preset {
	my ($self, $group) = @_;
	
	my @presets = $self->selected_presets($group);
	
	my $s_presets = $Slic3r::GUI::Settings->{presets};
	my $changed = !$s_presets->{$group} || $s_presets->{$group} ne $presets[0]->name;
    $s_presets->{$group} = $presets[0]->name;
    $s_presets->{"${group}_${_}"} = $presets[$_]->name for 1..$#presets;
	
	wxTheApp->save_settings;
	
	# Ignore overrides in the plater, we only care about the preset configs.
	my $config = $self->config(1);
	
	$self->on_extruders_change(scalar @{$config->get('nozzle_diameter')});
    
    if ($group eq 'print') {
        my $o_config = $self->{settings_override_config};
        my $o_panel  = $self->{settings_override_panel};
        
        my $shortcuts = $config->get('shortcuts');
        
        # Re-populate the override panel with the configured shortcuts
        # and the dirty options.
        $o_config->clear;
        foreach my $opt_key (@$shortcuts, $presets[0]->dirty_options) {
            # Don't add shortcut for shortcuts!
            next if $opt_key eq 'shortcuts';
            $o_config->set($opt_key, $config->get($opt_key));
        }
        
        $o_panel->set_default_config($config);
        $o_panel->set_fixed_options(\@$shortcuts);
        $o_panel->update_optgroup;
    } elsif ($group eq 'printer') {
        # reload print and filament settings to honor their compatible_printer options
        $self->load_presets;
    }
    
    $self->config_changed;
}

sub load_config {
    my ($self, $config) = @_;
    
    # This method is called with the CLI options.
    # We add them to the visible overrides.
    $self->{settings_override_config}->apply($config);
    $self->{settings_override_panel}->update_optgroup;
    
    $self->config_changed;
}

sub GetFrame {
    my ($self) = @_;
    return &Wx::GetTopLevelParent($self);
}

sub load_presets {
    my ($self) = @_;
    
    my $selected_printer_name;
    foreach my $group (qw(printer filament print)) {
        my @presets = @{wxTheApp->presets->{$group}};
        
        # Skip presets not compatible with the selected printer, if they
        # have other compatible printers configured (and at least one of them exists).
        if ($group eq 'filament' || $group eq 'print') {
            my %printer_names = map { $_->name => 1 } @{ wxTheApp->presets->{printer} };
            for (my $i = 0; $i <= $#presets; ++$i) {
                my $config = $presets[$i]->dirty_config;
                next if !$config->has('compatible_printers');
                my @compat = @{$config->compatible_printers};
                if (@compat
                    && (none { $_ eq $selected_printer_name } @compat)
                    && (any { $printer_names{$_} } @compat)) {
                    splice @presets, $i, 1;
                    --$i;
                }
            }
        }
        
        # Only show the default presets if we have no other presets.
        if (@presets > 1) {
            @presets = grep { !$_->default } @presets;
        }
        
        # get the wxChoice objects for this group
        my @choosers = @{ $self->{preset_choosers}{$group} };
        
        # find the currently selected one(s) according to the saved file
        my @sel = ();
        if (my $current = $Slic3r::GUI::Settings->{presets}{$group}) {
            push @sel, grep defined, first { $presets[$_]->name eq $current } 0..$#presets;
        }
        for my $i (1..(@choosers-1)) {
            if (my $current = $Slic3r::GUI::Settings->{presets}{"${group}_$i"}) {
                push @sel, grep defined, first { $presets[$_]->name eq $current } 0..$#presets;
            }
        }
        @sel = (0) if !@sel;
        
        # populate the wxChoice objects
        my @preset_names = ();
        foreach my $choice (@choosers) {
            $choice->Clear;
            $self->{preset_choosers_names}{$choice} = [];
            foreach my $preset (@presets) {
                # load/generate the proper icon
                my $bitmap;
                if ($group eq 'filament') {
                    my $config = $preset->dirty_config;
                    if ($preset->default || !$config->has('filament_colour')) {
                        $bitmap = Wx::Bitmap->new($Slic3r::var->("spool.png"), wxBITMAP_TYPE_PNG);
                    } else {
                        my $rgb_hex = $config->filament_colour->[0];
                    
                        $rgb_hex =~ s/^#//;
                        my @rgb = unpack 'C*', pack 'H*', $rgb_hex;
                        my $image = Wx::Image->new(16,16);
                        $image->SetRGB(Wx::Rect->new(0,0,16,16), @rgb);
                        $bitmap = Wx::Bitmap->new($image);
                    }
                } elsif ($group eq 'print') {
                    $bitmap = Wx::Bitmap->new($Slic3r::var->("cog.png"), wxBITMAP_TYPE_PNG);
                } elsif ($group eq 'printer') {
                    $bitmap = Wx::Bitmap->new($Slic3r::var->("printer_empty.png"), wxBITMAP_TYPE_PNG);
                }
                $choice->AppendString($preset->dropdown_name, $bitmap);
                push @{$self->{preset_choosers_names}{$choice}}, $preset->name;
            }
            
            my $selected = shift @sel;
            if (defined $selected && $selected <= $#presets) {
                # call SetSelection() only after SetString() otherwise the new string
                # won't be picked up as the visible string
                $choice->SetSelection($selected);
                
                my $preset_name = $self->{preset_choosers_names}{$choice}[$selected];
                push @preset_names, $preset_name;
                # TODO: populate other filament preset placeholders
                $selected_printer_name = $preset_name if $group eq 'printer';
            }
        }
        
        $self->{print}->placeholder_parser->set_multiple("${group}_preset", [ @preset_names ]);
    }
}

sub select_preset_by_name {
    my ($self, $name, $group, $n) = @_;
    
    # $n is optional
    
    my $presets = wxTheApp->presets->{$group};
    my $choosers = $self->{preset_choosers}{$group};
    my $names = $self->{preset_choosers_names}{$choosers->[0]};
    my $i = first { $names->[$_] eq $name } 0..$#$names;
    return if !defined $i;
    
    if (defined $n && $n <= $#$choosers) {
        $choosers->[$n]->SetSelection($i);
    } else {
        $_->SetSelection($i) for @$choosers;
    }
    $self->_on_select_preset($group);
}

sub selected_presets {
    my ($self, $group) = @_;
    
    my %presets = ();
    foreach my $group (qw(printer filament print)) {
        $presets{$group} = [];
        foreach my $choice (@{$self->{preset_choosers}{$group}}) {
            my $sel = $choice->GetSelection;
            $sel = 0 if $sel == -1;
            push @{ $presets{$group} },
                grep { $_->name eq $self->{preset_choosers_names}{$choice}[$sel] }
                @{wxTheApp->presets->{$group}};
        }
    }
    return $group ? @{$presets{$group}} : %presets;
}

sub show_unique_preset_dialog {
    my($self, $group) = @_;
    my $dlg;
    my $preset_editor;
    if( $self->{'preset_dialogs'}->{$group} ) {
        $dlg = $self->{'preset_dialogs'}->{$group};
    }
    else {        
        my $class = "Slic3r::GUI::PresetEditorDialog::" . ucfirst($group);
        $dlg = $class->new($self);
        $self->{'preset_dialogs'}->{$group} = $dlg;        
    }
    $dlg->Show;    
    $preset_editor = $dlg->preset_editor;
    return $preset_editor;
}

sub show_preset_editor {
    my ($self, $group, $i, $panel) = @_;
    
    wxTheApp->CallAfter(sub {
        my @presets = $self->selected_presets($group);
    
        my $preset_editor;
        my $mainframe = $self->GetFrame;
        my $tabpanel = $mainframe->{tabpanel};
        if (exists $mainframe->{preset_editor_tabs}{$group}) {
            my $tabindex = 0;
            $tabindex = 1 if $Slic3r::GUI::Settings->{_}{show_host};
            $tabindex += 1 if $group eq 'print';
            $tabindex += 2 if $group eq 'filament';
            $tabindex += 3 if $group eq 'printer';
            # we already have an open editor
            $tabpanel->SetSelection($tabindex);
            return;
        } elsif ($Slic3r::GUI::Settings->{_}{tabbed_preset_editors}) {
            my $class = "Slic3r::GUI::PresetEditor::" . ucfirst($group);
            $mainframe->{preset_editor_tabs}{$group} = $preset_editor = $class->new($tabpanel);
            $tabpanel->AddPage($preset_editor, ucfirst($group) . " Settings");
        } else {
           $preset_editor = $self->show_unique_preset_dialog($group);
        }
    
        $preset_editor->select_preset_by_name($presets[$i // 0]->name);
        $preset_editor->on_value_change(sub {
            # Re-load the presets in order to toggle the (modified) suffix
            $self->load_presets;
        
            # Update shortcuts
            $self->_on_select_preset($group);
        
            # Use the new config wherever we actually use its contents
            $self->config_changed;
        });
        my $cb = sub {
            my ($group, $preset) = @_;
        
            # Re-load the presets as they might have changed.
            $self->load_presets;
        
            # Select the preset in plater too
            $self->select_preset_by_name($preset->name, $group, $i, 1);
        };
        $preset_editor->on_select_preset($cb);
        $preset_editor->on_save_preset($cb);
    });
}

# Returns the current config by merging the selected presets and the overrides.
sub config {
    my ($self, $ignore_overrides) = @_;
    
    # use a DynamicConfig because FullPrintConfig is not enough
    my $config = Slic3r::Config->new_from_defaults;
    
    # get defaults also for the values tracked by the Plater's config
    # (for example 'shortcuts')
    $config->apply(Slic3r::Config->new_from_defaults(@{$self->{config}->get_keys}));
    
    my %classes = map { $_ => "Slic3r::GUI::PresetEditor::".ucfirst($_) }
        qw(print filament printer);
    
    my %presets = $self->selected_presets;
    $config->apply($_->dirty_config) for @{ $presets{printer} };
    if (@{ $presets{filament} }) {
        my $filament_config = $presets{filament}[0]->dirty_config;
        
        for my $i (1..$#{ $presets{filament} }) {
            my $preset = $presets{filament}[$i];
            my $config = $preset->dirty_config;
            foreach my $opt_key (@{$config->get_keys}) {
                if ($filament_config->has($opt_key)) {
                    my $value = $filament_config->get($opt_key);
                    next unless ref $value eq 'ARRAY';
                    $value->[$i] = $config->get($opt_key)->[0];
                    $filament_config->set($opt_key, $value);
                }
            }
        }
        
        $config->apply($filament_config);
    }
    $config->apply($_->dirty_config) for @{ $presets{print} };
    $config->apply($self->{settings_override_config})
        unless $ignore_overrides;
    
    return $config;
}

sub get_object_index {
    my $self = shift;
    my ($object_indentifier) = @_;
    return undef if !defined $object_indentifier;

    for (my $i = 0; $i <= $#{$self->{objects}}; $i++){
        if ($self->{objects}->[$i]->identifier eq $object_indentifier) {
            return $i;
        }
    }
    return undef;
}

sub add_undo_operation {
    my $self = shift;
    my @parameters = @_;

    my $type = $parameters[0];
    my $object_identifier = $parameters[1];
    my @attributes = @parameters[2..$#parameters]; # operation values.

    my $new_undo_operation = new Slic3r::GUI::Plater::UndoOperation($type, $object_identifier, \@attributes);

    push @{$self->{undo_stack}}, $new_undo_operation;

    $self->{redo_stack} = [];

    $self->limit_undo_operations(8); # Current limit of undo/redo operations.
    $self->GetFrame->on_undo_redo_stacks_changed;

    return $new_undo_operation;
}

sub limit_undo_operations {
    my ($self, $limit)= @_;
    return if !defined $limit;
    # Delete undo operations succeeded by 4 operations or more to save memory.
    while ($#{$self->{undo_stack}} + 1 > $limit) {
        print "Removing an old operation.\n";
        splice @{$self->{undo_stack}}, 0, 1;
    }
}

sub undo {
    my $self = shift;

    my $operation = pop @{$self->{undo_stack}};
    return if !defined $operation;

    push @{$self->{redo_stack}}, $operation;

    my $type = $operation->{type};

    if ($type eq "TRANSFORM") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $trafo = $operation->{attributes}->[0];
        $self->transform($trafo->inverse(), 'true'); # Apply inverse transformation.

    } elsif ($type eq "ROTATE_Z") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $angle = $operation->{attributes}->[0];
        $self->rotate(-1 * $angle, Z, 'true'); # Apply inverse transformation.

    } elsif ($type eq "SCALE_UNIFORM") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $new_scale = $operation->{attributes}->[0];
        $self->changescale(undef, undef, $new_scale, 'true');

    } elsif ($type eq "INCREASE") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $copies = $operation->{attributes}->[0];
        $self->decrease($copies, 'true');

    } elsif ($type eq "DECREASE") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $copies = $operation->{attributes}->[0];
        $self->increase($copies, 'true');

    } elsif ($type eq "REMOVE") {
        my $_model = $operation->{attributes}->[0];
        $self->load_model_objects(@{$_model->objects});
        $self->{object_identifier}--; # Decrement the identifier as we will change the object identifier with the saved one.
        $self->{objects}->[-1]->identifier($operation->{object_identifier});

    } elsif ($type eq "CUT" || $type eq "SPLIT") {
        # Delete the produced objects.
        my $obj_identifiers_start = $operation->{attributes}->[2];
        for (my $i_object = 0; $i_object < $#{$operation->{attributes}->[1]->objects} + 1; $i_object++) {
            $self->remove($self->get_object_index($obj_identifiers_start++), 'true');
        }
        # Add the original object.
        $self->load_model_objects(@{$operation->{attributes}->[0]->objects});
        $self->{object_identifier}--;
        $self->{objects}->[-1]->identifier($operation->{object_identifier}); # Add the original assigned identifier.

    } elsif ($type eq "RESET") {
        # Revert changes to the plater object identifier. It's modified when adding new objects only not when undo/redo is executed.
        my $current_objects_identifier = $self->{object_identifier};
        my $_model = $operation->{attributes}->[0];
        $self->load_model_objects(@{$_model->objects});
        $self->{object_identifier} = $current_objects_identifier;

        # don't forget the identifiers.
        my $objects_count = $#{$operation->{attributes}->[0]->objects} + 1;

        foreach my $identifier (@{$operation->{attributes}->[1]})
        {
            $self->{objects}->[-$objects_count]->identifier($identifier);
            $objects_count--;
        }

    } elsif ($type eq "ADD") {
        my $objects_count = $#{$operation->{attributes}->[0]->objects} + 1;
        my $identifier_start = $operation->{attributes}->[1];
        for (my $identifier = $identifier_start; $identifier < $objects_count + $identifier_start; $identifier++) {
			my $obj_idx = $self->get_object_index($identifier);
            $self->remove($obj_idx, 'true');
        }
    } elsif ($type eq "GROUP"){
        my @ops = @{$operation->{attributes}};
        push @{$self->{undo_stack}}, @ops;
        foreach my $op (@ops) {
            $self->undo;
            pop @{$self->{redo_stack}};
        }
    }
}

sub redo {
    my $self = shift;

    my $operation = pop @{$self->{redo_stack}};
    return if !defined $operation;

    push @{$self->{undo_stack}}, $operation;

    my $type = $operation->{type};

    if ($type eq "TRANSFORM") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $trafo = $operation->{attributes}->[0];
        $self->transform($trafo, 'true'); # Reapply transformation.

    } elsif ($type eq "ROTATE_Z") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $angle = $operation->{attributes}->[0];
        $self->rotate($angle, Z, 'true'); # Reapply transformation.

    } elsif ($type eq "SCALE_UNIFORM") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $new_scale = $operation->{attributes}->[1];
        $self->changescale(undef, undef, $new_scale, 'true');

    } elsif ($type eq "INCREASE") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $copies = $operation->{attributes}->[0];
        $self->increase($copies, 'true');

    } elsif ($type eq "DECREASE") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        my $copies = $operation->{attributes}->[0];
        $self->decrease($copies, 'true');

    } elsif ($type eq "REMOVE") {
        my $object_id = $operation->{object_identifier};
        my $obj_idx = $self->get_object_index($object_id);
        $self->select_object($obj_idx);

        $self->remove(undef, 'true');

    } elsif ($type eq "CUT" || $type eq "SPLIT") {
        # Delete the org objects.
        $self->remove($self->get_object_index($operation->{object_identifier}), 'true');
        # Add the new objects and revert changes to the plater object identifier.
        my $current_objects_identifier = $self->{object_identifier};
        $self->load_model_objects(@{$operation->{attributes}->[1]->objects});
        $self->{object_identifier} = $current_objects_identifier;
        # Add their identifiers.
        my $obj_identifiers_start = $operation->{attributes}->[2];
        my $obj_count = $#{$operation->{attributes}->[1]->objects} + 1;
        for (my $i_object = 0; $i_object <= $#{$operation->{attributes}->[1]->objects}; $i_object++){
            $self->{objects}->[-$obj_count]->identifier($obj_identifiers_start++);
            $obj_count--;
        }
    } elsif ($type eq "RESET") {
        $self->reset('true');
    } elsif ($type eq "ADD") {
        # Revert changes to the plater object identifier. It's modified when adding new objects only not when undo/redo is executed.
        my $current_objects_identifier = $self->{object_identifier};
        $self->load_model_objects(@{$operation->{attributes}->[0]->objects});
        $self->{object_identifier} = $current_objects_identifier;

        my $objects_count = $#{$operation->{attributes}->[0]->objects} + 1;
        my $start_identifier = $operation->{attributes}->[1];
        foreach my $object (@{$operation->{attributes}->[0]->objects})
        {
            $self->{objects}->[-$objects_count]->identifier($start_identifier++);
            $objects_count--;
        }
    } elsif ($type eq "GROUP"){
        my @ops = @{$operation->{attributes}};
        foreach my $op (@ops) {
            push @{$self->{redo_stack}}, $op;
            $self->redo;
            pop @{$self->{undo_stack}};
        }
    }
}

sub add {
    my $self = shift;
    
    # Save the current object identifier to track added objects.
    my $start_object_id = $self->{object_identifier};
    
    my @input_files = wxTheApp->open_model($self);
    $self->load_file($_) for @input_files;
    
    # Check if no objects are added.
    if ($start_object_id == $self->{object_identifier}) {
		return;
	}
    
    # Save the added objects.
    my $new_model = $self->{model}->new;
    
    # Get newly added objects count.
    my $new_objects_count = $self->{object_identifier} - $start_object_id;
    for (my $i_object = $start_object_id; $i_object < $new_objects_count + $start_object_id; $i_object++){
			my $object_index =  $self->get_object_index($i_object);
            $new_model->add_object($self->{model}->get_object($object_index));
    }

    $self->add_undo_operation("ADD", undef, $new_model, $start_object_id);
}

sub add_tin {
    my $self = shift;
    
    my @input_files = wxTheApp->open_model($self);
    return if !@input_files;
    
    my $offset = Wx::GetNumberFromUser("", "Enter the minimum thickness in mm (i.e. the offset from the lowest point):", "2.5D TIN",
        5, 0, 1000000, $self);
    return if $offset < 0;
    
    foreach my $input_file (@input_files) {
        my $model = eval { Slic3r::Model->read_from_file($input_file) };
        Slic3r::GUI::show_error($self, $@) if $@;
        next if !$model;

        if ($model->looks_like_multipart_object) {
            Slic3r::GUI::show_error($self, "Multi-part models cannot be opened as 2.5D TIN files. Please load a single continuous mesh.");
            next;
        }
        
        my $model_object = $model->get_object(0);
        eval {
            $model_object->get_volume(0)->extrude_tin($offset);
        };
        Slic3r::GUI::show_error($self, $@) if $@;
        
        $self->load_model_objects($model_object);
    }
}

sub load_file {
    my $self = shift;
    my ($input_file, $obj_idx_to_load) = @_;
    
    $Slic3r::GUI::Settings->{recent}{skein_directory} = dirname($input_file);
    wxTheApp->save_settings;
    
    my $process_dialog = Wx::ProgressDialog->new('Loading…', "Processing input file…", 100, $self, 0);
    $process_dialog->Pulse;
    
    local $SIG{__WARN__} = Slic3r::GUI::warning_catcher($self);
    
    my $model = eval { Slic3r::Model->read_from_file($input_file) };
    Slic3r::GUI::show_error($self, $@) if $@;
    
    my @obj_idx = ();
    if (defined $model) {
        if ($model->looks_like_multipart_object) {
            my $dialog = Wx::MessageDialog->new($self,
                "This file contains several objects positioned at multiple heights. "
                . "Instead of considering them as multiple objects, should I consider\n"
                . "this file as a single object having multiple parts?\n",
                'Multi-part object detected', wxICON_WARNING | wxYES | wxNO);
            if ($dialog->ShowModal() == wxID_YES) {
                $model->convert_multipart_object;
            }
        }
        
        for my $obj_idx (0..($model->objects_count-1)) {
            my $object = $model->objects->[$obj_idx];
            $object->set_input_file($input_file);
            for my $vol_idx (0..($object->volumes_count-1)) {
                my $volume = $object->get_volume($vol_idx);
                $volume->set_input_file($input_file);
                $volume->set_input_file_obj_idx($obj_idx);
                $volume->set_input_file_obj_idx($vol_idx);
            }
        }
        
        my $i = 0;
        
        if (defined $obj_idx_to_load) {
            return () if $obj_idx_to_load >= $model->objects_count;
            @obj_idx = $self->load_model_objects($model->get_object($obj_idx_to_load));
            $i = $obj_idx_to_load;
        } else {
            @obj_idx = $self->load_model_objects(@{$model->objects});
        }
        
        foreach my $obj_idx (@obj_idx) {
            $self->{objects}[$obj_idx]->input_file($input_file);
            $self->{objects}[$obj_idx]->input_file_obj_idx($i++);
        }

        $self->statusbar->SetStatusText("Loaded " . basename($input_file));

        if($self->{scaled_down}) {
            $self->statusbar->SetStatusText('Your object appears to be too large, so it was automatically scaled down to fit your print bed.');
        }
        if($self->{outside_bounds}) {
            $self->statusbar->SetStatusText('Some of your object(s) appear to be outside the print bed. Use the arrange button to correct this.');
        }

    }
    
    $process_dialog->Destroy;

    # Empty the redo stack
    $self->{redo_stack} = [];

    return @obj_idx;
}

sub load_model_objects {
    my ($self, @model_objects) = @_;
    
    # Always restart background process when adding new objects.
    # This prevents lack of processing in some circumstances when background process is
    # running but adding a new object does not invalidate anything.
    $self->stop_background_process;
    
    my $bed_centerf = $self->bed_centerf;
    my $bed_shape = Slic3r::Polygon->new_scale(@{$self->{config}->bed_shape});
    my $bed_size = $bed_shape->bounding_box->size;
    
    my $need_arrange = 0;
    my @obj_idx = ();
    foreach my $model_object (@model_objects) {
        my $o = $self->{model}->add_object($model_object);
        $o->repair;

        push @{ $self->{objects} }, Slic3r::GUI::Plater::Object->new(
                name => $model_object->name || basename($model_object->input_file), identifier =>
                $self->{object_identifier}++
            );

        push @obj_idx, $#{ $self->{objects} };
    
        if ($model_object->instances_count == 0) {
            if ($Slic3r::GUI::Settings->{_}{autocenter}) {
                # if object has no defined position(s) we need to rearrange everything after loading
                $need_arrange = 1;

                # add a default instance and center object around origin
                $o->center_around_origin;  # also aligns object to Z = 0
                $o->add_instance(offset => $bed_centerf);
            } else {
                # if user turned autocentering off, automatic arranging would disappoint them
                $need_arrange = 0;

                if ($Slic3r::GUI::Settings->{_}{autoalignz}) {
                    $o->align_to_ground; # aligns object to Z = 0
                }
                $o->add_instance();
            }
        } else {
            if ($Slic3r::GUI::Settings->{_}{autoalignz}) {
                # if object has defined positions we still need to ensure it's aligned to Z = 0
                $o->align_to_ground;
            }
        }
        
        {
            # if the object is too large (more than 5 times the bed), scale it down
            my $size = $o->bounding_box->size;
            my $ratio = max(@$size[X,Y]) / unscale(max(@$bed_size[X,Y]));
            if ($ratio > 5) {
                $_->set_scaling_factor(1/$ratio) for @{$o->instances};
                $self->{scaled_down} = 1;
            }
        }

        {
           # if after scaling the object does not fit on the bed provide a warning
           my $bed_bounds = Slic3r::Geometry::BoundingBoxf->new_from_points($self->{config}->bed_shape);
           my $o_bounds = $o->bounding_box;
           my $min = Slic3r::Pointf->new($o_bounds->x_min, $o_bounds->y_min);
           my $max = Slic3r::Pointf->new($o_bounds->x_max, $o_bounds->y_max);
           if (!$bed_bounds->contains_point($min) || !$bed_bounds->contains_point($max))
           {
               $self->{outside_bounds} = 1;
           }
        }
    
        $self->{print}->auto_assign_extruders($o);
        $self->{print}->add_model_object($o);
    }
    
    $self->make_thumbnail($_) for @obj_idx;
    $self->arrange if $need_arrange;
    $self->on_model_change;
    
    # zoom to objects
    $self->{canvas3D}->zoom_to_volumes
        if $self->{canvas3D};
    
    $self->object_list_changed;
    
    return @obj_idx;
}

sub bed_centerf {
    my ($self) = @_;
    
    my $bed_shape = Slic3r::Polygon->new_scale(@{$self->{config}->bed_shape});
    my $bed_center = $bed_shape->bounding_box->center;
    return Slic3r::Pointf->new(unscale($bed_center->x), unscale($bed_center->y)); #)
}

sub remove {
    my $self = shift;
    my ($obj_idx, $dont_push) = @_;
    
    $self->stop_background_process;
    
    # Prevent toolpaths preview from rendering while we modify the Print object
    $self->{toolpaths2D}->enabled(0) if $self->{toolpaths2D};
    $self->{preview3D}->enabled(0) if $self->{preview3D};
    
    # if no object index is supplied, remove the selected one
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }

    # Save the object identifier and copy the object for undo/redo operations.
    my $object_id = $self->{objects}->[$obj_idx]->identifier;
    my $new_model = Slic3r::Model->new;  # store this before calling get_object()
    $new_model->add_object($self->{model}->get_object($obj_idx));

    splice @{$self->{objects}}, $obj_idx, 1;
    $self->{model}->delete_object($obj_idx);
    $self->{print}->delete_object($obj_idx);
    $self->object_list_changed;
    
    $self->select_object(undef);
    $self->on_model_change;

    if (!defined $dont_push) {
        $self->add_undo_operation("REMOVE", $object_id, $new_model);
    }
}

sub reset {
    my ($self, $dont_push) = @_;
    
    $self->stop_background_process;
    
    # Prevent toolpaths preview from rendering while we modify the Print object
    $self->{toolpaths2D}->enabled(0) if $self->{toolpaths2D};
    $self->{preview3D}->enabled(0) if $self->{preview3D};

    # Save the current model.
    my $current_model = $self->{model}->clone;

    if (!defined $dont_push) {
        # Get the identifiers of the current model objects.
        my $objects_identifiers = [];
        for (my $i = 0; $i <= $#{$self->{objects}}; $i++){
            push @{$objects_identifiers}, $self->{objects}->[$i]->identifier;
        }
        $self->add_undo_operation("RESET", undef, $current_model, $objects_identifiers);
    }

    @{$self->{objects}} = ();
    $self->{model}->clear_objects;
    $self->{print}->clear_objects;
    $self->object_list_changed;

    $self->select_object(undef);
    $self->on_model_change;
}

sub increase {
    my ($self, $copies, $dont_push) = @_;
    
    $copies //= 1;
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $instance = $model_object->instances->[-1];
    for my $i (1..$copies) {
        $instance = $model_object->add_instance(
            offset          => Slic3r::Pointf->new(map 10+$_, @{$instance->offset}),
            scaling_factor  => $instance->scaling_factor,
            rotation        => $instance->rotation,
        );
        $self->{print}->objects->[$obj_idx]->add_copy($instance->offset);
    }

    if (!defined $dont_push) {
        $self->add_undo_operation("INCREASE", $object->identifier , $copies);
    }

    # only autoarrange if user has autocentering enabled
    $self->stop_background_process;
    if ($Slic3r::GUI::Settings->{_}{autocenter}) {
        $self->arrange;
    } else {
        $self->on_model_change;
    }
}

sub decrease {
    my ($self, $copies, $dont_push) = @_;
    
    $copies //= 1;
    $self->stop_background_process;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    if ($model_object->instances_count > $copies) {
        for my $i (1..$copies) {
            $model_object->delete_last_instance;
            $self->{print}->objects->[$obj_idx]->delete_last_copy;
        }
        if (!defined $dont_push) {
            $self->add_undo_operation("DECREASE", $object->identifier, $copies);
        }
    } else {
        $self->remove;
    }
    
    $self->on_model_change;
}

sub set_number_of_copies {
    my ($self) = @_;
    
    $self->pause_background_process;
    
    # get current number of copies
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    
    # prompt user
    my $copies = Wx::GetNumberFromUser("", "Enter the number of copies of the selected object:", "Copies", $model_object->instances_count, 0, 1000, $self);
    return if $copies == -1;
    my $diff = $copies - $model_object->instances_count;
    if ($diff == 0) {
        # no variation
        $self->resume_background_process;
    } elsif ($diff > 0) {
        $self->increase($diff);
    } elsif ($diff < 0) {
        $self->decrease(-$diff);
    }
}

sub center_selected_object_on_bed {
    my ($self) = @_;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $bb = $model_object->bounding_box;
    my $size = $bb->size;
    my $vector = Slic3r::Pointf->new(
        $self->bed_centerf->x - $bb->x_min - $size->x/2,
        $self->bed_centerf->y - $bb->y_min - $size->y/2,    #//
    );
    $_->offset->translate(@$vector) for @{$model_object->instances};
    $self->on_model_change;
}

sub rotate_face {
    my $self = shift;
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    # Get the selected normal
    if (!$Slic3r::GUI::have_OpenGL) {
        Slic3r::GUI::show_error($self, "Please install the OpenGL modules to use this feature (see build instructions).");
        return;
    }
    my $dlg = Slic3r::GUI::Plater::ObjectRotateFaceDialog->new($self,
		object              => $self->{objects}[$obj_idx],
		model_object        => $self->{model}->objects->[$obj_idx],
	);
	return unless $dlg->ShowModal == wxID_OK;
    my $normal = $dlg->SelectedNormal;
    return if !defined $normal;
    my $axis = $dlg->SelectedAxis;
    return if !defined $axis;

    my $axis_vec = Slic3r::Pointf3->new(0,0,0);
    if($axis == Z){
        $axis_vec->set_z(-1);
    } else {
        if($axis == X){
            $axis_vec->set_x(-1);
        } else {
            $axis_vec->set_y(-1);
        }
    }

    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];

    $model_object->transform_by_instance($model_instance, 1);

    $model_object->reset_undo_trafo();
    $model_object->rotate_vec_to_vec($normal,$axis_vec);

    # realign object to Z = 0
    $model_object->align_to_ground;
    $self->make_thumbnail($obj_idx);

    $model_object->update_bounding_box;
    # update print and start background processing
    $self->{print}->add_model_object($model_object, $obj_idx);

    $self->selection_changed;  # refresh info (size etc.)
    $self->on_model_change;
    
    $self->add_undo_operation("TRANSFORM", $object->identifier, $model_object->get_undo_trafo());
}

sub rotate {
    my $self = shift;
    my ($angle, $axis, $dont_push) = @_;
    
    # angle is in degrees
    $axis //= Z;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # we need thumbnail to be computed before allowing rotation
    return if !$object->thumbnail;
    
    if (!defined $angle) {
        my $axis_name = $axis == X ? 'X' : $axis == Y ? 'Y' : 'Z';
        my $default = $axis == Z ? rad2deg($model_instance->rotation) : 0;
        # Wx::GetNumberFromUser() does not support decimal numbers
        $angle = Wx::GetTextFromUser("Enter the rotation angle:", "Rotate around $axis_name axis",
            $default, $self);
        return if !$angle || $angle !~ /^-?\d*(?:\.\d*)?$/ || $angle == -1;
        
        $angle = $angle - $default;
    }
    
    $self->stop_background_process;
    
    if ($axis == Z) {
        my $new_angle = deg2rad($angle);
        $_->set_rotation($_->rotation + $new_angle) for @{ $model_object->instances };
        $object->transform_thumbnail($self->{model}, $obj_idx);

        if (!defined $dont_push) {
            $self->add_undo_operation("ROTATE_Z", $object->identifier, $angle);
        }
    } else {
        # rotation around X and Y needs to be performed on mesh
        # so we first apply any Z rotation
        $model_object->transform_by_instance($model_instance, 1);

        $model_object->reset_undo_trafo();
        $model_object->rotate(deg2rad($angle), $axis);
        
        # realign object to Z = 0
        $model_object->align_to_ground;
        $self->make_thumbnail($obj_idx);

        if (!defined $dont_push) {
            $self->add_undo_operation("TRANSFORM", $object->identifier, $model_object->get_undo_trafo());
        }
    }

    $model_object->update_bounding_box;
    # update print and start background processing
    $self->{print}->add_model_object($model_object, $obj_idx);

    $self->selection_changed;  # refresh info (size etc.)
    $self->on_model_change;
}

sub mirror {
    my ($self, $axis, $dont_push) = @_;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # apply Z rotation before mirroring
    $model_object->transform_by_instance($model_instance, 1);
    
    $model_object->reset_undo_trafo();
    $model_object->mirror($axis);
    
    # realign object to Z = 0
    $model_object->align_to_ground;
    $self->make_thumbnail($obj_idx);
        
    # update print and start background processing
    $self->stop_background_process;
    $self->{print}->add_model_object($model_object, $obj_idx);

    if (!defined $dont_push) {
        $self->add_undo_operation("TRANSFORM", $object->identifier, $model_object->get_undo_trafo());
    }

    $self->selection_changed;  # refresh info (size etc.)
    $self->on_model_change;
}

sub transform {
    my ($self, $trafo, $dont_push) = @_;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # apply Z rotation before mirroring
    $model_object->transform_by_instance($model_instance, 1);
    
    $model_object->apply_transformation($trafo);
    $model_object->update_bounding_box;
    
    # realign object to Z = 0
    $model_object->align_to_ground;
    $self->make_thumbnail($obj_idx);
        
    # update print and start background processing
    $self->stop_background_process;
    $self->{print}->add_model_object($model_object, $obj_idx);

    if (!defined $dont_push) {
        $self->add_undo_operation("TRANSFORM", $object->identifier, $trafo);
    }

    $self->selection_changed;  # refresh info (size etc.)
    $self->on_model_change;
}

sub changescale {
    my ($self, $axis, $tosize, $saved_scale, $dont_push) = @_;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # we need thumbnail to be computed before allowing scaling
    return if !$object->thumbnail;
    
    my $object_size = $model_object->bounding_box->size;
    my $bed_size = Slic3r::Polygon->new_scale(@{$self->{config}->bed_shape})->bounding_box->size;

    my $old_scale;
    my $scale;

    if (defined $axis) {
        my $axis_name = $axis == X ? 'X' : $axis == Y ? 'Y' : 'Z';
        if (!defined $saved_scale) {
            if ($tosize) {
                my $cursize = $object_size->[$axis];
                # Wx::GetNumberFromUser() does not support decimal numbers
                my $newsize = Wx::GetTextFromUser(
                    sprintf("Enter the new size for the selected object (print bed: %smm):", $bed_size->[$axis]),
                    "Scale along $axis_name",
                    $cursize, $self);
                return if !$newsize || $newsize !~ /^\d*(?:\.\d*)?$/ || $newsize < 0;
                $scale = $newsize / $cursize * 100;
                $old_scale = $cursize / $newsize * 100;
            } else {
                # Wx::GetNumberFromUser() does not support decimal numbers
                $scale = Wx::GetTextFromUser("Enter the scale % for the selected object:",
                    "Scale along $axis_name", 100, $self);
                $scale =~ s/%$//;
                return if !$scale || $scale !~ /^\d*(?:\.\d*)?$/ || $scale < 0;
                $old_scale = 100 * 100 / $scale;
            }
        }
        else {
            $scale = $saved_scale;
        }

        # apply Z rotation before scaling
        $model_object->transform_by_instance($model_instance, 1);
        
        my $versor = [1,1,1];
        $versor->[$axis] = $scale/100;

        $model_object->reset_undo_trafo();
        $model_object->scale_xyz(Slic3r::Pointf3->new(@$versor));

        # object was already aligned to Z = 0, so no need to realign it
        $self->make_thumbnail($obj_idx);

        # Add the new undo operation.
        if (!defined $dont_push) {
            $self->add_undo_operation("TRANSFORM", $object->identifier, $model_object->get_undo_trafo());
        }
    } else {
        if (!defined $saved_scale) {
            if ($tosize) {
                my $cursize = max(@$object_size);
                # Wx::GetNumberFromUser() does not support decimal numbers
                my $newsize = Wx::GetTextFromUser("Enter the new max size for the selected object:",
                    "Scale", $cursize, $self);
                return if !$newsize || $newsize !~ /^\d*(?:\.\d*)?$/ || $newsize < 0;
                $scale = $model_instance->scaling_factor * $newsize / $cursize * 100;
                $old_scale = $model_instance->scaling_factor * 100;
            } else {
                # max scale factor should be above 2540 to allow importing files exported in inches
                # Wx::GetNumberFromUser() does not support decimal numbers
                $scale = Wx::GetTextFromUser("Enter the scale % for the selected object:", 'Scale',
                    $model_instance->scaling_factor * 100, $self);
                return if !$scale || $scale !~ /^\d*(?:\.\d*)?$/ || $scale < 0;
                $old_scale = $model_instance->scaling_factor * 100;
            }
            return if !$scale || $scale < 0;
        } else {
            $scale = $saved_scale;
        }

        $scale /= 100;  # turn percent into factor
        
        my $variation = $scale / $model_instance->scaling_factor;
        foreach my $range (@{ $model_object->layer_height_ranges }) {
            $range->[0] *= $variation;
            $range->[1] *= $variation;
        }
        $_->set_scaling_factor($scale) for @{ $model_object->instances };
        $object->transform_thumbnail($self->{model}, $obj_idx);

        $scale *= 100;

        # Add the new undo operation.
        if (!defined $dont_push) {
            $self->add_undo_operation("SCALE_UNIFORM", $object->identifier, $old_scale, $scale);
        }
    }


    $model_object->update_bounding_box;
    
    # update print and start background processing
    $self->stop_background_process;
    $self->{print}->add_model_object($model_object, $obj_idx);
    
    $self->selection_changed(1);  # refresh info (size, volume etc.)
    $self->on_model_change;
}

sub arrange {
    my $self = shift;
    
    $self->pause_background_process;
    
    my $bb = Slic3r::Geometry::BoundingBoxf->new_from_points($self->{config}->bed_shape);
    my $success = $self->{model}->arrange_objects($self->config->min_object_distance, $bb);
    # ignore arrange failures on purpose: user has visual feedback and we don't need to warn him
    # when parts don't fit in print bed

    $self->statusbar->SetStatusText('Objects were arranged.');
    $self->on_model_change(1);
}

sub split_object {
    my ($self, $dont_push) = @_;
    
    my ($obj_idx, $current_object)  = $self->selected_object;
    
    # we clone model object because split_object() adds the split volumes
    # into the same model object, thus causing duplicates when we call load_model_objects()
    my $new_model = $self->{model}->clone;  # store this before calling get_object()
    my $current_model_object = $new_model->get_object($obj_idx);
    
    if ($current_model_object->volumes_count > 1) {
        Slic3r::GUI::warning_catcher($self)->("The selected object can't be split because it contains more than one volume/material.");
        return;
    }
    
    $self->pause_background_process;

    # Save the current model object for undo/redo operataions.
    my $org_object_model = Slic3r::Model->new;
    $org_object_model->add_object($current_model_object);

    # Save the org object identifier.
    my $object_id = $self->{objects}->[$obj_idx]->identifier;

    my @model_objects = @{$current_model_object->split_object};
    if (@model_objects == 1) {
        $self->resume_background_process;
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be split because it contains only one part.");
        $self->resume_background_process;
        return;
    }
    
    foreach my $object (@model_objects) {
        $object->instances->[$_]->offset->translate($_ * 10, $_ * 10)
            for 1..$#{ $object->instances };
        
        # we need to center this single object around origin
        $object->center_around_origin;
    }

    # remove the original object before spawning the object_loaded event, otherwise 
    # we'll pass the wrong $obj_idx to it (which won't be recognized after the
    # thumbnail thread returns)
    $self->remove($obj_idx, 'true'); # Don't push to the undo stack it's considered a split operation not a remove one.
    $current_object = $obj_idx = undef;

    # Save the object identifiers used in undo/redo operations.
    my $new_objects_id_start = $self->{object_identifier};
    print "The new object identifier start for split is " .$new_objects_id_start . "\n";

    # load all model objects at once, otherwise the plate would be rearranged after each one
    # causing original positions not to be kept
    $self->load_model_objects(@model_objects);

    # Create two models to save the current object and the resulted objects.
    my $new_objects_model = Slic3r::Model->new;
    foreach my $new_object (@model_objects) {
        $new_objects_model->add_object($new_object);
    }

    $self->add_undo_operation("SPLIT", $object_id, $org_object_model, $new_objects_model, $new_objects_id_start);
}

sub toggle_print_stats {
    my ($self, $show) = @_;
    
    return if !$self->GetFrame->is_loaded;
    
    if ($show) {
        $self->{right_sizer}->Show($self->{sliced_info_box});
    } else {
        $self->{right_sizer}->Hide($self->{sliced_info_box});
    }
    $self->{right_sizer}->Layout;
}

sub config_changed {
    my $self = shift;
    
    my $config = $self->config;
    
    if ($Slic3r::GUI::autosave) {
        $config->save($Slic3r::GUI::autosave);
    }
    
    # Apply changes to the plater-specific config options.
    foreach my $opt_key (@{$self->{config}->diff($config)}) {
	    # Ignore overrides. No need to set them in our config; we'll use them directly below.
	    next if $opt_key eq 'overrides';
	    
        $self->{config}->set($opt_key, $config->get($opt_key));
        
        if ($opt_key eq 'bed_shape') {
            $self->{canvas}->update_bed_size;
            $self->{canvas3D}->update_bed_size if $self->{canvas3D};
            $self->{preview3D}->set_bed_shape($self->{config}->bed_shape)
                if $self->{preview3D};
            $self->on_model_change;
        } elsif ($opt_key eq 'serial_port') {
            if ($config->get('serial_port')) {
                $self->{btn_print}->Show;
            } else {
                $self->{btn_print}->Hide;
            }
            $self->Layout;
        } elsif ($opt_key eq 'print_host') {
            if ($config->get('print_host')) {
                $self->{btn_send_gcode}->Show;
            } else {
                $self->{btn_send_gcode}->Hide;
            }
            $self->Layout;
        }
    }
    
    return if !$self->GetFrame->is_loaded;
    
    $self->toggle_print_stats(0);
    
    if ($Slic3r::GUI::Settings->{_}{background_processing}) {
        # (re)start timer
        $self->schedule_background_process;
    } else {
        $self->async_apply_config;
    }
}

sub schedule_background_process {
    my ($self) = @_;
    
    warn 'schedule_background_process() is not supposed to be called when background processing is disabled'
        if !$Slic3r::GUI::Settings->{_}{background_processing};
    
    $self->{processed} = 0;
    
    if (defined $self->{apply_config_timer}) {
        $self->{apply_config_timer}->Start(PROCESS_DELAY, 1);  # 1 = one shot
    }
}

# Executed asynchronously by a timer every PROCESS_DELAY (0.5 second).
# The timer is started by schedule_background_process(), 
sub async_apply_config {
    my ($self) = @_;
    
    # pause process thread before applying new config
    # since we don't want to touch data that is being used by the threads
    $self->pause_background_process;
    
    # apply new config
    my $invalidated = $self->{print}->apply_config($self->config);
    
    # reset preview canvases (invalidated contents will be hidden)
    $self->{toolpaths2D}->reload_print if $self->{toolpaths2D};
    $self->{preview3D}->reload_print if $self->{preview3D};
    $self->{AdaptiveLayersDialog}->reload_preview if $self->{AdaptiveLayersDialog};
    
    if (!$Slic3r::GUI::Settings->{_}{background_processing}) {
        $self->hide_preview if $invalidated;
        return;
    }
    
    if ($invalidated) {
        # kill current thread if any
        $self->stop_background_process;
        # remove the sliced statistics box because something changed.
        $self->toggle_print_stats(0);
    } else {
        $self->resume_background_process;
    }
    
    # schedule a new process thread in case it wasn't running
    $self->start_background_process;
}

sub start_background_process {
    my ($self) = @_;
    
    return if !$Slic3r::have_threads;
    return if $self->{process_thread};
    
    if (!@{$self->{objects}}) {
        $self->on_process_completed;
        return;
    }
    
    # It looks like declaring a local $SIG{__WARN__} prevents the ugly
    # "Attempt to free unreferenced scalar" warning...
    local $SIG{__WARN__} = Slic3r::GUI::warning_catcher($self);
    
    # don't start process thread if config is not valid
    eval {
        # this will throw errors if config is not valid
        $self->config->validate;
        $self->{print}->validate;
    };
    if ($@) {
        $self->statusbar->SetStatusText($@);
        return;
    }
    
    if ($Slic3r::GUI::Settings->{_}{threads}) {
        $self->{print}->config->set('threads', $Slic3r::GUI::Settings->{_}{threads});
    }
    
    # start thread
    @_ = ();
    $self->{process_thread} = Slic3r::spawn_thread(sub {
        eval {
            $self->{print}->process;
        };
        if ($@) {
            Slic3r::debugf "Background process error: $@\n";
            Wx::PostEvent($self, Wx::PlThreadEvent->new(-1, $PROCESS_COMPLETED_EVENT, $@));
        } else {
            Wx::PostEvent($self, Wx::PlThreadEvent->new(-1, $PROCESS_COMPLETED_EVENT, undef));
        }
        Slic3r::thread_cleanup();
    });
    Slic3r::debugf "Background processing started.\n";
}

sub stop_background_process {
    my ($self) = @_;
    
    $self->{apply_config_timer}->Stop if defined $self->{apply_config_timer};
    $self->statusbar->SetCancelCallback(undef);
    $self->statusbar->StopBusy;
    $self->statusbar->SetStatusText("");
    
    $self->{toolpaths2D}->reload_print if $self->{toolpaths2D};
    $self->{preview3D}->reload_print if $self->{preview3D};
    $self->{AdaptiveLayersDialog}->reload_preview if $self->{AdaptiveLayersDialog};
    
    if ($self->{process_thread}) {
        Slic3r::debugf "Killing background process.\n";
        Slic3r::kill_all_threads();
        $self->{process_thread} = undef;
    } else {
        Slic3r::debugf "No background process running.\n";
    }
    
    # if there's an export process, kill that one as well
    if ($self->{export_thread}) {
        Slic3r::debugf "Killing background export process.\n";
        Slic3r::kill_all_threads();
        $self->{export_thread} = undef;
    }
}

sub pause_background_process {
    my ($self) = @_;
    
    if ($self->{process_thread} || $self->{export_thread}) {
        Slic3r::pause_all_threads();
        return 1;
    } elsif (defined $self->{apply_config_timer} && $self->{apply_config_timer}->IsRunning) {
        $self->{apply_config_timer}->Stop;
        return 0;  # we didn't actually pause any running thread; need to reschedule
    }
    
    return 0;
}

sub resume_background_process {
    my ($self) = @_;
    
    if ($self->{process_thread} || $self->{export_thread}) {
        Slic3r::resume_all_threads();
    }
}

sub export_gcode {
    my ($self, $output_file) = @_;
    
    return if !@{$self->{objects}};
    
    if ($self->{export_gcode_output_file}) {
        Wx::MessageDialog->new($self, "Another export job is currently running.", 'Error', wxOK | wxICON_ERROR)->ShowModal;
        return;
    }
    
    # if process is not running, validate config
    # (we assume that if it is running, config is valid)
    eval {
        # this will throw errors if config is not valid
        $self->config->validate;
        $self->{print}->validate;
    };
    Slic3r::GUI::catch_error($self) and return;
    
    
    # apply config and validate print
    my $config = $self->config;
    eval {
        # this will throw errors if config is not valid
        $config->validate;
        $self->{print}->apply_config($config);
        $self->{print}->validate;
    };
    if (!$Slic3r::have_threads) {
        Slic3r::GUI::catch_error($self) and return;
    }
    
    # select output file
    if ($output_file) {
        $self->{export_gcode_output_file} = $self->{print}->output_filepath($output_file);
    } else {
        my $default_output_file = $self->{print}->output_filepath($main::opt{output} // '');
        my $dlg = Wx::FileDialog->new($self, 'Save G-code file as:', wxTheApp->output_path(dirname($default_output_file)),
            basename($default_output_file), &Slic3r::GUI::FILE_WILDCARDS->{gcode}, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if ($dlg->ShowModal != wxID_OK) {
            $dlg->Destroy;
            return;
        }
        my $path = Slic3r::decode_path($dlg->GetPath);
        $Slic3r::GUI::Settings->{_}{last_output_path} = dirname($path);
        wxTheApp->save_settings;
        $self->{export_gcode_output_file} = $path;
        $dlg->Destroy;
    }
    
    $self->statusbar->StartBusy;
    
    if ($Slic3r::have_threads) {
        $self->statusbar->SetCancelCallback(sub {
            $self->stop_background_process;
            $self->statusbar->SetStatusText("Export cancelled");
            $self->{export_gcode_output_file} = undef;
            $self->{send_gcode_file} = undef;
            
            # this updates buttons status
            $self->object_list_changed;
        });
        
        # start background process, whose completion event handler
        # will detect $self->{export_gcode_output_file} and proceed with export
        $self->start_background_process;
    } else {
        eval {
            $self->{print}->process;
            $self->{print}->export_gcode(output_file => $self->{export_gcode_output_file});
        };
        my $result = !Slic3r::GUI::catch_error($self);
        $self->on_export_completed($result);
    }
    
    # this updates buttons status
    $self->object_list_changed;
    $self->toggle_print_stats(1);
    
    return $self->{export_gcode_output_file};
}

# This gets called only if we have threads.
sub on_process_completed {
    my ($self, $error) = @_;
    
    $self->statusbar->SetCancelCallback(undef);
    $self->statusbar->StopBusy;
    $self->statusbar->SetStatusText($error // "");
    
    Slic3r::debugf "Background processing completed.\n";
    $self->{process_thread}->detach if $self->{process_thread};
    $self->{process_thread} = undef;
    $self->{processed} = 1;
    
    # if we're supposed to perform an explicit export let's display the error in a dialog
    if ($error && $self->{export_gcode_output_file}) {
        $self->{export_gcode_output_file} = undef;
        Slic3r::GUI::show_error($self, $error);
    }
    
    return if $error;
    $self->{toolpaths2D}->reload_print if $self->{toolpaths2D};
    $self->{preview3D}->reload_print if $self->{preview3D};
    $self->{AdaptiveLayersDialog}->reload_preview if $self->{AdaptiveLayersDialog};
    
    # if we have an export filename, start a new thread for exporting G-code
    if ($self->{export_gcode_output_file}) {
        @_ = ();
        
        # workaround for "Attempt to free un referenced scalar..."
        our $_thread_self = $self;
        
        $self->{export_thread} = Slic3r::spawn_thread(sub {
            eval {
                $_thread_self->{print}->export_gcode(output_file => $_thread_self->{export_gcode_output_file});
            };
            if ($@) {
                Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $ERROR_EVENT, shared_clone([ $@ ])));
                Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $EXPORT_COMPLETED_EVENT, 0));
            } else {
                Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $EXPORT_COMPLETED_EVENT, 1));
            }
            Slic3r::thread_cleanup();
        });
        Slic3r::debugf "Background G-code export started.\n";
    }
}

# This gets called also if we have no threads.
sub on_progress_event {
    my ($self, $percent, $message) = @_;
    
    $self->statusbar->SetProgress($percent);
    $self->statusbar->SetStatusText("$message…");
}

# This gets called also if we don't have threads.
sub on_export_completed {
    my ($self, $result) = @_;
    
    $self->statusbar->SetCancelCallback(undef);
    $self->statusbar->StopBusy;
    $self->statusbar->SetStatusText("");
    
    Slic3r::debugf "Background export process completed.\n";
    $self->{export_thread}->detach if $self->{export_thread};
    $self->{export_thread} = undef;
    
    my $message;
    my $send_gcode = 0;
    my $do_print = 0;
    if ($result) {
        if ($self->{print_file}) {
            $message = "File added to print queue";
            $do_print = 1;
        } elsif ($self->{send_gcode_file}) {
            $message = "Sending G-code file to the " . $self->{config}->host_type . " server...";
            $send_gcode = 1;
        } else {
            $message = "G-code file exported to " . $self->{export_gcode_output_file};
        }
    } else {
        $message = "Export failed";
    }
    # Estimate print duration
    {
        my $estimator = Slic3r::GCode::TimeEstimator->new;
        $estimator->parse_file($self->{export_gcode_output_file});
        my $time = $estimator->time;
        $self->{print_info_tim}->SetLabel(sprintf(
            "%d hours, %d minutes and %d seconds",
            int($time / 3600),
            int(($time % 3600) / 60),
            int($time % 60),
        ));
    }
    $self->{export_gcode_output_file} = undef;
    $self->statusbar->SetStatusText($message);
    wxTheApp->notify($message);
    
    $self->do_print if $do_print;
    $self->send_gcode if $send_gcode;
    $self->{print_file} = undef;
    $self->{send_gcode_file} = undef;
    
    {
        my $fil = sprintf(
            '%.2fcm (%.2fcm³%s)',
            $self->{print}->total_used_filament / 10,
            $self->{print}->total_extruded_volume / 1000,
            $self->{print}->total_weight
                ? sprintf(', %.2fg', $self->{print}->total_weight)
                : '',
        );
        my $cost = $self->{print}->total_cost
            ? sprintf("%.2f" , $self->{print}->total_cost)
            : 'n.a.';
        $self->{print_info_fil}->SetLabel($fil);
        $self->{print_info_cost}->SetLabel($cost);
    }
    
    # this updates buttons status
    $self->object_list_changed;
}

sub do_print {
    my ($self) = @_;
    
    my $controller = $self->GetFrame->{controller} or return;
    
    my %current_presets = $self->selected_presets;
    
    my $printer_panel = $controller->add_printer($current_presets{printer}->[0], $self->config);
    
    my $filament_stats = $self->{print}->filament_stats;
    $filament_stats = { map { $current_presets{filament}[$_]->name => $filament_stats->{$_} } keys %$filament_stats };
    $printer_panel->load_print_job($self->{print_file}, $filament_stats);
    
    $self->GetFrame->select_tab(1);
}

sub prepare_send {
    my ($self, $skip_dialog) = @_;
    
    return if !$self->{btn_send_gcode}->IsEnabled;
    my $filename = basename($self->{print}->output_filepath($main::opt{output} // ''));

    if (!$skip_dialog) {
        # When the alt key is pressed, bypass the dialog.
        my $dlg = Slic3r::GUI::Plater::OctoPrintSpoolDialog->new($self, $filename);
        return unless $dlg->ShowModal == wxID_OK;
        $filename = $dlg->{filename};
    }

    if (!$Slic3r::GUI::Settings->{octoprint}{overwrite}) {
        my $progress = Wx::ProgressDialog->new('Querying OctoPrint…',
            "Checking whether file already exists…", 100, $self, 0);
        $progress->Pulse;

        my $host;
        if ($self->{config}->print_host !~ /^http/) {
            $host = "http://" . $self->{config}->print_host;
        }
        else {
            $host = $self->{config}->print_host;
        }

        my $ua = LWP::UserAgent->new;
        $ua->timeout(5);
        my $res;
        if ($self->{config}->print_host) {
            if($self->{config}->host_type eq 'octoprint'){
                $res = $ua->get(
                    $host . "/api/files/local",
                    'X-Api-Key' => $self->{config}->octoprint_apikey,
                );
            }else {
                $res = $ua->get(
                    "http://" . $self->{config}->print_host . "/rr_files",
                );            
            }
        }
        $progress->Destroy;
        if ($res->is_success) {
            my $searchterm = ($self->{config}->host_type eq 'octoprint') ? '/"name":\s*"\Q$filename\E"/' : '"'.$filename.'"';            
            if ($res->decoded_content =~ $searchterm) {
                my $dialog = Wx::MessageDialog->new($self,
                    "It looks like a file with the same name already exists in the server. "
                        . "Shall I overwrite it?",
                    $self->{config}->host_type, wxICON_WARNING | wxYES | wxNO);
                if ($dialog->ShowModal() == wxID_NO) {
                    return;
                }
            }
        } else {
            my $message = "Error while connecting to the " . $self->{config}->host_type . " server: " . $res->status_line;
            Slic3r::GUI::show_error($self, $message);
            return;
        }
    }

    $self->{send_gcode_file_print} = $Slic3r::GUI::Settings->{octoprint}{start};
    $self->{send_gcode_file} = $self->export_gcode(Wx::StandardPaths::Get->GetTempDir() . "/" . $filename);
}

sub send_gcode {
    my ($self) = @_;
    
    $self->statusbar->StartBusy;
    
    my $ua = LWP::UserAgent->new;
    $ua->timeout(180);
    
    my $path = Slic3r::encode_path($self->{send_gcode_file});
    my $filename = basename($self->{print}->output_filepath($main::opt{output} // ''));
    my $res;
    if($self->{config}->print_host){
        my $host;
        if ($self->{config}->print_host !~ /^http/) {
            $host = "http://" . $self->{config}->print_host;
        }
        else {
        $host = $self->{config}->print_host;
        }
        if($self->{config}->host_type eq 'octoprint'){
            $res = $ua->post(
                $host . "/api/files/local",
                Content_Type => 'form-data',
                'X-Api-Key' => $self->{config}->octoprint_apikey,
                Content => [
                    # OctoPrint doesn't like Windows paths so we use basename()
                    # Also, since we need to read from filesystem we process it through encode_path()
                    file => [ $path, basename($path) ],
                    print => $self->{send_gcode_file_print} ? 1 : 0,
                ],
            );
        }else{
            # slurp the file we would send into a string - should be someplace to reference this but could not find it?
            local $/=undef;
            open (my $gch,$path);
            my $gcode=<$gch>;
            close($gch);

            # get the time string            
            my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) = localtime(time);
            my $t = sprintf("%4d-%02d-%02dT%02d:%02d:%02d",$year+1900,$mon+1,$mday,$hour,$min,$sec);

            my $req = HTTP::Request->new(POST => "http://" . $self->{config}->print_host . "/rr_upload?name=0:/gcodes/" . basename($path) . "&time=$t",);
            $req->content( $gcode );
            $res = $ua->request($req);
 
            if ($res->is_success) {
                if ($self->{send_gcode_file_print}) {
                    $res = $ua->get(
                        "http://" . $self->{config}->print_host . "/rr_gcode?gcode=M32%20" . basename($path),
                    );
                }
            }
        }
    }

    $self->statusbar->StopBusy;
    
    if ($res->is_success) {
        $self->statusbar->SetStatusText("G-code file successfully uploaded to the " . $self->{config}->host_type . " server");
    } else {
        my $message = "Error while uploading to the " . $self->{config}->host_type . " server: " . $res->status_line;
        Slic3r::GUI::show_error($self, $message);
        $self->statusbar->SetStatusText($message);
    }
}

sub export_stl {
    my $self = shift;
    
    return if !@{$self->{objects}};
        
    my $output_file = $self->_get_export_file('STL') or return;
    $self->{model}->write_stl($output_file, 1);
    $self->statusbar->SetStatusText("STL file exported to $output_file");
}

sub reload_from_disk {
    my ($self) = @_;
    
    my ($obj_idx, $org_obj_plater) = $self->selected_object;
    return if !defined $obj_idx;
    
    if (!$org_obj_plater->input_file) {
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be reloaded because it isn't referenced to its input file any more. This is the case after performing operations like cut or split.");
        return;
    }
    if (!-e $org_obj_plater->input_file) {
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be reloaded because the file doesn't exist anymore on the disk.");
        return;
    }

    my $reload_behavior = $Slic3r::GUI::Settings->{_}{reload_behavior};
    my $reload_preserve_trafo = $Slic3r::GUI::Settings->{_}{reload_preserve_trafo};

    # ask the user how to proceed, if option is selected in preferences
    if (!$Slic3r::GUI::Settings->{_}{reload_hide_dialog}) {
        my $dlg = Slic3r::GUI::ReloadDialog->new(undef,$reload_behavior,$reload_preserve_trafo);
        my $res = $dlg->ShowModal;
        if ($res==wxID_CANCEL) {
            $dlg->Destroy;
            return;
        }
        $reload_behavior = $dlg->GetAdditionalOption;
        $reload_preserve_trafo = $dlg->GetPreserveTrafo;
        my $save = 0;
        if ($reload_behavior != $Slic3r::GUI::Settings->{_}{reload_behavior}) {
            $Slic3r::GUI::Settings->{_}{reload_behavior} = $reload_behavior;
            $save = 1;
        }
        if ($reload_preserve_trafo != $Slic3r::GUI::Settings->{_}{reload_preserve_trafo}) {
            $Slic3r::GUI::Settings->{_}{reload_preserve_trafo} = $reload_preserve_trafo;
            $save = 1;
        }
        if ($dlg->GetHideOnNext) {
            $Slic3r::GUI::Settings->{_}{reload_hide_dialog} = 1;
            $save = 1;
        }
        Slic3r::GUI->save_settings if $save;
        $dlg->Destroy;
    }
    
    # Only reload the selected object and not all objects from the input file.
    my @new_obj_idx = $self->load_file($org_obj_plater->input_file, $org_obj_plater->input_file_obj_idx);
    if (!@new_obj_idx) {
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be reloaded because the new file doesn't contain the object.");
        return;
    }

    my $org_obj = my $new_obj = $self->{model}->objects->[$obj_idx];

    my $volume_unmatched=0;

    foreach my $new_obj_idx (@new_obj_idx) {
        my $new_obj = $self->{model}->objects->[$new_obj_idx];
        $new_obj->clear_instances;
        $new_obj->add_instance($_) for @{$org_obj->instances};
        $new_obj->config->apply($org_obj->config);
        $new_obj->set_trafo_obj($org_obj->get_trafo_obj()) if $reload_preserve_trafo;
        
        my $new_vol_idx = 0;
        my $org_vol_idx = 0;
        my $new_vol_count=$new_obj->volumes_count;
        my $org_vol_count=$org_obj->volumes_count;
        
        while ($new_vol_idx<=$new_vol_count-1) {
            if (($org_vol_idx<=$org_vol_count-1) && ($org_obj->get_volume($org_vol_idx)->input_file eq $new_obj->input_file)) {
                # apply config and trafo from the matching volumes
                $new_obj->get_volume($new_vol_idx)->apply_transformation($org_obj->get_volume($org_vol_idx)->get_transformation) if $reload_preserve_trafo;
                $new_obj->get_volume($new_vol_idx++)->config->apply($org_obj->get_volume($org_vol_idx++)->config);
            } else {
                # reload has more volumes than original (first file), apply config and trafo from the parent object
                $new_obj->get_volume($new_vol_idx)->apply_transformation($org_obj->get_trafo_obj()) if $reload_preserve_trafo;
                $new_obj->get_volume($new_vol_idx++)->config->apply($org_obj->get_volume(0)->config);
                $volume_unmatched=1;
            }
        }
        $org_vol_idx=$org_vol_count if $reload_behavior==2; # Reload behavior: discard
        while (($org_vol_idx<=$org_vol_count-1) && ($org_obj->get_volume($org_vol_idx)->input_file eq $new_obj->input_file)) {
            # original has more volumes (first file), skip those
            $org_vol_idx++;
            $volume_unmatched=1;
        }
        while ($org_vol_idx<=$org_vol_count-1) {
            my $org_volume = $org_obj->get_volume($org_vol_idx);

            if ($reload_behavior==1) { # Reload behavior: copy

                my $new_volume = $new_obj->add_volume($org_volume);
                if ($new_volume->name =~ m/link to path\z/) {
                    my $new_name = $new_volume->name;
                    $new_name =~ s/ - no link to path$/ - copied/;
                    $new_volume->set_name($new_name);
                }elsif(!($new_volume->name =~ m/copied\z/)) {
                    $new_volume->set_name($new_volume->name . " - copied");
                }

            }else{ # Reload behavior: Reload all, also fallback solution if ini was manually edited to a wrong value

                if ($org_volume->input_file) {
                    my $model = eval { Slic3r::Model->read_from_file($org_volume->input_file) };
                    if ($@) {
                        $org_volume->set_input_file("");
                    }elsif ($org_volume->input_file_obj_idx > ($model->objects_count-1)) {
                        # Object Index for that part / modifier not found in current version of the file
                        $org_volume->set_input_file("");
                    }else{
                        my $prt_mod_obj = $model->objects->[$org_volume->input_file_obj_idx];
                        if ($org_volume->input_file_vol_idx > ($prt_mod_obj->volumes_count-1)) {
                            # Volume Index for that part / modifier not found in current version of the file
                            $org_volume->set_input_file("");
                        }else{
                            # all checks passed, load new mesh and copy metadata
                            my $new_volume = $new_obj->add_volume($prt_mod_obj->get_volume($org_volume->input_file_vol_idx));

                            $new_volume->apply_transformation($org_volume->get_transformation()) if $reload_preserve_trafo;

                            $new_volume->set_input_file($org_volume->input_file);
                            $new_volume->set_input_file_obj_idx($org_volume->input_file_obj_idx);
                            $new_volume->set_input_file_vol_idx($org_volume->input_file_vol_idx);
                            $new_volume->config->apply($org_volume->config);
                            $new_volume->set_modifier($org_volume->modifier);
                        }
                    }
                }
                if (!$org_volume->input_file) {
                    my $new_volume = $new_obj->add_volume($org_volume); # error -> copy old mesh
                    if ($new_volume->name =~ m/copied\z/) {
                        my $new_name = $new_volume->name;
                        $new_name =~ s/ - copied$/ - no link to path/;
                        $new_volume->set_name($new_name);
                    }elsif(!($new_volume->name =~ m/link to path\z/)) {
                        $new_volume->set_name($new_volume->name . " - no link to path");
                    }
                    $volume_unmatched=1;
                }
            }
            $org_vol_idx++;
        }
        $new_obj->center_around_origin();
    }
    $self->remove($obj_idx);
    
    # TODO: refresh object list which contains wrong count and scale
    
    # Trigger thumbnail generation again, because the remove() method altered
    # object indexes before background thumbnail generation called its completion
    # event, so the on_thumbnail_made callback is called with the wrong $obj_idx.
    # When porting to C++ we'll probably have cleaner ways to do this.
    $self->make_thumbnail($_-1) for @new_obj_idx;

    # update print
    $self->stop_background_process;
    $self->{print}->reload_object($_-1) for @new_obj_idx;
    $self->on_model_change;

    # Empty the redo stack
    $self->{redo_stack} = [];

    if ($volume_unmatched) {
        Slic3r::GUI::warning_catcher($self)->("At least 1 volume couldn't be matched between the original object and the reloaded one.");
    }
}

sub export_object_stl {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $model_object = $self->{model}->objects->[$obj_idx];
        
    my $output_file = $self->_get_export_file('STL') or return;
    $model_object->mesh->write_binary($output_file);
    $self->statusbar->SetStatusText("STL file exported to $output_file");
}

# Export function for a single AMF output
sub export_object_amf {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;
    
    my $local_model = Slic3r::Model->new;
    my $model_object = $self->{model}->objects->[$obj_idx];
    # copy model_object -> local_model
    $local_model->add_object($model_object);
        
    my $output_file = $self->_get_export_file('AMF') or return;
    $local_model->write_amf($output_file);
    $self->statusbar->SetStatusText("AMF file exported to $output_file");
}

# Export function for a single 3MF output
sub export_object_tmf {
    my $self = shift;

    my ($obj_idx, $object) = $self->selected_object;
    return if !defined $obj_idx;

    my $local_model = Slic3r::Model->new;
    my $model_object = $self->{model}->objects->[$obj_idx];
    # copy model_object -> local_model
    $local_model->add_object($model_object);

    my $output_file = $self->_get_export_file('TMF') or return;
    $local_model->write_tmf($output_file);
    $self->statusbar->SetStatusText("3MF file exported to $output_file");
}

sub export_amf {
    my $self = shift;
    
    return if !@{$self->{objects}};
        
    my $output_file = $self->_get_export_file('AMF') or return;
    $self->{model}->write_amf($output_file);
    $self->statusbar->SetStatusText("AMF file exported to $output_file");
}

sub export_tmf {
    my $self = shift;

    return if !@{$self->{objects}};

    my $output_file = $self->_get_export_file('TMF') or return;
    $self->{model}->write_tmf($output_file);
    $self->statusbar->SetStatusText("3MF file exported to $output_file");
}

sub _get_export_file {
    my $self = shift;
    my ($format) = @_;
    
    my $suffix = $format eq 'STL' ? '.stl' : ( $format eq 'AMF' ?  '.amf' : '.3mf');
    
    my $output_file = $main::opt{output};
    {
        $output_file = $self->{print}->output_filepath($output_file // '');
        $output_file =~ s/\.gcode$/$suffix/i;
        my $dlg;
        $dlg = Wx::FileDialog->new($self, "Save $format file as:", dirname($output_file),
            basename($output_file), &Slic3r::GUI::STL_MODEL_WILDCARD, wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
            if $format eq 'STL';

        $dlg = Wx::FileDialog->new($self, "Save $format file as:", dirname($output_file),
            basename($output_file), &Slic3r::GUI::AMF_MODEL_WILDCARD, wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
            if $format eq 'AMF';

        $dlg = Wx::FileDialog->new($self, "Save $format file as:", dirname($output_file),
            basename($output_file), &Slic3r::GUI::TMF_MODEL_WILDCARD, wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
            if $format eq 'TMF';

        if ($dlg->ShowModal != wxID_OK) {
            $dlg->Destroy;
            return undef;
        }
        $output_file = Slic3r::decode_path($dlg->GetPath);
        $dlg->Destroy;
    }
    return $output_file;
}

sub make_thumbnail {
    my $self = shift;
    my ($obj_idx) = @_;
    
    my $plater_object = $self->{objects}[$obj_idx];
    return if($plater_object->remaking_thumbnail);
    $plater_object->remaking_thumbnail(1);
    $plater_object->thumbnail(Slic3r::ExPolygon::Collection->new);
    my $cb = sub {
        $plater_object->make_thumbnail($self->{model}, $obj_idx);
        
        if ($Slic3r::have_threads) {
            Wx::PostEvent($self, Wx::PlThreadEvent->new(-1, $THUMBNAIL_DONE_EVENT, shared_clone([ $obj_idx ])));
            Slic3r::thread_cleanup();
            threads->exit;
        } else {
            $self->on_thumbnail_made($obj_idx);
        }
    };
    
    @_ = ();
    $Slic3r::have_threads
        ? threads->create(sub { $cb->(); Slic3r::thread_cleanup(); })->detach
        : $cb->();
}

sub on_thumbnail_made {
    my $self = shift;
    my ($obj_idx) = @_;
    
    $self->{objects}[$obj_idx]->remaking_thumbnail(0);
    $self->{objects}[$obj_idx]->transform_thumbnail($self->{model}, $obj_idx);
    $self->refresh_canvases;
}

# this method gets called whenever print center is changed or the objects' bounding box changes
# (i.e. when an object is added/removed/moved/rotated/scaled)
sub on_model_change {
    my ($self, $force_autocenter) = @_;
    
    # reload the select submenu (if already initialized)
    if (my $menu = $self->GetFrame->{plater_select_menu}) {
        $menu->DeleteItem($_) for $menu->GetMenuItems;
        for my $i (0..$#{$self->{objects}}) {
            my $name = $self->{objects}->[$i]->name;
            my $count = $self->{model}->get_object($i)->instances_count;
            if ($count > 1) {
                $name .= " (${count}x)";
            }
            my $item = wxTheApp->append_menu_item($menu, $name, 'Select object', sub {
                $self->select_object($i);
                $self->refresh_canvases;
            }, undef, undef, wxITEM_CHECK);
            $item->Check(1) if $self->{objects}->[$i]->selected;
        }
    }
    
    # reload the objects info choice
    if (my $choice = $self->{object_info_choice}) {
        $choice->Clear;
        for my $i (0..$#{$self->{objects}}) {
            my $name = $self->{objects}->[$i]->name;
            my $count = $self->{model}->get_object($i)->instances_count;
            if ($count > 1) {
                $name .= " (${count}x)";
            }
            $choice->Append($name);
        }
        my ($obj_idx, $object) = $self->selected_object;
        $choice->SetSelection($obj_idx // -1);
    }
    
    my $running = $self->pause_background_process;
    
    if ($Slic3r::GUI::Settings->{_}{autocenter} || $force_autocenter) {
        $self->{model}->center_instances_around_point($self->bed_centerf);
    }
    $self->refresh_canvases;
    
    my $invalidated = $self->{print}->reload_model_instances();
    
    if ($Slic3r::GUI::Settings->{_}{background_processing}) {
        if ($invalidated || !$running) {
            # The mere fact that no steps were invalidated when reloading model instances 
            # doesn't mean that all steps were done: for example, validation might have 
            # failed upon previous instance move, so we have no running thread and no steps
            # are invalidated on this move, thus we need to schedule a new run.
            $self->schedule_background_process;
            $self->toggle_print_stats(0);
        } else {
            $self->resume_background_process;
        }
    } else {
        $self->hide_preview;
    }
}

sub hide_preview {
    my ($self) = @_;
    
    my $sel = $self->{preview_notebook}->GetSelection;
    if ($sel == $self->{preview3D_page_idx} || $sel == $self->{toolpaths2D_page_idx}) {
        $self->{preview_notebook}->SetSelection(0);
    }
    $self->{processed} = 0;
}

sub on_extruders_change {
    my ($self, $num_extruders) = @_;
    
    my $choices = $self->{preset_choosers}{filament};
    while (@$choices < $num_extruders) {
        # copy strings from first choice
        my @presets = $choices->[0]->GetStrings;
        
        # initialize new choice
        my $choice = Wx::BitmapComboBox->new($self, -1, "", wxDefaultPosition, wxDefaultSize, [@presets], wxCB_READONLY);
        push @$choices, $choice;
        
        # copy icons from first choice
        $choice->SetItemBitmap($_, $choices->[0]->GetItemBitmap($_)) for 0..$#presets;
        
        # settings button
        my $settings_btn = Wx::BitmapButton->new($self, -1, Wx::Bitmap->new($Slic3r::var->("cog.png"), wxBITMAP_TYPE_PNG), 
            wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        
        # insert new row into sizer
        $self->{presets_sizer}->Insert(6 + ($#$choices-1)*3, 0, 0);
        $self->{presets_sizer}->Insert(7 + ($#$choices-1)*3, $choice, 0, wxEXPAND | wxBOTTOM, FILAMENT_CHOOSERS_SPACING);
        $self->{presets_sizer}->Insert(8 + ($#$choices-1)*3, $settings_btn, 0, wxEXPAND | wxLEFT, 4);
        
        # setup the listeners
        EVT_COMBOBOX($choice, $choice, sub {
            my ($choice) = @_;
            wxTheApp->CallAfter(sub {
                $self->_on_change_combobox('filament', $choice);
            });
        });
        
        EVT_BUTTON($self, $settings_btn, sub {
            $self->show_preset_editor('filament', $#$choices);
        });
        
        # initialize selection
        my $i = first { $choice->GetString($_) eq ($Slic3r::GUI::Settings->{presets}{"filament_" . $#$choices} || '') } 0 .. $#presets;
        $choice->SetSelection($i || 0);
    }
    
    # remove unused choices if any
    while (@$choices > $num_extruders) {
        my $i = 6 + ($#$choices-1)*3;
        
        $self->{presets_sizer}->Remove($i);  # label
        $self->{presets_sizer}->Remove($i);  # wxChoice
        
        my $settings_btn = $self->{presets_sizer}->GetItem($i)->GetWindow;
        $self->{presets_sizer}->Remove($i);  # settings btn
        $settings_btn->Destroy;
        
        $choices->[-1]->Destroy;
        pop @$choices;
    }
    $self->Layout;
}

sub object_cut_dialog {
    my $self = shift;
    my ($obj_idx) = @_;
    
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }
    
    if (!$Slic3r::GUI::have_OpenGL) {
        Slic3r::GUI::show_error($self, "Please install the OpenGL modules to use this feature (see build instructions).");
        return;
    }
    
    my $dlg = Slic3r::GUI::Plater::ObjectCutDialog->new($self,
		object              => $self->{objects}[$obj_idx],
		model_object        => $self->{model}->objects->[$obj_idx],
	);
	return unless $dlg->ShowModal == wxID_OK;
	
	if (my @new_objects = $dlg->NewModelObjects) {
	    my $process_dialog = Wx::ProgressDialog->new('Loading…', "Loading new objects…", 100, $self, 0);
        $process_dialog->Pulse;

        # Create two models to save the current object and the resulted objects.
        my $new_objects_model = Slic3r::Model->new;
        foreach my $new_object (@new_objects) {
            $new_objects_model->add_object($new_object);
        }

        my $org_object_model = Slic3r::Model->new;
        $org_object_model->add_object($self->{model}->get_object($obj_idx));

        # Save the object identifiers used in undo/redo operations.
        my $object_id = $self->{objects}->[$obj_idx]->identifier;
        my $new_objects_id_start = $self->{object_identifier};

        $self->add_undo_operation("CUT", $object_id, $org_object_model, $new_objects_model, $new_objects_id_start);

	    $self->remove($obj_idx, 'true');
	    $self->load_model_objects(grep defined($_), @new_objects);
	    $self->arrange if @new_objects <= 2; # don't arrange for grid cuts
	    
	    $process_dialog->Destroy;
	}
}

sub object_layers_dialog {
    my $self = shift;
    my ($obj_idx) = @_;

    $self->object_settings_dialog($obj_idx, adaptive_layers => 1);
}

sub object_settings_dialog {
    my $self = shift;
    my ($obj_idx, %params) = @_;
    
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }
    my $model_object = $self->{model}->objects->[$obj_idx];
    
    # validate config before opening the settings dialog because
    # that dialog can't be closed if validation fails, but user
    # can't fix any error which is outside that dialog
    return unless $self->validate_config;
    
    my $dlg = Slic3r::GUI::Plater::ObjectSettingsDialog->new($self,
		object          => $self->{objects}[$obj_idx],
		model_object    => $model_object,
		obj_idx         => $obj_idx,
	);
	# store pointer to the adaptive layer tab to push preview updates
	$self->{AdaptiveLayersDialog} = $dlg->{adaptive_layers};
	# and jump directly to the tab if called by "promo-button"
	$dlg->{tabpanel}->SetSelection(1) if $params{adaptive_layers};
	$self->pause_background_process;
	$dlg->ShowModal;
	$self->{AdaptiveLayersDialog} = undef;
	
    # update thumbnail since parts may have changed
    if ($dlg->PartsChanged) {
	    # recenter and re-align to Z = 0
	    $model_object->center_around_origin;
        $self->make_thumbnail($obj_idx);
    }
	
	# update print
	if ($dlg->PartsChanged || $dlg->PartSettingsChanged) {
	    $self->stop_background_process;
        $self->{print}->reload_object($obj_idx);
        $self->on_model_change;
    } else {
        $self->resume_background_process;
    }
}

sub object_list_changed {
    my $self = shift;
    
    my $have_objects = @{$self->{objects}} ? 1 : 0;
    my $method = $have_objects ? 'Enable' : 'Disable';
    $self->{"btn_$_"}->$method
        for grep $self->{"btn_$_"}, qw(reset arrange export_gcode export_stl print send_gcode);
    
    if ($self->{export_gcode_output_file} || $self->{send_gcode_file}) {
        $self->{btn_export_gcode}->Disable;
        $self->{btn_print}->Disable;
        $self->{btn_send_gcode}->Disable;
    }
    
    if ($self->{htoolbar}) {
        $self->{htoolbar}->EnableTool($_, $have_objects)
            for (TB_RESET, TB_ARRANGE);
    }
    
    # prepagate the event to the frame (a custom Wx event would be cleaner)
    $self->GetFrame->on_plater_object_list_changed($have_objects);
}

sub selection_changed {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $have_sel = defined $obj_idx;

    # Remove selection in 2d Plater.
    $self->{canvas}->{selected_instance} = undef;

    if (my $menu = $self->GetFrame->{plater_select_menu}) {
        $_->Check(0) for $menu->GetMenuItems;
        if ($have_sel) {
            $menu->FindItemByPosition($obj_idx)->Check(1);
        }
    }
    
    my $method = $have_sel ? 'Enable' : 'Disable';
    $self->{"btn_$_"}->$method
        for grep $self->{"btn_$_"}, qw(remove increase decrease rotateX90cw rotateX90ccw rotateY90cw rotateY90ccw rotateZ90cw rotateZ90ccw rotateZ45cw rotateZ45ccw rotateFace changescale split cut layers settings);
    
    if ($self->{htoolbar}) {
        $self->{htoolbar}->EnableTool($_, $have_sel)
            for (TB_REMOVE, TB_MORE, TB_FEWER, TB_X90CW, TB_X90CCW, TB_Y90CW, TB_Y90CCW, TB_Z90CW, TB_Z90CCW, TB_45CW, TB_45CCW, TB_ROTFACE, TB_SCALE, TB_SPLIT, TB_CUT, TB_LAYERS, TB_SETTINGS);
    }
    
    if ($self->{object_info_size}) { # have we already loaded the info pane?
        
        if ($have_sel) {
            my $model_object = $self->{model}->objects->[$obj_idx];
            $self->{object_info_choice}->SetSelection($obj_idx);
            $self->{object_info_copies}->SetLabel($model_object->instances_count);
            my $model_instance = $model_object->instances->[0];
            {
                my $size_string = sprintf "%.2f x %.2f x %.2f", @{$model_object->instance_bounding_box(0)->size};
                if ($model_instance->scaling_factor != 1) {
                    $size_string .= sprintf " (%s%%)", $model_instance->scaling_factor * 100;
                }
                $self->{object_info_size}->SetLabel($size_string);
            }
            $self->{object_info_materials}->SetLabel($model_object->materials_count);
            
            my $raw_mesh = $model_object->raw_mesh;
            $raw_mesh->repair;  # this calculates number_of_parts
            if (my $stats = $raw_mesh->stats) {
                $self->{object_info_volume}->SetLabel(sprintf('%.2f', $raw_mesh->volume * ($model_instance->scaling_factor**3)));
                $self->{object_info_facets}->SetLabel(sprintf('%d (%d shells)', $model_object->facets_count, $stats->{number_of_parts}));
                if (my $errors = sum(@$stats{qw(degenerate_facets edges_fixed facets_removed facets_added facets_reversed backwards_edges)})) {
                    $self->{object_info_manifold}->SetLabel(sprintf("Auto-repaired (%d errors)", $errors));
                    $self->{object_info_manifold_warning_icon}->Show;
                    
                    # we don't show normals_fixed because we never provide normals
	                # to admesh, so it generates normals for all facets
                    my $message = sprintf '%d degenerate facets, %d edges fixed, %d facets removed, %d facets added, %d facets reversed, %d backwards edges',
                        @$stats{qw(degenerate_facets edges_fixed facets_removed facets_added facets_reversed backwards_edges)};
                    $self->{object_info_manifold}->SetToolTipString($message);
                    $self->{object_info_manifold_warning_icon}->SetToolTipString($message);
                } else {
                    $self->{object_info_manifold}->SetLabel("Yes");
                }
            } else {
                $self->{object_info_facets}->SetLabel($object->facets);
            }
        } else {
            $self->{object_info_choice}->SetSelection(-1);
            $self->{"object_info_$_"}->SetLabel("") for qw(copies size volume facets materials manifold);
            $self->{object_info_manifold_warning_icon}->Hide;
            $self->{object_info_manifold}->SetToolTipString("");
        }
        $self->Layout;
    }
    
    # prepagate the event to the frame (a custom Wx event would be cleaner)
    $self->GetFrame->on_plater_selection_changed($have_sel);
}

sub select_object {
    my ($self, $obj_idx) = @_;

    $_->selected(0) for @{ $self->{objects} };
    $_->selected_instance(-1) for @{ $self->{objects} };

    if (defined $obj_idx) {
        $self->{objects}->[$obj_idx]->selected(1);
        $self->{objects}->[$obj_idx]->selected_instance(0);
    }
    $self->selection_changed(1);
}

sub select_next {
    my ($self) = @_;
    
    return if !@{$self->{objects}};
    my ($obj_idx, $object) = $self->selected_object;
    if (!defined $obj_idx || $obj_idx == $#{$self->{objects}}) {
        $obj_idx = 0;
    } else {
        $obj_idx++;
    }
    $self->select_object($obj_idx);
    $self->refresh_canvases;
}

sub select_prev {
    my ($self) = @_;
    
    return if !@{$self->{objects}};
    my ($obj_idx, $object) = $self->selected_object;
    if (!defined $obj_idx || $obj_idx == 0) {
        $obj_idx = $#{$self->{objects}};
    } else {
        $obj_idx--;
    }
    $self->select_object($obj_idx);
    $self->refresh_canvases;
}

sub selected_object {
    my $self = shift;
    
    my $obj_idx = first { $self->{objects}[$_]->selected } 0..$#{ $self->{objects} };
    return undef if !defined $obj_idx;
    return ($obj_idx, $self->{objects}[$obj_idx]),
}

sub refresh_canvases {
    my ($self) = @_;
    
    $self->{canvas}->Refresh;
    $self->{canvas3D}->update if $self->{canvas3D};
    $self->{preview3D}->reload_print if $self->{preview3D};
}

sub validate_config {
    my $self = shift;
    
    eval {
        $self->config->validate;
    };
    return 0 if Slic3r::GUI::catch_error($self);    
    return 1;
}

sub statusbar {
    my $self = shift;
    return $self->GetFrame->{statusbar};
}

sub object_menu {
    my ($self) = @_;
    
    my $frame = $self->GetFrame;
    my $menu = Wx::Menu->new;
    wxTheApp->append_menu_item($menu, "Delete\tCtrl+Del", 'Remove the selected object', sub {
        $self->remove;
    }, undef, 'brick_delete.png');
    wxTheApp->append_menu_item($menu, "Increase copies\tCtrl++", 'Place one more copy of the selected object', sub {
        $self->increase;
    }, undef, 'add.png');
    wxTheApp->append_menu_item($menu, "Decrease copies\tCtrl+-", 'Remove one copy of the selected object', sub {
        $self->decrease;
    }, undef, 'delete.png');
    wxTheApp->append_menu_item($menu, "Set number of copies…", 'Change the number of copies of the selected object', sub {
        $self->set_number_of_copies;
    }, undef, 'textfield.png');

    if (!$Slic3r::GUI::Settings->{_}{autocenter}){
        $menu->AppendSeparator();
        wxTheApp->append_menu_item($menu, "Move to bed center", 'Center object around bed center', sub {
            $self->center_selected_object_on_bed;
        }, undef, 'arrow_in.png');
    }
    wxTheApp->append_menu_item($menu, "Rotate Face to Plane", 'Rotates the selected object to have the selected face parallel with a plane', sub {
        $self->rotate_face;
    }, undef, 'rotate_face.png');
    $menu->AppendSeparator();
    
    {
        my $rotateMenu = Wx::Menu->new;
        wxTheApp->append_menu_item($rotateMenu, "180° around X axis", 'Rotate the selected object by 180° around X axis', sub {
            $self->rotate(180, X);
        }, undef, @rotateX90ccw);
        wxTheApp->append_menu_item($rotateMenu, "180° around Y axis", 'Rotate the selected object by 180° around Y axis', sub {
            $self->rotate(180, Y);
        }, undef, @rotateY90ccw);
        wxTheApp->append_menu_item($rotateMenu, "180° around Z axis", 'Rotate the selected object by 180° around Z axis', sub {
            $self->rotate(180, Z);
        }, undef, @rotateZ90ccw);
        $rotateMenu->AppendSeparator();
        wxTheApp->append_menu_item($rotateMenu, "Around X axis…", 'Rotate the selected object by an arbitrary angle around X axis', sub {
            $self->rotate(undef, X);
        }, undef, @rotateX90ccw);
        wxTheApp->append_menu_item($rotateMenu, "Around Y axis…", 'Rotate the selected object by an arbitrary angle around Y axis', sub {
            $self->rotate(undef, Y);
        }, undef, @rotateY90ccw);
        wxTheApp->append_menu_item($rotateMenu, "Around Z axis…", 'Rotate the selected object by an arbitrary angle around Z axis', sub {
            $self->rotate(undef, Z);
        }, undef, @rotateZ90ccw);
        wxTheApp->append_submenu($menu, "Rotate", 'Rotate the selected object', $rotateMenu, undef, 'arrow_rotate_anticlockwise.png');
    }
    
    {
        my $mirrorMenu = Wx::Menu->new;
        wxTheApp->append_menu_item($mirrorMenu, "Along X axis", 'Mirror the selected object along the X axis', sub {
            $self->mirror(X);
        }, undef, @mirrorX);
        wxTheApp->append_menu_item($mirrorMenu, "Along Y axis", 'Mirror the selected object along the Y axis', sub {
            $self->mirror(Y);
        }, undef, @mirrorY);
        wxTheApp->append_menu_item($mirrorMenu, "Along Z axis", 'Mirror the selected object along the Z axis', sub {
            $self->mirror(Z);
        }, undef, @mirrorZ);
        wxTheApp->append_submenu($menu, "Mirror", 'Mirror the selected object', $mirrorMenu, undef, 'shape_flip_horizontal.png');
    }
    
    {
        my $scaleMenu = Wx::Menu->new;
        wxTheApp->append_menu_item($scaleMenu, "Uniformly…", 'Scale the selected object along the XYZ axes', sub {
            $self->changescale(undef);
        });
        wxTheApp->append_menu_item($scaleMenu, "Along X axis…", 'Scale the selected object along the X axis', sub {
            $self->changescale(X);
        }, undef, 'bullet_red.png');
        wxTheApp->append_menu_item($scaleMenu, "Along Y axis…", 'Scale the selected object along the Y axis', sub {
            $self->changescale(Y);
        }, undef, 'bullet_green.png');
        wxTheApp->append_menu_item($scaleMenu, "Along Z axis…", 'Scale the selected object along the Z axis', sub {
            $self->changescale(Z);
        }, undef, 'bullet_blue.png');
        wxTheApp->append_submenu($menu, "Scale", 'Scale the selected object by a given factor', $scaleMenu, undef, 'arrow_out.png');
    }
    
    {
        my $scaleToSizeMenu = Wx::Menu->new;
        wxTheApp->append_menu_item($scaleToSizeMenu, "Uniformly…", 'Scale the selected object along the XYZ axes', sub {
            $self->changescale(undef, 1);
        });
        wxTheApp->append_menu_item($scaleToSizeMenu, "Along X axis…", 'Scale the selected object along the X axis', sub {
            $self->changescale(X, 1);
        }, undef, 'bullet_red.png');
        wxTheApp->append_menu_item($scaleToSizeMenu, "Along Y axis…", 'Scale the selected object along the Y axis', sub {
            $self->changescale(Y, 1);
        }, undef, 'bullet_green.png');
        wxTheApp->append_menu_item($scaleToSizeMenu, "Along Z axis…", 'Scale the selected object along the Z axis', sub {
            $self->changescale(Z, 1);
        }, undef, 'bullet_blue.png');
        wxTheApp->append_submenu($menu, "Scale to size", 'Scale the selected object to match a given size', $scaleToSizeMenu, undef, 'arrow_out.png');
    }
    
    wxTheApp->append_menu_item($menu, "Split", 'Split the selected object into individual parts', sub {
        $self->split_object;
    }, undef, 'shape_ungroup.png');
    wxTheApp->append_menu_item($menu, "Cut…", 'Open the 3D cutting tool', sub {
        $self->object_cut_dialog;
    }, undef, 'package.png');
    wxTheApp->append_menu_item($menu, "Layer heights…", 'Open the dynamic layer height control', sub {
        $self->object_layers_dialog;
    }, undef, 'variable_layer_height.png');
    $menu->AppendSeparator();
    wxTheApp->append_menu_item($menu, "Settings…", 'Open the object editor dialog', sub {
        $self->object_settings_dialog;
    }, undef, 'cog.png');
    $menu->AppendSeparator();
    wxTheApp->append_menu_item($menu, "Reload from Disk", 'Reload the selected file from Disk', sub {
        $self->reload_from_disk;
    }, undef, 'arrow_refresh.png');
    wxTheApp->append_menu_item($menu, "Export object as STL…", 'Export this single object as STL file', sub {
        $self->export_object_stl;
    }, undef, 'brick_go.png');
    wxTheApp->append_menu_item($menu, "Export object and modifiers as AMF…", 'Export this single object and all associated modifiers as AMF file', sub {
        $self->export_object_amf;
    }, undef, 'brick_go.png');
    wxTheApp->append_menu_item($menu, "Export object and modifiers as 3MF…", 'Export this single object and all associated modifiers as 3MF file', sub {
            $self->export_object_tmf;
    }, undef, 'brick_go.png');
    
    return $menu;
}

# Set a camera direction, zoom to all objects.
sub select_view {
    my ($self, $direction) = @_;
    my $idx_page = $self->{preview_notebook}->GetSelection;
    my $page = ($idx_page == &Wx::wxNOT_FOUND) ? '3D' : $self->{preview_notebook}->GetPageText($idx_page);
    if ($page eq 'Preview') {
        $self->{preview3D}->canvas->select_view($direction);
        $self->{canvas3D}->set_viewport_from_scene($self->{preview3D}->canvas);
    } else {
        $self->{canvas3D}->select_view($direction);
        $self->{preview3D}->canvas->set_viewport_from_scene($self->{canvas3D});
    }
}

sub zoom{
    my ($self, $direction) = @_;
    #Apply Zoom to the current active tab
    my ($currentSelection) = $self->{preview_notebook}->GetSelection;
    if($currentSelection == 0){
        $self->{canvas3D}->zoom($direction) if($self->{canvas3D});
    }
    elsif($currentSelection == 2){ #3d Preview tab
        $self->{preview3D}->canvas->zoom($direction) if($self->{preview3D});
    }
    elsif($currentSelection == 3) { #2D toolpaths tab
        $self->{toolpaths2D}->{canvas}->zoom($direction) if($self->{toolpaths2D});
    }
}

package Slic3r::GUI::Plater::DropTarget;
use Wx::DND;
use base 'Wx::FileDropTarget';

sub new {
    my $class = shift;
    my ($window) = @_;
    my $self = $class->SUPER::new;
    $self->{window} = $window;
    return $self;
}

sub OnDropFiles {
    my $self = shift;
    my ($x, $y, $filenames) = @_;
    
    # stop scalars leaking on older perl
    # https://rt.perl.org/rt3/Public/Bug/Display.html?id=70602
    @_ = ();
    
    # only accept STL, OBJ and AMF files
    return 0 if grep !/\.(?:stl|obj|amf(?:\.xml)?)$/i, @$filenames;
    
    $self->{window}->load_file($_) for @$filenames;
}

# 2D preview of an object. Each object is previewed by its convex hull.
package Slic3r::GUI::Plater::Object;
use Moo;

use List::Util qw(first);
use Slic3r::Geometry qw(X Y Z MIN MAX deg2rad);

has 'name'                  => (is => 'rw', required => 1);
has 'identifier'            => (is => 'rw', required => 1);
has 'input_file'            => (is => 'rw');
has 'input_file_obj_idx'    => (is => 'rw');
has 'thumbnail'             => (is => 'rw'); # ExPolygon::Collection in scaled model units with no transforms
has 'transformed_thumbnail' => (is => 'rw');
has 'remaking_thumbnail'    => (is => 'rw', default => sub { 0 });
has 'instance_thumbnails'   => (is => 'ro', default => sub { [] });  # array of ExPolygon::Collection objects, each one representing the actual placed thumbnail of each instance in pixel units
has 'selected'              => (is => 'rw', default => sub { 0 });
has 'selected_instance'     => (is => 'rw', default => sub { -1 });

sub make_thumbnail {
    my ($self, $model, $obj_idx) = @_;
    
    # make method idempotent
    $self->thumbnail->clear;
    
    my $mesh = $model->objects->[$obj_idx]->raw_mesh;
    # Apply x, y rotations and scaling vector in case of reading a 3MF model object.
    my $model_instance = $model->objects->[$obj_idx]->instances->[0];
    $mesh->transform($model_instance->additional_trafo);

    if ($mesh->facets_count <= 5000) {
        # remove polygons with area <= 1mm
        my $area_threshold = Slic3r::Geometry::scale 1;
        $self->thumbnail->append(
            grep $_->area >= $area_threshold,
            @{ $mesh->horizontal_projection },   # horizontal_projection returns scaled expolygons
        );
        $self->thumbnail->simplify(0.5);
    } else {
        my $convex_hull = Slic3r::ExPolygon->new($mesh->convex_hull);
        $self->thumbnail->append($convex_hull);
    }
    
    return $self->thumbnail;
}

sub transform_thumbnail {
    my ($self, $model, $obj_idx) = @_;
    
    return unless defined $self->thumbnail;
    
    my $model_object = $model->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # the order of these transformations MUST be the same everywhere, including
    # in Slic3r::Print->add_model_object()
    my $t = $self->thumbnail->clone;
    $t->rotate($model_instance->rotation, Slic3r::Point->new(0,0));
    $t->scale($model_instance->scaling_factor);
    
    $self->transformed_thumbnail($t);
}

package Slic3r::GUI::Plater::OctoPrintSpoolDialog;
use Wx qw(:dialog :id :misc :sizer :icon wxTheApp);
use Wx::Event qw(EVT_BUTTON EVT_TEXT_ENTER);
use base 'Wx::Dialog';

sub new {
    my $class = shift;
    my ($parent, $filename) = @_;
    my $self = $class->SUPER::new($parent, -1, "Send to Server", wxDefaultPosition,
        [400, -1]);
    
    $self->{filename} = $filename;
    $Slic3r::GUI::Settings->{octoprint} //= {};
    
    my $optgroup;
    $optgroup = Slic3r::GUI::OptionsGroup->new(
        parent  => $self,
        title   => 'Send to Server',
        on_change => sub {
            my ($opt_id) = @_;
            
            if ($opt_id eq 'filename') {
                $self->{filename} = $optgroup->get_value($opt_id);
            } else {
                $Slic3r::GUI::Settings->{octoprint}{$opt_id} = $optgroup->get_value($opt_id);
            }
        },
        label_width => 200,
    );
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'filename',
        type        => 's',
        label       => 'File name',
        width       => 200,
        tooltip     => 'The name used for labelling the print job.',
        default     => $filename,
    ));
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'overwrite',
        type        => 'bool',
        label       => 'Overwrite existing file',
        tooltip     => 'If selected, any existing file with the same name will be overwritten without confirmation.',
        default     => $Slic3r::GUI::Settings->{octoprint}{overwrite} // 0,
    ));
    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
        opt_id      => 'start',
        type        => 'bool',
        label       => 'Start print',
        tooltip     => 'If selected, print will start after the upload.',
        default     => $Slic3r::GUI::Settings->{octoprint}{start} // 0,
    ));
    
    my $sizer = Wx::BoxSizer->new(wxVERTICAL);
    $sizer->Add($optgroup->sizer, 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT | wxRIGHT, 10);
    
    my $buttons = $self->CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    $sizer->Add($buttons, 0, wxEXPAND | wxBOTTOM | wxLEFT | wxRIGHT, 10);
    EVT_BUTTON($self, wxID_OK, sub {
        wxTheApp->save_settings;
        $self->EndModal(wxID_OK);
        $self->Destroy;
    });
    
    $self->SetSizer($sizer);
    $sizer->SetSizeHints($self);
    
    return $self;
}

1;
