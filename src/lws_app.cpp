#include "args/args.hxx"
#include "json/json.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/string_cast.hpp"

#include "geometrycentral/surface/meshio.h"
#include "lws_app.h"
#include "polyscope/gl/ground_plane.h"
#include "utils.h"
#include <omp.h>

#include "polyscope/curve_network.h"
#include "product/test_matrices.h"
#include "spatial/tpe_bvh.h"
#include "multigrid/constraint_projector_domain.h"
#include "libgmultigrid/multigrid_hierarchy.h"
#include "libgmultigrid/nullspace_projector.h"

#include "poly_curve_network.h"
#include "obstacles/mesh_obstacle.h"
#include "obstacles/plane_obstacle.h"
#include "obstacles/sphere_obstacle.h"

#include "scene_file.h"
#include "applications/pathplanning.h"

#include <limits>
#include <random>
#include <iostream>
#include <fstream>
#include <queue>

#define _USE_MATH_DEFINES
#include <cmath>

#include "marchingcubes/CIsoSurface.h"

#include "curve_io.h"

using namespace geometrycentral;
using namespace geometrycentral::surface;
using std::cerr;
using std::cout;
using std::endl;
using std::string;

namespace LWS
{

  Vector3 colorOfValue(double value)
  {
    value = clamp(value, -1.0, 1.0);
    value = (value + 1) / 2;
    return Vector3{value, 2 * value * (1 - value), 1 - value};
  }

  void LWSApp::outputFrame()
  {
    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%04d", screenshotNum);
    string fname = "frames/frame" + std::string(buffer) + ".png";
    screenshotNum++;
    polyscope::screenshot(fname, false);
    std::cout << "Wrote screenshot to " << fname << std::endl;
  }

  void printMatrix(Eigen::MatrixXd &G, int precision)
  {
    std::cout.precision(precision);
    for (int i = 0; i < G.rows(); i++)
    {
      for (int j = 0; j < G.cols(); j++)
      {
        if (G(i, j) == 0)
          std::cout.precision(1);
        else
          std::cout.precision(precision);
        std::cout << std::fixed << G(i, j) << ((j == G.cols() - 1) ? "" : ", ");
      }
      std::cout << std::endl;
    }
  }

  void writeSparseMatrix(std::ofstream &file, Eigen::MatrixXd &G, int precision)
  {
    file.precision(precision);
    std::vector<int> rows;
    std::vector<int> cols;
    std::vector<double> values;

    for (int i = 0; i < G.rows(); i++)
    {
      for (int j = 0; j < G.cols(); j++)
      {
        if (G(i, j) != 0)
        {
          rows.push_back(i);
          cols.push_back(j);
          values.push_back(G(i, j));
        }
      }
    }

    for (size_t i = 0; i < rows.size(); i++)
    {
      file << rows[i];
      if (i < rows.size() - 1)
        file << ", ";
    }
    file << std::endl;

    for (size_t i = 0; i < cols.size(); i++)
    {
      file << cols[i];
      if (i < cols.size() - 1)
        file << ", ";
    }
    file << std::endl;

    for (size_t i = 0; i < values.size(); i++)
    {
      file << values[i];
      if (i < values.size() - 1)
        file << ", ";
    }
    file << std::endl;
  }

  void LWSApp::outputOBJFrame()
  {
    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%04d", objNum);
    objNum++;
    writeCurves(curves, "objs/curve" + std::string(buffer) + ".obj", "objTangents/curve" + std::string(buffer) + ".obj");
  }

  void LWSApp::benchmarkMethods()
  {
    size_t nVerts = curves->NumVertices();
    BVHNode3D *tree_root = 0;

    Eigen::MatrixXd vertGradients;
    vertGradients.setZero(nVerts, 3);

    // Assemble the L2 gradient
    long bh_start = Utils::currentTimeMilliseconds();
    tree_root = CreateBVHFromCurve(curves);
    tpeSolver->AddAllGradients(tree_root, vertGradients);
    Eigen::MatrixXd l2gradients = vertGradients;
    long bh_end = Utils::currentTimeMilliseconds();
    std::cout << "  Barnes-Hut: " << (bh_end - bh_start) << " ms" << std::endl;

    // Set up multigrid stuff
    long mg_setup_start = Utils::currentTimeMilliseconds();
    using MultigridDomain = ConstraintProjectorDomain<VariableConstraintSet>;
    using MultigridSolver = MultigridHierarchy<MultigridDomain>;
    double sep = 1.0;
    MultigridDomain *domain = new MultigridDomain(curves, 3, 6, sep, 0);
    MultigridSolver *multigrid = new MultigridSolver(domain);
    long mg_setup_end = Utils::currentTimeMilliseconds();
    std::cout << "  Multigrid setup: " << (mg_setup_end - mg_setup_start) << " ms" << std::endl;

    // Use multigrid to compute the Sobolev gradient
    long mg_start = Utils::currentTimeMilliseconds();
    double soboDot = tpeSolver->ProjectGradientMultigrid<MultigridDomain, MultigridSolver::EigenCG>(vertGradients, multigrid, vertGradients, 1e-2);
    double dot_acc = soboDot / (l2gradients.norm() * vertGradients.norm());
    long mg_end = Utils::currentTimeMilliseconds();
    std::cout << "  Multigrid solve: " << (mg_end - mg_start) << " ms" << std::endl;
    std::cout << "  Sobolev gradient norm = " << soboDot << std::endl;

    delete multigrid;
    if (tree_root)
      delete tree_root;
  }

