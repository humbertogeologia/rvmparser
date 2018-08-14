#include "RVMParser.h"
#include "RVMVisitor.h"
#include "Store.h"

#include <iostream>
#include <string>
#include <cstdio>
#include <cctype>
#include <vector>

#include <cassert>

namespace {

  struct Context
  {
    Store* store;
    std::vector<Group*> group_stack;
  };


  const char* read_uint32_be(uint32_t& rv, const char* p, const char* e)
  {
    auto * q = reinterpret_cast<const uint8_t*>(p);
    rv = q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    return p + 4;
  }

  const char* read_float32_be(float& rv, const char* p, const char* e)
  {
    union {
      float f;
      uint32_t u;
    };

    auto * q = reinterpret_cast<const uint8_t*>(p);
    u = q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    rv = f;
    return p + 4;
  }

  constexpr uint32_t id(const char* str)
  {
    return str[3] << 24 | str[2] << 16 | str[1] << 8 | str[0];
  }

  const char* read_string(const char** dst, Store* store, const char* p, const char* e)
  {
    uint32_t len;
    p = read_uint32_be(len, p, e);

    unsigned l = 4 * len;
    for (unsigned i = 0; i < l; i++) {
      if (p[i] == 0) {
        l = i;
        break;
      }
    }

    auto * str = (char*)store->alloc(l + 1);
    *dst = str;

    std::memcpy(str, p, l);
    str[l] = '\0';

    return p + 4 * len;
  }

  const char* parse_chunk_header(char* id, uint32_t& len, uint32_t& dunno, const char* p, const char* e)
  {
    unsigned i = 0;
    for (i = 0; i < 4 && p + 4 <= e; i++) {
      assert(p[0] == 0);
      assert(p[1] == 0);
      assert(p[2] == 0);
      id[i] = p[3];
      p += 4;
    }
    for (; i < 4; i++) {
      id[i] = ' ';
    }
    if (p + 8 <= e) {
      p = read_uint32_be(len, p, e);
      p = read_uint32_be(dunno, p, e);
    }
    else {
      len = ~0;
      dunno = ~0;
      fprintf(stderr, "Chunk '%s' EOF after %zd bytes\n", id, e - p);
      p = e;
    }
    return p;
  }


  const char* parse_head(Context* ctx, const char* p, const char* e)
  {
    assert(ctx->group_stack.empty());
    auto * g = ctx->store->newGroup(nullptr, Group::Kind::File);
    ctx->group_stack.push_back(g);

    uint32_t version;
    p = read_uint32_be(version, p, e);
    p = read_string(&g->file.info, ctx->store, p, e);
    p = read_string(&g->file.note, ctx->store, p, e);
    p = read_string(&g->file.date, ctx->store, p, e);
    p = read_string(&g->file.user, ctx->store, p, e);
    if (2 <= version) {
      p = read_string(&g->file.encoding, ctx->store, p, e);
    }
    else {
      auto * encoding = (char*)ctx->store->alloc(1);
      encoding[0] = '\0';
      g->file.encoding = encoding;
    }

    return p;
  }

  const char* parse_modl(Context* ctx, const char* p, const char* e)
  {
    assert(!ctx->group_stack.empty());
    auto * g = ctx->store->newGroup(ctx->group_stack.back(), Group::Kind::Model);
    ctx->group_stack.push_back(g);

    uint32_t version;
    p = read_uint32_be(version, p, e);

    p = read_string(&g->model.project, ctx->store, p, e);
    p = read_string(&g->model.name, ctx->store, p, e);

    return p;
  }

