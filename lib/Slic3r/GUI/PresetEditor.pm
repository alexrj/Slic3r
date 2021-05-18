package Slic3r::GUI::PresetEditor;
use strict;
use warnings;
use utf8;

use File::Basename qw(basename);
use List::Util qw(first any);
use Wx qw(:bookctrl :dialog :keycode :icon :id :misc :panel :sizer :treectrl :window
    :button wxTheApp);
use Wx::Event qw(EVT_BUTTON EVT_CHOICE EVT_KEY_DOWN EVT_TREE_SEL_CHANGED EVT_CHECKBOX);
use base qw(Wx::Panel Class::Accessor);

__PACKAGE__->mk_accessors(qw(current_preset config));

sub new {
    my $class = shift;
    my ($parent, %params) = @_;
    my $self = $class->SUPER::new($parent, -1, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    
    $self->{presets} = wxTheApp->presets->{$self->name};
    
    # horizontal sizer
    $self->{sizer} = Wx::BoxSizer->new(wxHORIZONTAL);
    #$self->{sizer}->SetSizeHints($self);
    $self->SetSizer($self->{sizer});
    
    # left vertical sizer
    my $left_sizer = Wx::BoxSizer->new(wxVERTICAL);
    $self->{sizer}->Add($left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 3);
    
    my $left_col_width = -1;
    
    # preset chooser
    {
        
        # choice menu
        $self->{presets_choice} = Wx::Choice->new($self, -1, wxDefaultPosition, [$left_col_width, -1], []);
        $self->{presets_choice}->SetFont($Slic3r::GUI::small_font);
        
        # buttons
        $self->{btn_save_preset} = Wx::BitmapButton->new($self, -1, Wx::Bitmap->new($Slic3r::var->("disk.png"), wxBITMAP_TYPE_PNG), 
            wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        $self->{btn_delete_preset} = Wx::BitmapButton->new($self, -1, Wx::Bitmap->new($Slic3r::var->("delete.png"), wxBITMAP_TYPE_PNG), 
            wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        $self->{btn_save_preset}->SetToolTipString("Save current " . lc($self->title));
        $self->{btn_delete_preset}->SetToolTipString("Delete this preset");
        $self->{btn_delete_preset}->Disable;
        
        ### These cause GTK warnings:
        ###my $box = Wx::StaticBox->new($self, -1, "Presets:", wxDefaultPosition, [$left_col_width, 50]);
        ###my $hsizer = Wx::StaticBoxSizer->new($box, wxHORIZONTAL);
        
        my $hsizer = Wx::BoxSizer->new(wxHORIZONTAL);
        
        $left_sizer->Add($hsizer, 0, wxEXPAND | wxBOTTOM, 5);
        $hsizer->Add($self->{presets_choice}, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 3);
        $hsizer->Add($self->{btn_save_preset}, 0, wxALIGN_CENTER_VERTICAL);
        $hsizer->Add($self->{btn_delete_preset}, 0, wxALIGN_CENTER_VERTICAL);
    }
    
    # tree
    $self->{treectrl} = Wx::TreeCtrl->new($self, -1, wxDefaultPosition, [$left_col_width, -1], wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_SUNKEN | wxWANTS_CHARS);
    $left_sizer->Add($self->{treectrl}, 1, wxEXPAND);
    $self->{icons} = Wx::ImageList->new(16, 16, 1);
    $self->{treectrl}->AssignImageList($self->{icons});
    $self->{iconcount} = -1;
    $self->{treectrl}->AddRoot("root");
    $self->{pages} = [];
    $self->{treectrl}->SetIndent(0);
    $self->{disable_tree_sel_changed_event} = 0;
    EVT_TREE_SEL_CHANGED($parent, $self->{treectrl}, sub {
        return if $self->{disable_tree_sel_changed_event};
        my $page = first { $_->{title} eq $self->{treectrl}->GetItemText($self->{treectrl}->GetSelection) } @{$self->{pages}}
            or return;
        $_->Hide for @{$self->{pages}};
        $page->Show;
        $self->{sizer}->Layout;
        $self->Refresh;
    });
    EVT_KEY_DOWN($self->{treectrl}, sub {
        my ($treectrl, $event) = @_;
        if ($event->GetKeyCode == WXK_TAB) {
            $treectrl->Navigate($event->ShiftDown ? &Wx::wxNavigateBackward : &Wx::wxNavigateForward);
        } else {
            $event->Skip;
        }
    });
    
    EVT_CHOICE($parent, $self->{presets_choice}, sub {
        $self->_on_select_preset;
    });
    
    EVT_BUTTON($self, $self->{btn_save_preset}, sub { $self->save_preset });
    
    EVT_BUTTON($self, $self->{btn_delete_preset}, sub {
        my $res = Wx::MessageDialog->new($self, "Are you sure you want to delete the selected preset?", 'Delete Preset', wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION)->ShowModal;
        return unless $res == wxID_YES;
        
        $self->current_preset->delete;
        $self->current_preset(undef);
        wxTheApp->load_presets;
        $self->load_presets;
        $self->select_preset(0, 1);
    });
    
    $self->config(Slic3r::Config->new_from_defaults($self->options));
    
    $self->build;
    $self->update_tree;
    $self->load_presets;
    $self->_update;
    
    return $self;
}

# This is called by the save button.
sub save_preset {
    my ($self) = @_;
    
    # since buttons (and choices too) don't get focus on Mac, we set focus manually
    # to the treectrl so that the EVT_* events are fired for the input field having
    # focus currently. is there anything better than this?
    $self->{treectrl}->SetFocus;
    
    my $preset = $self->current_preset;
    $preset->save_prompt($self);
    $self->load_presets;
    $self->select_preset_by_name($preset->name);
    
    $self->{on_save_preset}->($self->name, $preset) if $self->{on_save_preset};
    
    return 1;
}

sub on_save_preset {
    my ($self, $cb) = @_;
    $self->{on_save_preset} = $cb;
}

sub on_value_change {
    my ($self, $cb) = @_;
    $self->{on_value_change} = $cb;
}

# This method is supposed to be called whenever new values are loaded
# or changed by user (so also when a preset is loaded).
# propagate event to the parent
sub _on_value_change {
    my ($self, $opt_key) = @_;
    wxTheApp->CallAfter(sub {
        $self->current_preset->_dirty_config->apply($self->config);
        $self->{on_value_change}->($opt_key) if $self->{on_value_change};
        $self->load_presets;
        $self->_update($opt_key);
    });
}

sub _update {}

sub on_preset_loaded {}

sub select_preset {
    my ($self, $i, $force) = @_;
    
    $self->{presets_choice}->SetSelection($i);
    $self->_on_select_preset($force);
}

sub select_preset_by_name {
    my ($self, $name, $force) = @_;
    
    my $presets = wxTheApp->presets->{$self->name};
    my $i = first { $presets->[$_]->name eq $name } 0..$#$presets;
    if (!defined $i) {
        warn "No preset named $name";
        return 0;
    }
    $self->{presets_choice}->SetSelection($i);
    $self->_on_select_preset($force);
}

sub prompt_unsaved_changes {
    my ($self) = @_;
    
    return 1 if !$self->current_preset;
    return $self->current_preset->prompt_unsaved_changes($self);
}

sub on_select_preset {
    my ($self, $cb) = @_;
    $self->{on_select_preset} = $cb;
}

sub _on_select_preset {
    my ($self, $force) = @_;
    
    # This method is called:
    # - upon first initialization;
    # - whenever user selects a preset from the dropdown;
    # - whenever select_preset() or select_preset_by_name() are called.
    
    # Get the selected name.
    my $preset = wxTheApp->presets->{$self->name}->[$self->{presets_choice}->GetSelection];
    
    # If selection didn't change, do nothing.
    # (But still reset current_preset because it might contain an older object of the
    # current preset)
    if (defined $self->current_preset && $preset->name eq $self->current_preset->name) {
        $self->current_preset($preset);
        return;
    }
    
    # If we have unsaved changes, prompt user.
    if (!$force && !$self->prompt_unsaved_changes) {
        # User decided not to save the current changes, so we restore the previous selection.
        my $presets = wxTheApp->presets->{$self->name};
        my $i = first { $presets->[$_]->name eq $self->current_preset->name } 0..$#$presets;
        $self->{presets_choice}->SetSelection($i);
        return;
    }
    
    $self->current_preset($preset);
    
    # We reload presets in order to remove the "(modified)" suffix in case user was 
    # prompted and chose to discard changes.
    $self->load_presets;
    
    $self->reload_preset;
    
    eval {
        local $SIG{__WARN__} = Slic3r::GUI::warning_catcher($self);
        ($preset->default || $preset->external)
            ? $self->{btn_delete_preset}->Disable
            : $self->{btn_delete_preset}->Enable;
        
        $self->_update;
        $self->on_preset_loaded;
    };
    if ($@) {
        $@ = "I was unable to load the selected config file: $@";
        Slic3r::GUI::catch_error($self);
    }
    
    $self->{on_select_preset}->($self->name, $preset) if $self->{on_select_preset};
}

sub add_options_page {
    my $self = shift;
    my ($title, $icon, %params) = @_;
    
    if ($icon) {
        my $bitmap = Wx::Bitmap->new($Slic3r::var->($icon), wxBITMAP_TYPE_PNG);
        $self->{icons}->Add($bitmap);
        $self->{iconcount}++;
    }
    
    my $page = Slic3r::GUI::PresetEditor::Page->new($self, $title, $self->{iconcount});
    $page->Hide;
    $self->{sizer}->Add($page, 1, wxEXPAND | wxLEFT, 5);
    push @{$self->{pages}}, $page;
    return $page;
}

sub reload_preset {
    my ($self) = @_;
    
    $self->current_preset->load_config if !$self->current_preset->_loaded;
    $self->config->clear;
    $self->config->apply($self->current_preset->dirty_config);
    $self->reload_config;
}

sub reload_config {
    my $self = shift;
    
    $_->reload_config for @{$self->{pages}};
}

sub update_tree {
    my ($self) = @_;
    
    # get label of the currently selected item
    my $selected = $self->{treectrl}->GetItemText($self->{treectrl}->GetSelection);
    
    my $rootItem = $self->{treectrl}->GetRootItem;
    $self->{treectrl}->DeleteChildren($rootItem);
    my $have_selection = 0;
    foreach my $page (@{$self->{pages}}) {
        my $itemId = $self->{treectrl}->AppendItem($rootItem, $page->{title}, $page->{iconID});
        if ($page->{title} eq $selected) {
            $self->{disable_tree_sel_changed_event} = 1;
            $self->{treectrl}->SelectItem($itemId);
            $self->{disable_tree_sel_changed_event} = 0;
            $have_selection = 1;
        }
    }
    
    if (!$have_selection) {
        # this is triggered on first load, so we don't disable the sel change event
        $self->{treectrl}->SelectItem($self->{treectrl}->GetFirstChild($rootItem));
    }
}

sub load_presets {
    my $self = shift;
    
    my $presets = wxTheApp->presets->{$self->name};
    $self->{presets_choice}->Clear;
    foreach my $preset (@$presets) {
        $self->{presets_choice}->Append($preset->dropdown_name);
        
        # Preserve selection.
        if ($self->current_preset && $self->current_preset->name eq $preset->name) {
            $self->{presets_choice}->SetSelection($self->{presets_choice}->GetCount-1);
        }
    }
}

# This is called internally whenever we make automatic adjustments to configuration
# based on user actions.
sub _load_config {
    my $self = shift;
    my ($config) = @_;
    
    my $diff = $self->config->diff($config);
    $self->config->set($_, $config->get($_)) for @$diff;
    # First apply all changes, then call all the _on_value_change triggers.
    $self->_on_value_change($_) for @$diff;
    $self->reload_config;
    $self->_update;
}

sub get_field {
    my ($self, $opt_key, $opt_index) = @_;
    
    foreach my $page (@{ $self->{pages} }) {
        my $field = $page->get_field($opt_key, $opt_index);
        return $field if defined $field;
    }
    return undef;
}

sub set_value {
    my $self = shift;
    my ($opt_key, $value) = @_;
    
    my $changed = 0;
    foreach my $page (@{ $self->{pages} }) {
        $changed = 1 if $page->set_value($opt_key, $value);
    }
    return $changed;
}

sub _compatible_printers_widget {
    my ($self) = @_;
    
    return sub {
        my ($parent) = @_;
        
        my $checkbox = $self->{compatible_printers_checkbox} = Wx::CheckBox->new($parent, -1, "All");
        
        my $btn = $self->{compatible_printers_btn} = Wx::Button->new($parent, -1, "Set…", wxDefaultPosition, wxDefaultSize,
            wxBU_LEFT | wxBU_EXACTFIT);
        $btn->SetFont($Slic3r::GUI::small_font);
        if ($Slic3r::GUI::have_button_icons) {
            $btn->SetBitmap(Wx::Bitmap->new($Slic3r::var->("printer_empty.png"), wxBITMAP_TYPE_PNG));
        }
        
        my $sizer = Wx::BoxSizer->new(wxHORIZONTAL);
        $sizer->Add($checkbox, 0, wxALIGN_CENTER_VERTICAL);
        $sizer->Add($btn, 0, wxALIGN_CENTER_VERTICAL);
        
        EVT_CHECKBOX($self, $checkbox, sub {
            if ($checkbox->GetValue) {
                $btn->Disable;
            } else {
                $btn->Enable;
            }
        });
        
        EVT_BUTTON($self, $btn, sub {
            my @presets = map $_->name, grep !$_->default && !$_->external,
                @{wxTheApp->presets->{printer}};
            
            my $dlg = Wx::MultiChoiceDialog->new($self,
                "Select the printers this profile is compatible with.",
                "Compatible printers", \@presets);
            
            my @selections = ();
            foreach my $preset_name (@{ $self->config->get('compatible_printers') }) {
                push @selections, first { $presets[$_] eq $preset_name } 0..$#presets;
            }
            $dlg->SetSelections(@selections);
            
            if ($dlg->ShowModal == wxID_OK) {
                my $value = [ @presets[$dlg->GetSelections] ];
                if (!@$value) {
                    $checkbox->SetValue(1);
                    $btn->Disable;
                }
                $self->config->set('compatible_printers', $value);
                $self->_on_value_change('compatible_printers');
            }
        });
        
        return $sizer;
    };
}

sub _reload_compatible_printers_widget {
    my ($self) = @_;
    
    if (@{ $self->config->get('compatible_printers') }) {
        $self->{compatible_printers_checkbox}->SetValue(0);
        $self->{compatible_printers_btn}->Enable;
    } else {
        $self->{compatible_printers_checkbox}->SetValue(1);
        $self->{compatible_printers_btn}->Disable;
    }
}

sub options { die "Unimplemented options()"; }
sub overridable_options { () }
sub overriding_options  { () }

package Slic3r::GUI::PresetEditor::Print;
use base 'Slic3r::GUI::PresetEditor';

use List::Util qw(first any);
use Wx qw(:icon :dialog :id :misc :button :sizer);
use Wx::Event qw(EVT_BUTTON EVT_CHECKLISTBOX);

sub name { 'print' }
sub title { 'Print Settings' }

sub options {
    return qw(
        layer_height first_layer_height
        adaptive_slicing adaptive_slicing_quality match_horizontal_surfaces
        perimeters spiral_vase
        top_solid_layers min_shell_thickness min_top_bottom_shell_thickness bottom_solid_layers
        extra_perimeters avoid_crossing_perimeters thin_walls overhangs
        seam_position external_perimeters_first
        fill_density fill_pattern top_infill_pattern bottom_infill_pattern fill_gaps
        infill_every_layers infill_only_where_needed
        solid_infill_every_layers fill_angle solid_infill_below_area 
        only_retract_when_crossing_perimeters infill_first
        max_print_speed max_volumetric_speed
        perimeter_speed small_perimeter_speed external_perimeter_speed infill_speed 
        solid_infill_speed top_solid_infill_speed support_material_speed 
        support_material_interface_speed bridge_speed gap_fill_speed
        travel_speed
        first_layer_speed
        perimeter_acceleration infill_acceleration bridge_acceleration 
        first_layer_acceleration default_acceleration
        skirts skirt_distance skirt_height min_skirt_length
        brim_connections_width brim_layers brim_ears brim_ears_max_angle brim_width interior_brim_width
        support_material support_material_threshold support_material_max_layers support_material_enforce_layers
        raft_layers
        support_material_pattern support_material_spacing support_material_angle
        support_material_pillar_size support_material_pillar_spacing
        support_material_interface_layers support_material_interface_spacing
        support_material_contact_distance support_material_buildplate_only dont_support_bridges
        notes
        complete_objects extruder_clearance_radius extruder_clearance_height
        gcode_comments output_filename_format
        label_printed_objects
        post_process
        perimeter_extruder infill_extruder solid_infill_extruder
        support_material_extruder support_material_interface_extruder
        ooze_prevention standby_temperature_delta
        interface_shells regions_overlap
        extrusion_width first_layer_extrusion_width perimeter_extrusion_width 
        external_perimeter_extrusion_width infill_extrusion_width solid_infill_extrusion_width 
        top_infill_extrusion_width support_material_extrusion_width
        support_material_interface_extrusion_width infill_overlap bridge_flow_ratio
        xy_size_compensation resolution shortcuts compatible_printers
        print_settings_id
    )
}

sub build {
    my $self = shift;
    
    my $shortcuts_widget = sub {
        my ($parent) = @_;
        
        my $Options = $Slic3r::Config::Options;
        my %options = (
            map { $_ => sprintf('%s > %s', $Options->{$_}{category}, $Options->{$_}{full_label} // $Options->{$_}{label}) }
                grep { exists $Options->{$_} && $Options->{$_}{category} } $self->options
        );
        my @opt_keys = sort { $options{$a} cmp $options{$b} } keys %options;
        $self->{shortcuts_opt_keys} = [ @opt_keys ];
        
        my $listbox = $self->{shortcuts_list} = Wx::CheckListBox->new($parent, -1,
            wxDefaultPosition, [-1, 320], [ map $options{$_}, @opt_keys ]);
        
        EVT_CHECKLISTBOX($self, $listbox, sub {
            my $value = [ map $opt_keys[$_], grep $listbox->IsChecked($_), 0..$#opt_keys ];
            $self->config->set('shortcuts', $value);
            $self->_on_value_change('shortcuts');
        });
        
        my $sizer = Wx::BoxSizer->new(wxVERTICAL);
        $sizer->Add($listbox, 0, wxEXPAND);
        
        return $sizer;
    };
    
    {
        my $page = $self->add_options_page('Layers and perimeters', 'layers.png');
        {
            my $optgroup = $page->new_optgroup('Layer height');
            $optgroup->append_single_option_line('layer_height');
            $optgroup->append_single_option_line('first_layer_height');
            $optgroup->append_single_option_line('adaptive_slicing');
            $optgroup->append_single_option_line('adaptive_slicing_quality');
            $optgroup->get_field('adaptive_slicing_quality')->set_scale(1);
            $optgroup->append_single_option_line('match_horizontal_surfaces');
        }
        {
            my $optgroup = $page->new_optgroup('Vertical shells');
            $optgroup->append_single_option_line('perimeters');
            $optgroup->append_single_option_line('min_shell_thickness');
            $optgroup->append_single_option_line('spiral_vase');
        }
        {
            my $optgroup = $page->new_optgroup('Horizontal shells');
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label => 'Solid layers',
            );
            $line->append_option($optgroup->get_option('top_solid_layers'));
            $line->append_option($optgroup->get_option('bottom_solid_layers'));
            $optgroup->append_line($line);

            $optgroup->append_single_option_line('min_top_bottom_shell_thickness');
        }
        {
            my $optgroup = $page->new_optgroup('Quality (slower slicing)');
            $optgroup->append_single_option_line('extra_perimeters');
            $optgroup->append_single_option_line('avoid_crossing_perimeters');
            $optgroup->append_single_option_line('thin_walls');
            $optgroup->append_single_option_line('overhangs');
        }
        {
            my $optgroup = $page->new_optgroup('Advanced');
            $optgroup->append_single_option_line('seam_position');
            $optgroup->append_single_option_line('external_perimeters_first');
        }
    }
    
    {
        my $page = $self->add_options_page('Infill', 'infill.png');
        {
            my $optgroup = $page->new_optgroup('Infill');
            $optgroup->append_single_option_line('fill_density');
            $optgroup->append_single_option_line('fill_pattern');
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label => 'External infill pattern',
                );
                $line->append_option($optgroup->get_option('top_infill_pattern'));
                $line->append_option($optgroup->get_option('bottom_infill_pattern'));
                $optgroup->append_line($line);
            }
        }
        {
            my $optgroup = $page->new_optgroup('Reducing printing time');
            $optgroup->append_single_option_line('infill_every_layers');
            $optgroup->append_single_option_line('infill_only_where_needed');
        }
        {
            my $optgroup = $page->new_optgroup('Advanced');
            $optgroup->append_single_option_line('fill_gaps');
            $optgroup->append_single_option_line('solid_infill_every_layers');
            $optgroup->append_single_option_line('fill_angle');
            $optgroup->append_single_option_line('solid_infill_below_area');
            $optgroup->append_single_option_line('only_retract_when_crossing_perimeters');
            $optgroup->append_single_option_line('infill_first');
        }
    }
    
    {
        my $page = $self->add_options_page('Skirt and brim', 'box.png');
        {
            my $optgroup = $page->new_optgroup('Skirt');
            $optgroup->append_single_option_line('skirts');
            $optgroup->append_single_option_line('skirt_distance');
            $optgroup->append_single_option_line('skirt_height');
            $optgroup->append_single_option_line('min_skirt_length');
        }
        {
            my $optgroup = $page->new_optgroup('Brim');
            $optgroup->append_single_option_line('brim_width');
            $optgroup->append_single_option_line('brim_ears');
            $optgroup->append_single_option_line('brim_layers');
            $optgroup->append_single_option_line('brim_ears_max_angle');
            $optgroup->append_single_option_line('interior_brim_width');
            $optgroup->append_single_option_line('brim_connections_width');
        }
    }
    
    {
        my $page = $self->add_options_page('Support material', 'building.png');
        {
            my $optgroup = $page->new_optgroup('Support material');
            $optgroup->append_single_option_line('support_material');
            $optgroup->append_single_option_line('support_material_threshold');
            $optgroup->append_single_option_line('support_material_max_layers');
            $optgroup->append_single_option_line('support_material_enforce_layers');
        }
        {
            my $optgroup = $page->new_optgroup('Raft');
            $optgroup->append_single_option_line('raft_layers');
        }
        {
            my $optgroup = $page->new_optgroup('Options for support material and raft');
            $optgroup->append_single_option_line('support_material_contact_distance');
            $optgroup->append_single_option_line('support_material_pattern');
            $optgroup->append_single_option_line('support_material_spacing');
            $optgroup->append_single_option_line('support_material_angle');
            $optgroup->append_single_option_line('support_material_pillar_size');
            $optgroup->append_single_option_line('support_material_pillar_spacing');
            $optgroup->append_single_option_line('support_material_interface_layers');
            $optgroup->append_single_option_line('support_material_interface_spacing');
            $optgroup->append_single_option_line('support_material_buildplate_only');
            $optgroup->append_single_option_line('dont_support_bridges');
        }
    }

    {
        my $page = $self->add_options_page('Speed', 'time.png');
        {
            my $optgroup = $page->new_optgroup('Speed for print moves');
            $optgroup->append_single_option_line($_, undef, width => 100)
                for qw(perimeter_speed small_perimeter_speed external_perimeter_speed
                    infill_speed solid_infill_speed top_solid_infill_speed
                    gap_fill_speed bridge_speed
                    support_material_speed support_material_interface_speed
                );
        }
        {
            my $optgroup = $page->new_optgroup('Speed for non-print moves');
            $optgroup->append_single_option_line('travel_speed');
        }
        {
            my $optgroup = $page->new_optgroup('Modifiers');
            $optgroup->append_single_option_line('first_layer_speed');
        }
        {
            my $optgroup = $page->new_optgroup('Acceleration control (advanced)');
            $optgroup->append_single_option_line('perimeter_acceleration');
            $optgroup->append_single_option_line('infill_acceleration');
            $optgroup->append_single_option_line('bridge_acceleration');
            $optgroup->append_single_option_line('first_layer_acceleration');
            $optgroup->append_single_option_line('default_acceleration');
        }
        {
            my $optgroup = $page->new_optgroup('Autospeed (advanced)');
            $optgroup->append_single_option_line('max_print_speed');
            $optgroup->append_single_option_line('max_volumetric_speed');
        }
    }
    
    {
        my $page = $self->add_options_page('Multiple extruders', 'funnel.png');
        {
            my $optgroup = $page->new_optgroup('Extruders');
            $optgroup->append_single_option_line('perimeter_extruder');
            $optgroup->append_single_option_line('infill_extruder');
            $optgroup->append_single_option_line('solid_infill_extruder');
            $optgroup->append_single_option_line('support_material_extruder');
            $optgroup->append_single_option_line('support_material_interface_extruder');
        }
        {
            my $optgroup = $page->new_optgroup('Ooze prevention');
            $optgroup->append_single_option_line('ooze_prevention');
            $optgroup->append_single_option_line('standby_temperature_delta');
        }
        {
            my $optgroup = $page->new_optgroup('Advanced');
            $optgroup->append_single_option_line('regions_overlap');
            $optgroup->append_single_option_line('interface_shells');
        }
    }
    
    {
        my $page = $self->add_options_page('Advanced', 'wand.png');
        {
            my $optgroup = $page->new_optgroup('Extrusion width',
                label_width => 180,
            );
            $optgroup->append_single_option_line($_, undef, width => 100)
                for qw(extrusion_width first_layer_extrusion_width
                    perimeter_extrusion_width external_perimeter_extrusion_width
                    infill_extrusion_width solid_infill_extrusion_width
                    top_infill_extrusion_width support_material_interface_extrusion_width 
                    support_material_extrusion_width);
        }
        {
            my $optgroup = $page->new_optgroup('Overlap');
            $optgroup->append_single_option_line('infill_overlap');
        }
        {
            my $optgroup = $page->new_optgroup('Flow');
            $optgroup->append_single_option_line('bridge_flow_ratio');
        }
        {
            my $optgroup = $page->new_optgroup('Other');
            $optgroup->append_single_option_line('xy_size_compensation');
            $optgroup->append_single_option_line('resolution');
        }
    }
    
    {
        my $page = $self->add_options_page('Output options', 'page_white_go.png');
        {
            my $optgroup = $page->new_optgroup('Sequential printing');
            $optgroup->append_single_option_line('complete_objects');
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label => 'Extruder clearance (mm)',
            );
            foreach my $opt_key (qw(extruder_clearance_radius extruder_clearance_height)) {
                my $option = $optgroup->get_option($opt_key);
                $option->width(60);
                $line->append_option($option);
            }
            $optgroup->append_line($line);
        }
        {
            my $optgroup = $page->new_optgroup('Output file');
            $optgroup->append_single_option_line('gcode_comments');
            $optgroup->append_single_option_line('label_printed_objects');
            
            {
                my $option = $optgroup->get_option('output_filename_format');
                $option->full_width(1);
                $optgroup->append_single_option_line($option);
            }
        }
        {
            my $optgroup = $page->new_optgroup('Post-processing scripts',
                label_width => 0,
            );
            my $option = $optgroup->get_option('post_process');
            $option->full_width(1);
            $option->height(50);
            $optgroup->append_single_option_line($option);
        }
    }
    
    {
        my $page = $self->add_options_page('Notes', 'note.png');
        {
            my $optgroup = $page->new_optgroup('Notes',
                label_width => 0,
            );
            my $option = $optgroup->get_option('notes');
            $option->full_width(1);
            $option->height(250);
            $optgroup->append_single_option_line($option);
        }
    }
    
    {
        my $page = $self->add_options_page('Shortcuts', 'wrench.png');
        {
            my $optgroup = $page->new_optgroup('Profile preferences');
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label       => 'Compatible printers',
                    widget      => $self->_compatible_printers_widget,
                );
                $optgroup->append_line($line);
            }
        }
        {
            my $optgroup = $page->new_optgroup('Show shortcuts for the following settings');
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    widget      => $shortcuts_widget,
                    full_width  => 1,
                );
                $optgroup->append_line($line);
            }
        }
    }
}