  void LWSApp::writeCurves(PolyCurveNetwork *network, const std::string &positionFilename, const std::string &tangentFilename)
  {
    std::vector<Vector3> all_positions;
    std::vector<Vector3> all_tangents;
    std::vector<std::vector<size_t>> edges;

    // build vectors of positions and tangents
    int nV = network->NumVertices();
    all_positions.resize(nV);
    all_tangents.resize(nV);
    for (int i = 0; i < nV; i++)
    {
      CurveVertex *vi = network->GetVertex(i);
      all_positions[i] = vi->Position();
      all_tangents[i] = vi->Tangent();
    }

    // build vector<vector<size_t>> components
    int nE = network->NumEdges();
    edges.resize(nE);
    for (int i = 0; i < nE; i++)
    {
      std::vector<size_t> edge(2);
      CurveEdge *ei = network->GetEdge(i);
      edge[0] = ei->prevVert->GlobalIndex();
      edge[1] = ei->nextVert->GlobalIndex();
      edges[i] = edge;
    }

    CurveIO::writeOBJLineElements(positionFilename.c_str(), all_positions, edges);
    CurveIO::writeOBJLineElements(tangentFilename.c_str(), all_tangents, edges);
  }

  void LWSApp::customWindow()
  {

    ImGuiIO &io = ImGui::GetIO();

    ImGui::Begin("Curve options", &LWSOptions::showWindow);

    if (ImGui::Button("Export implicit surface"))
    {
      WriteImplicitSurface();
    }

    if (ImGui::Button("Output frame"))
    {
      outputFrame();
    }

    ImGui::Checkbox("Run TPE", &LWSOptions::runTPE);
    ImGui::SameLine(160);
    ImGui::Checkbox("Normalize view", &LWSOptions::normalizeView);

    ImGui::Checkbox("Output frames", &LWSOptions::outputFrames);
    ImGui::SameLine(160);
    ImGui::Checkbox("Output OBJs", &writeOBJs);

    // ImGui::Checkbox("Log performance", &perfLogging);
    // if (perfLogging && !tpeSolver->PerformanceLogEnabled()) {
    //   tpeSolver->EnablePerformanceLog("performance_" + curveName + ".csv");
    // }

    bool buttonStepTPE = ImGui::Button("Single TPE step");

    ImGui::Checkbox("Use Sobolev", &LWSOptions::useSobolev);
    ImGui::SameLine(160);
    ImGui::Checkbox("Project constraints", &useBackproj);
    ImGui::Checkbox("Use Barnes-Hut", &LWSOptions::useBarnesHut);
    ImGui::SameLine(160);
    ImGui::Checkbox("Use multigrid", &LWSOptions::useMultigrid);

    if (LWSOptions::runTPE || buttonStepTPE)
    {
      tpeSolver->SetExponents(LWSOptions::tpeAlpha, LWSOptions::tpeBeta);
      currentStep++;

      if (LWSOptions::outputFrames && screenshotNum == 0)
      {
        outputFrame();
      }
      if (writeOBJs && objNum == 0)
      {
        outputOBJFrame();
      }

      bool good_step;
      if (LWSOptions::useSobolev)
      {
        if (LWSOptions::useMultigrid)
        {
          good_step = tpeSolver->StepSobolevLSIterative(0, useBackproj);
        }
        else
        {
          good_step = tpeSolver->StepSobolevLS(LWSOptions::useBarnesHut, useBackproj);
        }
      }
      else
      {
        good_step = tpeSolver->StepLS(LWSOptions::useBarnesHut);
        // good_step = tpeSolver->StepLSConstrained(LWSOptions::useBarnesHut, useBackproj);
      }

      UpdateCurvePositions();
      if (tpeSolver->soboNormZero)
      {
        std::cout << "Stopped because flow is (probably) near a local minimum." << std::endl;
        LWSOptions::runTPE = false;
      }
      if (!good_step)
      {
        numStuckIterations++;
        if (numStuckIterations >= 5 && tpeSolver->TargetLengthReached())
        {
          std::cout << "Stopped because flow hasn't made progress in a while." << std::endl;
          LWSOptions::runTPE = false;
        }
      }
      else if (stepLimit > 0 && currentStep >= stepLimit)
      {
        std::cout << "Stopped because maximum number of steps was reached." << std::endl;
        LWSOptions::runTPE = false;
      }
      else
      {
        numStuckIterations = 0;
      }

      double averageLength = curves->TotalLength() / curves->NumEdges();
      if (averageLength > 2 * initialAverageLength && subdivideCount < subdivideLimit)
      {
        subdivideCount++;
        SubdivideCurve();
      }

      if (LWSOptions::outputFrames)
      {
        outputFrame();
      }
      if (writeOBJs)
      {
        outputOBJFrame();
      }
    }

    if (ImGui::Button("Curve to OBJ"))
    {
      writeCurves(curves, "curve_positions.obj", "curve_tangents.obj");
    }

    if (ImGui::Button("BVH to OBJ"))
    {                                      // write bounding volume hierarchy to mesh file
      std::ofstream outPos("bvh_pos.obj"); // bounding boxes around positions
      std::ofstream outTan("bvh_tan.obj"); // bounding boxes around tangents

      // re-build the tree (can't always assume it was already built by solver)
      LWS::BVHNode3D *tree = CreateEdgeBVHFromCurve(curves);
      tree->assignIDs();

      // iterate over nodes of tree in breadth-first order
      std::queue<BVHNode3D *> Q;
      Q.push(tree);
      int nBoxes = 0; // track the total number of boxes
      while (!Q.empty())
      {
        BVHNode3D *n = Q.front();
        Q.pop(); // get a node

        // get the box corners
        PosTan b[2] = {n->minBound(), n->maxBound()};

        // output eight corners of the box by alternating max/min for each coordinate
        for (int i = 0; i < 2; i++)
          for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
            {
              // write both position and tangent coordinates
              outPos << "v " << b[i].position.x << " " << b[j].position.y << " " << b[k].position.z << endl;
              outTan << "v " << b[i].tangent.x << " " << b[j].tangent.y << " " << b[k].tangent.z << endl;
            }
        nBoxes++;

        for (BVHNode3D *c : n->children)
        {
          Q.push(c);
        }
      }

      // write faces for all boxes (using 1-based indices)
      for (int i = 0; i < nBoxes; i++)
      {
        int I = 1 + 8 * i;

        // position faces
        outPos << "f " << I + 0 << " " << I + 1 << " " << I + 3 << " " << I + 2 << endl;
        outPos << "f " << I + 6 << " " << I + 7 << " " << I + 5 << " " << I + 4 << endl;
        outPos << "f " << I + 0 << " " << I + 2 << " " << I + 6 << " " << I + 4 << endl;
        outPos << "f " << I + 5 << " " << I + 7 << " " << I + 3 << " " << I + 1 << endl;
        outPos << "f " << I + 4 << " " << I + 5 << " " << I + 1 << " " << I + 0 << endl;
        outPos << "f " << I + 2 << " " << I + 3 << " " << I + 7 << " " << I + 6 << endl;

        // tangent faces (which have identical indices)
        outTan << "f " << I + 0 << " " << I + 1 << " " << I + 3 << " " << I + 2 << endl;
        outTan << "f " << I + 6 << " " << I + 7 << " " << I + 5 << " " << I + 4 << endl;
        outTan << "f " << I + 0 << " " << I + 2 << " " << I + 6 << " " << I + 4 << endl;
        outTan << "f " << I + 5 << " " << I + 7 << " " << I + 3 << " " << I + 1 << endl;
        outTan << "f " << I + 4 << " " << I + 5 << " " << I + 1 << " " << I + 0 << endl;
        outTan << "f " << I + 2 << " " << I + 3 << " " << I + 7 << " " << I + 6 << endl;
      }
    }

    double delta = 0.001;

    if (ImGui::Button("Benchmark methods"))
    {
      benchmarkMethods();
    }


    ImGui::End();
  }

