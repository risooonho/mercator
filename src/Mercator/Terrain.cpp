// This file may be redistributed and modified only under the terms of
// the GNU General Public License (See COPYING for details).
// Copyright (C) 2003 Alistair Riddoch, Damien McGinnes

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "iround.h"

#include "Terrain.h"

#include "Matrix.h"
#include "Segment.h"
#include "TerrainMod.h"
#include "Shader.h"
#include "Area.h"
#include "Surface.h"

#include <iostream>
#include <algorithm>

#include <cstdio>

namespace Mercator {

const unsigned int Terrain::DEFAULT;
const unsigned int Terrain::SHADED;
constexpr float Terrain::defaultLevel;


Terrain::Terrain(unsigned int options, unsigned int resolution) : m_options(options),
                                                                  m_res(resolution),
                                                                  m_spacing(resolution)
{

}

Terrain::~Terrain()
{
    Segmentstore::const_iterator I = m_segments.begin(); 
    Segmentstore::const_iterator Iend = m_segments.end();
    for (; I != Iend; ++I) {
        auto J = I->second.begin();
        auto Jend = I->second.end();
        for (; J != Jend; ++J) {
            Segment * s = J->second;
            delete s;
        }
    }
}

void Terrain::addShader(const Shader * t, int id)
{
    if (m_shaders.count(id)) {
        std::cerr << "WARNING: duplicate use of shader ID " << id << std::endl;
    }
    
    m_shaders[id] = t;
    
    Segmentstore::const_iterator I = m_segments.begin(); 
    Segmentstore::const_iterator Iend = m_segments.end(); 
    for (; I != Iend; ++I) {
        auto J = I->second.begin();
        auto Jend = I->second.end();
        for (; J != Jend; ++J) {
            Segment *seg=J->second;

            Segment::Surfacestore & sss = seg->getSurfaces();
            sss[id] = t->newSurface(*seg);
        }
    }
}

void Terrain::removeShader(const Shader * t, int id)
{

    m_shaders.erase(m_shaders.find(id));

    // Delete all surfaces for this shader
    Segmentstore::const_iterator I = m_segments.begin();
    Segmentstore::const_iterator Iend = m_segments.end();
    for (; I != Iend; ++I) {
        auto J = I->second.begin();
        auto Jend = I->second.end();
        for (; J != Jend; ++J) {
            Segment *seg=J->second;

            Segment::Surfacestore & sss = seg->getSurfaces();
            auto K = sss.find(id);
            if (K != sss.end()) {
                delete K->second;
                sss.erase(K);
            }
        }
    }
}



void Terrain::addSurfaces(Segment & seg)
{
    Segment::Surfacestore & sss = seg.getSurfaces();
    if (!sss.empty()) {
        std::cerr << "WARNING: Adding surfaces to a terrain segment which has surfaces"
                  << std::endl << std::flush;
        sss.clear();
    }
    
    Shaderstore::const_iterator I = m_shaders.begin();
    Shaderstore::const_iterator Iend = m_shaders.end();
    for (; I != Iend; ++I) {
        // shader doesn't touch this segment, skip
        if (!I->second->checkIntersect(seg)) {
            continue;
        }
        
        sss[I->first] = I->second->newSurface(seg);
    }
}

void Terrain::shadeSurfaces(Segment & seg)
{
    seg.populateSurfaces();
}


float Terrain::get(float x, float z) const
{
    Segment * s = getSegmentAtIndex(posToIndex(x), posToIndex(z));
    if ((s == nullptr) || (!s->isValid())) {
        return Terrain::defaultLevel;
    }
    return s->get(I_ROUND(x) - s->getXRef(), I_ROUND(z) - s->getZRef());
}

bool Terrain::getHeight(float x, float z, float& h) const
{
    Segment * s = getSegmentAtIndex(posToIndex(x), posToIndex(z));
    if ((s == nullptr) || (!s->isValid())) {
        return false;
    }
    s->getHeight(x - s->getXRef(), z - s->getZRef(), h);
    return true;
}

bool Terrain::getHeightAndNormal(float x, float z, float & h,
                                  WFMath::Vector<3> & n) const
{
    Segment * s = getSegmentAtIndex(posToIndex(x), posToIndex(z));
    if ((s == nullptr) || (!s->isValid())) {
        return false;
    }
    s->getHeightAndNormal(x - s->getXRef(), z - s->getZRef(), h, n);
    return true;
}

bool Terrain::getBasePoint(int x, int z, BasePoint& y) const
{
    auto I = m_basePoints.find(x);
    if (I == m_basePoints.end()) {
        return false;
    }
    auto J = I->second.find(z);
    if (J == I->second.end()) {
        return false;
    }
    y = J->second;
    return true;
}

void Terrain::setBasePoint(int x, int z, const BasePoint& y)
{
    m_basePoints[x][z] = y;
    bool pointIsSet[3][3];
    BasePoint existingPoint[3][3];
    for(int i = x - 1, ri = 0; i < x + 2; ++i, ++ri) {
        for(int j = z - 1, rj = 0; j < z + 2; ++j, ++rj) {
            pointIsSet[ri][rj] = getBasePoint(i, j, existingPoint[ri][rj]);
        }
    }
    for(int i = x - 1, ri = 0; i < x + 1; ++i, ++ri) {
        for(int j = z - 1, rj = 0; j < z + 1; ++j, ++rj) {
            Segment * s = getSegmentAtIndex(i, j);
            if (!s) {
                bool complete = pointIsSet[ri][rj] &&
                                pointIsSet[ri + 1][rj + 1] &&
                                pointIsSet[ri + 1][rj] &&
                                pointIsSet[ri][rj + 1];
                if (!complete) {
                    continue;
                }
                s = new Segment(i * m_res, j * m_res, m_res);
                Matrix<2, 2, BasePoint> & cp = s->getControlPoints();
                for(unsigned int k = 0; k < 2; ++k) {
                    for(unsigned int l = 0; l < 2; ++l) {
                        cp(k, l) = existingPoint[ri + k][rj + l];
                    }
                }

                for (auto& entry : m_terrainMods) {
                    const TerrainMod* terrainMod = std::get<0>(entry.second);
                    if (terrainMod->checkIntersects(*s)) {
                        s->updateMod(entry.first, terrainMod);
                    }
                }

                // apply shaders last, after all other data is in place
                if (isShaded()) {
                    addSurfaces(*s);
                }
                
                m_segments[i][j] = s;
                continue;
            }
            s->setCornerPoint(ri ? 0 : 1, rj ? 0 : 1, y);
        }
    }
}

Segment * Terrain::getSegmentAtIndex(int x, int z) const
{
    auto I = m_segments.find(x);
    if (I == m_segments.end()) {
        return nullptr;
    }
    auto J = I->second.find(z);
    if (J == I->second.end()) {
        return nullptr;
    }
    return J->second;
}

void Terrain::processSegments(const WFMath::AxisBox<2>& area,
        const std::function<void(Segment&, int, int)>& func) const
{
    int lx = I_ROUND(std::floor((area.lowCorner()[0]) / m_spacing));
    int lz = I_ROUND(std::floor((area.lowCorner()[1]) / m_spacing));
    int hx = I_ROUND(std::ceil((area.highCorner()[0]) / m_spacing));
    int hz = I_ROUND(std::ceil((area.highCorner()[1]) / m_spacing));

    for (int i = lx; i < hx; ++i) {
        for (int j = lz; j < hz; ++j) {
            Segment *s = getSegmentAtIndex(i, j);
            if (!s) {
                continue;
            }
            func(*s, i, j);
        }
    }
}


Terrain::Rect Terrain::updateMod(long id, const TerrainMod * mod)
{
    std::set<Segment*> removed, added, updated;

    auto I = m_terrainMods.find(id);

    Rect old_box;
    if (I != m_terrainMods.end()) {
        std::tuple<const TerrainMod *, Rect>& entry = I->second;

        old_box = std::get<1>(entry);



        int lx=I_ROUND(std::floor((old_box.lowCorner()[0] - 1.f) / m_spacing));
        int lz=I_ROUND(std::floor((old_box.lowCorner()[1] - 1.f) / m_spacing));
        int hx=I_ROUND(std::ceil((old_box.highCorner()[0] + 1.f) / m_spacing));
        int hz=I_ROUND(std::ceil((old_box.highCorner()[1] + 1.f) / m_spacing));

        for (int i=lx;i<hx;++i) {
           for (int j=lz;j<hz;++j) {
               Segment *s=getSegmentAtIndex(i,j);
               if (!s) {
                   continue;
               }

               removed.insert(s);

           } // of y loop
        } // of x loop

        if (mod) {
            std::get<0>(entry) = mod;
            std::get<1>(entry) = mod->bbox();
        } else {
            m_terrainMods.erase(id);
        }
    } else if (mod) {
        m_terrainMods.emplace(id, std::make_tuple(mod, mod->bbox()));
    }

    if (mod) {
        int lx=I_ROUND(std::floor((mod->bbox().lowCorner()[0] - 1.f) / m_spacing));
        int lz=I_ROUND(std::floor((mod->bbox().lowCorner()[1] - 1.f) / m_spacing));
        int hx=I_ROUND(std::ceil((mod->bbox().highCorner()[0] + 1.f) / m_spacing));
        int hz=I_ROUND(std::ceil((mod->bbox().highCorner()[1] + 1.f) / m_spacing));

        for (int i=lx;i<hx;++i) {
            for (int j=lz;j<hz;++j) {
                Segment *s=getSegmentAtIndex(i,j);
                if (!s) {
                    continue;
                }

                auto J = removed.find(s);
                if (J == removed.end()) {
                    added.insert(s);
                } else {
                    updated.insert(s);
                    removed.erase(J);
                }
            } // of y loop
        } // of x loop
    }

    for (auto& segment : removed) {
        segment->updateMod(id, nullptr);
    }
    for (auto& segment : added) {
        if (mod->checkIntersects(*segment)) {
            segment->updateMod(id, mod);
        }
    }
    for (auto& segment : updated) {
        if (mod->checkIntersects(*segment)) {
            segment->updateMod(id, mod);
        } else {
            segment->updateMod(id, nullptr);
        }
    }

    return old_box;
}

bool Terrain::hasMod(long id) const
{
    return m_terrainMods.find(id) != m_terrainMods.end();
}

const TerrainMod* Terrain::getMod(long id) const
{
    auto I = m_terrainMods.find(id);
    if (I != m_terrainMods.end()) {
        return std::get<0>(I->second);
    }
    return nullptr;
}

void Terrain::addArea(const Area * area)
{
    int layer = area->getLayer();

    Shaderstore::const_iterator I = m_shaders.find(layer);
    if (I != m_shaders.end()) {
        area->setShader(I->second);
    }
    
    //work out which segments are overlapped by this effector
    //note that the bbox is expanded by one grid unit because
    //segments share edges. this ensures a mod along an edge
    //will affect both segments.

    m_terrainAreas.emplace(area, area->bbox());

    int lx=I_ROUND(std::floor((area->bbox().lowCorner()[0] - 1.f) / m_spacing));
    int lz=I_ROUND(std::floor((area->bbox().lowCorner()[1] - 1.f) / m_spacing));
    int hx=I_ROUND(std::ceil((area->bbox().highCorner()[0] + 1.f) / m_spacing));
    int hz=I_ROUND(std::ceil((area->bbox().highCorner()[1] + 1.f) / m_spacing));

    for (int i=lx;i<hx;++i) {
        for (int j=lz;j<hz;++j) {
            Segment *s=getSegmentAtIndex(i,j);
            if (s) {
                if (area->checkIntersects(*s)) {
                    s->addArea(area);
                }
            }
        } // of y loop
    } // of x loop
}

Terrain::Rect Terrain::updateArea(const Area * area)
{
    std::set<Segment*> removed, added, updated;

     auto I = m_terrainAreas.find(area);

     Rect old_box;
     if (I != m_terrainAreas.end()) {

         old_box = I->second;

         int lx=I_ROUND(std::floor((old_box.lowCorner()[0] - 1.f) / m_spacing));
         int lz=I_ROUND(std::floor((old_box.lowCorner()[1] - 1.f) / m_spacing));
         int hx=I_ROUND(std::ceil((old_box.highCorner()[0] + 1.f) / m_spacing));
         int hz=I_ROUND(std::ceil((old_box.highCorner()[1] + 1.f) / m_spacing));

         for (int i=lx;i<hx;++i) {
            for (int j=lz;j<hz;++j) {
                Segment *s=getSegmentAtIndex(i,j);
                if (!s) {
                    continue;
                }

                removed.insert(s);

            } // of y loop
         } // of x loop

         I->second = area->bbox();

     } else {
         m_terrainAreas.emplace(area, area->bbox());
     }



     int lx=I_ROUND(std::floor((area->bbox().lowCorner()[0] - 1.f) / m_spacing));
     int lz=I_ROUND(std::floor((area->bbox().lowCorner()[1] - 1.f) / m_spacing));
     int hx=I_ROUND(std::ceil((area->bbox().highCorner()[0] + 1.f) / m_spacing));
     int hz=I_ROUND(std::ceil((area->bbox().highCorner()[1] + 1.f) / m_spacing));

     for (int i=lx;i<hx;++i) {
         for (int j=lz;j<hz;++j) {
             Segment *s=getSegmentAtIndex(i,j);
             if (!s) {
                 continue;
             }

             auto J = removed.find(s);
             if (J == removed.end()) {
                 added.insert(s);
             } else {
                 updated.insert(s);
                 removed.erase(J);
             }
         } // of y loop
     } // of x loop

     for (auto& segment : removed) {
         segment->removeArea(area);
     }
     for (auto& segment : added) {
         if (area->checkIntersects(*segment)) {
             segment->addArea(area);
         }
     }
     for (auto& segment : updated) {
         if (area->checkIntersects(*segment)) {
             if (segment->updateArea(area) != 0) {
                 segment->addArea(area);
             }
         } else {
             segment->removeArea(area);
         }
     }

     return old_box;
}


void Terrain::removeArea(const Area * area)
{
    m_terrainAreas.erase(area);

    const Rect & eff_box = area->bbox();

    int lx=I_ROUND(std::floor((eff_box.lowCorner()[0] - 1.f) / m_spacing));
    int lz=I_ROUND(std::floor((eff_box.lowCorner()[1] - 1.f) / m_spacing));
    int hx=I_ROUND(std::ceil((eff_box.highCorner()[0] + 1.f) / m_spacing));
    int hz=I_ROUND(std::ceil((eff_box.highCorner()[1] + 1.f) / m_spacing));

    for (int i=lx;i<hx;++i) {
        for (int j=lz;j<hz;++j) {
            Segment *s=getSegmentAtIndex(i,j);
            if (s) {
                s->removeArea(area);
            }
        } // of y loop
    } // of x loop
}

bool Terrain::hasArea(const Area* a) const
{
    return m_terrainAreas.find(a) != m_terrainAreas.end();
}


} // namespace Mercator
