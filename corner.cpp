#include "internal.hpp"

using namespace osh;

int main(int argc, char** argv) {
  auto lib = Library(&argc, &argv);
  auto world = lib.world();
  Mesh mesh;
  if (world->rank() == 0) {
    build_box(&mesh, 1, 1, 1, 4, 4, 4);
    classify_by_angles(&mesh, PI / 4);
  }
  mesh.set_comm(world);
  mesh.balance();
  mesh.add_tag<Real>(VERT, "size", 1, OSH_LINEAR_INTERP);
  vtk::FullWriter writer(&mesh, "out");
  do {
    Write<Real> size(mesh.nverts());
    auto coords = mesh.coords();
    auto f = LAMBDA(LO v) {
      auto x = get_vec<3>(coords, v);
      auto coarse = 0.3;
      auto fine = 0.05;
      auto radius = norm(x);
      auto diagonal = sqrt(3) - 0.5;
      auto distance = fabs(radius - 0.5) / diagonal;
      size[v] = coarse * distance + fine * (1.0 - distance);
    };
    parallel_for(mesh.nverts(), f);
    mesh.set_tag(VERT, "size", Reals(size));
    mesh.ask_edge_lengths();
    writer.write();
  } while(refine_by_size(&mesh, 4.0 / 3.0, 0.47));
}