  void LWSApp::centerLoopBarycenter(PolyCurveNetwork *curves)
  {
    Vector3 center = curves->Barycenter();
    int nVerts = curves->NumVertices();

    for (int i = 0; i < nVerts; i++)
    {
      CurveVertex *v = curves->GetVertex(i);
      v->SetPosition(v->Position() - center);
    }

    UpdateCurvePositions();
  }

  void LWSApp::initSolver()
  {
    numStuckIterations = 0;
    subdivideCount = 0;

    useBackproj = true;

    if (sceneData.subdivideLimit > 0)
    {
      subdivideLimit = sceneData.subdivideLimit;
      std::cout << "Setting curve subdivision limit to " << subdivideLimit << std::endl;
    }
    if (sceneData.iterationLimit > 0)
    {
      stepLimit = sceneData.iterationLimit;
      std::cout << "Setting iteration limit to " << stepLimit << std::endl;
    }

    // for (int i = 0; i < curves->NumVertices(); i++) {
    //   if (curves->GetVertex(i)->numEdges() != 2) continue;
    //   Vector3 v1 = curves->GetVertex(i)->edge(0)->Vector().normalize();
    //   Vector3 v2 = curves->GetVertex(i)->edge(1)->Vector().normalize();
    //   if (fabs(dot(v1, v2)) < 0.999) {
    //     std::cout << "Vertex " << i << " has angle " << acos(dot(v1, v2)) << " (dot " << dot(v1, v2) << ")" << std::endl;
    //   }
    // }

    if (!tpeSolver)
    {
      if (curves->appliedConstraints.size() == 0)
      {
        std::cout << "No constraints specified; defaulting to barycenter and edge lengths" << std::endl;
        curves->appliedConstraints.push_back(ConstraintType::Barycenter);
        curves->appliedConstraints.push_back(ConstraintType::EdgeLengths);
      }
      // Set up solver
      double alpha = LWSOptions::tpeAlpha;
      double beta = LWSOptions::tpeBeta;
      tpeSolver = new TPEFlowSolverSC(curves, alpha, beta);

      for (ObstacleData &data : sceneData.obstacles)
      {
        std::cout << "Adding scene obstacle from " << data.filename << " (weight " << data.weight << ")" << std::endl;
        AddMeshObstacle(data.filename, Vector3{0, 0, 0}, beta - alpha, data.weight);
      }

      for (PlaneObstacleData &data : sceneData.planes)
      {
        std::cout << "Adding plane obstacle (center " << data.center << ", normal "
                  << data.normal << ", weight " << data.weight << std::endl;
        AddPlaneObstacle(data.center, data.normal, beta - alpha, data.weight);
      }

      for (std::string &surfaceName : sceneData.surfacesToShow)
      {
        VisualizeMesh(surfaceName);
      }

      for (PotentialData &data : sceneData.extraPotentials)
      {
        switch (data.type)
        {
        case PotentialType::Length:
          std::cout << "Adding length potential (weight = " << data.weight << ")" << std::endl;
          tpeSolver->potentials.push_back(new TotalLengthPotential(data.weight));
          break;
        case PotentialType::LengthDiff:
          std::cout << "Adding length diff potential (weight = " << data.weight << ")" << std::endl;
          tpeSolver->potentials.push_back(new LengthDifferencePotential(data.weight));
          break;
        case PotentialType::PinAngles:
          std::cout << "Adding pin angle potential (weight = " << data.weight << ")" << std::endl;
          tpeSolver->potentials.push_back(new PinBendingPotential(data.weight));
          break;
        case PotentialType::Area:
          std::cerr << "Area potential is not implemented yet" << std::endl;
          break;
        case PotentialType::VectorField:
          if (data.extraInfo == "constant")
          {
            std::cout << "Adding constant vector field potential (weight = " << data.weight << ")" << std::endl;
            tpeSolver->potentials.push_back(new VectorFieldPotential(data.weight, new ConstantVectorField(Vector3{1, 0, 1})));
          }
          else if (data.extraInfo == "circular")
          {
            std::cout << "Adding circular vector field potential (weight = " << data.weight << ")" << std::endl;
            tpeSolver->potentials.push_back(new VectorFieldPotential(data.weight, new CircularVectorField()));
          }
          else if (data.extraInfo == "interesting")
          {
            std::cout << "Adding interesting vector field potential (weight = " << data.weight << ")" << std::endl;
            tpeSolver->potentials.push_back(new VectorFieldPotential(data.weight, new InterestingVectorField()));
          }
          else
          {
            std::cerr << "Invalid vector field " << data.extraInfo << std::endl;
            exit(1);
          }
          break;
        }
      }

      if (sceneData.useLengthScale && sceneData.edgeLengthScale != 1)
      {
        tpeSolver->SetEdgeLengthScaleTarget(sceneData.edgeLengthScale);
      }
      else if (sceneData.useTotalLengthScale && sceneData.totalLengthScale != 1)
      {
        tpeSolver->SetTotalLengthScaleTarget(sceneData.totalLengthScale);
      }

      initialAverageLength = curves->TotalLength() / curves->NumEdges();
    }
  }

