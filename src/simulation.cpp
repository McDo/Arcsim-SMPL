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

#include "simulation.hpp"

#include "collision.hpp"
#include "dynamicremesh.hpp"
#include "geometry.hpp"
#include "magic.hpp"
#include "nearobs.hpp"
#include "physics.hpp"
#include "plasticity.hpp"
#include "popfilter.hpp"
#include "proximity.hpp"
#include "separate.hpp"
#include "strainlimiting.hpp"
#include <iostream>
#include <fstream>
using namespace std;

static const bool verbose = false;
static const int proximity = Simulation::Proximity,
                 physics = Simulation::Physics,
                 strainlimiting = Simulation::StrainLimiting,
                 collision = Simulation::Collision,
                 remeshing = Simulation::Remeshing,
                 separation = Simulation::Separation,
                 popfilter = Simulation::PopFilter,
                 plasticity = Simulation::Plasticity;

void physics_step (Simulation &sim, const vector<Constraint*> &cons);
void plasticity_step (Simulation &sim);
void strainlimiting_step (Simulation &sim, const vector<Constraint*> &cons);
void strainzeroing_step (Simulation &sim);
void equilibration_step (Simulation &sim);
void collision_step (Simulation &sim);
void remeshing_step (Simulation &sim, bool initializing=false);

void validate_handles (const Simulation &sim);

void prepare (Simulation &sim) {
    sim.cloth_meshes.resize(sim.cloths.size());
    for (int c = 0; c < sim.cloths.size(); c++) {
        compute_masses(sim.cloths[c]);
        sim.cloth_meshes[c] = &sim.cloths[c].mesh;
        update_x0(*sim.cloth_meshes[c]);
    }
    sim.obstacle_meshes.resize(sim.obstacles.size());
    for (int o = 0; o < sim.obstacles.size(); o++) {
        sim.obstacle_meshes[o] = &sim.obstacles[o].get_mesh();
        update_x0(*sim.obstacle_meshes[o]);
    }
}

void relax_initial_state (Simulation &sim) {
    validate_handles(sim);
    if (::magic.preserve_creases)
        for (int c = 0; c < sim.cloths.size(); c++)
            reset_plasticity(sim.cloths[c]);
    bool equilibrate = true;
    if (equilibrate) {
        equilibration_step(sim);
        remeshing_step(sim, true);
        equilibration_step(sim);
    } else {
        remeshing_step(sim, true);
        strainzeroing_step(sim);
        remeshing_step(sim, true);
        strainzeroing_step(sim);
    }
    if (::magic.preserve_creases)
        for (int c = 0; c < sim.cloths.size(); c++)
            reset_plasticity(sim.cloths[c]);
    ::magic.preserve_creases = false;
    if (::magic.fixed_high_res_mesh)
        sim.enabled[remeshing] = false;
}

void validate_handles (const Simulation &sim) {
    for (int h = 0; h < sim.handles.size(); h++) {
        vector<Node*> nodes = sim.handles[h]->get_nodes();
        for (int n = 0; n < nodes.size(); n++) {
            if (!nodes[n]->preserve) {
                cout << "Constrained node " << nodes[n]->index << " will not be preserved by remeshing" << endl;
                abort();
            }
        }
    }
}

vector<Constraint*> get_constraints (Simulation &sim, bool include_proximity);
void delete_constraints (const vector<Constraint*> &cons);
void update_obstacles (Simulation &sim, bool update_positions=true);

void advance_step (Simulation &sim);

void advance_frame (Simulation &sim) {
    for (int s = 0; s < sim.frame_steps; s++)
        advance_step(sim);
}

