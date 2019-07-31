/*
    Copyright (C) 2000-2019 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/
#include <vector>

#include <stdio.h>
#include <glibmm.h>
#include <glibmm/miscutils.h>

#include "ardour/directory_names.h"
#include "ardour/filename_extensions.h"
#include "ardour/filesystem_paths.h"
#include "ardour/mixer_snapshot_manager.h"
#include "ardour/mixer_snapshot.h"
#include "ardour/search_paths.h"
#include "ardour/session_directory.h"
#include "ardour/template_utils.h"

#include "pbd/basename.h"
#include "pbd/file_utils.h"
#include "pbd/gstdio_compat.h"

using namespace ARDOUR;
using namespace std;

MixerSnapshotManager::MixerSnapshotManager (Session* s)
{
    if(!s) {
        throw failed_constructor();
    }
    _session = s;

    _global_path = user_route_template_directory();
    _local_path  = Glib::build_filename(_session->session_directory().root_path(), route_templates_dir_name);

    refresh();
}

void MixerSnapshotManager::ensure_snapshot_dir(bool global)
{
    const string path = global ? _global_path : _local_path;
    if(!Glib::file_test(path.c_str(), Glib::FILE_TEST_EXISTS & Glib::FILE_TEST_IS_DIR)) {
        ::g_mkdir(path.c_str(), 0775);
    }
}

void MixerSnapshotManager::refresh()
{
    clear();
    vector<TemplateInfo> global_templates;
    find_route_templates(global_templates);

    if(!global_templates.empty()) {
        for(vector<TemplateInfo>::const_iterator it = global_templates.begin(); it != global_templates.end(); it++) {
            TemplateInfo info = (*it);

            MixerSnapshot* snap = new MixerSnapshot(_session, info.path);
            snap->set_label(info.name);
            snap->set_path(info.path);
            _global_snapshots.insert(snap);

            printf("Global - name: %s\n", snap->get_label().c_str());
            printf("Global - path: %s\n", info.path.c_str());
        }
    }


    //this should be in search_paths and then integrated into template_utils
    //but having it be based on the session presents... complications
    PBD::Searchpath spath (_session->session_directory().root_path());
    spath.add_subdirectory_to_paths(route_templates_dir_name);

    vector<string> local_templates;
    string pattern = "*" + string(template_suffix);
    find_files_matching_pattern(local_templates, spath, pattern);

    printf("Searching for templates with pattern %s in searchpath %s\n", pattern.c_str(), spath.to_string().c_str());
    if(!local_templates.empty()) {
        for(vector<string>::const_iterator it = local_templates.begin(); it != local_templates.end(); it++) {
            const string path  = (*it);
            const string label = PBD::basename_nosuffix(path);

            MixerSnapshot* snap = new MixerSnapshot(_session, path);
            snap->set_label(label);
            _local_snapshots.insert(snap);

            printf("Local - name: %s\n", snap->get_label().c_str());
            printf("Local - path: %s\n", path.c_str());
        }
    }
}

bool MixerSnapshotManager::promote(MixerSnapshot* snapshot) {
    if(!snapshot) {
        return false;
    }

    const string path = snapshot->get_path();
    if(Glib::file_test(path.c_str(), Glib::FILE_TEST_EXISTS)) {
        const string dir = Glib::build_filename(user_config_directory(-1), route_templates_dir_name);
        //this will just write the snapshot to the global dir
        snapshot->write(dir);

        //make new snapshot
        string new_path = Glib::build_filename(dir, snapshot->get_label() + string(template_suffix));
        MixerSnapshot* new_snap = new MixerSnapshot(_session, new_path);
        new_snap->set_label(snapshot->get_label());
        new_snap->set_path(new_path);

        //this might've been overwritten and thus needs a new pointer
        MixerSnapshot* old_snapshot = get_snapshot_by_name(new_snap->get_label(), true);
        set<MixerSnapshot*>::iterator iter = _global_snapshots.find(old_snapshot);
        if(iter != _global_snapshots.end()) {
            _global_snapshots.erase(iter);
        }

        //insert the new snapshot
        _global_snapshots.insert(new_snap);
    }
    return true;
}

bool MixerSnapshotManager::rename_snapshot(MixerSnapshot* snapshot, const string& new_name) {
    if(!snapshot) {
        return false;
    }

    if(new_name.empty()) {
        return false;
    }
    const string old_path = snapshot->get_path();
    const string dir = Glib::path_get_dirname(old_path);
    snapshot->set_label(new_name);

    const string new_path = Glib::build_filename(dir, snapshot->get_label() + string(template_suffix));
    ::g_rename(old_path.c_str(), new_path.c_str());
    snapshot->set_path(new_path);
    return true;
}

bool MixerSnapshotManager::remove_snapshot(MixerSnapshot* snapshot) {
    if(!snapshot) {
        return false;
    }

    const string path = snapshot->get_path();
    const string dir = Glib::path_get_dirname(path);

    ::g_remove(path.c_str());

    set<MixerSnapshot*>::iterator iter;
    if(dir == _global_path) {
        iter = _global_snapshots.find(snapshot);
        if(iter != _global_snapshots.end()) {
            _global_snapshots.erase(iter);
        }
    } else {
        iter = _local_snapshots.find(snapshot);
        if(iter != _local_snapshots.end()) {
            _local_snapshots.erase(iter);
        }
    }
    return true;
}

bool MixerSnapshotManager::demote(MixerSnapshot* snapshot) {
    return true;
}

MixerSnapshot* MixerSnapshotManager::get_snapshot_by_name(const string& name, bool global)
{
    set<MixerSnapshot*> snapshots_list = global ? _global_snapshots : _local_snapshots;

    set<MixerSnapshot*>::iterator it;
    for(it = snapshots_list.begin(); it != snapshots_list.end(); it++) {
        if((*it)->get_label() == name) {
            return (*it);
        }
    }
}

void MixerSnapshotManager::create_snapshot(std::string const& label, RouteList& rl, bool global)
{
    ensure_snapshot_dir(global);
    const string path = global ? _global_path : _local_path;
    MixerSnapshot* snapshot = new MixerSnapshot(_session);

    if(!rl.empty()) {
        //just this routelist
        snapshot->snap(rl);
    } else {
        //the whole session
        snapshot->snap();
    }

    snapshot->set_label(label);
    snapshot->write(path);

    const string full_path = Glib::build_filename(path, snapshot->get_label() + string(template_suffix));
    snapshot->set_path(full_path);

    MixerSnapshot* old_snapshot = get_snapshot_by_name(snapshot->get_label(), global);
    set<MixerSnapshot*>& snapshots_list = global ? _global_snapshots : _local_snapshots;
    set<MixerSnapshot*>::iterator iter = snapshots_list.find(old_snapshot);

    //remove it from it's set
    if(iter != snapshots_list.end()) {
        snapshots_list.erase(iter);
    }
    //and insert the new one
    snapshots_list.insert(snapshot);
}

void MixerSnapshotManager::create_snapshot(std::string const& label, std::string const& from_path, bool global)
{
    ensure_snapshot_dir(global);
    const string path = global ? _global_path : _local_path;
    MixerSnapshot* snapshot = new MixerSnapshot(_session, from_path);
    snapshot->set_label(label);
    snapshot->write(path);

    const string full_path = Glib::build_filename(path, snapshot->get_label() + string(template_suffix));
    snapshot->set_path(full_path);

    MixerSnapshot* old_snapshot = get_snapshot_by_name(snapshot->get_label(), global);
    set<MixerSnapshot*>& snapshots_list = global ? _global_snapshots : _local_snapshots;
    set<MixerSnapshot*>::iterator iter = snapshots_list.find(old_snapshot);

    //remove it from it's set
    if(iter != snapshots_list.end()) {
        snapshots_list.erase(iter);
    }
    //and insert the new one
    snapshots_list.insert(snapshot);
}