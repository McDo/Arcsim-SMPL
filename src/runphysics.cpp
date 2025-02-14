/*
  Copyright ©2013 The Regents of the University of California
  (Regents). All Rights Reserved. Permission to use, copy, modify, and
  distribute this software and its documentation for educational,
  research, and not-for-profit purposes, without fee and without a
  signed licensing agreement, is hereby granted, provided that the
  above copyright notice, this paragraph and the following two
  paragraphs appear in all copies, modifications, and
  distributions. Contact The Office of Technology Licensing, UC
  Berkeley, 2150 Shattuck Avenue, Suite 510, Berkeley, CA 94720-1620,
  (510) 643-7201, for commercial licensing opportunities.

  IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT,
  INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
  LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
  DOCUMENTATION, EVEN IF REGENTS HAS BEEN ADVISED OF THE POSSIBILITY
  OF SUCH DAMAGE.

  REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE. THE SOFTWARE AND ACCOMPANYING
  DOCUMENTATION, IF ANY, PROVIDED HEREUNDER IS PROVIDED "AS
  IS". REGENTS HAS NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT,
  UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
*/

#include "runphysics.hpp"

#include "conf.hpp"
#include "io.hpp"
#include "misc.hpp"
#include "separateobs.hpp"
#include "simulation.hpp"
#include "timer.hpp"
#include "util.hpp"
#include <boost/filesystem.hpp>
#include <cstdio>
#include <fstream>

using namespace std;

static string outprefix;
static fstream timingfile;

Simulation sim;
int frame;
Timer fps;

void copy_file (const string &input, const string &output);

bool is_number(const std::string &s) {
    std::string::const_iterator it = s.begin();
    while (it != s.end() && std::isdigit(*it))
        ++it;
    return !s.empty() && it == s.end();
}

void init_physics (const string &json_file, string outprefix,
                   bool is_reloading) {
    load_json(json_file, sim);
    ::outprefix = outprefix;
    if (!sim.non_rigid && !outprefix.empty()) {
        ::timingfile.open(stringf("%s/timing", outprefix.c_str()).c_str(),
                          is_reloading ? ios::out|ios::app : ios::out);
        // Make a copy of the config file for future use
        copy_file(json_file.c_str(), stringf("%s/conf.json",outprefix.c_str()));
        // And copy over all the obstacles
        vector<Mesh*> base_meshes(sim.obstacles.size());
        for (int o = 0; o < sim.obstacles.size(); o++)
            base_meshes[o] = &sim.obstacles[o].base_mesh;
        save_objs(base_meshes, stringf("%s/obs", outprefix.c_str()), false);
    }
    prepare(sim);
    if (!is_reloading) {
        separate_obstacles(sim.obstacle_meshes, sim.cloth_meshes);
        relax_initial_state(sim);
    }
}

static void save (const vector<Mesh*> &meshes, int frame, bool non_rigid) {
    if (!outprefix.empty() && frame < 10000) {
        if (non_rigid)
            save_objs(meshes, stringf("%s/cloth%04d", outprefix.c_str(), frame), true);
        else
            save_objs(meshes, stringf("%s/%04d", outprefix.c_str(), frame), false);
    }
}

static void save_obstacle_transforms (const vector<Obstacle> &obs, int frame,
                                      double time) {
    if (!outprefix.empty() && frame < 10000) {
        for (int o = 0; o < obs.size(); o++) {
            Transformation trans = identity();
            if (obs[o].transform_spline)
                trans = get_dtrans(*obs[o].transform_spline, time).first;
            save_transformation(trans, stringf("%s/%04dobs%02d.txt",
                                               outprefix.c_str(), frame, o));
        }
    }
}

static void save_timings () {
    static double old_totals[Simulation::nModules] = {};
    if (!::timingfile)
        return; // printing timing data to stdout is getting annoying
    ostream &out = ::timingfile ? ::timingfile : cout;
    for (int i = 0; i < Simulation::nModules; i++) {
        out << sim.timers[i].total - old_totals[i] << " ";
        old_totals[i] = sim.timers[i].total;
    }
    out << endl;
}

void save (const Simulation &sim, int frame) {
    save(sim.cloth_meshes, frame, sim.non_rigid);
    if (!sim.non_rigid) {
        save_obstacle_transforms(sim.obstacles, frame, sim.time);
    }
}