void advance_step (Simulation &sim) {
    sim.time += sim.step_time;
    sim.step++;
    if (sim.non_rigid)
        update_obstacles(sim, true);
    else
        update_obstacles(sim, false);
    vector<Constraint*> cons = get_constraints(sim, true);
    physics_step(sim, cons);
    plasticity_step(sim);
    strainlimiting_step(sim, cons);
    collision_step(sim);
    if (sim.init_frame_steps) {
        if (sim.step == sim.init_frame_steps + 1) {
            sim.init_frame_steps = 0;
            sim.step_time = sim.frame_time/sim.frame_steps;
            remeshing_step(sim);
            if (!sim.init_wait_frames)
                sim.frame++;
        }
    } else {
        if ((sim.step - 1) % sim.frame_steps == 0) {
            remeshing_step(sim);
            sim.init_wait_frames = max(0, sim.init_wait_frames - 1);
            if (!sim.init_wait_frames)
                sim.frame++;
        }
    }
    delete_constraints(cons);
}

vector<Constraint*> get_constraints (Simulation &sim, bool include_proximity) {
    vector<Constraint*> cons;
    for (int h = 0; h < sim.handles.size(); h++)
        append(cons, sim.handles[h]->get_constraints(sim.time));
    if (include_proximity && sim.enabled[proximity]) {
        sim.timers[proximity].tick();
        append(cons, proximity_constraints(sim.cloth_meshes,
                                           sim.obstacle_meshes,
                                           sim.friction, sim.obs_friction));
        sim.timers[proximity].tock();
    }
    return cons;
}

void delete_constraints (const vector<Constraint*> &cons) {
    for (int c = 0; c < cons.size(); c++)
        delete cons[c];
}

// Steps

void update_velocities (vector<Mesh*> &meshes, vector<Vec3> &xold, double dt);

void step_mesh (Mesh &mesh, double dt);

void physics_step (Simulation &sim, const vector<Constraint*> &cons) {
    if (!sim.enabled[physics])
        return;
    sim.timers[physics].tick();
    for (int c = 0; c < sim.cloths.size(); c++) {
        int nn = sim.cloths[c].mesh.nodes.size();
        vector<Vec3> fext(nn, Vec3(0));
        vector<Mat3x3> Jext(nn, Mat3x3(0));
        add_external_forces(sim.cloths[c], sim.gravity, sim.wind, fext, Jext);
        for (int m = 0; m < sim.morphs.size(); m++)
            if (sim.morphs[m].mesh == &sim.cloths[c].mesh)
                add_morph_forces(sim.cloths[c], sim.morphs[m], sim.time,
                                 sim.step_time, fext, Jext);
        implicit_update(sim.cloths[c], fext, Jext, cons, sim.step_time, false);
    }
    for (int c = 0; c < sim.cloth_meshes.size(); c++)
        step_mesh(*sim.cloth_meshes[c], sim.step_time);
    for (int o = 0; o < sim.obstacle_meshes.size(); o++)
        step_mesh(*sim.obstacle_meshes[o], sim.step_time);
    sim.timers[physics].tock();
}

void step_mesh (Mesh &mesh, double dt) {
    for (int n = 0; n < mesh.nodes.size(); n++)
        mesh.nodes[n]->x += mesh.nodes[n]->v*dt;
}

void plasticity_step (Simulation &sim) {
    if (!sim.enabled[plasticity])
        return;
    sim.timers[plasticity].tick();
    for (int c = 0; c < sim.cloths.size(); c++) {
        plastic_update(sim.cloths[c]);
        optimize_plastic_embedding(sim.cloths[c]);
    }
    sim.timers[plasticity].tock();
}

void strainlimiting_step (Simulation &sim, const vector<Constraint*> &cons) {
    if (!sim.enabled[strainlimiting])
        return;
    sim.timers[strainlimiting].tick();
    vector<Vec3> xold = node_positions(sim.cloth_meshes);
    strain_limiting(sim.cloth_meshes, get_strain_limits(sim.cloths), cons);
    update_velocities(sim.cloth_meshes, xold, sim.step_time);
    sim.timers[strainlimiting].tock();
}

