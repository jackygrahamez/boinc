<?php
// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2014 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

// functions for display and editing project preferences
// Preferences are represented in two ways:
// - As a PHP structure (usually called $prefs)
//   The field "project_specific" is plain XML
// - As XML (usually called $prefs_xml)
//
// This XML has the general structure
//
// <project_preferences>
//    <resource_share>4</resource_share>
//    [ <allow_beta_work>0|1</allow_beta_work> ]
//    [ <no_cpu>1</no_cpu> ]
//    [ <no_cuda>1</no_cuda> ]
//    [ <no_ati>1</no_ati> ]
//    [ <no_intel_gpu>1</no_intel_gpu> ]
//    <project-specific>
//            ... (arbitrary project-specific XML)
//    </project-specific>
//    <venue name="home">
//       ...
//    </venue>
// </project_preferences>
//
// In addition there are some fields of the user table
// (send_email and show_hosts) that are treated as project preferences

include_once("../inc/prefs_util.inc");
include_once("../project/project_specific_prefs.inc");

$app_types = get_app_types();

$project_pref_descs = array(
    new PREF_NUM(
        $x = tra("Resource share")
            ."<br><p class=\"text-muted\">"
            .tra("Determines the proportion of your computer's resources allocated to this project. Example: if you participate in two BOINC projects with resource shares of 100 and 200, the first will get 1/3 of your resources and the second will get 2/3. %2")
            ."</p>",
        "resource_share",
        new NUM_SPEC("", 0, 9999999, 100)
    ),
);

if (!empty($accelerate_gpu_apps_pref)) {
    $project_pref_descs[] = new PREF_BOOL(
        tra("Accelerate GPU tasks by dedicating a CPU to each one?"),
        "accelerate_gpu_apps",
        false
    );
}

if ($app_types->cpu) {
    $project_pref_descs[] = new PREF_BOOL (
        tra("Use CPU")
            ."<br><p class=\"text-muted\">"
            .tra("Enforced by version 6.10+")
            ."</p>",
        "no_cpu",
        false,
        true
    );
}
if ($app_types->ati) {
    $project_pref_descs[] = new PREF_BOOL (
        tra("Use ATI GPU")
            ."<br><p class=\"text-muted\">"
            .tra("Enforced by version 6.10+")
            ."</p>",
        "no_ati",
        false,
        true
    );
}
if ($app_types->cuda) {
    $project_pref_descs[] = new PREF_BOOL (
        tra("Use NVIDIA GPU")
            ."<br><p class=\"text-muted\">"
            .tra("Enforced by version 6.10+")
            ."</p>",
        "no_cuda",
        false,
        true
    );
}
if ($app_types->intel_gpu) {
    $project_pref_descs[] = new PREF_BOOL (
        tra("Use Intel GPU")
            ."<br><p class=\"text-muted\">"
            .tra("Enforced by version 7.2+")
            ."</p>",
        "no_intel_gpu",
        false,
        true
    );
}

if (project_has_beta()) {
    $project_pref_descs[] = new PREF_BOOL(
        tra("Run test applications?")
            ."<br><p class=\"text-muted\">"
            .tra("This helps us develop applications, but may cause jobs to fail on your computer")
            ."</p>",
        "allow_beta_work",
        false
    );
}
if (defined("EMAIL_FROM")) {
    $x = "<br><p class=\"text-muted\">"
        .tra("Emails will be sent from %1; make sure your spam filter accepts this address.", EMAIL_FROM)
        ."</p>";
} else {
    $x = "";
}

$privacy_pref_descs = array (
    new PREF_BOOL(
        tra("Is it OK for %1 and your team (if any) to email you?", PROJECT).$x,
        "send_email",
        true,
        false
    ),
    new PREF_BOOL(
        tra("Should %1 show your computers on its web site?", PROJECT),
        "show_hosts",
        true,
        false
    ),
);

global $text;
global $parse_result;
global $top_parse_result;
global $in_project_specific;
global $venue_name;