void sim_step(const int num_frames) {
    fps.tick();
    advance_step(sim);

    // double time;
    // int frame, step;
    // std::vector<Cloth> cloths;
    // // constants
    // int frame_steps;
    // double frame_time, step_time;
    // double end_time, end_frame;
    printf("wait frames = %d | wind velocity = %d | sim.step = %d | sim.frame = %d | sim.time = %.2f\n",
        int(sim.init_wait_frames), int(sim.wind.velocity[0]), int(sim.step), int(sim.frame), sim.time
    );

    int frame_steps = sim.frame_steps;
    if (sim.non_rigid) {
        if (sim.init_frame_steps)
            frame_steps = sim.init_frame_steps;
        if (sim.init_wait_frames <= 1 && sim.step > 0 && sim.step % frame_steps == 0) {
            save(sim, sim.frame);
        }
    } else {
        if (sim.step % frame_steps == 0) {
            save(sim, sim.frame);
            save_timings();
        }
    }
    fps.tock();
    if (sim.time >= sim.end_time || sim.frame >= sim.end_frame)
        exit(EXIT_SUCCESS);

    if (sim.frame == num_frames)
        exit(EXIT_SUCCESS);
}

void offline_loop(const int num_frames) {
    while (true)
        sim_step(num_frames);
}

void run_physics (const vector<string> &args) {
    if (args.size() == 0 || args.size() > 3) {
        cout << "Runs the simulation in batch mode." << endl;
        cout << "Arguments:" << endl;
        cout << "    <scene-file>: JSON file describing the simulation setup" << endl;
        cout << "    <out-dir> (optional): Directory to save output in" << endl;
        cout << "    <num_frames> (optional): Number of frames to render" << endl;
        exit(EXIT_FAILURE);
    }

    // Check whether the maximum number of frames to render is set.
    int num_frames = DEFAULT_NUM_FRAMES;
    if (is_number(args[args.size() - 1]))
    {
        // Final parameter is integer, this is the number of frames to render for
        num_frames = atoi(args[args.size() - 1].c_str());
        cout << "Found 'number of frames' parameter, rendering " << num_frames << " frames." << endl;
    }

    string json_file = args[0];
    string outprefix = args.size()>1 ? args[1] : "";
    if (!outprefix.empty())
        ensure_existing_directory(outprefix);
    init_physics(json_file, outprefix, false);
    if (!sim.non_rigid && !outprefix.empty())
        save(sim, 0);

    offline_loop(num_frames);
}

void init_resume(const vector<string> &args) {
    assert(args.size() == 2);
    string outprefix = args[0];
    string start_frame_str = args[1];
    // Load like we would normally begin physics
    init_physics(stringf("%s/conf.json", outprefix.c_str()), outprefix, true);
    // Get the initialization information
    sim.frame = atoi(start_frame_str.c_str());
    sim.time = sim.frame * sim.frame_time;
    sim.step = sim.frame * sim.frame_steps;
    for(int i = 0; i < sim.obstacles.size(); ++i) {
        if (sim.non_rigid) {
            sim.obstacles[i].get_mesh(sim.time, sim.frame);
        } else {
            sim.obstacles[i].get_mesh(sim.time);
        }
    }
    load_objs(sim.cloth_meshes, stringf("%s/%04d",outprefix.c_str(),sim.frame));
    prepare(sim); // re-prepare the new cloth meshes
    separate_obstacles(sim.obstacle_meshes, sim.cloth_meshes);
}

void resume_physics (const vector<string> &args) {
    if (args.size() != 2) {
        cout << "Resumes an incomplete simulation in batch mode." << endl;
        cout << "Arguments:" << endl;
        cout << "    <out-dir>: Directory containing simulation output files"
             << endl;
        cout << "    <resume-frame>: Frame number to resume from" << endl;
        exit(EXIT_FAILURE);
    }
    init_resume(args);
    offline_loop(DEFAULT_NUM_FRAMES);
}

void copy_file (const string &input, const string &output) {
    if(input == output) {
        return;
    }
    if(boost::filesystem::exists(output)) {
        boost::filesystem::remove(output);
    }
    boost::filesystem::copy_file(
        input, output);
}