  void LWSApp::UpdateCurvePositions()
  {
    // Update the positions on the space curve
    polyscope::CurveNetwork *curveNetwork = polyscope::getCurveNetwork(curveName);
    size_t nVerts = curves->NumVertices();
    std::vector<glm::vec3> curve_vecs(nVerts);

    // if the "normalize view" button is checked, center
    // and normalize the vertex positions that are sent
    // to Polyscope (but do not change the actual positions)
    Vector3 center{0., 0., 0.};
    double radius = 1.;
    if (LWSOptions::normalizeView)
    {
      for (size_t i = 0; i < nVerts; i++)
      {
        CurveVertex *v_i = curves->GetVertex(i);
        center += v_i->Position();
      }
      center /= nVerts;

      radius = 0.;
      for (size_t i = 0; i < nVerts; i++)
      {
        CurveVertex *v_i = curves->GetVertex(i);
        radius = fmax(radius, (v_i->Position() - center).norm2());
      }
      radius = sqrt(radius);
    }

    for (size_t i = 0; i < nVerts; i++)
    {
      CurveVertex *v_i = curves->GetVertex(i);
      Vector3 v = v_i->Position();
      v = (v - center) / radius;
      curve_vecs[v_i->GlobalIndex()] = glm::vec3{v.x, v.y, v.z};
    }

    curveNetwork->updateNodePositions(curve_vecs);
    polyscope::requestRedraw();
  }

