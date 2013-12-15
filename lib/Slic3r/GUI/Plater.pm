package Slic3r::GUI::Plater;
use strict;
use warnings;
use utf8;

use File::Basename qw(basename dirname);
use List::Util qw(max sum first);
use Slic3r::Geometry::Clipper qw(offset JT_ROUND);
use Slic3r::Geometry qw(X Y Z MIN MAX convex_hull scale unscale);
use threads::shared qw(shared_clone);
use Wx qw(:bitmap :brush :button :cursor :dialog :filedialog :font :keycode :icon :id :listctrl :misc :panel :pen :sizer :toolbar :window);
use Wx::Event qw(EVT_BUTTON EVT_COMMAND EVT_KEY_DOWN EVT_LIST_ITEM_ACTIVATED EVT_LIST_ITEM_DESELECTED EVT_LIST_ITEM_SELECTED EVT_MOUSE_EVENTS EVT_PAINT EVT_TOOL EVT_CHOICE);
use base 'Wx::Panel';

use constant TB_ADD             => &Wx::NewId;
use constant TB_REMOVE          => &Wx::NewId;
use constant TB_RESET           => &Wx::NewId;
use constant TB_ARRANGE         => &Wx::NewId;
use constant TB_EXPORT_GCODE    => &Wx::NewId;
use constant TB_EXPORT_STL      => &Wx::NewId;
use constant TB_MORE    => &Wx::NewId;
use constant TB_FEWER   => &Wx::NewId;
use constant TB_45CW    => &Wx::NewId;
use constant TB_45CCW   => &Wx::NewId;
use constant TB_ROTATE  => &Wx::NewId;
use constant TB_SCALE   => &Wx::NewId;
use constant TB_SPLIT   => &Wx::NewId;
use constant TB_VIEW    => &Wx::NewId;
use constant TB_SETTINGS => &Wx::NewId;

# package variables to avoid passing lexicals to threads
our $THUMBNAIL_DONE_EVENT    : shared = Wx::NewEventType;
our $PROGRESS_BAR_EVENT      : shared = Wx::NewEventType;
our $MESSAGE_DIALOG_EVENT    : shared = Wx::NewEventType;
our $EXPORT_COMPLETED_EVENT  : shared = Wx::NewEventType;
our $EXPORT_FAILED_EVENT     : shared = Wx::NewEventType;

use constant CANVAS_SIZE => [335,335];
use constant CANVAS_TEXT => join('-', +(localtime)[3,4]) eq '13-8'
    ? 'What do you want to print today? ™' # Sept. 13, 2006. The first part ever printed by a RepRap to make another RepRap.
    : 'Drag your objects here';
use constant FILAMENT_CHOOSERS_SPACING => 3;

