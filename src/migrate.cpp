#include "migrate.hpp"

#include <iostream>

#include "loop.hpp"
#include "map.hpp"
#include "owners.hpp"
#include "remotes.hpp"
#include "scan.hpp"
#include "simplices.hpp"
#include "tag.hpp"

namespace osh {

Remotes form_down_use_owners(Mesh* mesh, Int high_dim, Int low_dim) {
  auto uses2lows = mesh->ask_down(high_dim, low_dim).ab2b;
  auto lows2owners = mesh->ask_owners(low_dim);
  return unmap(uses2lows, lows2owners);
}

Dist find_unique_use_owners(Dist uses2old_owners) {
  auto old_owners2uses = uses2old_owners.invert();
  auto nold_owners = old_owners2uses.nroots();
  auto nserv_uses = old_owners2uses.nitems();
  auto serv_uses2ranks = old_owners2uses.items2ranks();
  auto old_owners2serv_uses = old_owners2uses.roots2items();
  Write<I8> keep(nserv_uses, 1);
  Write<LO> degrees(nold_owners);
  auto f = LAMBDA(LO old_owner) {
    LO degree = 0;
    auto begin = old_owners2serv_uses[old_owner];
    auto end = old_owners2serv_uses[old_owner + 1];
    for (LO serv_use = begin; serv_use < end; ++serv_use) {
      if (!keep[serv_use]) continue;
      ++degree;
      auto rank = serv_uses2ranks[serv_use];
      for (LO serv_use2 = serv_use + 1; serv_use2 < end; ++serv_use2) {
        if (serv_uses2ranks[serv_use2] == rank) {
          keep[serv_use2] = 0;
        }
      }
    }
    degrees[old_owner] = degree;
  };
  parallel_for(nold_owners, f);
  auto uniq_serv_uses = collect_marked(Read<I8>(keep));
  auto uniq_serv_uses2ranks = unmap(uniq_serv_uses, serv_uses2ranks, 1);
  auto old_owners2uniq_serv_uses = offset_scan(LOs(degrees));
  Dist old_owners2uniq_uses;
  old_owners2uniq_uses.set_parent_comm(uses2old_owners.parent_comm());
  old_owners2uniq_uses.set_dest_ranks(uniq_serv_uses2ranks);
  old_owners2uniq_uses.set_roots2items(old_owners2uniq_serv_uses);
  return old_owners2uniq_uses;
}

LOs form_new_conn(Dist new_ents2old_owners, Dist old_owners2new_uses) {
  auto nnew_ents = new_ents2old_owners.nitems();
  auto serv_ents2new_idxs = new_ents2old_owners.exch(LOs(nnew_ents, 0, 1), 1);
  auto old_owners2new_ents = new_ents2old_owners.invert();
  auto serv_ents2ranks = old_owners2new_ents.items2ranks();
  auto serv_uses2ranks = old_owners2new_uses.items2ranks();
  auto nserv_uses = old_owners2new_uses.nitems();
  auto old_owners2serv_uses = old_owners2new_uses.roots2items();
  auto old_owners2serv_ents = old_owners2new_ents.roots2items();
  auto nold_owners = old_owners2new_ents.nroots();
  Write<LO> serv_uses2new_idxs(nserv_uses);
  auto f = LAMBDA(LO old_owner) {
    auto ebegin = old_owners2serv_ents[old_owner];
    auto eend = old_owners2serv_ents[old_owner + 1];
    auto ubegin = old_owners2serv_uses[old_owner];
    auto uend = old_owners2serv_uses[old_owner + 1];
    for (auto u = ubegin; u < uend; ++u) {
      auto rank = serv_uses2ranks[u];
      LO idx = -1;
      for (auto e = ebegin; e < eend; ++e) {
        if (serv_ents2ranks[e] == rank) {
          idx = serv_ents2new_idxs[e];
          break;
        }
      }
      serv_uses2new_idxs[u] = idx;
    }
  };
  parallel_for(nold_owners, f);
  auto serv_uses2new_uses = old_owners2new_uses;
  serv_uses2new_uses.set_roots2items(LOs());
  return serv_uses2new_uses.exch(LOs(serv_uses2new_idxs), 1);
}

void push_down(Mesh* old_mesh, Int ent_dim, Int low_dim,
    Dist old_owners2new_ents, Adj& new_ents2new_lows,
    Dist& old_low_owners2new_lows) {
  auto nlows_per_high = simplex_degrees[ent_dim][low_dim];
  auto old_use_owners = form_down_use_owners(old_mesh, ent_dim, low_dim);
  Remotes new_use_owners =
      old_owners2new_ents.exch(old_use_owners, nlows_per_high);
  Dist low_uses2old_owners(
      old_mesh->comm(), new_use_owners, old_mesh->nents(low_dim));
  old_low_owners2new_lows = find_unique_use_owners(low_uses2old_owners);
  auto new_lows2old_owners = old_low_owners2new_lows.invert();
  auto old_low_owners2new_uses = low_uses2old_owners.invert();
  auto new_conn = form_new_conn(new_lows2old_owners, old_low_owners2new_uses);
  new_ents2new_lows.ab2b = new_conn;
  auto old_codes = old_mesh->ask_down(ent_dim, low_dim).codes;
  if (!old_codes.exists()) return;
  auto new_codes = old_owners2new_ents.exch(old_codes, nlows_per_high);
  new_ents2new_lows.codes = new_codes;
}

void push_tags(Mesh const* old_mesh, Mesh* new_mesh, Int ent_dim,
    Dist old_owners2new_ents) {
  CHECK(old_owners2new_ents.nroots() == old_mesh->nents(ent_dim));
  for (Int i = 0; i < old_mesh->ntags(ent_dim); ++i) {
    auto tag = old_mesh->get_tag(ent_dim, i);
    if (is<I8>(tag)) {
      auto array = to<I8>(tag)->array();
      array = old_owners2new_ents.exch(array, tag->ncomps());
      new_mesh->add_tag<I8>(
          ent_dim, tag->name(), tag->ncomps(), tag->xfer(), array);
    } else if (is<I32>(tag)) {
      auto array = to<I32>(tag)->array();
      array = old_owners2new_ents.exch(array, tag->ncomps());
      new_mesh->add_tag<I32>(
          ent_dim, tag->name(), tag->ncomps(), tag->xfer(), array);
    } else if (is<I64>(tag)) {
      auto array = to<I64>(tag)->array();
      array = old_owners2new_ents.exch(array, tag->ncomps());
      new_mesh->add_tag<I64>(
          ent_dim, tag->name(), tag->ncomps(), tag->xfer(), array);
    } else if (is<Real>(tag)) {
      auto array = to<Real>(tag)->array();
      array = old_owners2new_ents.exch(array, tag->ncomps());
      new_mesh->add_tag<Real>(
          ent_dim, tag->name(), tag->ncomps(), tag->xfer(), array);
    }
  }
}

void push_ents(Mesh* old_mesh, Mesh* new_mesh, Int ent_dim,
    Dist new_ents2old_owners, Dist old_owners2new_ents, osh_parting mode) {
  push_tags(old_mesh, new_mesh, ent_dim, old_owners2new_ents);
  Read<I32> own_ranks;
  if ((mode == OMEGA_H_GHOSTED) ||
      ((mode == OMEGA_H_VERT_BASED) && (ent_dim == VERT))) {
    auto old_own_ranks = old_mesh->ask_owners(ent_dim).ranks;
    own_ranks = old_owners2new_ents.exch(old_own_ranks, 1);
  }
  auto owners = update_ownership(new_ents2old_owners, own_ranks);
  new_mesh->set_owners(ent_dim, owners);
}

static void print_migrate_stats(CommPtr comm, Dist new_elems2old_owners) {
  auto msgs2ranks = new_elems2old_owners.msgs2ranks();
  auto msgs2content = new_elems2old_owners.msgs2content();
  auto msg_content_size = get_degrees(msgs2content);
  auto msg_content_size_h = HostRead<LO>(msg_content_size);
  auto msgs2ranks_h = HostRead<I32>(msgs2ranks);
  LO nintern = 0;
  for (LO msg = 0; msg < msgs2ranks_h.size(); ++msg) {
    if (msgs2ranks_h[msg] == comm->rank()) {
      nintern = msg_content_size_h[msg];
    }
  }
  auto npulled = msgs2content.last();
  auto nextern = npulled - nintern;
  auto total_pulled = comm->allreduce(GO(npulled), OMEGA_H_SUM);
  auto total_extern = comm->allreduce(GO(nextern), OMEGA_H_SUM);
  if (comm->rank() == 0) {
    std::cout << "migration pulling (" << total_extern << " remote) / ("
              << total_pulled << " total) elements\n";
  }
}

void migrate_mesh(Mesh* old_mesh, Mesh* new_mesh, Dist new_elems2old_owners,
    osh_parting mode, bool verbose) {
  auto comm = old_mesh->comm();
  auto dim = old_mesh->dim();
  if (verbose) print_migrate_stats(comm, new_elems2old_owners);
  Dist new_ents2old_owners = new_elems2old_owners;
  auto old_owners2new_ents = new_ents2old_owners.invert();
  for (Int d = dim; d > VERT; --d) {
    Adj high2low;
    Dist old_low_owners2new_lows;
    push_down(old_mesh, d, d - 1, old_owners2new_ents, high2low,
        old_low_owners2new_lows);
    new_mesh->set_ents(d, high2low);
    new_ents2old_owners = old_owners2new_ents.invert();
    push_ents(
        old_mesh, new_mesh, d, new_ents2old_owners, old_owners2new_ents, mode);
    old_owners2new_ents = old_low_owners2new_lows;
  }
  auto new_verts2old_owners = old_owners2new_ents.invert();
  auto nnew_verts = new_verts2old_owners.nitems();
  new_mesh->set_verts(nnew_verts);
  push_ents(old_mesh, new_mesh, VERT, new_verts2old_owners, old_owners2new_ents,
      mode);
}

void migrate_mesh(Mesh* mesh, Dist new_elems2old_owners, bool verbose) {
  auto new_mesh = mesh->copy_meta();
  migrate_mesh(mesh, &new_mesh, new_elems2old_owners, OMEGA_H_ELEM_BASED, verbose);
  *mesh = new_mesh;
}

void migrate_mesh(Mesh* mesh, Remotes new_elems2old_owners, bool verbose) {
  migrate_mesh(
      mesh, Dist(mesh->comm(), new_elems2old_owners, mesh->nelems()), verbose);
}

}  // end namespace osh