void equilibration_step (Simulation &sim) {
    sim.timers[remeshing].tick();
    vector<Constraint*> cons;// = get_constraints(sim, true);
    // double stiff = 1;
    // swap(stiff, ::magic.handle_stiffness);
    for (int c = 0; c < sim.cloths.size(); c++) {
        Mesh &mesh = sim.cloths[c].mesh;
        for (int n = 0; n < mesh.nodes.size(); n++)
            mesh.nodes[n]->acceleration = Vec3(0);
        apply_pop_filter(sim.cloths[c], cons, 1);
    }
    // swap(stiff, ::magic.handle_stiffness);
    sim.timers[remeshing].tock();
    delete_constraints(cons);
    cons = get_constraints(sim, false);
    if (sim.enabled[collision]) {
        sim.timers[collision].tick();
        collision_response(sim.cloth_meshes, cons, sim.obstacle_meshes);
        sim.timers[collision].tock();
    }
    delete_constraints(cons);
}

void strainzeroing_step (Simulation &sim) {
    sim.timers[strainlimiting].tick();
    vector<Vec2> strain_limits(size<Face>(sim.cloth_meshes), Vec2(1,1));
    vector<Constraint*> cons =
        proximity_constraints(sim.cloth_meshes, sim.obstacle_meshes,
                              sim.friction, sim.obs_friction);
    strain_limiting(sim.cloth_meshes, strain_limits, cons);
    delete_constraints(cons);
    sim.timers[strainlimiting].tock();
    if (sim.enabled[collision]) {
        sim.timers[collision].tock();
        collision_response(sim.cloth_meshes, vector<Constraint*>(),
                           sim.obstacle_meshes);
        sim.timers[collision].tock();
    }
}

void collision_step (Simulation &sim) {
    if (!sim.enabled[collision])
        return;
    sim.timers[collision].tick();
    vector<Vec3> xold = node_positions(sim.cloth_meshes);
    vector<Constraint*> cons = get_constraints(sim, false);
    collision_response(sim.cloth_meshes, cons, sim.obstacle_meshes);
    delete_constraints(cons);
    update_velocities(sim.cloth_meshes, xold, sim.step_time);
    sim.timers[collision].tock();
}

void remeshing_step (Simulation &sim, bool initializing) {
    if (!sim.enabled[remeshing])
        return;
    // copy old meshes
    vector<Mesh> old_meshes(sim.cloths.size());
    vector<Mesh*> old_meshes_p(sim.cloths.size()); // for symmetry in separate()
    for (int c = 0; c < sim.cloths.size(); c++) {
        old_meshes[c] = deep_copy(sim.cloths[c].mesh);
        old_meshes_p[c] = &old_meshes[c];
    }
    // back up residuals
    typedef vector<Residual> MeshResidual;
    vector<MeshResidual> res;
    if (sim.enabled[plasticity] && !initializing) {
        sim.timers[plasticity].tick();
        res.resize(sim.cloths.size());
        for (int c = 0; c < sim.cloths.size(); c++)
            res[c] = back_up_residuals(sim.cloths[c].mesh);
        sim.timers[plasticity].tock();
    }
    // remesh
    sim.timers[remeshing].tick();
    for (int c = 0; c < sim.cloths.size(); c++) {
        if (::magic.fixed_high_res_mesh)
            static_remesh(sim.cloths[c]);
        else {
            vector<Plane> planes = nearest_obstacle_planes(sim.cloths[c].mesh,
                                                           sim.obstacle_meshes);
            dynamic_remesh(sim.cloths[c], planes, sim.enabled[plasticity]);
        }
    }
    sim.timers[remeshing].tock();
    // restore residuals
    if (sim.enabled[plasticity] && !initializing) {
        sim.timers[plasticity].tick();
        for (int c = 0; c < sim.cloths.size(); c++)
            restore_residuals(sim.cloths[c].mesh, old_meshes[c], res[c]);
        sim.timers[plasticity].tock();
    }
    // separate
    if (sim.enabled[separation]) {
        sim.timers[separation].tick();
        separate(sim.cloth_meshes, old_meshes_p, sim.obstacle_meshes);
        sim.timers[separation].tock();
    }
    // apply pop filter
    if (sim.enabled[popfilter] && !initializing) {
        sim.timers[popfilter].tick();
        vector<Constraint*> cons = get_constraints(sim, true);
        for (int c = 0; c < sim.cloths.size(); c++)
            apply_pop_filter(sim.cloths[c], cons);
        delete_constraints(cons);
        sim.timers[popfilter].tock();
    }
    // delete old meshes
    for (int c = 0; c < sim.cloths.size(); c++)
        delete_mesh(old_meshes[c]);
}