sub new {
    my $class = shift;
    my ($parent) = @_;
    my $self = $class->SUPER::new($parent, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    $self->{config} = Slic3r::Config->new_from_defaults(qw(
        bed_size print_center complete_objects extruder_clearance_radius skirts skirt_distance
    ));
    $self->{model} = Slic3r::Model->new;
    $self->{objects} = [];
    
    $self->{canvas} = Wx::Panel->new($self, -1, wxDefaultPosition, CANVAS_SIZE, wxTAB_TRAVERSAL);
    $self->{canvas}->SetBackgroundColour(Wx::wxWHITE);
    EVT_PAINT($self->{canvas}, \&repaint);
    EVT_MOUSE_EVENTS($self->{canvas}, \&mouse_event);
    
    $self->{objects_brush} = Wx::Brush->new(Wx::Colour->new(210,210,210), wxSOLID);
    $self->{selected_brush} = Wx::Brush->new(Wx::Colour->new(255,128,128), wxSOLID);
    $self->{dragged_brush} = Wx::Brush->new(Wx::Colour->new(128,128,255), wxSOLID);
    $self->{transparent_brush} = Wx::Brush->new(Wx::Colour->new(0,0,0), wxTRANSPARENT);
    $self->{grid_pen} = Wx::Pen->new(Wx::Colour->new(230,230,230), 1, wxSOLID);
    $self->{print_center_pen} = Wx::Pen->new(Wx::Colour->new(200,200,200), 1, wxSOLID);
    $self->{clearance_pen} = Wx::Pen->new(Wx::Colour->new(0,0,200), 1, wxSOLID);
    $self->{skirt_pen} = Wx::Pen->new(Wx::Colour->new(150,150,150), 1, wxSOLID);
    
    # toolbar for object manipulation
    if (!&Wx::wxMSW) {
        Wx::ToolTip::Enable(1);
        $self->{htoolbar} = Wx::ToolBar->new($self, -1, wxDefaultPosition, wxDefaultSize, wxTB_HORIZONTAL | wxTB_TEXT | wxBORDER_SIMPLE | wxTAB_TRAVERSAL);
        $self->{htoolbar}->AddTool(TB_ADD, "Add…", Wx::Bitmap->new("$Slic3r::var/brick_add.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_REMOVE, "Delete", Wx::Bitmap->new("$Slic3r::var/brick_delete.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_RESET, "Delete All", Wx::Bitmap->new("$Slic3r::var/cross.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_ARRANGE, "Arrange", Wx::Bitmap->new("$Slic3r::var/bricks.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
        $self->{htoolbar}->AddTool(TB_MORE, "More", Wx::Bitmap->new("$Slic3r::var/add.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_FEWER, "Fewer", Wx::Bitmap->new("$Slic3r::var/delete.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
        $self->{htoolbar}->AddTool(TB_45CCW, "45° ccw", Wx::Bitmap->new("$Slic3r::var/arrow_rotate_anticlockwise.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_45CW, "45° cw", Wx::Bitmap->new("$Slic3r::var/arrow_rotate_clockwise.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_ROTATE, "Rotate…", Wx::Bitmap->new("$Slic3r::var/arrow_rotate_clockwise.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_SCALE, "Scale…", Wx::Bitmap->new("$Slic3r::var/arrow_out.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_SPLIT, "Split", Wx::Bitmap->new("$Slic3r::var/shape_ungroup.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddSeparator;
        $self->{htoolbar}->AddTool(TB_VIEW, "View", Wx::Bitmap->new("$Slic3r::var/package.png", wxBITMAP_TYPE_PNG), '');
        $self->{htoolbar}->AddTool(TB_SETTINGS, "Settings…", Wx::Bitmap->new("$Slic3r::var/cog.png", wxBITMAP_TYPE_PNG), '');
    } else {
        my %tbar_buttons = (
            load            => "Add…",
            remove          => "Delete",
            reset           => "Delete All",
            arrange         => "Arrange",
            increase        => "",
            decrease        => "",
            rotate45ccw     => "",
            rotate45cw      => "",
            rotate          => "Rotate…",
            changescale     => "Scale…",
            split           => "Split",
            view            => "View",
            settings        => "Settings…",
        );
        $self->{btoolbar} = Wx::BoxSizer->new(wxHORIZONTAL);
        for (qw(load remove reset arrange increase decrease rotate45ccw rotate45cw rotate changescale split view settings)) {
            $self->{"btn_$_"} = Wx::Button->new($self, -1, $tbar_buttons{$_}, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            $self->{btoolbar}->Add($self->{"btn_$_"});
        }
    }

    $self->{list} = Wx::ListView->new($self, -1, wxDefaultPosition, wxDefaultSize, wxLC_SINGLE_SEL | wxLC_REPORT | wxBORDER_SUNKEN | wxTAB_TRAVERSAL | wxWANTS_CHARS);
    $self->{list}->InsertColumn(0, "Name", wxLIST_FORMAT_LEFT, 145);
    $self->{list}->InsertColumn(1, "Copies", wxLIST_FORMAT_CENTER, 45);
    $self->{list}->InsertColumn(2, "Scale", wxLIST_FORMAT_CENTER, wxLIST_AUTOSIZE_USEHEADER);
    EVT_LIST_ITEM_SELECTED($self, $self->{list}, \&list_item_selected);
    EVT_LIST_ITEM_DESELECTED($self, $self->{list}, \&list_item_deselected);
    EVT_LIST_ITEM_ACTIVATED($self, $self->{list}, \&list_item_activated);
    EVT_KEY_DOWN($self->{list}, sub {
        my ($list, $event) = @_;
        if ($event->GetKeyCode == WXK_TAB) {
            $list->Navigate($event->ShiftDown ? &Wx::wxNavigateBackward : &Wx::wxNavigateForward);
        } else {
            $event->Skip;
        }
    });
    
    # right pane buttons
    $self->{btn_export_gcode} = Wx::Button->new($self, -1, "Export G-code…", wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
    $self->{btn_export_stl} = Wx::Button->new($self, -1, "Export STL…", wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
    $self->{btn_export_gcode}->SetFont($Slic3r::GUI::small_font);
    $self->{btn_export_stl}->SetFont($Slic3r::GUI::small_font);
    
    if ($Slic3r::GUI::have_button_icons) {
        my %icons = qw(
            load            brick_add.png
            remove          brick_delete.png
            reset           cross.png
            arrange         bricks.png
            export_gcode    cog_go.png
            export_stl      brick_go.png
            
            increase        add.png
            decrease        delete.png
            rotate45cw      arrow_rotate_clockwise.png
            rotate45ccw     arrow_rotate_anticlockwise.png
            rotate          arrow_rotate_clockwise.png
            changescale     arrow_out.png
            split           shape_ungroup.png
            view            package.png
            settings        cog.png
        );
        for (grep $self->{"btn_$_"}, keys %icons) {
            $self->{"btn_$_"}->SetBitmap(Wx::Bitmap->new("$Slic3r::var/$icons{$_}", wxBITMAP_TYPE_PNG));
        }
    }
    $self->selection_changed(0);
    $self->object_list_changed;
    EVT_BUTTON($self, $self->{btn_export_gcode}, \&export_gcode);
    EVT_BUTTON($self, $self->{btn_export_stl}, \&export_stl);
    
    if ($self->{htoolbar}) {
        EVT_TOOL($self, TB_ADD, \&add);
        EVT_TOOL($self, TB_REMOVE, sub { $self->remove() }); # explicitly pass no argument to remove
        EVT_TOOL($self, TB_RESET, \&reset);
        EVT_TOOL($self, TB_ARRANGE, \&arrange);
        EVT_TOOL($self, TB_MORE, \&increase);
        EVT_TOOL($self, TB_FEWER, \&decrease);
        EVT_TOOL($self, TB_45CW, sub { $_[0]->rotate(-45) });
        EVT_TOOL($self, TB_45CCW, sub { $_[0]->rotate(45) });
        EVT_TOOL($self, TB_ROTATE, sub { $_[0]->rotate(undef) });
        EVT_TOOL($self, TB_SCALE, \&changescale);
        EVT_TOOL($self, TB_SPLIT, \&split_object);
        EVT_TOOL($self, TB_VIEW, sub { $_[0]->object_preview_dialog });
        EVT_TOOL($self, TB_SETTINGS, sub { $_[0]->object_settings_dialog });
    } else {
        EVT_BUTTON($self, $self->{btn_add}, \&add);
        EVT_BUTTON($self, $self->{btn_remove}, sub { $self->remove() }); # explicitly pass no argument to remove
        EVT_BUTTON($self, $self->{btn_reset}, \&reset);
        EVT_BUTTON($self, $self->{btn_arrange}, \&arrange);
        EVT_BUTTON($self, $self->{btn_increase}, \&increase);
        EVT_BUTTON($self, $self->{btn_decrease}, \&decrease);
        EVT_BUTTON($self, $self->{btn_rotate45cw}, sub { $_[0]->rotate(-45) });
        EVT_BUTTON($self, $self->{btn_rotate45ccw}, sub { $_[0]->rotate(45) });
        EVT_BUTTON($self, $self->{btn_changescale}, \&changescale);
        EVT_BUTTON($self, $self->{btn_rotate}, sub { $_[0]->rotate(undef) });
        EVT_BUTTON($self, $self->{btn_split}, \&split_object);
        EVT_BUTTON($self, $self->{btn_view}, sub { $_[0]->object_preview_dialog });
        EVT_BUTTON($self, $self->{btn_settings}, sub { $_[0]->object_settings_dialog });
    }
    
    $_->SetDropTarget(Slic3r::GUI::Plater::DropTarget->new($self))
        for $self, $self->{canvas}, $self->{list};
    
    EVT_COMMAND($self, -1, $THUMBNAIL_DONE_EVENT, sub {
        my ($self, $event) = @_;
        my ($obj_idx) = @{$event->GetData};
        return if !$self->{objects}[$obj_idx];  # object was deleted before thumbnail generation completed
        
        $self->on_thumbnail_made($obj_idx);
    });
    
    EVT_COMMAND($self, -1, $PROGRESS_BAR_EVENT, sub {
        my ($self, $event) = @_;
        my ($percent, $message) = @{$event->GetData};
        $self->statusbar->SetProgress($percent);
        $self->statusbar->SetStatusText("$message…");
    });
    
    EVT_COMMAND($self, -1, $MESSAGE_DIALOG_EVENT, sub {
        my ($self, $event) = @_;
        Wx::MessageDialog->new($self, @{$event->GetData})->ShowModal;
    });
    
    EVT_COMMAND($self, -1, $EXPORT_COMPLETED_EVENT, sub {
        my ($self, $event) = @_;
        $self->on_export_completed(@{$event->GetData});
    });
    
    EVT_COMMAND($self, -1, $EXPORT_FAILED_EVENT, sub {
        my ($self, $event) = @_;
        $self->on_export_failed;
    });
    
    $self->_update_bed_size;
    $self->recenter;
    
    {
        my $presets;
        if ($self->skeinpanel->{mode} eq 'expert') {
            $presets = Wx::BoxSizer->new(wxVERTICAL);
            my %group_labels = (
                print       => 'Print settings',
                filament    => 'Filament',
                printer     => 'Printer',
            );
            $self->{preset_choosers} = {};
            $self->{preset_choosers_sizers} = {};
            for my $group (qw(print filament printer)) {
                my $text = Wx::StaticText->new($self, -1, "$group_labels{$group}:", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
                $text->SetFont($Slic3r::GUI::small_font);
                my $choice = Wx::Choice->new($self, -1, wxDefaultPosition, [140, -1], []);
                $choice->SetFont($Slic3r::GUI::small_font);
                $self->{preset_choosers}{$group} = [$choice];
                EVT_CHOICE($choice, $choice, sub { $self->on_select_preset($group, @_) });
                
                $self->{preset_choosers_sizers}{$group} = Wx::BoxSizer->new(wxVERTICAL);
                $self->{preset_choosers_sizers}{$group}->Add($choice, 0, wxEXPAND | wxBOTTOM, FILAMENT_CHOOSERS_SPACING);
                
                $presets->Add($text, 0, wxALIGN_LEFT | wxRIGHT, 4);
                $presets->Add($self->{preset_choosers_sizers}{$group}, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, 8);
            }
        }
        
        my $object_info_sizer;
        {
            my $box = Wx::StaticBox->new($self, -1, "Info");
            $object_info_sizer = Wx::StaticBoxSizer->new($box, wxVERTICAL);
            my $grid_sizer = Wx::FlexGridSizer->new(3, 4, 5, 5);
            $grid_sizer->SetFlexibleDirection(wxHORIZONTAL);
            $grid_sizer->AddGrowableCol(1, 1);
            $grid_sizer->AddGrowableCol(3, 1);
            $object_info_sizer->Add($grid_sizer, 0, wxEXPAND);
            
            my @info = (
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
                    $self->{object_info_manifold_warning_icon} = Wx::StaticBitmap->new($self, -1, Wx::Bitmap->new("$Slic3r::var/error.png", wxBITMAP_TYPE_PNG));
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
        
        my $right_buttons_sizer = Wx::BoxSizer->new(wxVERTICAL);
        $right_buttons_sizer->Add($presets, 0, wxEXPAND, 0) if defined $presets;
        $right_buttons_sizer->Add($self->{btn_export_gcode}, 0, wxEXPAND | wxTOP, 8);
        $right_buttons_sizer->Add($self->{btn_export_stl}, 0, wxEXPAND | wxTOP, 2);
        
        my $right_top_sizer = Wx::BoxSizer->new(wxHORIZONTAL);
        $right_top_sizer->Add($self->{list}, 1, wxEXPAND | wxLEFT, 5);
        $right_top_sizer->Add($right_buttons_sizer, 0, wxEXPAND | wxALL, 10);
        
        my $right_sizer = Wx::BoxSizer->new(wxVERTICAL);
        $right_sizer->Add($right_top_sizer, 1, wxEXPAND | wxBOTTOM, 10);
        $right_sizer->Add($object_info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
        
        my $hsizer = Wx::BoxSizer->new(wxHORIZONTAL);
        $hsizer->Add($self->{canvas}, 0, wxTOP, 1);
        $hsizer->Add($right_sizer, 1, wxEXPAND | wxBOTTOM, 0);
        
        my $sizer = Wx::BoxSizer->new(wxVERTICAL);
        $sizer->Add($self->{htoolbar}, 0, wxEXPAND, 0) if $self->{htoolbar};
        $sizer->Add($self->{btoolbar}, 0, wxEXPAND, 0) if $self->{btoolbar};
        $sizer->Add($hsizer, 1, wxEXPAND, 0);
        
        $sizer->SetSizeHints($self);
        $self->SetSizer($sizer);
    }
    return $self;
}

sub on_select_preset {
	my $self = shift;
	my ($group, $choice) = @_;
	
	if ($group eq 'filament' && @{$self->{preset_choosers}{filament}} > 1) {
		my @filament_presets = $self->filament_presets;
		$Slic3r::GUI::Settings->{presets}{filament} = $choice->GetString($filament_presets[0]) . ".ini";
		$Slic3r::GUI::Settings->{presets}{"filament_${_}"} = $choice->GetString($filament_presets[$_])
			for 1 .. $#filament_presets;
		Slic3r::GUI->save_settings;
		return;
	}
	$self->skeinpanel->{options_tabs}{$group}->select_preset($choice->GetSelection);
}

sub skeinpanel {
    my $self = shift;
    return $self->GetParent->GetParent;
}

sub update_presets {
    my $self = shift;
    my ($group, $items, $selected) = @_;
    
    foreach my $choice (@{ $self->{preset_choosers}{$group} }) {
        my $sel = $choice->GetSelection;
        $choice->Clear;
        $choice->Append($_) for @$items;
        $choice->SetSelection($sel) if $sel <= $#$items;
    }
    $self->{preset_choosers}{$group}[0]->SetSelection($selected);
}

sub filament_presets {
    my $self = shift;
    
    return map $_->GetSelection, @{ $self->{preset_choosers}{filament} };
}

sub add {
    my $self = shift;
    
    my $dir = $Slic3r::GUI::Settings->{recent}{skein_directory} || $Slic3r::GUI::Settings->{recent}{config_directory} || '';
    my $dialog = Wx::FileDialog->new($self, 'Choose one or more files (STL/OBJ/AMF):', $dir, "", &Slic3r::GUI::SkeinPanel::MODEL_WILDCARD, wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
    if ($dialog->ShowModal != wxID_OK) {
        $dialog->Destroy;
        return;
    }
    my @input_files = $dialog->GetPaths;
    $dialog->Destroy;
    $self->load_file($_) for @input_files;
}

sub load_file {
    my $self = shift;
    my ($input_file) = @_;
    
    $Slic3r::GUI::Settings->{recent}{skein_directory} = dirname($input_file);
    Slic3r::GUI->save_settings;
    
    my $process_dialog = Wx::ProgressDialog->new('Loading…', "Processing input file…", 100, $self, 0);
    $process_dialog->Pulse;
    
    local $SIG{__WARN__} = Slic3r::GUI::warning_catcher($self);
    my $model = Slic3r::Model->read_from_file($input_file);
    if( !defined $model ) {
        $process_dialog->Destroy;
        $self->statusbar->SetStatusText("Unrecognized file format");
        return;
    }

    $self->load_model_object($_) for @{$model->objects};
    
    $process_dialog->Destroy;
    $self->statusbar->SetStatusText("Loaded " . basename($input_file));
}

sub load_model_object {
    my ($self, $model_object) = @_;
    
    my $o = $self->{model}->add_object($model_object);
        
    push @{ $self->{objects} }, Slic3r::GUI::Plater::Object->new(
        name => basename($model_object->input_file),
    );
    
    my $need_arrange = 0;
    if (!defined $model_object->instances) {
        # if object has no defined position(s) we need to rearrange everything after loading
        $need_arrange = 1;
        
        # add a default instance and center object around origin
        $o->center_around_origin;
        $o->add_instance(offset => [30,30]);
    }
    
    $self->object_loaded($#{ $self->{objects} }, no_arrange => !$need_arrange);
}

sub object_loaded {
    my $self = shift;
    my ($obj_idx, %params) = @_;
    
    my $object = $self->{objects}[$obj_idx];
    my $model_object = $self->{model}->objects->[$obj_idx];
    $self->{list}->InsertStringItem($obj_idx, $object->name);
    $self->{list}->SetItemFont($obj_idx, Wx::Font->new(10, wxDEFAULT, wxNORMAL, wxNORMAL))
        if $self->{list}->can('SetItemFont');  # legacy code for wxPerl < 0.9918 not supporting SetItemFont()
    
    $self->{list}->SetItem($obj_idx, 1, $model_object->instances_count);
    $self->{list}->SetItem($obj_idx, 2, ($model_object->instances->[0]->scaling_factor * 100) . "%");
    
    $self->make_thumbnail($obj_idx);
    $self->arrange unless $params{no_arrange};
    $self->recenter;
    $self->{list}->Update;
    $self->{list}->Select($obj_idx, 1);
    $self->object_list_changed;
}

sub remove {
    my $self = shift;
    my ($obj_idx) = @_;
    
    # if no object index is supplied, remove the selected one
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }
    
    splice @{$self->{objects}}, $obj_idx, 1;
    $self->{model}->delete_object($obj_idx);
    $self->{list}->DeleteItem($obj_idx);
    $self->object_list_changed;
    
    $self->select_object(undef);
    $self->recenter;
    $self->{canvas}->Refresh;
}

sub reset {
    my $self = shift;
    
    @{$self->{objects}} = ();
    $self->{model}->delete_all_objects;
    $self->{list}->DeleteAllItems;
    $self->object_list_changed;
    
    $self->select_object(undef);
    $self->{canvas}->Refresh;
}

sub increase {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $last_instance = $model_object->instances->[-1];
    $model_object->add_instance(
        offset          => [ map 10+$_, @{$last_instance->offset} ],
        scaling_factor  => $last_instance->scaling_factor,
        rotation        => $last_instance->rotation,
    );
    $self->{list}->SetItem($obj_idx, 1, $model_object->instances_count);
    $self->arrange;
}

sub decrease {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    if ($model_object->instances_count >= 2) {
        $model_object->delete_last_instance;
        $self->{list}->SetItem($obj_idx, 1, $model_object->instances_count);
    } else {
        $self->remove;
    }
    
    if ($self->{objects}[$obj_idx]) {
        $self->{list}->Select($obj_idx, 0);
        $self->{list}->Select($obj_idx, 1);
    }
    $self->recenter;
    $self->{canvas}->Refresh;
}

sub rotate {
    my $self = shift;
    my ($angle) = @_;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # we need thumbnail to be computed before allowing rotation
    return if !$object->thumbnail;
    
    if (!defined $angle) {
        $angle = Wx::GetNumberFromUser("", "Enter the rotation angle:", "Rotate", $model_instance->rotation, -364, 364, $self);
        return if !$angle || $angle == -1;
        $angle = 0 - $angle;  # rotate clockwise (be consistent with button icon)
    }
    
    {
        my $new_angle = $model_instance->rotation + $angle;
        $_->rotation($new_angle) for @{ $model_object->instances };
        $model_object->update_bounding_box;
        $object->transform_thumbnail($self->{model}, $obj_idx);
    }
    $self->selection_changed;  # refresh info (size etc.)
    $self->recenter;
    $self->{canvas}->Refresh;
}

sub changescale {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $model_object = $self->{model}->objects->[$obj_idx];
    my $model_instance = $model_object->instances->[0];
    
    # we need thumbnail to be computed before allowing scaling
    return if !$object->thumbnail;
    
    # max scale factor should be above 2540 to allow importing files exported in inches
    my $scale = Wx::GetNumberFromUser("", "Enter the scale % for the selected object:", "Scale", $model_instance->scaling_factor*100, 0, 100000, $self);
    return if !$scale || $scale == -1;
    
    $self->{list}->SetItem($obj_idx, 2, "$scale%");
    $scale /= 100;  # turn percent into factor
    {
        my $variation = $scale / $model_instance->scaling_factor;
        foreach my $range (@{ $model_object->layer_height_ranges }) {
            $range->[0] *= $variation;
            $range->[1] *= $variation;
        }
        $_->scaling_factor($scale) for @{ $model_object->instances };
        $model_object->update_bounding_box;
        $object->transform_thumbnail($self->{model}, $obj_idx);
    }
    $self->selection_changed(1);  # refresh info (size, volume etc.)
    $self->arrange;
}

sub arrange {
    my $self = shift;
    
    eval {
        $self->{model}->arrange_objects($self->skeinpanel->config);
    };
    # ignore arrange warnings on purpose
    
    $self->recenter;
    $self->{canvas}->Refresh;
}

sub split_object {
    my $self = shift;
    
    my ($obj_idx, $current_object)  = $self->selected_object;
    my $current_model_object        = $self->{model}->objects->[$obj_idx];
    
    if (@{$current_model_object->volumes} > 1) {
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be split because it contains more than one volume/material.");
        return;
    }
    
    my @new_meshes = @{$current_model_object->volumes->[0]->mesh->split};
    if (@new_meshes == 1) {
        Slic3r::GUI::warning_catcher($self)->("The selected object couldn't be split because it already contains a single part.");
        return;
    }

    # remove the original object before spawning the object_loaded event, otherwise 
    # we'll pass the wrong $obj_idx to it (which won't be recognized after the
    # thumbnail thread returns)
    $self->remove($obj_idx);
    $current_object = $obj_idx = undef;
    
    # create a bogus Model object, we only need to instantiate the new Model::Object objects
    my $new_model = Slic3r::Model->new;
    
    foreach my $mesh (@new_meshes) {
        my $model_object = $new_model->add_object(
            input_file              => $current_model_object->input_file,
            config                  => $current_model_object->config->clone,
            layer_height_ranges     => $current_model_object->layer_height_ranges,  # TODO: clone this
            material_mapping        => $current_model_object->material_mapping,     # TODO: clone this
        );
        $model_object->add_volume(
            mesh        => $mesh,
            material_id => $current_model_object->volumes->[0]->material_id,
        );
        
        for my $instance_idx (0..$#{ $current_model_object->instances }) {
            my $current_instance = $current_model_object->instances->[$instance_idx];
            $model_object->add_instance(
                offset          => [
                    $current_instance->offset->[X] + ($instance_idx * 10),
                    $current_instance->offset->[Y] + ($instance_idx * 10),
                ],
                rotation        => $current_instance->rotation,
                scaling_factor  => $current_instance->scaling_factor,
            );
        }
        # we need to center this single object around origin
        $model_object->center_around_origin;
        $self->load_model_object($model_object);
    }
}

sub export_gcode {
    my $self = shift;
    
    if ($self->{export_thread}) {
        Wx::MessageDialog->new($self, "Another slicing job is currently running.", 'Error', wxOK | wxICON_ERROR)->ShowModal;
        return;
    }
    
    # get config before spawning the thread because it needs GetParent and it's not available there
    our $config          = $self->skeinpanel->config;
    our $extra_variables = $self->skeinpanel->extra_variables;
    
    # select output file
    $self->{output_file} = $main::opt{output};
    {
        $self->{output_file} = $self->skeinpanel->init_print->expanded_output_filepath($self->{output_file}, $self->{model}->objects->[0]->input_file);
        my $dlg = Wx::FileDialog->new($self, 'Save G-code file as:', Slic3r::GUI->output_path(dirname($self->{output_file})),
            basename($self->{output_file}), &Slic3r::GUI::SkeinPanel::FILE_WILDCARDS->{gcode}, wxFD_SAVE);
        if ($dlg->ShowModal != wxID_OK) {
            $dlg->Destroy;
            return;
        }
        $Slic3r::GUI::Settings->{_}{last_output_path} = dirname($dlg->GetPath);
        Slic3r::GUI->save_settings;
        $self->{output_file} = $Slic3r::GUI::SkeinPanel::last_output_file = $dlg->GetPath;
        $dlg->Destroy;
    }
    
    $self->statusbar->StartBusy;
    
    # It looks like declaring a local $SIG{__WARN__} prevents the ugly
    # "Attempt to free unreferenced scalar" warning...
    local $SIG{__WARN__} = Slic3r::GUI::warning_catcher($self);
    
    if ($Slic3r::have_threads) {
        @_ = ();
        
        # some perls (including 5.14.2) crash on threads->exit if we pass lexicals to the thread
        our $_thread_self = $self;
        
        $self->{export_thread} = threads->create(sub {
            $_thread_self->export_gcode2(
                $config,
                $extra_variables,
                $_thread_self->{output_file},
                progressbar     => sub { Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $PROGRESS_BAR_EVENT, shared_clone([@_]))) },
                message_dialog  => sub { Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $MESSAGE_DIALOG_EVENT, shared_clone([@_]))) },
                on_completed    => sub { Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $EXPORT_COMPLETED_EVENT, shared_clone([@_]))) },
                catch_error     => sub {
                    Slic3r::GUI::catch_error($_thread_self, $_[0], sub {
                        Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $MESSAGE_DIALOG_EVENT, shared_clone([@_])));
                        Wx::PostEvent($_thread_self, Wx::PlThreadEvent->new(-1, $EXPORT_FAILED_EVENT, undef));
                    });
                },
            );
            Slic3r::thread_cleanup();
        });
        $self->statusbar->SetCancelCallback(sub {
            $self->{export_thread}->kill('KILL')->join;
            $self->{export_thread} = undef;
            $self->statusbar->StopBusy;
            $self->statusbar->SetStatusText("Export cancelled");
        });
    } else {
        $self->export_gcode2(
            $config,
            $extra_variables,
            $self->{output_file},
            progressbar => sub {
                my ($percent, $message) = @_;
                $self->statusbar->SetProgress($percent);
                $self->statusbar->SetStatusText("$message…");
            },
            message_dialog => sub { Wx::MessageDialog->new($self, @_)->ShowModal },
            on_completed => sub { $self->on_export_completed(@_) },
            catch_error => sub { Slic3r::GUI::catch_error($self, @_) && $self->on_export_failed },
        );
    }
}

sub export_gcode2 {
    my $self = shift;
    my ($config, $extra_variables, $output_file, %params) = @_;
    local $SIG{'KILL'} = sub {
        Slic3r::debugf "Exporting cancelled; exiting thread...\n";
        Slic3r::thread_cleanup();
        threads->exit();
    } if $Slic3r::have_threads;
    
    my $print = Slic3r::Print->new(
        config          => $config,
        extra_variables => $extra_variables,
    );
    
    eval {
        $print->config->validate;
        $print->add_model_object($_) for @{ $self->{model}->objects };
        $print->validate;
        
        {
            my @warnings = ();
            local $SIG{__WARN__} = sub { push @warnings, $_[0] };
            
            my %params = (
                output_file => $output_file,
                status_cb   => sub { $params{progressbar}->(@_) },
                quiet       => 1,
            );
            if ($params{export_svg}) {
                $print->export_svg(%params);
            } else {
                $print->export_gcode(%params);
            }
            Slic3r::GUI::warning_catcher($self, $Slic3r::have_threads ? sub {
                Wx::PostEvent($self, Wx::PlThreadEvent->new(-1, $MESSAGE_DIALOG_EVENT, shared_clone([@_])));
            } : undef)->($_) for @warnings;
        }
        
        my $message = "Your files were successfully sliced";
        if ($print->processing_time) {
            $message .= ' in';
            my $minutes = int($print->processing_time/60);
            $message .= sprintf " %d minutes and", $minutes if $minutes;
            $message .= sprintf " %.1f seconds", $print->processing_time - $minutes*60;
        }
        $message .= ".";
        $params{on_completed}->($message);
    };
    $params{catch_error}->();
}

sub on_export_completed {
    my $self = shift;
    my ($message) = @_;
    
    $self->{export_thread}->detach if $self->{export_thread};
    $self->{export_thread} = undef;
    $self->statusbar->SetCancelCallback(undef);
    $self->statusbar->StopBusy;
    $self->statusbar->SetStatusText("G-code file exported to $self->{output_file}");
    &Wx::wxTheApp->notify($message);
}

sub on_export_failed {
    my $self = shift;
    
    $self->{export_thread}->detach if $self->{export_thread};
    $self->{export_thread} = undef;
    $self->statusbar->SetCancelCallback(undef);
    $self->statusbar->StopBusy;
    $self->statusbar->SetStatusText("Export failed");
}

sub export_stl {
    my $self = shift;
        
    my $output_file = $self->_get_export_file('STL') or return;
    Slic3r::Format::STL->write_file($output_file, $self->{model}, binary => 1);
    $self->statusbar->SetStatusText("STL file exported to $output_file");
}

sub export_amf {
    my $self = shift;
        
    my $output_file = $self->_get_export_file('AMF') or return;
    Slic3r::Format::AMF->write_file($output_file, $self->{model});
    $self->statusbar->SetStatusText("AMF file exported to $output_file");
}

sub _get_export_file {
    my $self = shift;
    my ($format) = @_;
    
    my $suffix = $format eq 'STL' ? '.stl' : '.amf.xml';
    
    my $output_file = $main::opt{output};
    {
        $output_file = $self->skeinpanel->init_print->expanded_output_filepath($output_file, $self->{model}->objects->[0]->input_file);
        $output_file =~ s/\.gcode$/$suffix/i;
        my $dlg = Wx::FileDialog->new($self, "Save $format file as:", dirname($output_file),
            basename($output_file), &Slic3r::GUI::SkeinPanel::MODEL_WILDCARD, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if ($dlg->ShowModal != wxID_OK) {
            $dlg->Destroy;
            return undef;
        }
        $output_file = $Slic3r::GUI::SkeinPanel::last_output_file = $dlg->GetPath;
        $dlg->Destroy;
    }
    return $output_file;
}

sub make_thumbnail {
    my $self = shift;
    my ($obj_idx) = @_;
    
    my $plater_object = $self->{objects}[$obj_idx];
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
    $Slic3r::have_threads ? threads->create($cb)->detach : $cb->();
}

sub on_thumbnail_made {
    my $self = shift;
    my ($obj_idx) = @_;
    
    $self->{objects}[$obj_idx]->transform_thumbnail($self->{model}, $obj_idx);
    $self->recenter;
    $self->{canvas}->Refresh;
}

sub clean_instance_thumbnails {
    my ($self) = @_;
    
    foreach my $object (@{ $self->{objects} }) {
        @{ $object->instance_thumbnails } = ();
    }
}

# this method gets called whenever bed is resized or the objects' bounding box changes
# (i.e. when an object is added/removed/moved/rotated/scaled)
sub recenter {
    my $self = shift;
    
    return unless @{$self->{objects}};
    
    # get model bounding box in pixels
    my $print_bb = $self->{model}->bounding_box;
    $print_bb->scale($self->{scaling_factor});
    
    # get model size in pixels
    my $print_size = $print_bb->size;
    
    # $self->{shift} contains the offset in pixels to add to object thumbnails
    # in order to center them
    $self->{shift} = [
        -$print_bb->x_min + ($self->{canvas}->GetSize->GetWidth  - $print_size->[X]) / 2,
        -$print_bb->y_min + ($self->{canvas}->GetSize->GetHeight - $print_size->[Y]) / 2,
    ];
}

sub on_config_change {
    my $self = shift;
    my ($opt_key, $value) = @_;
    if ($opt_key eq 'extruders_count' && defined $value) {
        my $choices = $self->{preset_choosers}{filament};
        while (@$choices < $value) {
        	my @presets = $choices->[0]->GetStrings;
            push @$choices, Wx::Choice->new($self, -1, wxDefaultPosition, [150, -1], [@presets]);
            $choices->[-1]->SetFont($Slic3r::GUI::small_font);
            $self->{preset_choosers_sizers}{filament}->Add($choices->[-1], 0, wxEXPAND | wxBOTTOM, FILAMENT_CHOOSERS_SPACING);
            EVT_CHOICE($choices->[-1], $choices->[-1], sub { $self->on_select_preset('filament', @_) });
            my $i = first { $choices->[-1]->GetString($_) eq ($Slic3r::GUI::Settings->{presets}{"filament_" . $#$choices} || '') } 0 .. $#presets;
        	$choices->[-1]->SetSelection($i || 0);
        }
        while (@$choices > $value) {
            $self->{preset_choosers_sizers}{filament}->Remove(-1);
            $choices->[-1]->Destroy;
            pop @$choices;
        }
        $self->Layout;
    } elsif ($self->{config}->has($opt_key)) {
        $self->{config}->set($opt_key, $value);
        $self->_update_bed_size if $opt_key eq 'bed_size';
    }
}

sub _update_bed_size {
    my $self = shift;
    
    # supposing the preview canvas is square, calculate the scaling factor
    # to constrain print bed area inside preview
    # when the canvas is not rendered yet, its GetSize() method returns 0,0
    # scaling_factor is expressed in pixel / mm
    $self->{scaling_factor} = CANVAS_SIZE->[X] / max(@{ $self->{config}->bed_size });
    $self->recenter;
}

# this is called on the canvas
sub repaint {
    my ($self, $event) = @_;
    my $parent = $self->GetParent;
    
    my $dc = Wx::PaintDC->new($self);
    my $size = $self->GetSize;
    my @size = ($size->GetWidth, $size->GetHeight);
    
    # draw grid
    $dc->SetPen($parent->{grid_pen});
    my $step = 10 * $parent->{scaling_factor};
    for (my $x = $step; $x <= $size[X]; $x += $step) {
        $dc->DrawLine($x, 0, $x, $size[Y]);
    }
    for (my $y = $step; $y <= $size[Y]; $y += $step) {
        $dc->DrawLine(0, $y, $size[X], $y);
    }
    
    # draw print center
    if (@{$parent->{objects}}) {
        $dc->SetPen($parent->{print_center_pen});
        $dc->DrawLine($size[X]/2, 0, $size[X]/2, $size[Y]);
        $dc->DrawLine(0, $size[Y]/2, $size[X], $size[Y]/2);
        $dc->SetTextForeground(Wx::Colour->new(0,0,0));
        $dc->SetFont(Wx::Font->new(10, wxDEFAULT, wxNORMAL, wxNORMAL));
        $dc->DrawLabel("X = " . $parent->{config}->print_center->[X], Wx::Rect->new(0, 0, $self->GetSize->GetWidth, $self->GetSize->GetHeight), wxALIGN_CENTER_HORIZONTAL | wxALIGN_BOTTOM);
        $dc->DrawRotatedText("Y = " . $parent->{config}->print_center->[Y], 0, $size[Y]/2+15, 90);
    }
    
    # draw frame
    if (0) {
        $dc->SetPen(wxBLACK_PEN);
        $dc->SetBrush($parent->{transparent_brush});
        $dc->DrawRectangle(0, 0, @size);
    }
    
    # draw text if plate is empty
    if (!@{$parent->{objects}}) {
        $dc->SetTextForeground(Wx::Colour->new(150,50,50));
        $dc->SetFont(Wx::Font->new(14, wxDEFAULT, wxNORMAL, wxNORMAL));
        $dc->DrawLabel(CANVAS_TEXT, Wx::Rect->new(0, 0, $self->GetSize->GetWidth, $self->GetSize->GetHeight), wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }
    
    # draw thumbnails
    $dc->SetPen(wxBLACK_PEN);
    $parent->clean_instance_thumbnails;
    for my $obj_idx (0 .. $#{$parent->{objects}}) {
        my $object = $parent->{objects}[$obj_idx];
        my $model_object = $parent->{model}->objects->[$obj_idx];
        next unless defined $object->thumbnail;
        for my $instance_idx (0 .. $#{$model_object->instances}) {
            my $instance = $model_object->instances->[$instance_idx];
            next if !defined $object->transformed_thumbnail;
            
            my $thumbnail = $object->transformed_thumbnail->clone;                  # in scaled coordinates
            $thumbnail->scale(&Slic3r::SCALING_FACTOR * $parent->{scaling_factor}); # in unscaled pixels
            $thumbnail->translate(map $_ * $parent->{scaling_factor}, @{$instance->offset});
            $thumbnail->translate(@{$parent->{shift}});
            
            $object->instance_thumbnails->[$instance_idx] = $thumbnail;
            
            if (defined $self->{drag_object} && $self->{drag_object}[0] == $obj_idx && $self->{drag_object}[1] == $instance_idx) {
                $dc->SetBrush($parent->{dragged_brush});
            } elsif ($object->selected) {
                $dc->SetBrush($parent->{selected_brush});
            } else {
                $dc->SetBrush($parent->{objects_brush});
            }
            foreach my $expolygon (@$thumbnail) {
                my $points = $expolygon->contour->pp;
                $dc->DrawPolygon($parent->_y($points), 0, 0);
            }
            
            if (0) {
                # draw bounding box for debugging purposes
                my $bb = $model_object->instance_bounding_box($instance_idx);
                $bb->scale($parent->{scaling_factor});
                # no need to translate by instance offset because instance_bounding_box() does that
                $bb->translate(@{$parent->{shift}}, 0);
                my $points = $bb->polygon->pp;
                $dc->SetPen($parent->{clearance_pen});
                $dc->SetBrush($parent->{transparent_brush});
                $dc->DrawPolygon($parent->_y($points), 0, 0);
            }
            
            # if sequential printing is enabled and we have more than one object, draw clearance area
            if ($parent->{config}->complete_objects && (map @{$_->instances}, @{$parent->{model}->objects}) > 1) {
                my ($clearance) = @{offset([$thumbnail->convex_hull], ($parent->{config}->extruder_clearance_radius / 2) * $parent->{scaling_factor}, 100, JT_ROUND)};
                $dc->SetPen($parent->{clearance_pen});
                $dc->SetBrush($parent->{transparent_brush});
                $dc->DrawPolygon($parent->_y($clearance), 0, 0);
            }
        }
    }
    
    # draw skirt
    if (@{$parent->{objects}} && $parent->{config}->skirts) {
        my @points = map @{$_->contour}, map @$_, map @{$_->instance_thumbnails}, @{$parent->{objects}};
        if (@points >= 3) {
            my ($convex_hull) = @{offset([convex_hull(\@points)], $parent->{config}->skirt_distance * $parent->{scaling_factor}, 100, JT_ROUND)};
            $dc->SetPen($parent->{skirt_pen});
            $dc->SetBrush($parent->{transparent_brush});
            $dc->DrawPolygon($parent->_y($convex_hull), 0, 0);
        }
    }
    
    $event->Skip;
}

sub mouse_event {
    my ($self, $event) = @_;
    my $parent = $self->GetParent;
    
    my $point = $event->GetPosition;
    my $pos = Slic3r::Point->new(@{$parent->_y([[$point->x, $point->y]])->[0]}); # in pixels
    if ($event->ButtonDown(&Wx::wxMOUSE_BTN_LEFT)) {
        $parent->select_object(undef);
        for my $obj_idx (0 .. $#{$parent->{objects}}) {
            my $object = $parent->{objects}->[$obj_idx];
            for my $instance_idx (0 .. $#{ $object->instance_thumbnails }) {
                my $thumbnail = $object->instance_thumbnails->[$instance_idx];
                if ($thumbnail->contains_point($pos)) {
                    $parent->select_object($obj_idx);
                    my $instance = $parent->{model}->objects->[$obj_idx]->instances->[$instance_idx];
                    $self->{drag_start_pos} = [   # displacement between the click and the instance origin
                        $pos->x - $parent->{shift}[X] - ($instance->offset->[X] * $parent->{scaling_factor}),
                        $pos->y - $parent->{shift}[Y] - ($instance->offset->[Y] * $parent->{scaling_factor}),
                    ];
                    $self->{drag_object} = [ $obj_idx, $instance_idx ];
                }
            }
        }
        $parent->Refresh;
    } elsif ($event->ButtonUp(&Wx::wxMOUSE_BTN_LEFT)) {
        $parent->recenter;
        $parent->Refresh;
        $self->{drag_start_pos} = undef;
        $self->{drag_object} = undef;
        $self->SetCursor(wxSTANDARD_CURSOR);
    } elsif ($event->ButtonDClick) {
    	$parent->object_preview_dialog if $parent->selected_object;
    } elsif ($event->Dragging) {
        return if !$self->{drag_start_pos}; # concurrency problems
        my ($obj_idx, $instance_idx) = @{ $self->{drag_object} };
        my $model_object = $parent->{model}->objects->[$obj_idx];
        $model_object->instances->[$instance_idx]->offset([
            ($pos->[X] - $self->{drag_start_pos}[X] - $parent->{shift}[X]) / $parent->{scaling_factor},
            ($pos->[Y] - $self->{drag_start_pos}[Y] - $parent->{shift}[Y]) / $parent->{scaling_factor},
        ]);
        $model_object->update_bounding_box;
        $parent->Refresh;
    } elsif ($event->Moving) {
        my $cursor = wxSTANDARD_CURSOR;
        if (defined first { $_->contains_point($pos) } map @{$_->instance_thumbnails}, @{ $parent->{objects} }) {
            $cursor = Wx::Cursor->new(wxCURSOR_HAND);
        }
        $self->SetCursor($cursor);
    }
}

sub list_item_deselected {
    my ($self, $event) = @_;
    
    if ($self->{list}->GetFirstSelected == -1) {
        $self->select_object(undef);
        $self->{canvas}->Refresh;
    }
}

sub list_item_selected {
    my ($self, $event) = @_;
    
    my $obj_idx = $event->GetIndex;
    $self->select_object($obj_idx);
    $self->{canvas}->Refresh;
}

sub list_item_activated {
    my ($self, $event, $obj_idx) = @_;
    
    $obj_idx //= $event->GetIndex;
	$self->object_preview_dialog($obj_idx);
}

sub object_preview_dialog {
    my $self = shift;
    my ($obj_idx) = @_;
    
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }
    
    if (!$Slic3r::GUI::have_OpenGL) {
        Slic3r::GUI::show_error($self, "Please install the OpenGL modules to use this feature (see build instructions).");
        return;
    }
    
    my $dlg = Slic3r::GUI::Plater::ObjectPreviewDialog->new($self,
		object          => $self->{objects}[$obj_idx],
		model_object    => $self->{model}->objects->[$obj_idx],
	);
	$dlg->ShowModal;
}

sub object_settings_dialog {
    my $self = shift;
    my ($obj_idx) = @_;
    
    if (!defined $obj_idx) {
        ($obj_idx, undef) = $self->selected_object;
    }
    
    # validate config before opening the settings dialog because
    # that dialog can't be closed if validation fails, but user
    # can't fix any error which is outside that dialog
    return unless $self->validate_config;
    
    my $dlg = Slic3r::GUI::Plater::ObjectSettingsDialog->new($self,
		object          => $self->{objects}[$obj_idx],
		model_object    => $self->{model}->objects->[$obj_idx],
	);
	$dlg->ShowModal;
}

sub object_list_changed {
    my $self = shift;
    
    my $have_objects = @{$self->{objects}} ? 1 : 0;
    my $method = $have_objects ? 'Enable' : 'Disable';
    $self->{"btn_$_"}->$method
        for grep $self->{"btn_$_"}, qw(reset arrange export_gcode export_stl);
    
    if ($self->{htoolbar}) {
        $self->{htoolbar}->EnableTool($_, $have_objects)
            for (TB_RESET, TB_ARRANGE);
    }
}

sub selection_changed {
    my $self = shift;
    
    my ($obj_idx, $object) = $self->selected_object;
    my $have_sel = defined $obj_idx;
    
    my $method = $have_sel ? 'Enable' : 'Disable';
    $self->{"btn_$_"}->$method
        for grep $self->{"btn_$_"}, qw(remove increase decrease rotate45cw rotate45ccw rotate changescale split view settings);
    
    if ($self->{htoolbar}) {
        $self->{htoolbar}->EnableTool($_, $have_sel)
            for (TB_REMOVE, TB_MORE, TB_FEWER, TB_45CW, TB_45CCW, TB_ROTATE, TB_SCALE, TB_SPLIT, TB_VIEW, TB_SETTINGS);
    }
    
    if ($self->{object_info_size}) { # have we already loaded the info pane?
        if ($have_sel) {
            my $model_object = $self->{model}->objects->[$obj_idx];
            my $model_instance = $model_object->instances->[0];
            $self->{object_info_size}->SetLabel(sprintf("%.2f x %.2f x %.2f", @{$model_object->instance_bounding_box(0)->size}));
            $self->{object_info_materials}->SetLabel($model_object->materials_count);
            
            if (my $stats = $model_object->mesh_stats) {
                $self->{object_info_volume}->SetLabel(sprintf('%.2f', $stats->{volume} * ($model_instance->scaling_factor**3)));
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
            $self->{"object_info_$_"}->SetLabel("") for qw(size volume facets materials manifold);
            $self->{object_info_manifold_warning_icon}->Hide;
            $self->{object_info_manifold}->SetToolTipString("");
        }
        $self->Layout;
    }
}

sub select_object {
    my ($self, $obj_idx) = @_;
    
    $_->selected(0) for @{ $self->{objects} };
    if (defined $obj_idx) {
        $self->{objects}->[$obj_idx]->selected(1);
        $self->{list}->Select($obj_idx, 1);
    } else {
        # TODO: deselect all in list
    }
    $self->selection_changed(1);
}

sub selected_object {
    my $self = shift;
    
    my $obj_idx = first { $self->{objects}[$_]->selected } 0..$#{ $self->{objects} };
    return undef if !defined $obj_idx;
    return ($obj_idx, $self->{objects}[$obj_idx]),
}

sub validate_config {
    my $self = shift;
    
    eval {
        $self->skeinpanel->config->validate;
    };
    return 0 if Slic3r::GUI::catch_error($self);    
    return 1;
}

sub statusbar {
    my $self = shift;
    return $self->skeinpanel->GetParent->{statusbar};
}

sub to_pixel {
    my $self = shift;
    return $_[0] * $self->{scaling_factor} * &Slic3r::SCALING_FACTOR;
}

sub to_units {
    my $self = shift;
    return $_[0] / $self->{scaling_factor} / &Slic3r::SCALING_FACTOR;
}

sub _y {
    my $self = shift;
    my ($points) = @_;
    my $height = $self->{canvas}->GetSize->GetHeight;
    return [ map [ $_->[X], $height - $_->[Y] ], @$points ];
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

package Slic3r::GUI::Plater::Object;
use Moo;

use List::Util qw(first);
use Slic3r::Geometry qw(X Y Z MIN MAX deg2rad);

has 'name'                  => (is => 'rw', required => 1);
has 'thumbnail'             => (is => 'rw'); # ExPolygon::Collection in scaled model units with no transforms
has 'transformed_thumbnail' => (is => 'rw');
has 'instance_thumbnails'   => (is => 'ro', default => sub { [] });  # array of ExPolygon::Collection objects, each one representing the actual placed thumbnail of each instance in pixel units
has 'selected'              => (is => 'rw', default => sub { 0 });

sub make_thumbnail {
    my ($self, $model, $obj_idx) = @_;
    
    my $mesh = $model->objects->[$obj_idx]->raw_mesh;
    
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
    $t->rotate(deg2rad($model_instance->rotation), Slic3r::Point->new(0,0));
    $t->scale($model_instance->scaling_factor);
    
    $self->transformed_thumbnail($t);
}

1;