sub reload_config {
    my ($self) = @_;
    
    $self->_reload_compatible_printers_widget;
    
    {
        my %shortcuts = map { $_ => 1 } @{ $self->config->get('shortcuts') };
        for my $i (0..$#{$self->{shortcuts_opt_keys}}) {
            $self->{shortcuts_list}->Check($i, $shortcuts{ $self->{shortcuts_opt_keys}[$i] });
        }
    }
    
    $self->SUPER::reload_config;
}

sub _update {
    my ($self, $key) = @_;
    my $opt_key = $key;
    $opt_key = "all_keys" if (length($key // '') == 0); 
    my $config = $self->{config};

    if (any { /$opt_key/ } qw(all_keys spiral_vase perimeters top_solid_layers fill_density support_material min_shell_thickness min_top_bottom_shell_thickness)) {
        if ($config->spiral_vase && !($config->perimeters == 1 && $config->min_shell_thickness == 0 && $config->min_top_bottom_shell_thickness == 0 && $config->top_solid_layers == 0 && $config->fill_density == 0 && $config->support_material == 0)) {
            my $dialog = Wx::MessageDialog->new($self,
                "The Spiral Vase mode requires:\n"
                . "- one perimeter\n"
                . "- shell thickness to be 0\n"
                . "- no top solid layers\n"
                . "- 0% fill density\n"
                . "- no support material\n"
                . "\nShall I adjust those settings in order to enable Spiral Vase?",
                'Spiral Vase', wxICON_WARNING | wxYES | wxNO);
            if ($dialog->ShowModal() == wxID_YES) {
                my $new_conf = Slic3r::Config->new;
                $new_conf->set("perimeters", 1);
                $new_conf->set("min_shell_thickness", 0);
                $new_conf->set("min_top_bottom_shell_thickness", 0);
                $new_conf->set("top_solid_layers", 0);
                $new_conf->set("fill_density", 0);
                $new_conf->set("support_material", 0);
                $self->_load_config($new_conf);
            } else {
                my $new_conf = Slic3r::Config->new;
                $new_conf->set("spiral_vase", 0);
                $self->_load_config($new_conf);
            }
        }
    }

    if (any { /$opt_key/ } qw(all_keys support_material)) {
        if ($config->support_material) {
            # Ask only once.
            if (! $self->{support_material_overhangs_queried}) {
                $self->{support_material_overhangs_queried} = 1;
                if ($config->overhangs != 1) {
                    my $dialog = Wx::MessageDialog->new($self,
                            "Supports work better, if the following feature is enabled:\n"
                            . "- Detect bridging perimeters\n"
                            . "\nShall I adjust those settings for supports?",
                            'Support Generator', wxICON_WARNING | wxYES | wxNO | wxCANCEL);
                    my $answer = $dialog->ShowModal();
                    my $new_conf = Slic3r::Config->new;
                    if ($answer == wxID_YES) {
                        # Enable "detect bridging perimeters".
                        $new_conf->set("overhangs", 1);
                    } elsif ($answer == wxID_NO) {
                        # Do nothing, leave supports on and "detect bridging perimeters" off.
                    } elsif ($answer == wxID_CANCEL) {
                        # Disable supports.
                        $new_conf->set("support_material", 0);
                        $self->{support_material_overhangs_queried} = 0;
                    }
                    $self->_load_config($new_conf);
                }
            }
        } else {
            $self->{support_material_overhangs_queried} = 0;
        }
    }
    
    if (any { /$opt_key/ } qw(all_keys fill_density fill_pattern top_infill_pattern)) {
        if ($config->fill_density == 100
                && !first { $_ eq $config->fill_pattern } @{$Slic3r::Config::Options->{top_infill_pattern}{values}}) {
            my $dialog = Wx::MessageDialog->new($self,
                    "The " . $config->fill_pattern . " infill pattern is not supposed to work at 100% density.\n"
                    . "\nShall I switch to rectilinear fill pattern?",
                    'Infill', wxICON_WARNING | wxYES | wxNO);

            my $new_conf = Slic3r::Config->new;
            if ($dialog->ShowModal() == wxID_YES) {
                $new_conf->set("fill_pattern", 'rectilinear');
            } else {
                $new_conf->set("fill_density", 40);
            }
            $self->_load_config($new_conf);
        }
    }

    my $have_perimeters = ($config->perimeters > 0) || ($config->min_shell_thickness > 0);
    if (any { /$opt_key/ } qw(all_keys perimeters)) {
        $self->get_field($_)->toggle($have_perimeters)
            for qw(extra_perimeters thin_walls overhangs seam_position external_perimeters_first
                    external_perimeter_extrusion_width
                    perimeter_speed small_perimeter_speed external_perimeter_speed);
    }

    my $have_adaptive_slicing = $config->adaptive_slicing;
    if (any { /$opt_key/ } qw(all_keys adaptive_slicing)) {
        $self->get_field($_)->toggle($have_adaptive_slicing)
            for qw(adaptive_slicing_quality match_horizontal_surfaces);
        $self->get_field($_)->toggle(!$have_adaptive_slicing)
            for qw(layer_height);
    }

    my $have_infill = $config->fill_density > 0;
    if (any { /$opt_key/ } qw(all_keys fill_density)) {
        # infill_extruder uses the same logic as in Print::extruders()
        $self->get_field($_)->toggle($have_infill)
            for qw(fill_pattern infill_every_layers infill_only_where_needed solid_infill_every_layers
                    solid_infill_below_area infill_extruder);
    }

    my $have_solid_infill = ($config->top_solid_layers > 0) || ($config->bottom_solid_layers > 0) || ($config->min_top_bottom_shell_thickness > 0);
    if (any { /$opt_key/ } qw(all_keys top_solid_layers bottom_solid_layers)) {
        # solid_infill_extruder uses the same logic as in Print::extruders()
        $self->get_field($_)->toggle($have_solid_infill)
            for qw(top_infill_pattern bottom_infill_pattern infill_first solid_infill_extruder
                    solid_infill_extrusion_width solid_infill_speed);
    }

    if (any { /$opt_key/ } qw(all_keys top_solid_layers bottom_solid_layers fill_density)) {
        $self->get_field($_)->toggle($have_infill || $have_solid_infill)
            for qw(fill_angle infill_extrusion_width infill_speed);
    }

    if (any { /$opt_key/ } qw(all_keys fill_density perimeters)) {
        $self->get_field('fill_gaps')->toggle($have_perimeters && $have_infill);
        $self->get_field('gap_fill_speed')->toggle($have_perimeters && $have_infill && $config->fill_gaps);
    }

    my $have_top_solid_infill = ($config->top_solid_layers > 0) || ($config->min_top_bottom_shell_thickness > 0);

    $self->get_field($_)->toggle($have_top_solid_infill)
        for qw(top_infill_extrusion_width top_solid_infill_speed);

    my $have_autospeed = any { $config->get("${_}_speed") eq '0' }
    qw(perimeter external_perimeter small_perimeter
            infill solid_infill top_solid_infill gap_fill support_material
            support_material_interface);
    $self->get_field('max_print_speed')->toggle($have_autospeed);

    my $have_default_acceleration = $config->default_acceleration > 0;
    $self->get_field($_)->toggle($have_default_acceleration)
        for qw(perimeter_acceleration infill_acceleration bridge_acceleration first_layer_acceleration);

    my $have_skirt = $config->skirts > 0 || $config->min_skirt_length > 0;
    $self->get_field($_)->toggle($have_skirt)
        for qw(skirt_distance skirt_height);
    
    my $have_brim = $config->brim_width > 0 || $config->interior_brim_width
        || $config->brim_connections_width;
    # perimeter_extruder uses the same logic as in Print::extruders()
    $self->get_field('perimeter_extruder')->toggle($have_perimeters || $have_brim);
    
    $self->get_field('brim_ears')->toggle($have_brim);
    $self->get_field('brim_ears_max_angle')->toggle($have_brim && $config->brim_ears);
    
    my $have_support_material = $config->support_material || $config->raft_layers > 0;
    my $have_support_interface = $config->support_material_interface_layers > 0;
    my $have_support_pillars = $have_support_material && $config->support_material_pattern eq 'pillars';
    $self->get_field($_)->toggle($have_support_material)
        for qw(support_material_threshold support_material_pattern 
            support_material_spacing support_material_angle
            support_material_interface_layers dont_support_bridges
            support_material_extrusion_width support_material_interface_extrusion_width
            support_material_contact_distance);
    
    # Disable features that need support to be enabled.            
    $self->get_field($_)->toggle($config->support_material)
        for qw(support_material_max_layers);
    
    # Only enable pillar configuration when using pillars
    $self->get_field($_)->toggle($have_support_pillars)
        for qw(support_material_pillar_size support_material_pillar_spacing);

    $self->get_field($_)->toggle($have_support_material && $have_support_interface)
        for qw(support_material_interface_spacing support_material_interface_extruder
            support_material_interface_speed);
    
    $self->get_field('perimeter_extrusion_width')->toggle($have_perimeters || $have_skirt || $have_brim);
    $self->get_field('support_material_extruder')->toggle($have_support_material || $have_skirt);
    $self->get_field('support_material_speed')->toggle($have_support_material || $have_brim || $have_skirt);
    
    my $have_sequential_printing = $config->complete_objects;
    $self->get_field($_)->toggle($have_sequential_printing)
        for qw(extruder_clearance_radius extruder_clearance_height);
    
    my $have_ooze_prevention = $config->ooze_prevention;
    $self->get_field($_)->toggle($have_ooze_prevention)
        for qw(standby_temperature_delta);
}

package Slic3r::GUI::PresetEditor::Filament;
use base 'Slic3r::GUI::PresetEditor';

use Wx qw(wxTheApp);

sub name { 'filament' }
sub title { 'Filament Settings' }

sub options {
    return qw(
        filament_colour filament_diameter filament_notes filament_max_volumetric_speed extrusion_multiplier filament_density filament_cost
        temperature first_layer_temperature bed_temperature first_layer_bed_temperature
        fan_always_on cooling compatible_printers
        min_fan_speed max_fan_speed bridge_fan_speed disable_fan_first_layers
        fan_below_layer_time slowdown_below_layer_time min_print_speed
        start_filament_gcode end_filament_gcode
        filament_settings_id
    );
}

sub overriding_options {
    return (
        Slic3r::GUI::PresetEditor::Printer->overridable_options,
    );
}

sub build {
    my $self = shift;
    
    {
        my $page = $self->add_options_page('Filament', 'spool.png');
        {
            my $optgroup = $page->new_optgroup('Filament');
            $optgroup->append_single_option_line('filament_colour', 0);
            $optgroup->append_single_option_line('filament_diameter', 0);
            $optgroup->append_single_option_line('extrusion_multiplier', 0);
        }
        {
            my $optgroup = $page->new_optgroup('Temperature (°C)');
        
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label => 'Extruder',
                );
                $line->append_option($optgroup->get_option('first_layer_temperature', 0));
                $line->append_option($optgroup->get_option('temperature', 0));
                $optgroup->append_line($line);
            }
        
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label => 'Bed',
                );
                $line->append_option($optgroup->get_option('first_layer_bed_temperature'));
                $line->append_option($optgroup->get_option('bed_temperature'));
                $optgroup->append_line($line);
            }
        }
        {
            my $optgroup = $page->new_optgroup('Optional information');
            $optgroup->append_single_option_line('filament_density', 0);
            $optgroup->append_single_option_line('filament_cost', 0);
        }
    }
    
    {
        my $page = $self->add_options_page('Cooling', 'hourglass.png');
        {
            my $optgroup = $page->new_optgroup('Enable');
            $optgroup->append_single_option_line('fan_always_on');
            $optgroup->append_single_option_line('cooling');
            
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label       => '',
                full_width  => 1,
                widget      => sub {
                    my ($parent) = @_;
                    return $self->{description_line} = Slic3r::GUI::OptionsGroup::StaticText->new($parent);
                },
            );
            $optgroup->append_line($line);
        }
        {
            my $optgroup = $page->new_optgroup('Fan settings');
            
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label => 'Fan speed',
                );
                $line->append_option($optgroup->get_option('min_fan_speed'));
                $line->append_option($optgroup->get_option('max_fan_speed'));
                $optgroup->append_line($line);
            }
            
            $optgroup->append_single_option_line('bridge_fan_speed');
            $optgroup->append_single_option_line('disable_fan_first_layers');
        }
        {
            my $optgroup = $page->new_optgroup('Cooling thresholds',
                label_width => 250,
            );
            $optgroup->append_single_option_line('fan_below_layer_time');
            $optgroup->append_single_option_line('slowdown_below_layer_time');
            $optgroup->append_single_option_line('min_print_speed');
        }
    }
    {
        my $page = $self->add_options_page('Custom G-code', 'script.png');
        {
            my $optgroup = $page->new_optgroup('Start G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('start_filament_gcode', 0);
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('End G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('end_filament_gcode', 0);
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
    }

    {
        my $page = $self->add_options_page('Notes', 'note.png');
        {
            my $optgroup = $page->new_optgroup('Notes',
                label_width => 0,
            );
            my $option = $optgroup->get_option('filament_notes', 0);
            $option->full_width(1);
            $option->height(250);
            $optgroup->append_single_option_line($option);
        }
    }
    
    {
        my $page = $self->add_options_page('Overrides', 'wrench.png');
        {
            my $optgroup = $page->new_optgroup('Profile preferences');
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label       => 'Compatible printers',
                widget      => $self->_compatible_printers_widget,
            );
            $optgroup->append_line($line);
        }
        {
            my $optgroup = $page->new_optgroup('Overrides');
            $optgroup->append_single_option_line('filament_max_volumetric_speed', 0);
            
            # Populate the overrides config.
            my @overridable = $self->overriding_options;
            $self->{overrides_config} = Slic3r::Config->new;
            
            # Populate the defaults with the current preset.
            $self->{overrides_default_config} = Slic3r::Config->new;
            $self->{overrides_default_config}->apply_only
                (wxTheApp->{mainframe}->{plater}->config, \@overridable);
            
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label       => '',
                full_width  => 1,
                widget      => sub {
                    my ($parent) = @_;
                    
                    $self->{overrides_panel} = my $panel = Slic3r::GUI::Plater::OverrideSettingsPanel->new($parent,
                        size => [-1, 300],
                        on_change => sub {
                            my ($opt_key) = @_;
                            $self->config->erase($_) for @overridable;
                            $self->current_preset->_dirty_config->erase($_) for @overridable;
                            $self->config->apply($self->{overrides_config});
                            $self->_on_value_change($opt_key);
                        });
                    $panel->set_editable(1);
                    $panel->set_default_config($self->{overrides_default_config});
                    $panel->set_config($self->{overrides_config});
                    $panel->set_opt_keys([@overridable]);
                    
                    return $panel;
                },
            );
            $optgroup->append_line($line);
        }
    }
}

