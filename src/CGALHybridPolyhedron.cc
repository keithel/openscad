// Portions of this file are Copyright 2021 Google LLC, and licensed under GPL2+. See COPYING.
#include "CGALHybridPolyhedron.h"

#include "cgalutils.h"
#include "hash.h"
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/helpers.h>
#include <fstream>
#include <sstream>
#include <stdio.h>


CGALHybridPolyhedron::CGALHybridPolyhedron(const shared_ptr<CGAL_HybridNef>& nef)
{
  assert(nef);
  data = nef;
}

CGALHybridPolyhedron::CGALHybridPolyhedron(const shared_ptr<CGAL_HybridMesh>& mesh)
{
  assert(mesh);
  data = mesh;

}

CGALHybridPolyhedron::CGALHybridPolyhedron(const CGALHybridPolyhedron& other)
{
  *this = other;
}

CGALHybridPolyhedron& CGALHybridPolyhedron::operator=(const CGALHybridPolyhedron& other)
{
  if (auto nef = other.getNefPolyhedron()) {
    data = make_shared<CGAL_HybridNef>(*nef);
  } else if (auto mesh = other.getMesh()) {
    data = make_shared<CGAL_HybridMesh>(*mesh);
  } else {
    assert(!"Bad hybrid polyhedron state");
  }
  return *this;
}

CGALHybridPolyhedron::CGALHybridPolyhedron()
{
  data = make_shared<CGAL_HybridMesh>();
}

std::shared_ptr<CGAL_HybridNef> CGALHybridPolyhedron::getNefPolyhedron() const
{
  auto pp = boost::get<shared_ptr<CGAL_HybridNef>>(&data);
  return pp ? *pp : nullptr;
}

std::shared_ptr<CGAL_HybridMesh> CGALHybridPolyhedron::getMesh() const
{
  auto pp = boost::get<shared_ptr<CGAL_HybridMesh>>(&data);
  return pp ? *pp : nullptr;
}

bool CGALHybridPolyhedron::isEmpty() const
{
  return numFacets() == 0;
}

size_t CGALHybridPolyhedron::numFacets() const
{
  if (auto nef = getNefPolyhedron()) {
    return nef->number_of_facets();
  } else if (auto mesh = getMesh()) {
    return mesh->number_of_faces();
  }
  assert(!"Bad hybrid polyhedron state");
  return 0;
}

size_t CGALHybridPolyhedron::numVertices() const
{
  if (auto nef = getNefPolyhedron()) {
    return nef->number_of_vertices();
  } else if (auto mesh = getMesh()) {
    return mesh->number_of_vertices();
  }
  assert(!"Bad hybrid polyhedron state");
  return 0;
}

bool CGALHybridPolyhedron::isManifold() const
{
  if (auto mesh = getMesh()) {
    // Note: haven't tried mesh->is_valid() but it could be too expensive.
    // TODO: use is_valid_polygon_mesh and remember
    return CGAL::is_closed(*mesh);
  } else if (auto nef = getNefPolyhedron()) {
    return nef->is_simple();
  }
  assert(!"Bad hybrid polyhedron state");
  return false;
}

shared_ptr<const PolySet> CGALHybridPolyhedron::toPolySet() const
{
  if (auto mesh = getMesh()) {
    auto ps = make_shared<PolySet>(3, /* convex */ unknown);
    auto err = CGALUtils::createPolySetFromMesh(*mesh, *ps);
    assert(!err);
    return ps;
  } else if (auto nef = getNefPolyhedron()) {
    auto ps = make_shared<PolySet>(3, /* convex */ unknown);
    auto err = CGALUtils::createPolySetFromNefPolyhedron3(*nef, *ps);
    assert(!err);
    return ps;
  } else {
    assert(!"Bad hybrid polyhedron state");
    return nullptr;
  }
}

void CGALHybridPolyhedron::clear()
{
  data = make_shared<CGAL_HybridMesh>();
}