// functions to parse preferences XML into a struct
//
function element_start_project($parser, $name, $attrs) {
    global $top_parse_result;
    global $parse_result;
    global $text;
    global $in_project_specific;
    global $venue_name;

    switch($name) {
    case "venue":
        $venue_name = $attrs["name"];
        $top_parse_result = $parse_result;
        $parse_result = default_prefs_project();
        break;
    case "project_specific":
        $in_project_specific = 1;
        $text = "";
        break;
    default:
        if ($in_project_specific) {
            $text= $text."<$name>";
        } else {
            $text = "";
        }
    }
}

function element_end_project($parser, $name) {
    global $text;
    global $parse_result;
    global $in_project_specific;
    global $top_parse_result;
    global $venue_name;
    global $project_pref_descs;

    foreach($project_pref_descs as $p) {
        if ($p->xml_parse($parse_result, $name, $text)) {
            return;
        }
    }
    switch($name) {
    case "venue":
        $top_parse_result->$venue_name = $parse_result;
        $parse_result = $top_parse_result;
        break;
    case "project_specific":
        $parse_result->project_specific = $text;
        $in_project_specific = false;
        break;
    case "project_preferences":
        break;
    default:
        if ($in_project_specific) {
            $text = $text."</$name>";
        } else {
            //echo "Unknown tag: $name\n";
        }
    }
}

function default_prefs_project() {
    global $project_pref_descs;

    $p = new StdClass;
    foreach ($project_pref_descs as $pref) {
        $pref->set_default($p);
    }
    $p->project_specific = project_specific_prefs_default();
    return $p;
}

// parse prefs from XML to a struct
//
function prefs_parse_project($prefs_xml) {
    global $parse_result;
    $parse_result = default_prefs_project();
    $xml_parser = xml_parser_create();
    xml_parser_set_option($xml_parser, XML_OPTION_CASE_FOLDING, 0);
    xml_set_element_handler($xml_parser, "element_start_project", "element_end_project");
    xml_set_character_data_handler($xml_parser, "char_handler");
    xml_parse($xml_parser, $prefs_xml, 1);
    return $parse_result;
}

function prefs_show_project($prefs, $columns=false) {
    global $project_pref_descs;
    if ($columns) {
        foreach ($project_pref_descs as $p) {
            $p->show_cols($prefs);
        }
    } else {
        foreach ($project_pref_descs as $p) {
            $p->show($prefs);
        }
    }
}

function prefs_show_privacy($user, $columns) {
    global $privacy_pref_descs;
    if ($columns) {
        foreach ($privacy_pref_descs as $p) {
            $p->show_cols($user);
        }
    } else {
        foreach ($privacy_pref_descs as $p) {
            $p->show($user);
        }
    }
}

function prefs_show_project_specific($prefs, $columns=false) {
    if ($columns) {
        $project_specific_prefs = project_specific_prefs_parse($prefs->project_specific);
        $project_specific_prefs->home = isset($prefs->home) ? project_specific_prefs_parse($prefs->home->project_specific) : "";
        $project_specific_prefs->work = isset($prefs->work) ? project_specific_prefs_parse($prefs->work->project_specific) : "";
        $project_specific_prefs->school = isset($prefs->school) ? project_specific_prefs_parse($prefs->school->project_specific) : "";
    } else {
        $project_specific_prefs = project_specific_prefs_parse($prefs->project_specific);
    }
    project_specific_prefs_show($project_specific_prefs, $columns);
}