void update_velocities (vector<Mesh*> &meshes, vector<Vec3> &xold, double dt) {
    double inv_dt = 1/dt;
#pragma omp parallel for
    for (int n = 0; n < xold.size(); n++) {
        Node *node = get<Node>(n, meshes);
        node->v += (node->x - xold[n])*inv_dt;
    }
}

void update_obstacles (Simulation &sim, bool update_positions) {
    double decay_time = 0.1, blend = 0.;
    
    if (sim.non_rigid) {
        int frame_steps = sim.frame_steps;
        if (sim.init_frame_steps)
            frame_steps = sim.init_frame_steps;
        blend = 1. / frame_steps;
    } else {
        blend = sim.step_time/decay_time;
        blend = blend/(1 + blend);
    }

    for (int o = 0; o < sim.obstacles.size(); o++) {
        if (sim.non_rigid) {
            if (sim.init_wait_frames) {
                if (sim.step <= sim.init_frame_steps) {
                    sim.obstacles[o].get_mesh(sim.time, sim.frame);
                    sim.obstacles[o].blend_with_next(blend);
                }
            } else {
                sim.obstacles[o].get_mesh(sim.time, sim.frame);
                sim.obstacles[o].blend_with_next(blend);
            }
        } else {
            sim.obstacles[o].get_mesh(sim.time);
            sim.obstacles[o].blend_with_previous(sim.time, sim.step_time, blend);
        }
        if (!update_positions) {
            // put positions back where they were
            Mesh &mesh = sim.obstacles[o].get_mesh();
            for (int n = 0; n < mesh.nodes.size(); n++) {
                Node *node = mesh.nodes[n];
                node->v = (node->x - node->x0)/sim.step_time;
                node->x = node->x0;
            }
        }
    }
}

// Helper functions

template <typename Prim> int size (const vector<Mesh*> &meshes) {
    int np = 0;
    for (int m = 0; m < meshes.size(); m++) np += get<Prim>(*meshes[m]).size();
    return np;
}
template int size<Vert> (const vector<Mesh*>&);
template int size<Node> (const vector<Mesh*>&);
template int size<Edge> (const vector<Mesh*>&);
template int size<Face> (const vector<Mesh*>&);

template <typename Prim> int get_index (const Prim *p,
                                        const vector<Mesh*> &meshes) {
    int i = 0;
    for (int m = 0; m < meshes.size(); m++) {
        const vector<Prim*> &ps = get<Prim>(*meshes[m]);
        if (p->index < ps.size() && p == ps[p->index])
            return i + p->index;
        else i += ps.size();
    }
    return -1;
}
template int get_index (const Vert*, const vector<Mesh*>&);
template int get_index (const Node*, const vector<Mesh*>&);
template int get_index (const Edge*, const vector<Mesh*>&);
template int get_index (const Face*, const vector<Mesh*>&);

template <typename Prim> Prim *get (int i, const vector<Mesh*> &meshes) {
    for (int m = 0; m < meshes.size(); m++) {
        const vector<Prim*> &ps = get<Prim>(*meshes[m]);
        if (i < ps.size())
            return ps[i];
        else
            i -= ps.size();
    }
    return NULL;
}
template Vert *get (int, const vector<Mesh*>&);
template Node *get (int, const vector<Mesh*>&);
template Edge *get (int, const vector<Mesh*>&);
template Face *get (int, const vector<Mesh*>&);

vector<Vec3> node_positions (const vector<Mesh*> &meshes) {
    vector<Vec3> xs(size<Node>(meshes));
    for (int n = 0; n < xs.size(); n++)
        xs[n] = get<Node>(n, meshes)->x;
    return xs;
}