void CGALHybridPolyhedron::operator+=(CGALHybridPolyhedron& other)
{
  if (canCorefineWith(other)) {
    if (meshBinOp("corefinement mesh union", other, [&](CGAL_HybridMesh& lhs, CGAL_HybridMesh& rhs, CGAL_HybridMesh& out) {
      return CGALUtils::corefineAndComputeUnion(lhs, rhs, out);
    })) return;
  }

  nefPolyBinOp("nef union", other,
               [&](CGAL_HybridNef& destinationNef, CGAL_HybridNef& otherNef) {
    CGALUtils::inPlaceNefUnion(destinationNef, otherNef);
  });
}

void CGALHybridPolyhedron::operator*=(CGALHybridPolyhedron& other)
{
  if (canCorefineWith(other)) {
    if (meshBinOp("corefinement mesh intersection", other,
                  [&](CGAL_HybridMesh& lhs, CGAL_HybridMesh& rhs, CGAL_HybridMesh& out) {
      return CGALUtils::corefineAndComputeIntersection(lhs, rhs, out);
    })) return;
  }

  nefPolyBinOp("nef intersection", other,
               [&](CGAL_HybridNef& destinationNef, CGAL_HybridNef& otherNef) {
    CGALUtils::inPlaceNefIntersection(destinationNef, otherNef);
  });
}

void CGALHybridPolyhedron::operator-=(CGALHybridPolyhedron& other)
{
  if (canCorefineWith(other)) {
    if (meshBinOp("corefinement mesh difference", other,
                  [&](CGAL_HybridMesh& lhs, CGAL_HybridMesh& rhs, CGAL_HybridMesh& out) {
      return CGALUtils::corefineAndComputeDifference(lhs, rhs, out);
    })) return;
  }

  nefPolyBinOp("nef difference", other,
               [&](CGAL_HybridNef& destinationNef, CGAL_HybridNef& otherNef) {
    CGALUtils::inPlaceNefDifference(destinationNef, otherNef);
  });
}

bool CGALHybridPolyhedron::canCorefineWith(const CGALHybridPolyhedron& other) const
{
  if (Feature::ExperimentalFastCsgTrustCorefinement.is_enabled()) {
    return true;
  }
  const char *reasonWontCorefine = nullptr;
  if (sharesAnyVertexWith(other)) {
    reasonWontCorefine = "operands share some vertices";
  } else if (!isManifold() || !other.isManifold()) {
    reasonWontCorefine = "non manifoldness detected";
  }
  if (reasonWontCorefine) {
    LOG(message_group::None, Location::NONE, "",
        "[fast-csg] Performing safer but slower nef operation instead of corefinement because %1$s. "
        "(can override with fast-csg-trust-corefinement)",
        reasonWontCorefine);
  }
  return !reasonWontCorefine;
}

void CGALHybridPolyhedron::minkowski(CGALHybridPolyhedron& other)
{
  nefPolyBinOp("minkowski", other,
               [&](CGAL_HybridNef& destinationNef, CGAL_HybridNef& otherNef) {
    CGALUtils::inPlaceNefMinkowski(destinationNef, otherNef);
  });
}

void CGALHybridPolyhedron::transform(const Transform3d& mat)
{
  if (mat.matrix().determinant() == 0) {
    LOG(message_group::Warning, Location::NONE, "", "Scaling a 3D object with 0 - removing object");
    clear();
  } else {
    auto t = CGALUtils::createAffineTransformFromMatrix<CGAL_HybridKernel3>(mat);

    if (auto mesh = getMesh()) {
      CGALUtils::transform(*mesh, mat);
      CGALUtils::cleanupMesh(*mesh, /* is_corefinement_result */ false);
    } else if (auto nef = getNefPolyhedron()) {
      CGALUtils::transform(*nef, mat);
    } else {
      assert(!"Bad hybrid polyhedron state");
    }
  }
}