  void LWSApp::VisualizeMesh(std::string objName)
  {
    std::unique_ptr<HalfedgeMesh> mesh;
    std::unique_ptr<VertexPositionGeometry> geometry;
    std::tie(mesh, geometry) = loadMesh(objName);

    std::string name = polyscope::guessNiceNameFromPath(objName);
    polyscope::registerSurfaceMesh(name, geometry->inputVertexPositions,
                                   mesh->getFaceVertexList(), polyscopePermutations(*mesh));
  }

  void LWSApp::AddMeshObstacle(std::string objName, Vector3 center, double p, double weight)
  {
    std::unique_ptr<HalfedgeMesh> mesh;
    std::unique_ptr<VertexPositionGeometry> geometry;
    std::tie(mesh, geometry) = loadMesh(objName);

    std::string name = polyscope::guessNiceNameFromPath(objName);
    polyscope::registerSurfaceMesh(name, geometry->inputVertexPositions,
                                   mesh->getFaceVertexList(), polyscopePermutations(*mesh));

    std::shared_ptr<HalfedgeMesh> mesh_shared(std::move(mesh));
    std::shared_ptr<VertexPositionGeometry> geom_shared(std::move(geometry));

    geom_shared->requireVertexPositions();
    geom_shared->requireVertexNormals();
    geom_shared->requireVertexDualAreas();

    tpeSolver->obstacles.push_back(new MeshObstacle(mesh_shared, geom_shared, p, weight));
  }

  void LWSApp::AddPlaneObstacle(Vector3 center, Vector3 normal, double p, double weight)
  {
    int numObs = tpeSolver->obstacles.size();
    tpeSolver->obstacles.push_back(new PlaneObstacle(center, normal, p, weight));
    DisplayPlane(center, normal, "obstacle" + std::to_string(numObs));
  }

  void LWSApp::AddSphereObstacle(Vector3 center, double radius)
  {
    int numObs = tpeSolver->obstacles.size();
    tpeSolver->obstacles.push_back(new SphereObstacle(center, radius, 2));
    DisplayWireSphere(center, radius, "obstacle" + std::to_string(numObs));
  }

  void LWSApp::SubdivideCurve()
  {
    PolyCurveNetwork *subdivided = curves->Subdivide();
    glm::vec3 col = polyscope::getCurveNetwork(curveName)->baseColor;
    DisplayCurves(subdivided, curveName);
    polyscope::getCurveNetwork(curveName)->baseColor = col;
    tpeSolver->ReplaceCurve(subdivided);
    delete curves;
    curves = subdivided;
  }