  const char* parse_prim(Context* ctx, const char* p, const char* e)
  {
    assert(!ctx->group_stack.empty());
    assert(ctx->group_stack.back()->kind == Group::Kind::Group);

    uint32_t version, kind;
    p = read_uint32_be(version, p, e);
    p = read_uint32_be(kind, p, e);

    auto * g = ctx->store->newGeometry(ctx->group_stack.back());

    for (unsigned i = 0; i < 12; i++) {
      p = read_float32_be(g->M_3x4[i], p, e);
    }
    for (unsigned i = 0; i < 6; i++) {
      p = read_float32_be(g->bbox[i], p, e);
    }

    switch (kind) {
    case 1:
      g->kind = Geometry::Kind::Pyramid;
      p = read_float32_be(g->pyramid.bottom[0], p, e);
      p = read_float32_be(g->pyramid.bottom[1], p, e);
      p = read_float32_be(g->pyramid.top[0], p, e);
      p = read_float32_be(g->pyramid.top[1], p, e);
      p = read_float32_be(g->pyramid.offset[0], p, e);
      p = read_float32_be(g->pyramid.offset[1], p, e);
      p = read_float32_be(g->pyramid.height, p, e);
      break;

    case 2:
      g->kind = Geometry::Kind::Box;
      p = read_float32_be(g->box.lengths[0], p, e);
      p = read_float32_be(g->box.lengths[1], p, e);
      p = read_float32_be(g->box.lengths[2], p, e);
      break;

    case 3:
      g->kind = Geometry::Kind::RectangularTorus;
      p = read_float32_be(g->rectangularTorus.inner_radius, p, e);
      p = read_float32_be(g->rectangularTorus.outer_radius, p, e);
      p = read_float32_be(g->rectangularTorus.height, p, e);
      p = read_float32_be(g->rectangularTorus.angle, p, e);
      break;

    case 4:
      g->kind = Geometry::Kind::CircularTorus;
      p = read_float32_be(g->circularTorus.offset, p, e);
      p = read_float32_be(g->circularTorus.radius, p, e);
      p = read_float32_be(g->circularTorus.angle, p, e);
      break;

    case 5:
      g->kind = Geometry::Kind::EllipticalDish;
      p = read_float32_be(g->ellipticalDish.diameter, p, e);
      p = read_float32_be(g->ellipticalDish.radius, p, e);
      break;

    case 6:
      g->kind = Geometry::Kind::SphericalDish;
      p = read_float32_be(g->sphericalDish.diameter, p, e);
      p = read_float32_be(g->sphericalDish.height, p, e);
      break;

    case 7:
      g->kind = Geometry::Kind::Snout;
      p = read_float32_be(g->snout.radius_b, p, e);
      p = read_float32_be(g->snout.radius_t, p, e);
      p = read_float32_be(g->snout.height, p, e);
      p = read_float32_be(g->snout.offset[0], p, e);
      p = read_float32_be(g->snout.offset[1], p, e);
      p = read_float32_be(g->snout.bshear[0], p, e);
      p = read_float32_be(g->snout.bshear[1], p, e);
      p = read_float32_be(g->snout.tshear[0], p, e);
      p = read_float32_be(g->snout.tshear[1], p, e);
      break;

    case 8:
      g->kind = Geometry::Kind::Cylinder;
      p = read_float32_be(g->cylinder.radius, p, e);
      p = read_float32_be(g->cylinder.height, p, e);
      break;

    case 9:
      g->kind = Geometry::Kind::Cylinder;
      p = read_float32_be(g->sphere.diameter, p, e);
      break;

    case 10:
      g->kind = Geometry::Kind::Line;
      p = read_float32_be(g->line.a, p, e);
      p = read_float32_be(g->line.b, p, e);
      break;

    case 11:
      g->kind = Geometry::Kind::FacetGroup;

      p = read_uint32_be(g->facetGroup.polygons_n, p, e);
      g->facetGroup.polygons = (Polygon*)ctx->store->alloc(sizeof(Polygon)*g->facetGroup.polygons_n);

      for (unsigned pi = 0; pi < g->facetGroup.polygons_n; pi++) {
        auto & poly = g->facetGroup.polygons[pi];

        p = read_uint32_be(poly.contours_n, p, e);
        poly.coutours = (Contour*)ctx->store->alloc(sizeof(Contour)*poly.contours_n);
        for (unsigned gi = 0; gi < poly.contours_n; gi++) {
          auto & cont = poly.coutours[gi];

          p = read_uint32_be(cont.vertices_n, p, e);
          cont.vertices = (float*)ctx->store->alloc(3 * sizeof(float)*cont.vertices_n);
          cont.normals = (float*)ctx->store->alloc(3 * sizeof(float)*cont.vertices_n);

          for (unsigned vi = 0; vi < cont.vertices_n; vi++) {
            for (unsigned i = 0; i < 3; i++) {
              p = read_float32_be(cont.vertices[3 * vi + i], p, e);
            }
            for (unsigned i = 0; i < 3; i++) {
              p = read_float32_be(cont.normals[3 * vi + i], p, e);
            }
          }
        }
      }
      break;

    default:
      fprintf(stderr, "Unknown primitive kind %d\n", kind);
      assert(false);
      break;
    }
    return p;
  }

