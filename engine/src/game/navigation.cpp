#include "game/navigation.h"

#include <cmath>

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include "core/log.h"

namespace game {

struct Navigation::Impl {
  dtNavMesh* mesh = nullptr;
  dtNavMeshQuery* query = nullptr;
  int polys = 0;
  ~Impl() {
    dtFreeNavMeshQuery(query);
    dtFreeNavMesh(mesh);
  }
};

Navigation::Navigation() : p_(std::make_unique<Impl>()) {}
Navigation::~Navigation() = default;
bool Navigation::ok() const { return p_->query != nullptr; }
int Navigation::polygonCount() const { return p_->polys; }

bool Navigation::build(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices,
                       const NavSettings& s) {
  if (vertices.empty() || indices.size() < 3) return false;
  const float* verts = &vertices[0].x;
  const int nverts = int(vertices.size()), ntris = int(indices.size() / 3);
  std::vector<int> tris(indices.begin(), indices.end());

  // Mesmo roteiro do exemplo "solo mesh" do Recast.
  rcConfig cfg{};
  cfg.cs = s.cellSize;
  cfg.ch = s.cellHeight;
  cfg.walkableSlopeAngle = s.agentMaxSlopeDeg;
  cfg.walkableHeight = int(std::ceil(s.agentHeight / cfg.ch));
  cfg.walkableClimb = int(std::floor(s.agentClimb / cfg.ch));
  cfg.walkableRadius = int(std::ceil(s.agentRadius / cfg.cs));
  cfg.maxEdgeLen = int(12.0f / cfg.cs);
  cfg.maxSimplificationError = 1.3f;
  cfg.minRegionArea = 8 * 8;
  cfg.mergeRegionArea = 20 * 20;
  cfg.maxVertsPerPoly = 6;
  cfg.detailSampleDist = 6.0f * cfg.cs;
  cfg.detailSampleMaxError = cfg.ch;
  rcCalcBounds(verts, nverts, cfg.bmin, cfg.bmax);
  rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

  rcContext ctx(false);
  rcHeightfield* solid = rcAllocHeightfield();
  rcCompactHeightfield* chf = rcAllocCompactHeightfield();
  rcContourSet* cset = rcAllocContourSet();
  rcPolyMesh* pmesh = rcAllocPolyMesh();
  rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
  bool ok = rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
  std::vector<unsigned char> areas(size_t(ntris), 0);
  if (ok) {
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris.data(), ntris, areas.data());
    ok = rcRasterizeTriangles(&ctx, verts, nverts, tris.data(), areas.data(), ntris, *solid, cfg.walkableClimb);
  }
  if (ok) {
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
    ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf) &&
         rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) && rcBuildDistanceField(&ctx, *chf) &&
         rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea) &&
         rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset) &&
         rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) &&
         rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
  }
  unsigned char* navData = nullptr;
  int navDataSize = 0;
  if (ok && pmesh->npolys > 0) {
    for (int i = 0; i < pmesh->npolys; ++i) pmesh->flags[i] = pmesh->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.agentClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    ok = dtCreateNavMeshData(&params, &navData, &navDataSize);
  } else {
    ok = false;
  }
  p_->polys = ok ? pmesh->npolys : 0;
  rcFreeHeightField(solid);
  rcFreeCompactHeightfield(chf);
  rcFreeContourSet(cset);
  rcFreePolyMesh(pmesh);
  rcFreePolyMeshDetail(dmesh);
  if (!ok) {
    LOG_ERROR("Navegação: não consegui gerar a malha (%d triângulos)", ntris);
    return false;
  }
  p_->mesh = dtAllocNavMesh();
  if (dtStatusFailed(p_->mesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
    dtFree(navData);
    return false;
  }
  p_->query = dtAllocNavMeshQuery();
  if (dtStatusFailed(p_->query->init(p_->mesh, 4096))) return false;
  LOG_INFO("Navegação: %d polígonos andáveis de %d triângulos (agente %.1f m x %.2f m)", p_->polys, ntris,
           s.agentHeight, s.agentRadius);
  return true;
}

bool Navigation::findPath(glm::vec3 start, glm::vec3 end, std::vector<glm::vec3>& path) const {
  path.clear();
  if (!p_->query) return false;
  dtQueryFilter filter;
  filter.setIncludeFlags(0xffff);
  filter.setExcludeFlags(0);
  const float ext[3] = {2.0f, 4.0f, 2.0f};
  dtPolyRef startRef = 0, endRef = 0;
  float sp[3], ep[3];
  p_->query->findNearestPoly(&start.x, ext, &filter, &startRef, sp);
  p_->query->findNearestPoly(&end.x, ext, &filter, &endRef, ep);
  if (!startRef || !endRef) return false;
  dtPolyRef polys[512];
  int npolys = 0;
  p_->query->findPath(startRef, endRef, sp, ep, &filter, polys, &npolys, 512);
  if (npolys == 0) return false;
  float target[3] = {ep[0], ep[1], ep[2]};
  if (polys[npolys - 1] != endRef)  // destino inalcançável: vai até o ponto mais perto dele
    p_->query->closestPointOnPoly(polys[npolys - 1], ep, target, nullptr);
  float straight[512 * 3];
  int nstraight = 0;
  p_->query->findStraightPath(sp, target, polys, npolys, straight, nullptr, nullptr, &nstraight, 512);
  for (int i = 0; i < nstraight; ++i) path.emplace_back(straight[3 * i], straight[3 * i + 1], straight[3 * i + 2]);
  return !path.empty();
}

}  // namespace game
