/**
 * featurelayers.cpp
 */

#include "./featurelayers.hpp"
#include "./verticaladjuster.hpp"

#include "utility/expect.hpp"
#include "math/filters.hpp"

#include <limits>

#include <ogrsf_frmts.h>

namespace geo {
    
namespace ut = utility;

/** class FeatureLayers */

void FeatureLayers::load(::GDALDataset &dataset
    , const boost::optional<SrsDefinition> & sourceSrs) {
    
    // initialize
    layers.resize(0);
    
    // cycle through layers
    for (int i = 0; i < dataset.GetLayerCount(); i++) {
        
        using boost::format;
        
        // obtain layer
        OGRLayer * ilayer = dataset.GetLayer(i);
        
        // extract srs
        SrsDefinition srcSrs;

        if (sourceSrs) {
            srcSrs = sourceSrs.get();
        } else {
        
            OGRSpatialReference * ogrsr = ilayer->GetSpatialRef();
            ut::expect(ogrsr, "No srs provided in input dataset, and no "
                "override given.");
        
            char * eogrsr;
            ut::expect(ogrsr->exportToProj4(& eogrsr) == OGRERR_NONE,
                "Failed to convert input layer srs to proj4.");
            
            srcSrs = SrsDefinition( eogrsr, SrsDefinition::Type::proj4);
        } 
        
        LOG( info1 ) 
            << "Layer " << i << " srs: \""  << srcSrs.string() << "\".";

        // initialize layer
        layers.push_back({ilayer->GetName(), srcSrs});
           
        auto & layer(layers.back());
        uint unsupported(0);
        
        // cycle through features
        OGRFeature * ifeature;
        ilayer->ResetReading();
        uint id = 0;
        
        //OGRFeatureDefn *ilayerDefn = ilayer->GetLayerDefn();
        
        while ((ifeature = ilayer->GetNextFeature()) != 0x0) {
            
            bool zDefined;
            
            // extract properties
            Features::Properties properties;
            
            OGRFeatureDefn *ifeatureDefn = ifeature->GetDefnRef();
            
            for (int j = 0; j < ifeatureDefn->GetFieldCount(); j++)
                properties[ifeatureDefn->GetFieldDefn(j)->GetNameRef()] 
                    = ifeature->GetFieldAsString(j);
        
            // extract geometry
            OGRGeometry *igeometry = ifeature->GetGeometryRef();
           
            if ( igeometry == 0x0 ) {
                LOG(warn2) << "Geometry-less feature encountered, skipping.";
                continue;
            }
           
            if (igeometry->getCoordinateDimension() != 2 
                && igeometry->getCoordinateDimension() != 3) {
               
                LOG(warn2) << "Unknown feature dimension, skipping.";
                continue;
            } 
           
            zDefined = (igeometry->getCoordinateDimension() == 3 );
           
            /* point */
            if (wkbFlatten(igeometry->getGeometryType()) == wkbPoint ) {
               
                OGRPoint * ipoint = (OGRPoint *) igeometry;
                
                math::Point3 point(
                    ipoint->getX(), ipoint->getY(), ipoint->getZ());
                layer.features.addPoint( 
                        {(format("%s-%d") % layer.name % id++ ).str(), 
                        point, properties, zDefined});
                    
                layer.updateBB(layer.features.points.back().point);
                continue;
            }
           
            /* line string */
            if (wkbFlatten(igeometry->getGeometryType()) == wkbLineString) {
               
                OGRLineString * ilinestring = (OGRLineString *) igeometry;
                math::Points3 points;
                                
                for (int k = 0; k < ilinestring->getNumPoints(); k++) {
                    
                    OGRPoint ipoint;
                    ilinestring->getPoint(k, & ipoint);
                    points.push_back(math::Point3(ipoint.getX()
                        , ipoint.getY(), ipoint.getZ()));
                    layer.updateBB(points.back());
                }
                
                layer.features.addMultiLineString(
                        {(format("%s-%d") % layer.name % id++ ).str(), 
                            { points }, properties, zDefined});
                
                continue;
            }
           
            /* multi line string */
            if (wkbFlatten(igeometry->getGeometryType()) == wkbMultiLineString) {
               
                auto imultilinestring = static_cast<OGRMultiLineString *> 
                    (igeometry);
                std::vector<math::Points3> lines;
                
                for (int j = 0; j < imultilinestring->getNumGeometries(); j++) {
                    
                    lines.push_back(math::Points3());
                    auto & points( lines.back());
                    
                    ut::expect(wkbFlatten
                        (imultilinestring->getGeometryRef(j)->getGeometryType())
                        == wkbLineString, "Malformed multilinestring?");
                    
                    auto ilinestring = static_cast<OGRLineString *>
                        (imultilinestring->getGeometryRef(j));
                        
                    for (int k = 0; k < ilinestring->getNumPoints(); k++) {
                    
                        OGRPoint ipoint;
                        ilinestring->getPoint(k, & ipoint);
                        points.push_back(math::Point3(ipoint.getX()
                            , ipoint.getY(), ipoint.getZ()));
                        layer.updateBB(points.back());
                    }
                }
                
                layer.features.addMultiLineString(
                        {(format("%s-%d") % layer.name % id++ ).str(), 
                            lines, properties, zDefined});
                
                continue;
            }
                   
            /* polygon */        
            if (wkbFlatten(igeometry->getGeometryType()) == wkbPolygon ) {
               
                OGRPolygon * ipolygon = (OGRPolygon *) igeometry;
                Features::MultiPolygon::Polygon polygon;
                
                ipolygon->closeRings(); // probably unnecessary

                OGRLinearRing * extring = ipolygon->getExteriorRing();
                                                
                for (int k = 0; k < extring->getNumPoints() - 1; k++) {
                    
                    OGRPoint ipoint;
                    extring->getPoint(k, &ipoint);
                    polygon.exterior.push_back(math::Point3(
                        ipoint.getX(), ipoint.getY(), ipoint.getZ()));
                    layer.updateBB(polygon.exterior.back());                    
                }
                
                for (int k = 0; k < ipolygon->getNumInteriorRings(); k++) {
                    
                    OGRLinearRing *intring = ipolygon->getInteriorRing(k);
                    polygon.interiors.push_back({});
                  
                    for ( int l = 0; l < intring->getNumPoints() -1; l++ ) {
                        OGRPoint ipoint;
                        intring->getPoint(l, &ipoint);
                        polygon.interiors.back().push_back(
                            math::Point3(
                                ipoint.getX(), ipoint.getY(), ipoint.getZ()));                                            
                    }
                }
                
                layer.features.addMultiPolygon({
                    (format( "%s-%d" ) % layer.name % id++ ).str(),
                    properties, { polygon }, zDefined });
                continue;
            }
               
            /* unknown */        
            LOG(warn2) ("Unsupported feature type 0x%X, skipping.", 
                igeometry->getGeometryType());
            unsupported++;
        }
        
        // end layer
        if (!unsupported) {
            LOG(info2) << format( 
                "%s: %5d points, %5d (multi)linestrings, %5d (multi)polygons." ) 
                % layer.name
                % layer.features.points.size()
                % layer.features.multilinestrings.size()
                % layer.features.multipolygons.size();
        } else {
            LOG(info2) << format( 
                "%s: %5d points, %5d (multi)linestrings, %5d (multi)polygons, "
                "%5d unsupported features." ) 
                % layer.name
                % layer.features.points.size()
                % layer.features.multilinestrings.size()
                % layer.features.multipolygons.size()
                % unsupported;            
        }
    }
        
    // all done
}

void FeatureLayers::transform(const SrsDefinition & targetSrs) {
    
    
    // for each layer
    for (auto &layer: layers) {
    
        SrsDefinition sourceSrs(layer.srs);
        
        // optimization
        if (areSame(sourceSrs,targetSrs)) continue;
        
        // sanity checks
        if ( targetSrs.reference().IsGeocentric() && !layer.is3D()) {
            LOGTHROW( err2, std::runtime_error ) << "Transformation to "
                "geocentric SRS requested, but not all features are 3D. "
                "Need heightcoding?";
        }
        
        if (!areSame(sourceSrs, targetSrs, SrsEquivalence::geographic)
            && !layer.is2D()) {
            LOG( warn2 ) << "Source and target SRS have different datums "
                " and not all features are 3D. Need heightcoding?";
        }
        
        // create converter object
        CsConvertor csTrafo(sourceSrs, targetSrs);
        
        // transform features
        layer.featuresBB = boost::none;
        
        for (auto &point: layer.features.points) {
            point.point = csTrafo(point.point); layer.updateBB(point.point);
        }
        
        for (auto &multilinestring: layer.features.multilinestrings)
            for (auto &linestring: multilinestring.lines)
                for (auto &p: linestring) { p = csTrafo(p); layer.updateBB(p); }
        
        for (auto &mp: layer.features.multipolygons)
            for (auto &polygon: mp.polygons) {
                for (auto &p: polygon.exterior) {
                    p = csTrafo(p); layer.updateBB(p);
                }
                
                for (auto &interior: polygon.interiors)
                    for (auto &p: interior) p = csTrafo(p);
            }
        
        for (auto &s: layer.features.surfaces)
            for (auto &v: s.vertices) {
                v = csTrafo(v); layer.updateBB(v);
            }
            
        // done with layer
        layer.srs = targetSrs;
    }
    
    
}

void FeatureLayers::heightcode(const GeoDataset & demDataset
        , boost::optional<SrsDefinition> workingSrs
        , bool verticalAdjustment
        , HeightcodeMode mode ) {
    
    // establish working srs if not given
    if (!workingSrs) {
        
        // obtain the geographic datum from dem
        /*OGRSpatialReference *ogrsrs = demDataset.srs().reference().CloneGeogCS();        
        workingSrs = SrsDefinition::fromReference(*ogrsrs);*/
        workingSrs = demDataset.srs();        
        // delete ogrsrs;
    }
    
    LOG(info1) 
        << "Heightcoding SRS: \"" 
        << workingSrs->as(SrsDefinition::Type::proj4).string() << "\"";
    
    // determine extents and pixel size
    boost::optional<math::Extents3> bb3 = boundingBox(workingSrs);
    
    if (!bb3) {
        LOG(info2) << "Skipping heightcoding for an empty features dataset.";
        return;
    }

    math::Extents3 bb;
    
    bb.ll = bb3->ll - 0.05 * (bb3->ur - bb3->ll);
    bb.ur = bb3->ur + 0.05 * (bb3->ur - bb3->ll);
    
    math::Size2f size(math::size(bb).width
                    , math::size(bb).height);
    
    math::Extents2 bb2( 
        subrange(bb.ll, 0, 2), 
        subrange(bb.ur, 0, 2));

    math::Size2i psize(
        std::min(1024.0, 4 * size.width / demDataset.resolution()[0]), 
        std::min(1024.0, 4 * size.height / demDataset.resolution()[1]));
    
    // warp dem into working srs
    // demDataset.expectGray();
    
    auto wdem = geo::GeoDataset::deriveInMemory(demDataset, 
        workingSrs.get(), psize, bb2);
    
    demDataset.warpInto(wdem, geo::GeoDataset::Resampling::dem);

    LOG(info2) ("Warped DEM to %dx%d pixels at [%.1f,%.1f] resolution."
        , wdem.size().width, wdem.size().height 
        , wdem.resolution()[0], wdem.resolution()[1]);
    
    // heightcode 
    math::CatmullRom2 filter(2,2);
    
    for (auto & layer: layers) {
                
        if (mode == HeightcodeMode::auto_ 
            && layer.features.zAlwaysDefined ) continue;

        CsConvertor ltwTrafo(layer.srs, workingSrs.get());
        CsConvertor wtlTrafo = ltwTrafo.inverse();
        VerticalAdjuster adjuster(workingSrs.get());
        
        for (auto & point: layer.features.points) {
            
            if (point.zDefined && mode == HeightcodeMode::auto_ )
                continue;
 
            // layer srs -> working srs            
            auto p(point.point);
            if (layer.adjustVertical) p = adjuster(p, true);
            p = ltwTrafo(p);

            // z value
            auto value(
                reconstruct(wdem.cdata()
                          , wdem.mask()
                          , subrange( 
                              wdem.geoTransform().iconvert<double>(p)
                              , 0, 2)
                          , filter ) );
            ut::expect(bool(value), "Could not obtain DEM value.");
            p(2) = value.get(); 

            // working srs -> layer srs
            point.point = wtlTrafo(p);
            if (verticalAdjustment) point.point = adjuster(point.point);

            point.zDefined = true;
            layer.features.zNeverDefined = false;
        }
        
        for ( auto & multilinestring : layer.features.multilinestrings ) {
            
            if ( multilinestring.zDefined && mode == HeightcodeMode::auto_ )
                continue;

            for (auto & linestring: multilinestring.lines) 
                for (math::Point3 & point : linestring) {
                
                    // layer srs -> working srs
                    auto p(point);
                    if (layer.adjustVertical) p = adjuster(p, true);
                    p = ltwTrafo(p);

                    // z value
                    auto value(reconstruct(wdem.data()
                        , wdem.mask()
                        , subrange(
                            wdem.geoTransform().iconvert<double>(p), 0, 2)
                        , filter));
                    ut::expect(bool(value), "Could not obtain DEM value.");
                    p(2) = value.get(); 
                
                    // working srs -> layer srs
                    point = wtlTrafo(p);
                    if (verticalAdjustment) point = adjuster(point);
                }
            
            multilinestring.zDefined = true;
            layer.features.zNeverDefined = false;
        }
        
        // heightcode polygons
        for (auto & multipolygon: layer.features.multipolygons) {
            
            if (!multipolygon.zDefined) {
            
                LOGONCE(warn3) << "2D polygon heightcoding not implemented. "
                    "You may complain to the management.";
            }
        }
        
        // done with layer
        layer.features.zAlwaysDefined = true;
        layer.adjustVertical = verticalAdjustment;
        
    } // loop layers    
}

void FeatureLayers::convert3DPolygons() {
    
    // for each layer
    for (auto &layer: layers) {
        
        uint residual(layer.features.multipolygons.size());
        
        // iterate through polygons
        auto cur = layer.features.multipolygons.begin();
        auto last = layer.features.multipolygons.end() - 1;
        
        for (uint i = 0; i < layer.features.multipolygons.size();i++) {
            
            auto & multipolygon(*cur);

            // skip 2D polygons, these need converted through heightcoding
            if (!multipolygon.zDefined) { cur++; continue; };
            
            // build surface from polygon
            Features::Surface surface(multipolygon.id, multipolygon.properties);
            
            for (const auto & polygon : multipolygon.polygons)
                surface.addPatchesFromPolygon(polygon);

            // save surface
            layer.features.surfaces.push_back(surface);
            
            // mark polygon for removal
            std::swap(*cur, *last);
            residual--;
        }
        
        // remove converted polygons
        layer.features.multipolygons.resize(residual);
    }
}    

void FeatureLayers::dumpLegacyGeodata(std::ostream & os
            , const std::string & pointStyle
            , const std::string & linestringStyle) {

    // build json value
    Json::Value root( Json::objectValue );
    
    root["version"] = Json::Value(1);
    
    auto & jlayers = root["groups"] = Json::Value( Json::arrayValue );
    
    // layers
    for (const auto & layer: layers) {

        math::Point3 origin;
        
        if ( layer.featuresBB )
            origin = subrange(0.5  * (layer.featuresBB->ll 
                + layer.featuresBB->ur), 0, 2);
        
        auto & jlayer = jlayers.append(Json::Value(Json::objectValue));
        jlayer["id"] = Json::Value(layer.name);
        jlayer["origin"] = buildPoint3( origin );
        
        if ( layer.features.points.size() > 0 ) {
            
            auto & jpoints = jlayer["points"] = Json::arrayValue;
            
            for (const auto & point: layer.features.points) {
         
                auto & jpoint = jpoints.append( Json::objectValue );
                jpoint["style"] = pointStyle; 
                jpoint["id"] = point.id;
                jpoint["html"] = buildHtml( point.properties );
                jpoint["points"] = Json::arrayValue;
                jpoint["points"].append(buildPoint3(
                    point.point - origin));
                
                // end point
            }
        }
        
        if (layer.features.multilinestrings.size() > 0) {
            
            auto & jlinestrings = jlayer["lines"] = Json::arrayValue;
            
            for (const auto & multilinestring: layer.features.multilinestrings) {
                for (const auto & linestring: multilinestring.lines) {
                
                    auto & jlinestring = jlinestrings.append( Json::objectValue );
                    jlinestring["style"] = linestringStyle;
                    jlinestring["id"] = multilinestring.id; 
                    jlinestring["html"] = buildHtml( multilinestring.properties );
                
                    auto & jpoints = jlinestring["points"] = Json::arrayValue;
                
                    for (const auto & point : linestring) 
                        jpoints.append(buildPoint3(point - origin));
                                
                }
                // end multi linestring
            }
        }
        
        if (layer.features.multipolygons.size() > 0) {
            LOG(warn3) 
                << "Polygons may not be serialized to geodata, "
                << "please convert to surfaces first.";
        }
    
        if (layer.features.surfaces.size() > 0) {
            LOG(warn3) << "Surface serialization not implmented yet.";
        }
        
        // end layer
    }
    
    // write output
    Json::FastWriter writer;
    os << writer.write(root);
}

void FeatureLayers::dumpVTSGeodata(std::ostream & os, const uint resolution) {
    
    // transformation to local coordinates
    struct ToLocal_ {
       
        math::Point3 origin, scale;
       
        ToLocal_(const boost::optional<math::Extents3> & extents
                , const uint resolution):
            origin(0,0,0), scale(1,1,1) {
                
            if (extents) origin = extents->ll;
           
            math::Point3 dvect(extents->ur - extents->ll);
            scale[0] = resolution / dvect[0];
            scale[1] = resolution / dvect[1];
            scale[2] = resolution / dvect[2];
        }
       
        math::Point3i operator()( const math::Point3 & p ) const {

            return { static_cast<int>(round((p[0] - origin[0]) * scale[0]))
                   , static_cast<int>(round((p[1] - origin[1]) * scale[1]))
                   , static_cast<int>(round((p[2] - origin[2]) * scale[2])) };
        }
    };
   
    // root json object
    Json::Value root(Json::objectValue);
    root["version"] = Json::Value(1);
   
    auto & jlayers = root["groups"] = Json::arrayValue;
   
    // iterate through layers
    for (const auto & layer: layers) {

        auto & jlayer = jlayers.append(Json::objectValue);
       
        // name
        jlayer["id"] = layer.name;
       
        // bounding box 
        if (layer.featuresBB) {
        
            auto & bbox = jlayer["bbox"] = Json::arrayValue;
            bbox.append(buildPoint3(layer.featuresBB->ll));              
            bbox.append(buildPoint3(layer.featuresBB->ur));              
        }
       
        // resolution
        jlayer["resolution"] = resolution;
       
        // coordinate transformer
        ToLocal_ tolocal(layer.featuresBB, resolution); 
              
        // points
        if (layer.features.points.size() > 0) {
            
            auto &jpoints = jlayer["points"] = Json::arrayValue;
        
            for (const auto &point: layer.features.points) {
            
                auto &jpoint = jpoints.append(Json::objectValue);

                // id and properties
                jpoint["id"] = point.id;
                
                auto & properties = jpoint["properties"] = Json::objectValue;
           
                for (const auto & property: point.properties)
                    properties[property.first] = property.second;

                // geometry
                jpoint["points"] = Json::arrayValue;
                jpoint["points"].append(buildPoint3(tolocal(point.point)));
            }
           
        } // end points
       
        // linestrings
        if (layer.features.multilinestrings.size() > 0) {
        
            auto & jmultilinestrings = jlayer["lines"] = Json::arrayValue;
           
            for (const auto &multilinestring: layer.features.multilinestrings) {
                
                auto &jmultilinestring 
                    = jmultilinestrings.append(Json::objectValue);

                // id and properties
                jmultilinestring["id"] = multilinestring.id;

                auto & properties
                    = jmultilinestring["properties"] = Json::objectValue;
           
                for (const auto & property: multilinestring.properties)
                    properties[property.first] = property.second;
                
                // geometries
                jmultilinestring["lines"] = Json::arrayValue;
                
                for (const auto &linestring: multilinestring.lines) {
                
                    auto &jline 
                        = jmultilinestring["lines"].append(Json::arrayValue);
                
                    for (const auto & point: linestring)
                        jline.append(buildPoint3(tolocal(point)));
                }
            }
        } // end linestrings
       
        // polygons
        if (layer.features.multipolygons.size() > 0) {
            LOG(warn3) 
                << "Polygons may not be serialized to geodata, "
                << "please convert to surfaces first.";
        } 

        // surfaces
        if (layer.features.surfaces.size() > 0) {
        
            auto & jsurfaces = jlayer["polygons"] = Json::arrayValue;
           
            for (const auto & surface: layer.features.surfaces) {
                
                auto &jsurface = jsurfaces.append(Json::objectValue);

                // id and properties
                jsurface["id"] = surface.id;

                auto & properties = jsurface["properties"] = Json::objectValue;
           
                for (const auto & property: surface.properties)
                    properties[property.first] = property.second;
                
                // geometry
                auto & jvertices = jsurface["vertices"] = Json::arrayValue;
                
                for (const auto &vertex: surface.vertices)
                    jvertices.append(buildPoint3(tolocal(vertex)));
                
                auto & jpatches = jsurface["surface"] = Json::arrayValue;
                
                for (const auto &patch: surface.surface)
                    for ( int i; i < 3; i++) jpatches.append(patch[i]);
                
                auto & jboundaries = jsurface["borders"] = Json::arrayValue;
                
                for (const auto &boundary: surface.boundaries) {
                    
                    auto & jboundary = jboundaries.append(Json::arrayValue);
                    for (const auto & index: boundary) jboundary.append(index);
                }
            }
                
        } // end surfaces
       
    } // end layer
   
    // write output
    Json::FastWriter writer;
    os << writer.write(root);    
}

boost::optional<math::Extents3> FeatureLayers::boundingBox(
    boost::optional<SrsDefinition> srs) {
    
    SrsDefinition srs_;
    
    if (!srs && layers.size() == 0) return boost::none;
    
    srs_ = srs ? srs.get() : layers[0].srs;
    
    FeatureLayers lcopy(*this);
    lcopy.transform(srs_);
    
    boost::optional<math::Extents3> retval;
    
    for (auto & layer: lcopy.layers) {
        
        if (retval && layer.featuresBB)
            retval = unite(retval.get(), layer.featuresBB.get());
        
        if (!retval && layer.featuresBB)
            retval = layer.featuresBB;
    }
    
    return retval;
}


Json::Value FeatureLayers::buildHtml( const Features::Properties & props ) {
    
    using boost::format;
    
    std::ostringstream str;
    
    str << "<table>";
    
    for ( const auto & prop  : props ) {
        if ( ! prop.second.empty() )
            str << format( "<tr><td><b>%s</b></td><td>%s</td></tr>" ) 
                % prop.first % prop.second; 
    }
    
    str << "</table>";
    
    return Json::Value( str.str() );
}


/* Class FeatureLayers::Layer */


void FeatureLayers::Layer::updateBB( const math::Point3 & point ) {

    if (!featuresBB)
        featuresBB = math::Extents3(point,point);
    else
        update(featuresBB.get(), point);
}        


/** class FeatureLayers::Features::Surface */

void FeatureLayers::Features::Surface::addPatchesFromPolygon(
    const Features::MultiPolygon::Polygon & ) {

    LOGONCE(warn3) << "3D polygon to surface conversion not implemented yet.";
    
    // find the polygon principal component (plane) and establish trafo
    // transform polygon to xy plane
    // triangule boundary points
    // build surface by testing against polygon
    // reverse transform to original plane
    // done
}

} // namespace geo