  const char* parse_cntb(Context* ctx, const char* p, const char* e)
  {
    assert(!ctx->group_stack.empty());
    auto * g = ctx->store->newGroup(ctx->group_stack.back(), Group::Kind::Group);
    ctx->group_stack.push_back(g);

    uint32_t version;
    p = read_uint32_be(version, p, e);
    p = read_string(&g->group.name, ctx->store, p, e);

    // Translation seems to be a reference point that can be used as a local frame for objects in the group.
    // The transform is not relative to this reference point.
    for (unsigned i = 0; i < 3; i++) {
      p = read_float32_be(g->group.translation[i], p, e);
      g->group.translation[i] *= 0.001f;
    }

    p = read_uint32_be(g->group.material, p, e);

    // process children
    char chunk_id[5] = { 0, 0, 0, 0, 0 };
    auto l = p;
    uint32_t len, dunno;
    p = parse_chunk_header(chunk_id, len, dunno, p, e);
    auto id_chunk_id = id(chunk_id);
    while (p < e && id_chunk_id != id("CNTE")) {
      switch (id_chunk_id) {
      case id("CNTB"):
        p = parse_cntb(ctx, p, e);
        break;
      case id("PRIM"):
        p = parse_prim(ctx, p, e);
        break;
      default:
        fprintf(stderr, "Unknown chunk id '%s", chunk_id);
        assert(false);
      }
      l = p;
      p = parse_chunk_header(chunk_id, len, dunno, p, e);
      id_chunk_id = id(chunk_id);
    }

    if (id_chunk_id == id("CNTE")) {
      uint32_t version;
      p = read_uint32_be(version, p, e);
    }

    ctx->group_stack.pop_back();

    //ctx->v->EndGroup();
    return p;
  }

}

void parseRVM(class Store* store, const void * ptr, size_t size)
{
  Context ctx = { store };

  auto * p = reinterpret_cast<const char*>(ptr);
  auto * e = p + size;
  uint32_t len, dunno;

  auto l = p;

  char chunk_id[5] = { 0, 0, 0, 0, 0 };
  p = parse_chunk_header(chunk_id, len, dunno, p, e);
  assert(id(chunk_id) == id("HEAD"));
  p = parse_head(&ctx, p, e);
  assert(p - l == len);

  l = p;
  p = parse_chunk_header(chunk_id, len, dunno, p, e);
  assert(id(chunk_id) == id("MODL"));
  p = parse_modl(&ctx, p, e);

  l = p;
  p = parse_chunk_header(chunk_id, len, dunno, p, e);
  auto id_chunk_id = id(chunk_id);
  while (p < e && id_chunk_id != id("END:")) {
    switch (id_chunk_id) {
    case id("CNTB"):
      p = parse_cntb(&ctx, p, e);
      break;
    case id("PRIM"):
      p = parse_prim(&ctx, p, e);
      break;
    default:
      fprintf(stderr, "Unknown chunk id '%s", chunk_id);
      assert(false);
    }
    l = p;
    p = parse_chunk_header(chunk_id, len, dunno, p, e);
    id_chunk_id = id(chunk_id);
  }

  assert(ctx.group_stack.size() == 2);
  ctx.group_stack.pop_back();
  ctx.group_stack.pop_back();

  //v->endModel();
  //v->endFile();

}