void CGALHybridPolyhedron::resize(
  const Vector3d& newsize, const Eigen::Matrix<bool, 3, 1>& autosize)
{
  if (this->isEmpty()) return;

  transform(
    CGALUtils::computeResizeTransform(getExactBoundingBox(), getDimension(), newsize, autosize));
}

CGALHybridPolyhedron::bbox_t CGALHybridPolyhedron::getExactBoundingBox() const
{
  bbox_t result(0, 0, 0, 0, 0, 0);
  std::vector<point_t> points;
  // TODO(ochafik): Optimize this!
  foreachVertexUntilTrue([&](const auto& pt) {
    points.push_back(pt);
    return false;
  });
  if (points.size()) CGAL::bounding_box(points.begin(), points.end());
  return result;
}

std::string CGALHybridPolyhedron::dump() const
{
  assert(!"TODO: implement CGALHybridPolyhedron::dump!");
  return "?";
  // return OpenSCAD::dump_svg(toPolySet());
}

size_t CGALHybridPolyhedron::memsize() const
{
  size_t total = sizeof(CGALHybridPolyhedron);
  if (auto mesh = getMesh()) {
    total += numFacets() * 3 * sizeof(size_t);
    total += numVertices() * sizeof(point_t);
  } else if (auto nef = getNefPolyhedron()) {
    total += nef->bytes();
  }
  return total;
}

void CGALHybridPolyhedron::foreachVertexUntilTrue(
  const std::function<bool(const point_t& pt)>& f) const
{
  if (auto mesh = getMesh()) {
    for (auto v : mesh->vertices()) {
      if (f(mesh->point(v))) return;
    }
  } else if (auto nef = getNefPolyhedron()) {
    CGAL_HybridNef::Vertex_const_iterator vi;
    CGAL_forall_vertices(vi, *nef)
    {
      if (f(vi->point())) return;
    }
  } else {
    assert(!"Bad hybrid polyhedron state");
  }
}

std::string describeForDebug(const CGAL_HybridNef& nef)
{
  std::ostringstream stream;
  stream
  // << (nef.is_valid() ? "valid " : "INVALID ")
    << (nef.is_simple() ? "" : "NOT 2-manifold ")
    << nef.number_of_facets() << " facets"
  ;
  return stream.str();
}

std::string describeForDebug(const CGAL_HybridMesh& mesh) {
  std::ostringstream stream;
  stream
    << (CGAL::is_valid_polygon_mesh(mesh) ? "" : "INVALID ")
    << (CGAL::is_closed(mesh) ? "" : "UNCLOSED ")
    << mesh.number_of_faces() << " facets";
  return stream.str();
}

void CGALHybridPolyhedron::nefPolyBinOp(
  const std::string& opName, CGALHybridPolyhedron& other,
  const std::function<void(CGAL_HybridNef& destinationNef, CGAL_HybridNef& otherNef)>
  & operation)
{
  auto& lhs = *convertToNef();
  auto& rhs = *other.convertToNef();

  if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
    LOG(message_group::None, Location::NONE, "",
        "[fast-csg] %1$s: %2$s vs. %3$s",
        opName.c_str(), describeForDebug(lhs), describeForDebug(rhs));
  }

  operation(lhs, rhs);

  if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
    if (!lhs.is_simple()) {
      LOG(message_group::Warning, Location::NONE, "",
          "[fast-csg] %1$s output is a %2$s", opName.c_str(), describeForDebug(lhs));
    }
  }
}