function print_prefs_display_project($user, $columns=false) {
    $project_prefs = prefs_parse_project($user->project_prefs);

    start_table();
    $switch_link = " <font size=\"-2\"><a href=prefs.php?subset=project&cols=". (int)!$columns .">".tra("(Switch View)")."</a></font>";
    if ($columns) {
        row1(tra("Combined preferences").$switch_link, 2, "heading");
        echo "<tr><td colspan=2>";
        start_table();
        prefs_show_privacy($user, true);
        venue_show($user);
        row_top(tra("Project specific settings"));
        prefs_show_project($project_prefs, true);
        prefs_show_project_specific($project_prefs, true);
        row_links("project", $project_prefs);
        end_table();
        echo "</td></tr>\n";
    } else {
        if (isset($project_prefs->home) || isset($project_prefs->work) || isset($project_prefs->school)) {
            row1(tra("Primary (default) preferences").$switch_link, 2, "heading");
        }
        echo "<tr><td colspan=2>";
        start_table();
        prefs_show_project($project_prefs, false);
        prefs_show_privacy($user, false);
        venue_show($user);
        prefs_show_project_specific($project_prefs, false);
        $tokens = url_tokens($user->authenticator);
        row2("", "<a href=prefs_edit.php?subset=project$tokens>".tra("Edit %1 preferences", PROJECT)."</a>");
        end_table();
        echo "</td></tr>\n";
        prefs_display_venue($project_prefs, "home", "project");
        prefs_display_venue($project_prefs, "school", "project");
        prefs_display_venue($project_prefs, "work", "project");
    }
    end_table();
}

////////////////////////////////////////////
//
// Functions to display preference subsets as forms
//

function prefs_form_privacy($user) {
    global $privacy_pref_descs;
    foreach ($privacy_pref_descs as $p) {
        $p->show_form_row($user, false);
    }
}

function prefs_form_project($prefs, $error=false) {
    global $project_pref_descs;
    foreach ($project_pref_descs as $p) {
        $p->show_form_row($prefs, $error);
    }
}

function prefs_form_project_specific($prefs_xml, $error=false) {
    $prefs = project_specific_prefs_parse($prefs_xml);
    project_specific_prefs_edit($prefs, $error);
}

function prefs_resource_parse_form(&$prefs) {
    global $project_pref_descs;
    $error = false;
    foreach ($project_pref_descs as $p) {
        $p->parse_form($prefs, $error);
    }
    return $error;
}

function prefs_privacy_parse_form(&$user) {
    global $privacy_pref_descs;
    $error = false;
    foreach ($privacy_pref_descs as $p) {
        $p->parse_form($user, $error);
    }
    return $error;
}

// Parse the project specific prefs form.
// For details see project/project_specific_prefs.inc
//
function prefs_project_parse_form(&$prefs) {
    $error = false;
    $prefs->project_specific = project_specific_prefs_parse_form($error);
    return $error;
}

////////////////////////////////////////////
//
// convert prefs from structure to XML
//

// given a prefs structure, return the corresponding XML string
//
function project_prefs_make_xml($prefs, $primary=true) {
    global $project_pref_descs;
    $xml = "";
    if ($primary) {
        $xml = "<project_preferences>\n";
    }
    foreach ($project_pref_descs as $p) {
        $xml .= $p->xml_string($prefs);
    }
    if ($prefs->project_specific) {
        $x = trim($prefs->project_specific);
        $xml = $xml
            ."<project_specific>\n$x\n</project_specific>\n";
    }
    if (isset($prefs->home)) {
        $xml = $xml."<venue name=\"home\">\n".project_prefs_make_xml($prefs->home, false)."</venue>\n";
    }
    if (isset($prefs->work)) {
        $xml = $xml."<venue name=\"work\">\n".project_prefs_make_xml($prefs->work, false)."</venue>\n";
    }
    if (isset($prefs->school)) {
        $xml = $xml."<venue name=\"school\">\n".project_prefs_make_xml($prefs->school, false)."</venue>\n";
    }
    if ($primary) {
        $xml = $xml."</project_preferences>\n";
    }
    return $xml;
}

////////////////////////////////////////////
//
// Update user's prefs in database, from a given structure
//
function project_prefs_update(&$user, $prefs) {
    $prefs_xml = BoincDb::escape_string(project_prefs_make_xml($prefs));
    $send_email = $user->send_email?1:0;
    $show_hosts = $user->show_hosts?1:0;
    $retval = $user->update("project_prefs='$prefs_xml', send_email=$send_email, show_hosts=$show_hosts");
    if (!$retval) {
        return 1;
    }
    $user->project_prefs = $prefs_xml;
    return 0;
}

?>