  void LWSApp::MeshImplicitSurface(ImplicitSurface *surface)
  {
    CIsoSurface<double> *iso = new CIsoSurface<double>();

    std::cout << "Meshing the supplied implicit surface using marching cubes..." << std::endl;

    const int numCells = 50;
    Vector3 center = surface->BoundingCenter();
    double diameter = surface->BoundingDiameter();
    double cellSize = diameter / numCells;
    double radius = diameter / 2;

    Vector3 lowerCorner = center - Vector3{radius, radius, radius};

    int numCorners = numCells + 1;

    double* field = new double[numCorners * numCorners * numCorners];

    int nSlice = numCorners * numCorners;
    int nRow = numCorners;

    for (int x = 0; x < numCorners; x++)
    {
      for (int y = 0; y < numCorners; y++)
      {
        for (int z = 0; z < numCorners; z++)
        {
          Vector3 samplePt = lowerCorner + Vector3{(double)x, (double)y, (double)z} * cellSize;
          double value = surface->SignedDistance(samplePt);
          field[nSlice * z + nRow * y + x] = value;
        }
      }
    }

    iso->GenerateSurface(field, 0, numCells, numCells, numCells, cellSize, cellSize, cellSize);
    delete[] field;

    std::vector<glm::vec3> nodes;
    std::vector<std::array<size_t, 3>> triangles;

    int nVerts = iso->m_nVertices;

    for (int i = 0; i < nVerts; i++)
    {
      double x = iso->m_ppt3dVertices[i][0];
      double y = iso->m_ppt3dVertices[i][1];
      double z = iso->m_ppt3dVertices[i][2];

      Vector3 p = lowerCorner + Vector3{x, y, z};
      nodes.push_back(glm::vec3{p.x, p.y, p.z});
    }

    int nTris = iso->m_nTriangles;

    for (int i = 0; i < nTris; i++)
    {
      int i1 = iso->m_piTriangleIndices[3 * i];
      int i2 = iso->m_piTriangleIndices[3 * i + 1];
      int i3 = iso->m_piTriangleIndices[3 * i + 2];

      triangles.push_back({(size_t)i1, (size_t)i2, (size_t)i3});
    }

    polyscope::registerSurfaceMesh("implicitSurface", nodes, triangles);
    delete iso;
  }

  void LWSApp::WriteImplicitSurface()
  {
    polyscope::SurfaceMesh *mesh = polyscope::getSurfaceMesh("implicitSurface");
    std::cout << "Writing implicit surface to implicitSurface.obj..." << std::endl;

    std::ofstream objFile("implicitSurface.obj");

    size_t nVerts = mesh->vertices.size();

    for (size_t i = 0; i < nVerts; i++)
    {
      glm::vec3 pos = mesh->vertices[i];
      objFile << "v " << pos.x << " " << pos.y << " " << pos.z << std::endl;
    }

    size_t nFaces = mesh->faces.size();

    for (size_t i = 0; i < nFaces; i++)
    {
      size_t nInFace = mesh->faces[i].size();
      objFile << "f ";
      for (size_t j = 0; j < nInFace; j++)
      {
        objFile << mesh->faces[i][j] + 1;
        if (j < nInFace - 1)
        {
          objFile << " ";
        }
      }

      objFile << std::endl;
    }

    objFile.close();
    std::cout << "Done" << std::endl;
  }

  void LWSApp::DisplayPlane(Vector3 center, Vector3 normal, std::string name)
  {
    Vector3 v1{1, 0, 0};
    // If this axis is too close to parallel, then switch to a different one
    if (dot(v1, normal) > 0.99 || dot(v1, normal) < -0.99)
    {
      v1 = Vector3{0, 1, 0};
    }
    // Orthogonalize
    v1 = v1 - dot(v1, normal) * normal;
    v1 = v1.normalize();
    Vector3 v2 = cross(normal, v1).normalize();

    std::vector<glm::vec3> nodes;
    std::vector<std::array<size_t, 3>> triangles;

    Vector3 c1 = center - v1 - v2;
    Vector3 c2 = center + v1 - v2;
    Vector3 c3 = center - v1 + v2;
    Vector3 c4 = center + v1 + v2;
    nodes.push_back(glm::vec3{c1.x, c1.y, c1.z});
    nodes.push_back(glm::vec3{c2.x, c2.y, c2.z});
    nodes.push_back(glm::vec3{c3.x, c3.y, c3.z});
    nodes.push_back(glm::vec3{c4.x, c4.y, c4.z});

    triangles.push_back({0, 1, 2});
    triangles.push_back({2, 1, 3});

    polyscope::registerSurfaceMesh(name, nodes, triangles);
  }

  void LWSApp::DisplayWireSphere(Vector3 center, double radius, std::string name)
  {
    size_t numSegments = 32;
    size_t base = 0;

    std::vector<glm::vec3> nodes;
    std::vector<std::array<size_t, 2>> edges;

    for (int c = 0; c < 3; c++)
    {
      for (size_t i = 0; i < numSegments; i++)
      {
        double theta = i * (2 * M_PI) / numSegments;

        double x = cos(theta) * radius;
        double y = sin(theta) * radius;

        if (c == 0)
          nodes.push_back(glm::vec3{x, 0, y});
        else if (c == 1)
          nodes.push_back(glm::vec3{x, y, 0});
        else if (c == 2)
          nodes.push_back(glm::vec3{0, x, y});
        edges.push_back({base + i, base + (i + 1) % numSegments});
      }
      base += numSegments;
    }

    polyscope::registerCurveNetwork(name, nodes, edges);
  }