sub reload_config {
    my ($self) = @_;
    
    $self->_reload_compatible_printers_widget;
    
    {
        $self->{overrides_config}->clear;
        foreach my $opt_key (@{$self->{overrides_default_config}->get_keys}) {
            if ($self->config->has($opt_key)) {
                $self->{overrides_config}->set($opt_key, $self->config->get($opt_key));
            }
        }
        $self->{overrides_panel}->update_optgroup;
    }
    
    $self->SUPER::reload_config;
}

sub _update {
    my ($self) = @_;
    
    $self->_update_description;
    
    my $cooling = $self->{config}->cooling;
    $self->get_field($_)->toggle($cooling)
        for qw(max_fan_speed fan_below_layer_time slowdown_below_layer_time min_print_speed);
    $self->get_field($_)->toggle($cooling || $self->{config}->fan_always_on)
        for qw(min_fan_speed disable_fan_first_layers);
}

sub _update_description {
    my $self = shift;
    
    my $config = $self->config;
    
    my $msg = "";
    my $fan_other_layers = $config->fan_always_on
        ? sprintf "will always run at %d%%%s.", $config->min_fan_speed,
                ($config->disable_fan_first_layers > 1
                    ? " except for the first " . $config->disable_fan_first_layers . " layers"
                    : $config->disable_fan_first_layers == 1
                        ? " except for the first layer"
                        : "")
        : "will be turned off.";
    
    if ($config->cooling) {
        $msg = sprintf "If estimated layer time is below ~%ds, fan will run at %d%% and print speed will be reduced so that no less than %ds are spent on that layer (however, speed will never be reduced below %dmm/s).",
            $config->slowdown_below_layer_time, $config->max_fan_speed, $config->slowdown_below_layer_time, $config->min_print_speed;
        if ($config->fan_below_layer_time > $config->slowdown_below_layer_time) {
            $msg .= sprintf "\nIf estimated layer time is greater, but still below ~%ds, fan will run at a proportionally decreasing speed between %d%% and %d%%.",
                $config->fan_below_layer_time, $config->max_fan_speed, $config->min_fan_speed;
        }
        $msg .= "\nDuring the other layers, fan $fan_other_layers"
    } else {
        $msg = "Fan $fan_other_layers";
    }
    $self->{description_line}->SetText($msg);
}

