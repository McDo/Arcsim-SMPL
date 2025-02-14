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

#include "io.hpp"
#include "obstacle.hpp"
#include "util.hpp"
#include <cstdio>
#include <algorithm>
#include <fstream>

using namespace std;

Mesh& Obstacle::get_mesh() {
    return curr_state_mesh;
}

const Mesh& Obstacle::get_mesh() const {
    return curr_state_mesh;
}

Mesh& Obstacle::get_mesh(double time) {
    if (time > end_time) {
        delete_mesh(base_mesh);
        delete_mesh(curr_state_mesh);
        delete_mesh(next_state_mesh);
        delete_mesh(cache_mesh);
    }
    if (time < start_time || time > end_time)
        return curr_state_mesh;
    if (!activated) {
        curr_state_mesh = deep_copy(base_mesh);
        curr_frame = -1;
    }
    if (transform_spline) {
        DTransformation dtrans = get_dtrans(*transform_spline, time);
        Mesh &mesh = curr_state_mesh;
        for (int n = 0; n < curr_state_mesh.nodes.size(); n++)
            mesh.nodes[n]->x = apply_dtrans(dtrans, base_mesh.nodes[n]->x,
                                            &mesh.nodes[n]->v);
        compute_ws_data(mesh);
    }
    if (!activated) {
        update_x0(curr_state_mesh);
    }
    activated = true;
    return curr_state_mesh;
}

Mesh& Obstacle::get_mesh(double time, int frame) {
    if (time > end_time) {
        delete_mesh(base_mesh);
        delete_mesh(curr_state_mesh);
        delete_mesh(next_state_mesh);
        delete_mesh(cache_mesh);
    }
    if (time < start_time || time > end_time)
        return curr_state_mesh;

    if (!activated) {
        curr_state_mesh = deep_copy(base_mesh);
        update_x0(curr_state_mesh);
        curr_frame = -1;
        activated = true;
    } else {
        if (frame > curr_frame) {
            curr_frame = frame;
            string next_state_mesh_path = base_path + stringf("body%04d.obj", frame);
            if (!fstream(next_state_mesh_path.c_str(), ios::in)) {
                delete_mesh(base_mesh);
                delete_mesh(curr_state_mesh);
                delete_mesh(cache_mesh);
                delete_mesh(next_state_mesh);
                cout << "Done." << endl;
                exit(1);
            }
            delete_mesh(cache_mesh);
            cache_mesh = deep_copy(curr_state_mesh);
            delete_mesh(next_state_mesh);
            load_obj(next_state_mesh, next_state_mesh_path);
        }
    }
    return curr_state_mesh;
}

void Obstacle::blend_with_previous (double t, double dt, double blend) {
    const Motion *spline = transform_spline;
    Transformation trans = (spline)
                         ? get_trans(*spline, t)
                           * inverse(get_trans(*spline, t-dt))
                         : identity();
    Mesh &mesh = curr_state_mesh;
    for (int n = 0; n < mesh.nodes.size(); n++) {
        Node *node = mesh.nodes[n];
        Vec3 x0 = trans.apply(node->x0);
        node->x = x0 + blend * (node->x - x0);
    }
    compute_ws_data(mesh);
}

void Obstacle::blend_with_next (double blend) {
    Mesh &mesh = curr_state_mesh;
    for (int n = 0; n < mesh.nodes.size(); n++) {
        Node *node = mesh.nodes[n];
        node->x = node->x + blend * (next_state_mesh.nodes[n]->x - cache_mesh.nodes[n]->x);
    }
}