  void LWSApp::DisplayCurves(PolyCurveNetwork *curves, std::string name)
  {

    std::vector<glm::vec3> nodes;
    std::vector<std::array<size_t, 2>> edges;

    for (int i = 0; i < curves->NumVertices(); i++)
    {
      // Add interior edges and vertices
      Vector3 p = curves->GetVertex(i)->Position();
      nodes.push_back(glm::vec3{p.x, p.y, p.z});
    }
    for (int i = 0; i < curves->NumEdges(); i++)
    {
      CurveEdge *e = curves->GetEdge(i);
      edges.push_back({(size_t)e->prevVert->GlobalIndex(), (size_t)e->nextVert->GlobalIndex()});
    }

    polyscope::registerCurveNetwork(name, nodes, edges);
    polyscope::getCurveNetwork(name)->radius = 0.015f;
  }

  void LWSApp::DisplayCyclicList(std::vector<Vector3> &positions, std::string name)
  {
    std::vector<std::array<size_t, 2>> edges;

    for (size_t i = 0; i < positions.size() - 1; i++)
    {
      edges.push_back({i, i + 1});
    }
    edges.push_back({positions.size() - 1, 0});
    polyscope::registerCurveNetwork(name, positions, edges);
    polyscope::getCurveNetwork(name)->radius = 0.01f;
  }

  void LWSApp::processFileOBJ(std::string filename)
  {
    if (curves)
      delete curves;
    std::cout << "Make curves from OBJ " << filename << std::endl;

    std::tie(mesh, geom) = loadMesh(filename);
    geom->requireVertexPositions();

    VertexData<size_t> indices = mesh->getVertexIndices();

    int nVerts = mesh->nVertices();
    std::vector<Vector3> all_positions(nVerts);
    std::vector<std::array<size_t, 2>> all_edges;

    for (Vertex v : mesh->vertices())
    {
      size_t index = indices[v];
      all_positions[index] = geom->vertexPositions[v];
    }

    for (BoundaryLoop b : mesh->boundaryLoops())
    {
      Halfedge he = b.halfedge().twin();
      Halfedge start = b.halfedge().twin();

      do
      {
        Vector3 v = geom->vertexPositions[he.vertex()];
        size_t index = indices[he.vertex()];
        size_t next_index = indices[he.next().vertex()];
        all_edges.push_back({index, next_index});

        he = he.next();
      } while (he != start);
      std::cout << "Processed boundary curve of length " << b.degree() << std::endl;
    }

    curves = new PolyCurveNetwork(all_positions, all_edges);
    curveName = polyscope::guessNiceNameFromPath(filename);
  }

  void LWSApp::processLoopFile(std::string filename)
  {
    if (curves)
      delete curves;
    std::cout << "Make curves from indexed loop in " << filename << std::endl;

    std::vector<Vector3> all_positions;
    std::vector<std::array<size_t, 2>> all_edges;
    CurveIO::readVerticesAndEdges(filename, all_positions, all_edges);

    if (all_edges.size() == 0)
    {
      std::cout << "Did not find any OBJ line elements; reading edges from faces instead" << std::endl;
      CurveIO::readFaces(filename, all_edges);
    }

    curves = new PolyCurveNetwork(all_positions, all_edges);
    curveName = polyscope::guessNiceNameFromPath(filename);
  }

  void LWSApp::processSceneFile(std::string filename)
  {
    SceneData data = ParseSceneFile(filename);
    std::cout << data.curve_filename << std::endl;
    std::cout << "Loading curve from " << data.curve_filename << std::endl;
    processLoopFile(data.curve_filename);

    LWSOptions::tpeAlpha = data.tpe_alpha;
    LWSOptions::tpeBeta = data.tpe_beta;

    sceneData = data;

    // Add constraints
    for (ConstraintType type : data.constraints)
    {
      std::cout << "Adding constraint " << NameOfConstraint(type) << std::endl;
      curves->appliedConstraints.push_back(type);
    }
    for (int i : data.pinnedVertices)
    {
      std::cout << "Pinning vertex position " << i << std::endl;
      curves->PinVertex(i);
    }
    for (int i : data.pinnedTangents)
    {
      std::cout << "Pinning vertex tangent " << i << std::endl;
      curves->PinTangent(i);
    }

    curves->pinnedAllToSurface = false;

    // Pin all special vertices if specified
    if (data.pinSpecialVertices)
    {
      std::cout << "Pinning all special vertices" << std::endl;
      curves->PinAllSpecialVertices(data.pinSpecialTangents);
    }

    else if (data.pinEndpointVertices)
    {
      std::cout << "Pinning all endpoint vertices" << std::endl;
      curves->PinAllEndpoints(data.pinSpecialTangents);
    }

    if (data.constraintSurface)
    {
      curves->constraintSurface = data.constraintSurface;
      MeshImplicitSurface(curves->constraintSurface);
    }

    if (data.constrainAllToSurface)
    {
      std::cout << "Constraining all vertices to the implicit surface" << std::endl;
      for (int i = 0; i < curves->NumVertices(); i++)
      {
        curves->PinToSurface(i);
      }
      curves->pinnedAllToSurface = true;
    }
    else if (data.constrainEndpointsToSurface)
    {
      std::cout << "Constraining endpoint vertices to the implicit surface" << std::endl;
      for (int i = 0; i < curves->NumVertices(); i++)
      {
        CurveVertex *v_i = curves->GetVertex(i);
        if (v_i->numEdges() == 1)
        {
          curves->PinToSurface(i);
        }
      }
    }
    else
    {
      for (int i : data.surfaceConstrainedVertices)
      {
        std::cout << "Pinning vertex " << i << " to the implicit surface" << std::endl;
        curves->PinToSurface(i);
      }
    }
    curves->PrintPins();
  }
} // namespace LWS