bool CGALHybridPolyhedron::meshBinOp(
  const std::string& opName, CGALHybridPolyhedron& other,
  const std::function<bool(CGAL_HybridMesh& lhs, CGAL_HybridMesh& rhs, CGAL_HybridMesh& out)>& operation)
{
  auto previousData = data;
  auto previousOtherData = other.data;

  auto success = false;

  std::string lhsDebugDumpFile, rhsDebugDumpFile;

  try {
    auto& lhs = *convertToMesh();
    auto& rhs = *other.convertToMesh();

    size_t opNumber = 0;

    if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
      LOG(message_group::None, Location::NONE, "",
          "[fast-csg] %1$s #%2$lu: %3$s vs. %4$s",
          opName.c_str(), opNumber, describeForDebug(lhs), describeForDebug(rhs));

      static std::map<std::string, size_t> opCount;
      opNumber = opCount[opName]++;

      std::ostringstream lhsOut, rhsOut;
      lhsOut << opName << " " << opNumber << " lhs.off";
      rhsOut << opName << " " << opNumber << " rhs.off";
      lhsDebugDumpFile = lhsOut.str();
      rhsDebugDumpFile = rhsOut.str();

      std::ofstream(lhsDebugDumpFile) << lhs;
      std::ofstream(rhsDebugDumpFile) << rhs;
    }

    if ((success = operation(lhs, rhs, lhs))) {
      CGALUtils::cleanupMesh(lhs, /* is_corefinement_result */ true);

      if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
        remove(lhsDebugDumpFile.c_str());
        remove(rhsDebugDumpFile.c_str());
      }
    } else {
      LOG(message_group::Warning, Location::NONE, "", "[fast-csg] Corefinement %1$s failed",
          opName.c_str());
    }
    if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
      if (!CGAL::is_valid_polygon_mesh(lhs) || !CGAL::is_closed(lhs)) {
        LOG(message_group::Warning, Location::NONE, "",
            "[fast-csg] %1$s output is %2$s", opName.c_str(), describeForDebug(lhs));
      }
    }
  } catch (const std::exception& e) {
    // This can be a CGAL::Failure_exception, a CGAL::Intersection_of_constraints_exception or who
    // knows what else...
    success = false;
    LOG(message_group::Warning, Location::NONE, "",
        "[fast-csg] Corefinement %1$s failed with an error: %2$s\n", opName.c_str(), e.what());
    if (Feature::ExperimentalFastCsgDebug.is_enabled()) {
      LOG(message_group::Warning, Location::NONE, "",
          "Dumps of operands were written to %1$s and %2$s", lhsDebugDumpFile.c_str(), rhsDebugDumpFile.c_str());
    }
  }

  if (!success) {
    // Nef polyhedron is a costly object to create, and maybe we've just ditched some
    // to create our polyhedra. Revert back to whatever we had in case we already
    // had nefs.
    data = previousData;
    other.data = previousOtherData;
  }

  return success;
}

std::shared_ptr<CGAL_HybridNef> CGALHybridPolyhedron::convertToNef()
{
  if (auto mesh = getMesh()) {
    auto nef = make_shared<CGAL_HybridNef>(*mesh);
    data = nef;
    return nef;
  } else if (auto nef = getNefPolyhedron()) {
    return nef;
  } else {
    throw "Bad data state";
  }
}

std::shared_ptr<CGAL_HybridMesh> CGALHybridPolyhedron::convertToMesh()
{
  if (auto mesh = getMesh()) {
    return mesh;
  } else if (auto nef = getNefPolyhedron()) {
    auto mesh = make_shared<CGAL_HybridMesh>();
    CGALUtils::convertNefPolyhedronToTriangleMesh(*nef, *mesh);
    CGALUtils::cleanupMesh(*mesh, /* is_corefinement_result */ false);
    data = mesh;
    return mesh;
  } else {
    throw "Bad data state";
  }
}

bool CGALHybridPolyhedron::sharesAnyVertexWith(const CGALHybridPolyhedron& other) const
{
  if (other.numVertices() < numVertices()) {
    // The other has less vertices to index!
    return other.sharesAnyVertexWith(*this);
  }

  std::unordered_set<point_t> vertices;
  foreachVertexUntilTrue([&](const auto& p) {
    vertices.insert(p);
    return false;
  });

  auto foundCollision = false;
  other.foreachVertexUntilTrue(
    [&](const auto& p) {
    return foundCollision = vertices.find(p) != vertices.end();
  });

  return foundCollision;
}