package Slic3r::GUI::PresetEditor::Printer;
use base 'Slic3r::GUI::PresetEditor';
use Wx qw(wxTheApp :sizer :button :bitmap :misc :id :icon :dialog);
use Wx::Event qw(EVT_BUTTON);

sub name { 'printer' }
sub title { 'Printer Settings' }

sub options {
    return qw(
        bed_shape z_offset z_steps_per_mm has_heatbed
        fan_percentage
        gcode_flavor use_relative_e_distances
        serial_port serial_speed
        host_type print_host octoprint_apikey
        use_firmware_retraction pressure_advance vibration_limit
        use_volumetric_e
        start_gcode end_gcode before_layer_gcode layer_gcode toolchange_gcode between_objects_gcode
        nozzle_diameter extruder_offset min_layer_height max_layer_height
        retract_length retract_lift retract_speed retract_restart_extra retract_before_travel retract_layer_change wipe
        retract_length_toolchange retract_restart_extra_toolchange retract_lift_above retract_lift_below
        printer_settings_id
        printer_notes
        use_set_and_wait_bed use_set_and_wait_extruder
    );
}

sub overridable_options {
    return qw(
        pressure_advance
        retract_length retract_lift retract_speed retract_restart_extra
        retract_before_travel retract_layer_change wipe
    );
}