LWS::LWSApp *LWS::LWSApp::instance;

bool endsWith(const std::string &str, const std::string &suffix)
{
  return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void processFile(LWS::LWSApp *app, string filename)
{
  // Dispatch to correct variant
  if (endsWith(filename, ".obj"))
  {
    app->processLoopFile(filename);
  }
  else if (endsWith(filename, ".loop"))
  {
    app->processLoopFile(filename);
  }
  else if (endsWith(filename, ".txt"))
  {
    app->processSceneFile(filename);
  }
  else
  {
    cerr << "Unrecognized file type for " << filename << endl;
  }
}

void customWindow()
{
  LWS::LWSApp::instance->customWindow();
}

int main(int argc, char **argv)
{
  int default_threads = omp_get_max_threads();
  int used_threads = default_threads / 2 + 2;
  omp_set_num_threads(used_threads);
  std::cout << "OMP autodetected " << default_threads << " max threads; using " << used_threads << std::endl;

  // Configure the argument parser
  args::ArgumentParser parser("An optimizer for self-avoiding curve energies.",
                              "");
  args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
  args::Positional<string> file(parser, "curve", "Space curve to process");
  args::ValueFlagList<string> obstacleFiles(parser, "obstacles", "Obstacles to add", {'o'});
  args::ValueFlagList<string> visualizeFiles(parser, "visualize", "Extra meshes to visualize", {'v'});

  // Parse args
  try
  {
    parser.ParseCLI(argc, argv);
  }
  catch (args::Help)
  {
    std::cout << parser;
    return 0;
  }
  catch (args::ParseError e)
  {
    std::cerr << e.what() << std::endl;

    std::cerr << parser;
    return 1;
  }

  if (!file)
  {
    std::cout << "Must specify one space curve file." << std::endl;
    return 1;
  }

  // Options
  polyscope::options::autocenterStructures = false;
  // polyscope::view::windowWidth = 600;
  // polyscope::view::windowHeight = 800;
  polyscope::gl::groundPlaneEnabled = false;

  LWS::LWSApp *app = new LWS::LWSApp();
  LWS::LWSApp::instance = app;

  std::cout << "Using Eigen version " << EIGEN_WORLD_VERSION << "." << EIGEN_MAJOR_VERSION << "." << EIGEN_MINOR_VERSION << std::endl;

  // Initialize polyscope
  polyscope::init();
  // Add a few gui elements
  polyscope::state::userCallback = &customWindow;

  processFile(LWS::LWSApp::instance, file.Get());

  app->DisplayCurves(app->curves, app->curveName);
  // app->curves->PinVertex(10);
  // app->curves->PinTangent(10);

  std::cout << "Set up curve" << std::endl;
  app->initSolver();
  std::cout << "Set up solver" << std::endl;

  if (obstacleFiles)
  {
    for (string obsFile : obstacleFiles)
    {
      app->AddMeshObstacle(obsFile, Vector3{0, 0, 0}, 3, 1);
    }
  }
  if (visualizeFiles)
  {
    for (string visFile : visualizeFiles)
    {
      app->VisualizeMesh(visFile);
    }
  }

  // app->AddPlaneObstacle(Vector3{-2, 0, 0}, Vector3{1, 0, 0});
  // app->AddPlaneObstacle(Vector3{2, 0, 0}, Vector3{-1, 0, 0});
  // app->AddSphereObstacle(Vector3{0, 0, 0}, 1.5);

  // Show the gui
  polyscope::show();

  return 0;
}