sub build {
    my $self = shift;
    
    $self->{extruders_count} = 1;
    
    {
        my $page = $self->add_options_page('General', 'printer_empty.png');
        {
            my $optgroup = $page->new_optgroup('Size and coordinates');
            
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label => 'Bed shape',
            );
            $line->append_button("Set…", "cog.png", sub {
                my $dlg = Slic3r::GUI::BedShapeDialog->new($self, $self->config->bed_shape);
                if ($dlg->ShowModal == wxID_OK) {
                    my $value = $dlg->GetValue;
                    $self->config->set('bed_shape', $value);
                    $self->_on_value_change('bed_shape');
                }
            });
            $optgroup->append_line($line);
            
            $optgroup->append_single_option_line('z_offset');
        }
        {
            my $optgroup = $page->new_optgroup('Capabilities');
            {
                my $option = Slic3r::GUI::OptionsGroup::Option->new(
                    opt_id      => 'extruders_count',
                    type        => 'i',
                    default     => 1,
                    label       => 'Extruders',
                    tooltip     => 'Number of extruders of the printer.',
                    min         => 1,
                );
                $optgroup->append_single_option_line($option);
            }
            $optgroup->append_single_option_line('has_heatbed');
            $optgroup->on_change(sub {
                my ($opt_id) = @_;
                if ($opt_id eq 'extruders_count') {
                    wxTheApp->CallAfter(sub {
                        $self->_extruders_count_changed($optgroup->get_value('extruders_count'));
                    });
                } else {
                    wxTheApp->CallAfter(sub {
                        $self->_on_value_change($opt_id);
                    });
                }
            });
        }
        {
            my $optgroup = $page->new_optgroup('USB/Serial connection');
            my $line = Slic3r::GUI::OptionsGroup::Line->new(
                label => 'Serial port',
            );
            my $serial_port = $optgroup->get_option('serial_port');
            $serial_port->side_widget(sub {
                my ($parent) = @_;
                
                my $btn = Wx::BitmapButton->new($parent, -1, Wx::Bitmap->new($Slic3r::var->("arrow_rotate_clockwise.png"), wxBITMAP_TYPE_PNG),
                    wxDefaultPosition, wxDefaultSize, &Wx::wxBORDER_NONE);
                $btn->SetToolTipString("Rescan serial ports")
                    if $btn->can('SetToolTipString');
                EVT_BUTTON($self, $btn, \&_update_serial_ports);
                
                return $btn;
            });
            $line->append_option($serial_port);
            $line->append_option($optgroup->get_option('serial_speed'));
            $line->append_button("Test", "wrench.png", sub {
                my $sender = Slic3r::GCode::Sender->new;
                my $res = $sender->connect(
                    $self->config->serial_port,
                    $self->config->serial_speed,
                );
                if ($res && $sender->wait_connected) {
                    Slic3r::GUI::show_info($self, "Connection to printer works correctly.", "Success!");
                } else {
                    Slic3r::GUI::show_error($self, "Connection failed.");
                }
            }, \$self->{serial_test_btn});
            $optgroup->append_line($line);
        }
        {
            my $optgroup = $page->new_optgroup('Print server upload');

            $optgroup->append_single_option_line('host_type'); 
            
            my $host_line = $optgroup->create_single_option_line('print_host');
            $host_line->append_button("Browse…", "zoom.png", sub {
                # look for devices
                my $entries;
                {
                    my $res = Net::Bonjour->new('octoprint');
                    $res->discover;
                    $entries = [ $res->entries ];
                }
                if (@{$entries}) {
                    my $dlg = Slic3r::GUI::BonjourBrowser->new($self, $entries);
                    if ($dlg->ShowModal == wxID_OK) {
                        my $value = $dlg->GetValue . ":" . $dlg->GetPort;
                        $self->config->set('print_host', $value);
                        $self->_on_value_change('print_host');
                    }
                } else {
                    Wx::MessageDialog->new($self, 'No Bonjour device found', 'Device Browser', wxOK | wxICON_INFORMATION)->ShowModal;
                }
            }, \$self->{print_host_browse_btn}, !eval "use Net::Bonjour; 1");
            $host_line->append_button("Test", "wrench.png", sub {
                my $ua = LWP::UserAgent->new;
                $ua->timeout(10);

                my $res = $ua->get(
                    "http://" . $self->config->print_host . "/api/version",
                    'X-Api-Key' => $self->config->octoprint_apikey,
                );
                if ($res->is_success) {
                    Slic3r::GUI::show_info($self, "Connection to OctoPrint works correctly.", "Success!");
                } else {
                    Slic3r::GUI::show_error($self,
                        "I wasn't able to connect to OctoPrint (" . $res->status_line . "). "
                        . "Check hostname and OctoPrint version (at least 1.1.0 is required).");
                }
            }, \$self->{print_host_test_btn});
            $optgroup->append_line($host_line);
            $optgroup->append_single_option_line('octoprint_apikey');
        }
        {
            my $optgroup = $page->new_optgroup('Firmware');
            $optgroup->append_single_option_line('gcode_flavor');
        }
        {
            my $optgroup = $page->new_optgroup('Advanced');
            $optgroup->append_single_option_line('use_relative_e_distances');
            $optgroup->append_single_option_line('use_firmware_retraction');
            $optgroup->append_single_option_line('use_volumetric_e');
            $optgroup->append_single_option_line('pressure_advance');
            $optgroup->append_single_option_line('vibration_limit');
            $optgroup->append_single_option_line('z_steps_per_mm');
            $optgroup->append_single_option_line('use_set_and_wait_extruder');
            $optgroup->append_single_option_line('use_set_and_wait_bed');
            $optgroup->append_single_option_line('fan_percentage');
        }
    }
    {
        my $page = $self->add_options_page('Custom G-code', 'script.png');
        {
            my $optgroup = $page->new_optgroup('Start G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('start_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('End G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('end_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('Before layer change G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('before_layer_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('After layer change G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('layer_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('Tool change G-code',
                label_width => 0,
            );
            my $option = $optgroup->get_option('toolchange_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
        {
            my $optgroup = $page->new_optgroup('Between objects G-code (for sequential printing)',
                label_width => 0,
            );
            my $option = $optgroup->get_option('between_objects_gcode');
            $option->full_width(1);
            $option->height(150);
            $optgroup->append_single_option_line($option);
        }
    }

    $self->{extruder_pages} = [];
    $self->_build_extruder_pages;
    {
        my $page = $self->add_options_page('Notes', 'note.png');
        {
            my $optgroup = $page->new_optgroup('Notes',
                label_width => 0,
            );
            my $option = $optgroup->get_option('printer_notes');
            $option->full_width(1);
            $option->height(250);
            $optgroup->append_single_option_line($option);
        }
    }
    $self->_update_serial_ports;
}

sub _update_serial_ports {
    my ($self) = @_;
    
    $self->get_field('serial_port')->set_values([ wxTheApp->scan_serial_ports ]);
}

sub _extruders_count_changed {
    my ($self, $extruders_count) = @_;
    
    $self->{extruders_count} = $extruders_count;
    $self->_build_extruder_pages;
    $self->_on_value_change('extruders_count');
    $self->_update;
}

sub _extruder_options { qw(nozzle_diameter min_layer_height max_layer_height extruder_offset retract_length retract_lift retract_lift_above retract_lift_below retract_speed retract_restart_extra retract_before_travel wipe
    retract_layer_change retract_length_toolchange retract_restart_extra_toolchange) }

sub _build_extruder_pages {
    my $self = shift;
    
    my $default_config = Slic3r::Config::Full->new;
    
    foreach my $extruder_idx (@{$self->{extruder_pages}} .. $self->{extruders_count}-1) {
        # extend options
        foreach my $opt_key ($self->_extruder_options) {
            my $values = $self->config->get($opt_key);
            if (!defined $values) {
                $values = [ $default_config->get_at($opt_key, 0) ];
            } else {
                # use last extruder's settings for the new one
                my $last_value = $values->[-1];
                $values->[$extruder_idx] //= $last_value;
            }
            $self->config->set($opt_key, $values)
                or die "Unable to extend $opt_key";
        }
        
        # build page
        my $page = $self->{extruder_pages}[$extruder_idx] = $self->add_options_page("Extruder " . ($extruder_idx + 1), 'funnel.png');
        {
            my $optgroup = $page->new_optgroup('Size');
            $optgroup->append_single_option_line('nozzle_diameter', $extruder_idx);
        }
        {
            my $optgroup = $page->new_optgroup('Layer Height Limits');
            $optgroup->append_single_option_line($_, $extruder_idx)
               for qw(min_layer_height max_layer_height);
        }
        {
            my $optgroup = $page->new_optgroup('Position (for multi-extruder printers)');
            $optgroup->append_single_option_line('extruder_offset', $extruder_idx);
        }
        {
            my $optgroup = $page->new_optgroup('Retraction');
            $optgroup->append_single_option_line($_, $extruder_idx)
                for qw(retract_length retract_lift);
            
            {
                my $line = Slic3r::GUI::OptionsGroup::Line->new(
                    label => 'Only lift Z',
                );
                $line->append_option($optgroup->get_option('retract_lift_above', $extruder_idx));
                $line->append_option($optgroup->get_option('retract_lift_below', $extruder_idx));
                $optgroup->append_line($line);
            }
            
            $optgroup->append_single_option_line($_, $extruder_idx)
                for qw(retract_speed retract_restart_extra retract_before_travel retract_layer_change wipe);
        }
        {
            my $optgroup = $page->new_optgroup('Retraction when tool is disabled (advanced settings for multi-extruder setups)');
            $optgroup->append_single_option_line($_, $extruder_idx)
                for qw(retract_length_toolchange retract_restart_extra_toolchange);
        }
    }
    
    # remove extra pages
    if ($self->{extruders_count} <= $#{$self->{extruder_pages}}) {
        $_->Destroy for @{$self->{extruder_pages}}[$self->{extruders_count}..$#{$self->{extruder_pages}}];
        splice @{$self->{extruder_pages}}, $self->{extruders_count};
    }
    
    # remove extra config values
    foreach my $opt_key ($self->_extruder_options) {
        my $values = $self->config->get($opt_key);
        splice @$values, $self->{extruders_count} if $self->{extruders_count} <= $#$values;
        $self->config->set($opt_key, $values)
            or die "Unable to truncate $opt_key";
    }
    
    # rebuild page list
    @{$self->{pages}} = (
        (grep $_->{title} !~ /^Extruder \d+/, @{$self->{pages}}),
        @{$self->{extruder_pages}}[ 0 .. $self->{extruders_count}-1 ],
    );
    $self->update_tree;
}

sub _update {
    my ($self) = @_;
    
    my $config = $self->{config};
    
    my $serial_speed = $self->get_field('serial_speed');
    if ($serial_speed) {
        $self->get_field('serial_speed')->toggle($config->get('serial_port'));
        if ($config->get('serial_speed') && $config->get('serial_port')) {
            $self->{serial_test_btn}->Enable;
        } else {
            $self->{serial_test_btn}->Disable;
        }
    }
    if (($config->get('host_type') eq 'octoprint')) {
        $self->{print_host_browse_btn}->Enable;
    }else{
        $self->{print_host_browse_btn}->Disable;
    }
    if (($config->get('host_type') eq 'octoprint') && eval "use LWP::UserAgent; 1") {
        $self->{print_host_test_btn}->Enable;
    } else {    
        $self->{print_host_test_btn}->Disable;
    }
    $self->get_field('octoprint_apikey')->toggle($config->get('print_host'));
    
    my $have_multiple_extruders = $self->{extruders_count} > 1;
    $self->get_field('toolchange_gcode')->toggle($have_multiple_extruders);
    
    for my $i (0 .. ($self->{extruders_count}-1)) {
        my $have_retract_length = $config->get_at('retract_length', $i) > 0;
        
        # when using firmware retraction, firmware decides retraction length
        $self->get_field('retract_length', $i)->toggle(!$config->use_firmware_retraction);
        if ($config->use_firmware_retraction && 
            ($config->gcode_flavor eq 'reprap' || $config->gcode_flavor eq 'repetier') && 
            ($config->get_at('retract_length_toolchange', $i) > 0 || $config->get_at('retract_restart_extra_toolchange', $i) > 0)) {
            my $dialog = Wx::MessageDialog->new($self,
                "Retract length for toolchange on extruder " . $i . " is not available when using the Firmware Retraction mode.\n"
                . "\nShall I disable it in order to enable Firmware Retraction?",
                'Firmware Retraction', wxICON_WARNING | wxYES | wxNO);
            
            my $new_conf = Slic3r::Config->new;
            if ($dialog->ShowModal() == wxID_YES) {
                my $retract_length_toolchange = $config->retract_length_toolchange;
                $retract_length_toolchange->[$i] = 0;
                $new_conf->set("retract_length_toolchange", $retract_length_toolchange);
                $new_conf->set("retract_restart_extra_toolchange", $retract_length_toolchange);
            } else {
                $new_conf->set("use_firmware_retraction", 0);
            }
            $self->_load_config($new_conf);
        }
        
        # user can customize travel length if we have retraction length or we're using
        # firmware retraction
        $self->get_field('retract_before_travel', $i)->toggle($have_retract_length || $config->use_firmware_retraction);
        
        # user can customize other retraction options if retraction is enabled
        # Firmware retract has Z lift built in.
        my $retraction = ($have_retract_length || $config->use_firmware_retraction);
        $self->get_field($_, $i)->toggle($retraction && !$config->use_firmware_retraction)
            for qw(retract_lift);

        $self->get_field($_, $i)->toggle($retraction)
            for qw(retract_layer_change);
        
        # retract lift above/below only applies if using retract lift
        $self->get_field($_, $i)->toggle($retraction && $config->get_at('retract_lift', $i) > 0)
            for qw(retract_lift_above retract_lift_below);
        
        # some options only apply when not using firmware retraction
        $self->get_field($_, $i)->toggle($retraction && !$config->use_firmware_retraction)
            for qw(retract_restart_extra wipe);
        
        # retraction speed is also used by auto-speed pressure regulator, even when
        # user enabled firmware retraction
        $self->get_field('retract_speed', $i)->toggle($retraction);
        
        if ($config->use_firmware_retraction && $config->get_at('wipe', $i)) {
            my $dialog = Wx::MessageDialog->new($self,
                "The Wipe option is not available when using the Firmware Retraction mode.\n"
                . "\nShall I disable it in order to enable Firmware Retraction?",
                'Firmware Retraction', wxICON_WARNING | wxYES | wxNO);
            
            my $new_conf = Slic3r::Config->new;
            if ($dialog->ShowModal() == wxID_YES) {
                my $wipe = $config->wipe;
                $wipe->[$i] = 0;
                $new_conf->set("wipe", $wipe);
            } else {
                $new_conf->set("use_firmware_retraction", 0);
            }
            $self->_load_config($new_conf);
        }

        if ($config->use_firmware_retraction && $config->get_at('retract_lift', $i) > 0) {
            my $dialog = Wx::MessageDialog->new($self,
                "The Z Lift option is not available when using the Firmware Retraction mode.\n"
                . "\nShall I disable it in order to enable Firmware Retraction?",
                'Firmware Retraction', wxICON_WARNING | wxYES | wxNO);
            
            my $new_conf = Slic3r::Config->new;
            if ($dialog->ShowModal() == wxID_YES) {
                my $wipe = $config->retract_lift;
                $wipe->[$i] = 0;
                $new_conf->set("retract_lift", $wipe);
            } else {
                $new_conf->set("use_firmware_retraction", 0);
            }
            $self->_load_config($new_conf);
        }

        $self->get_field('retract_length_toolchange', $i)->toggle
            ($have_multiple_extruders && 
            !($config->use_firmware_retraction && ($config->gcode_flavor eq 'reprap' || $config->gcode_flavor eq 'repetier')));
        
        my $toolchange_retraction = $config->get_at('retract_length_toolchange', $i) > 0;
        $self->get_field('retract_restart_extra_toolchange', $i)->toggle
            ($have_multiple_extruders && $toolchange_retraction && 
            !($config->use_firmware_retraction && ($config->gcode_flavor eq 'reprap' || $config->gcode_flavor eq 'repetier')));
    }
}

# this gets executed after preset is loaded and before GUI fields are updated
sub on_preset_loaded {
    my $self = shift;
    
    # update the extruders count field
    {
        # update the GUI field according to the number of nozzle diameters supplied
        my $extruders_count = scalar @{ $self->config->nozzle_diameter };
        $self->set_value('extruders_count', $extruders_count);
        $self->_extruders_count_changed($extruders_count);
    }
}

sub load_config_file {
    my $self = shift;
    $self->SUPER::load_config_file(@_);
    
    Slic3r::GUI::warning_catcher($self)->(
        "Your configuration was imported. However, Slic3r is currently only able to import settings "
        . "for the first defined filament. We recommend you don't use exported configuration files "
        . "for multi-extruder setups and rely on the built-in preset management system instead.")
        if @{ $self->config->nozzle_diameter } > 1;
}

package Slic3r::GUI::PresetEditor::Page;
use Wx qw(wxTheApp :misc :panel :sizer);
use base 'Wx::ScrolledWindow';

sub new {
    my $class = shift;
    my ($parent, $title, $iconID) = @_;
    my $self = $class->SUPER::new($parent, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    $self->{optgroups}  = [];
    $self->{title}      = $title;
    $self->{iconID}     = $iconID;
    
    $self->{vsizer} = Wx::BoxSizer->new(wxVERTICAL);
    $self->SetSizer($self->{vsizer});
    
    # http://docs.wxwidgets.org/3.0/classwx_scrolled.html#details
    $self->SetScrollRate($Slic3r::GUI::scroll_step, $Slic3r::GUI::scroll_step);
    
    return $self;
}

sub new_optgroup {
    my ($self, $title, %params) = @_;
    
    my $optgroup = Slic3r::GUI::ConfigOptionsGroup->new(
        parent          => $self,
        title           => $title,
        config          => $self->GetParent->{config},
        label_width     => $params{label_width} // 200,
        on_change       => sub {
            my ($opt_key, $value) = @_;
            wxTheApp->CallAfter(sub {
                $self->GetParent->_on_value_change($opt_key);
            });
        },
    );
    
    push @{$self->{optgroups}}, $optgroup;
    $self->{vsizer}->Add($optgroup->sizer, 0, wxEXPAND | wxALL, 10);
    
    return $optgroup;
}

sub reload_config {
    my ($self) = @_;
    $_->reload_config for @{$self->{optgroups}};
}

sub get_field {
    my ($self, $opt_key, $opt_index) = @_;
    
    foreach my $optgroup (@{ $self->{optgroups} }) {
        my $field = $optgroup->get_fieldc($opt_key, $opt_index);
        return $field if defined $field;
    }
    return undef;
}

sub set_value {
    my $self = shift;
    my ($opt_key, $value) = @_;
    
    my $changed = 0;
    foreach my $optgroup (@{$self->{optgroups}}) {
        $changed = 1 if $optgroup->set_value($opt_key, $value);
    }
    return $changed;
}

package Slic3r::GUI::SavePresetWindow;
use Wx qw(:combobox :dialog :id :misc :sizer);
use Wx::Event qw(EVT_BUTTON EVT_TEXT_ENTER);
use base 'Wx::Dialog';

sub new {
    my $class = shift;
    my ($parent, %params) = @_;
    my $self = $class->SUPER::new($parent, -1, "Save preset", wxDefaultPosition, wxDefaultSize);
    
    my @values = @{$params{values}};
    
    my $text = Wx::StaticText->new($self, -1, "Save profile as:", wxDefaultPosition, wxDefaultSize);
    $self->{combo} = Wx::ComboBox->new($self, -1, $params{default}, wxDefaultPosition, wxDefaultSize, \@values,
                                       wxTE_PROCESS_ENTER);
    my $buttons = $self->CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    
    my $sizer = Wx::BoxSizer->new(wxVERTICAL);
    $sizer->Add($text, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 10);
    $sizer->Add($self->{combo}, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    $sizer->Add($buttons, 0, wxEXPAND | wxBOTTOM | wxLEFT | wxRIGHT, 10);
    
    EVT_BUTTON($self, wxID_OK, \&accept);
    EVT_TEXT_ENTER($self, $self->{combo}, \&accept);
    
    $self->SetSizer($sizer);
    $sizer->SetSizeHints($self);
    
    return $self;
}

sub accept {
    my ($self, $event) = @_;

    if (($self->{chosen_name} = $self->{combo}->GetValue)) {
        if ($self->{chosen_name} !~ /^[^<>:\/\\|?*\"]+$/i) {
            Slic3r::GUI::show_error($self, "The supplied name is not valid; the following characters are not allowed: <>:/\|?*\"");
        } elsif ($self->{chosen_name} eq '- default -') {
            Slic3r::GUI::show_error($self, "The supplied name is not available.");
        } else {
            $self->EndModal(wxID_OK);
        }
    }
}

sub get_name {
    my $self = shift;
    return $self->{chosen_name};
}

1;
