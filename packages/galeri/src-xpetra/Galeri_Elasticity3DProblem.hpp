// @HEADER
//
// ***********************************************************************
//
//           Galeri: Finite Element and Matrix Generation Package
//                 Copyright (2006) ETHZ/Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact
//                    Jeremie Gaidamour (jngaida@sandia.gov)
//                    Jonathan Hu       (jhu@sandia.gov)
//                    Ray Tuminaro      (rstumin@sandia.gov)
//
// ***********************************************************************
//
// @HEADER

#ifndef GALERI_ELASTICITY3DPROBLEM_HPP
#define GALERI_ELASTICITY3DPROBLEM_HPP

#include <Teuchos_SerialDenseMatrix.hpp>
#include <Teuchos_ParameterList.hpp>

#include "Galeri_Problem.hpp"
#include "Galeri_MultiVectorTraits.hpp"
#include "Galeri_XpetraUtils.hpp"

namespace Galeri {

  namespace Xpetra {

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    class Elasticity3DProblem : public Problem<Map,Matrix,MultiVector> {
    public:
      Elasticity3DProblem(Teuchos::ParameterList& list, const Teuchos::RCP<const Map>& map) : Problem<Map,Matrix,MultiVector>(list, map) {
        E  = list.get("E", 1e9);
        nu = list.get("nu", 0.25);

        nx_ = list.get("nx", -1);
        ny_ = list.get("ny", -1);
        nz_ = list.get("nz", -1);

        nDim = 3;
        double one = 1.0;
        stretch.push_back(list.get("stretchx", one));
        stretch.push_back(list.get("stretchy", one));
        stretch.push_back(list.get("stretchz", one));

        // NOTE: -1 is because galeri counts points, not elements
        dims.push_back(nx_-1);
        dims.push_back(ny_-1);
        dims.push_back(nz_-1);


        std::cout << "nx = " << nx_ << ", ny = " << ny_ << ", nz = " << nz_ << std::endl;
        TEUCHOS_TEST_FOR_EXCEPTION(nx_ <= 0 || ny_ <= 0 || nz_ <= 0, std::logic_error, "nx, ny and nz must be positive");
      }

      Teuchos::RCP<Matrix>      BuildMatrix();
      Teuchos::RCP<MultiVector> BuildNullspace();
      Teuchos::RCP<MultiVector> BuildCoords();

    private:
      typedef Scalar        SC;
      typedef LocalOrdinal  LO;
      typedef GlobalOrdinal GO;

      struct Point {
        SC x, y, z;

        Point() { z = Teuchos::ScalarTraits<SC>::zero(); }
        Point(SC x_, SC y_, SC z_ = Teuchos::ScalarTraits<SC>::zero()) : x(x_), y(y_), z(z_) { }
      };

      GlobalOrdinal                  nx_, ny_, nz_;
      size_t                         nDim;
      std::vector<GO>                dims;
      std::vector<Point>             nodes;
      std::vector<std::vector<LO> >  elements;
      std::vector<GO>                local2Global_;

      Scalar                         E, nu;
      std::vector<Scalar>            stretch;
      std::string                    mode_;

      std::vector<char>              dirichlet_;

      void EvalDxi  (const std::vector<Point>& refPoints, Point& gaussPoint, SC * dxi);
      void EvalDeta (const std::vector<Point>& refPoints, Point& gaussPoint, SC * deta);
      void EvalDzeta(const std::vector<Point>& refPoints, Point& gaussPoint, SC * dzeta);

      void BuildMesh();
      void BuildMaterialMatrix (Teuchos::SerialDenseMatrix<LO,SC>& D);
      void BuildReferencePoints(size_t& numRefPoints, std::vector<Point>& refPoints, size_t& numGaussPoints, std::vector<Point>& gaussPoints);
    };



    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    Teuchos::RCP<Matrix> Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildMatrix() {
      using Teuchos::SerialDenseMatrix;

      BuildMesh();

      const size_t numDofPerNode   = 3;
      const size_t numNodesPerElem = 8;
      const size_t numDofPerElem   = numNodesPerElem * numDofPerNode;

      TEUCHOS_TEST_FOR_EXCEPTION(elements[0].size() != numNodesPerElem, std::logic_error, "Incorrect number of element vertices");

      // Material constant
      SC t = 1;

      // Material matrix
      RCP<SerialDenseMatrix<LO,SC> > D(new SerialDenseMatrix<LO,SC>);
      BuildMaterialMatrix(*D);

      // Reference element, and reference Gauss points
      size_t numRefPoints, numGaussPoints;
      std::vector<Point> refPoints, gaussPoints;
      BuildReferencePoints(numRefPoints, refPoints, numGaussPoints, gaussPoints);

      // Evaluate the B matrix for the reference element
      size_t sDim = 8;
      size_t bDim = 9;
      std::vector<SerialDenseMatrix<LO,SC> > Bs(numGaussPoints);
      std::vector<SerialDenseMatrix<LO,SC> > Ss(numGaussPoints);

      for (size_t j = 0; j < numGaussPoints; j++) {
        SerialDenseMatrix<LO,SC>& S = Ss[j];
        S.shape(sDim, nDim);
        EvalDxi  (refPoints, gaussPoints[j], S[0]);
        EvalDeta (refPoints, gaussPoints[j], S[1]);
        EvalDzeta(refPoints, gaussPoints[j], S[2]);

        SerialDenseMatrix<LO,SC>& B = Bs[j];
        B.shape(bDim, numDofPerElem);

        for (size_t k = 0; k < numNodesPerElem; k++) {
          B(0, numDofPerNode*k + 0) = S(k,0);
          B(1, numDofPerNode*k + 0) = S(k,1);
          B(2, numDofPerNode*k + 0) = S(k,2);
          B(3, numDofPerNode*k + 1) = S(k,0);
          B(4, numDofPerNode*k + 1) = S(k,1);
          B(5, numDofPerNode*k + 1) = S(k,2);
          B(6, numDofPerNode*k + 2) = S(k,0);
          B(7, numDofPerNode*k + 2) = S(k,1);
          B(8, numDofPerNode*k + 2) = S(k,2);
        }
      }

      // Construct reordering matrix (see 6.2-9 from Cook)
      SerialDenseMatrix<LO,SC> R(D->numRows(), bDim);
      R(0,0) = R(1,4) = R(2,8) = R(3,1) = R(3,3) = R(4,5) = R(4,7) = R(5,2) = R(5,6) = 1;

      // FIXME
      this->A_ = MatrixTraits<Map,Matrix>::Build(this->Map_, 27*numDofPerNode);

      SC one = Teuchos::ScalarTraits<SC>::one(), zero = Teuchos::ScalarTraits<SC>::zero();
      for (size_t i = 0; i < elements.size(); i++) {
        SerialDenseMatrix<LO,SC> KE(numDofPerElem, numDofPerElem), K0(D->numRows(), numDofPerElem);

        // Select nodes subvector
        SerialDenseMatrix<LO,SC> elementNodes(numNodesPerElem, nDim);
        std::vector<LO>& elemNodes = elements[i];
        for (size_t j = 0; j < numNodesPerElem; j++) {
          elementNodes(j,0) = nodes[elemNodes[j]].x;
          elementNodes(j,1) = nodes[elemNodes[j]].y;
          elementNodes(j,2) = nodes[elemNodes[j]].z;
        }

        // Evaluate the stiffness matrix for the element
        for (size_t j = 0; j < numGaussPoints; j++) {
          SerialDenseMatrix<LO,SC>& B = Bs[j];
          SerialDenseMatrix<LO,SC>& S = Ss[j];

          SerialDenseMatrix<LO,SC> JAC(nDim, nDim);

          for (size_t p = 0; p < nDim; p++)
            for (size_t q = 0; q < nDim; q++) {
              JAC(p,q) = zero;

              for (size_t k = 0; k < numNodesPerElem; k++)
                JAC(p,q) += S(k,p)*elementNodes(k,q);
            }

          SC detJ = JAC(0,0)*JAC(1,1)*JAC(2,2) + JAC(2,0)*JAC(0,1)*JAC(1,2) + JAC(0,2)*JAC(2,1)*JAC(1,0) -
                    JAC(2,0)*JAC(1,1)*JAC(0,2) - JAC(0,0)*JAC(2,1)*JAC(1,2) - JAC(2,2)*JAC(0,1)*JAC(1,0);

          // J2 = inv([JAC zeros(3) zeros(3); zeros(3) JAC zeros(3); zeros(3) zeros(3) JAC])
          SerialDenseMatrix<LO,SC> J2(nDim*nDim,nDim*nDim);
          J2(0,0) = J2(3,3) = J2(6,6) =  (JAC(2,2)*JAC(1,1)-JAC(2,1)*JAC(1,2))/detJ;
          J2(0,1) = J2(3,4) = J2(6,7) = -(JAC(2,2)*JAC(0,1)-JAC(2,1)*JAC(0,2))/detJ;
          J2(0,2) = J2(3,5) = J2(6,8) =  (JAC(1,2)*JAC(0,1)-JAC(1,1)*JAC(0,2))/detJ;
          J2(1,0) = J2(4,3) = J2(7,6) = -(JAC(2,2)*JAC(1,0)-JAC(2,0)*JAC(1,2))/detJ;
          J2(1,1) = J2(4,4) = J2(7,7) =  (JAC(2,2)*JAC(0,0)-JAC(2,0)*JAC(0,2))/detJ;
          J2(1,2) = J2(4,5) = J2(7,8) = -(JAC(1,2)*JAC(0,0)-JAC(1,0)*JAC(0,2))/detJ;
          J2(2,0) = J2(5,3) = J2(8,6) =  (JAC(2,1)*JAC(1,0)-JAC(2,0)*JAC(1,1))/detJ;
          J2(2,1) = J2(5,4) = J2(8,7) = -(JAC(2,1)*JAC(0,0)-JAC(2,0)*JAC(0,1))/detJ;
          J2(2,2) = J2(5,5) = J2(8,8) =  (JAC(1,1)*JAC(0,0)-JAC(1,0)*JAC(0,1))/detJ;

          SerialDenseMatrix<LO,SC> B2(J2.numRows(), B.numCols());
          B2.multiply(Teuchos::NO_TRANS, Teuchos::NO_TRANS, Teuchos::ScalarTraits<SC>::one(), J2, B, zero);

          // KE = KE + t * J2B' * D * J2B * detJ
          SerialDenseMatrix<LO,SC> J2B(R.numRows(), B2.numCols());
          J2B.multiply(Teuchos::NO_TRANS, Teuchos::NO_TRANS,    one,   R,  B2, zero);
          K0 .multiply(Teuchos::NO_TRANS, Teuchos::NO_TRANS,    one,  *D, J2B, zero);
          KE .multiply(Teuchos::TRANS,    Teuchos::NO_TRANS, t*detJ, J2B,  K0, one);
        }

        Teuchos::Array<GO> elemDofs(numDofPerElem);
        for (size_t j = 0; j < numNodesPerElem; j++) { // FIXME: this may be inconsistent with the map
          elemDofs[numDofPerNode*j + 0] = local2Global_[elemNodes[j]]*numDofPerNode;
          elemDofs[numDofPerNode*j + 1] = elemDofs[numDofPerNode*j + 0] + 1;
          elemDofs[numDofPerNode*j + 2] = elemDofs[numDofPerNode*j + 0] + 2;
        }

        for (size_t j = 0; j < numNodesPerElem; j++)
          if (dirichlet_[elemNodes[j]]) {
            LO j0 = numDofPerNode*j+0;
            LO j1 = numDofPerNode*j+1;
            LO j2 = numDofPerNode*j+2;

            for (size_t k = 0; k < numDofPerElem; k++)
              KE[j0][k] = KE[k][j0] = KE[j1][k] = KE[k][j1] = KE[j2][k] = KE[k][j2] = zero;
            KE[j0][j0] = KE[j1][j1] = KE[j2][j2] = one;
          }

        // Insert KE into the global matrix
        // NOTE: KE is symmetric, therefore it does not matter that it is in the CSC format
        for (size_t j = 0; j < numDofPerElem; j++)
          this->A_->insertGlobalValues(elemDofs[j], elemDofs, Teuchos::ArrayView<SC>(KE[j], numDofPerElem));
      }
      this->A_->fillComplete();

      return this->A_;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    RCP<MultiVector> Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildCoords() {
      // FIXME: map here is an extended map, with multiple DOF per node
      // as we cannot construct a single DOF map in Problem, we repeat the coords
      this->Coords_ = MultiVectorTraits<Map,MultiVector>::Build(this->Map_, nDim);

      Teuchos::ArrayRCP<SC> x = this->Coords_->getDataNonConst(0);
      Teuchos::ArrayRCP<SC> y = this->Coords_->getDataNonConst(1);
      Teuchos::ArrayRCP<SC> z = this->Coords_->getDataNonConst(2);

      Teuchos::ArrayView<const GlobalOrdinal> GIDs = this->Map_->getNodeElementList();

      const SC hx = stretch[0], hy = stretch[1], hz = stretch[2];
      for (GO p = 0; p < GIDs.size(); p += 3) { // FIXME: we assume that DOF for the same node are label consequently
        GlobalOrdinal ind = GIDs[p] / 3;
        size_t i = ind % nx_, k = ind / (nx_*ny_), j = (ind - k*nx_*ny_) / nx_;

        x[p] = x[p+1] = x[p+2] = (i+1)*hx;
        y[p] = y[p+1] = y[p+2] = (j+1)*hy;
        z[p] = z[p+1] = z[p+2] = (k+1)*hz;
      }

      return this->Coords_;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    RCP<MultiVector> Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildNullspace() {
      this->Nullspace_ = MultiVectorTraits<Map,MultiVector>::Build(this->Map_, 6);

      if (this->Coords_ == Teuchos::null)
        BuildCoords();

      Teuchos::ArrayView<const GO> GIDs = this->Map_->getNodeElementList();

      size_t          numDofs = this->Map_->getNodeNumElements();
      Teuchos::ArrayRCP<SC> x = this->Coords_->getDataNonConst(0);
      Teuchos::ArrayRCP<SC> y = this->Coords_->getDataNonConst(1);
      Teuchos::ArrayRCP<SC> z = this->Coords_->getDataNonConst(2);

      SC one = Teuchos::ScalarTraits<SC>::one();

      // Translations
      Teuchos::ArrayRCP<SC> T0 = this->Nullspace_->getDataNonConst(0), T1 = this->Nullspace_->getDataNonConst(1), T2 = this->Nullspace_->getDataNonConst(2);
      for (size_t i = 0; i < numDofs; i += nDim) {
        T0[i]   = one;
        T1[i+1] = one;
        T2[i+2] = one;
      }

      // Calculate center
      Scalar cx = this->Coords_->getVector(0)->meanValue();
      Scalar cy = this->Coords_->getVector(1)->meanValue();
      Scalar cz = this->Coords_->getVector(2)->meanValue();

      // Rotations
      Teuchos::ArrayRCP<SC> R0 = this->Nullspace_->getDataNonConst(3), R1 = this->Nullspace_->getDataNonConst(4), R2 = this->Nullspace_->getDataNonConst(5);
      for (size_t i = 0; i < numDofs; i += nDim) {
        // Rotate in Y-Z Plane (around Z axis): [ -y; x]
        R0[i+0] = -(y[i]-cy);
        R0[i+1] =  (x[i]-cx);

        // Rotate in Y-Z Plane (around Z axis): [ -z; y]
        R1[i+1] = -(z[i]-cz);
        R1[i+2] =  (y[i]-cy);

        // Rotate in Y-Z Plane (around Z axis): [ z; -x]
        R2[i+0] =  (z[i]-cz);
        R2[i+2] = -(x[i]-cx);
      }
      // Normalize??

      return this->Nullspace_;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildMesh() {
      const SC hx = stretch[0], hy = stretch[1], hz = stretch[2];

      GO myPID = this->Map_->getComm()->getRank();
      GO nx = -1,                        ny = -1,                        nz = -1;
      GO mx = this->list_.get("mx", -1), my = this->list_.get("my", -1), mz = this->list_.get("mz", -1);
      GO shiftx, shifty, shiftz;

      Utils::getSubdomainData(dims[0], mx, myPID % mx, nx, shiftx);
      Utils::getSubdomainData(dims[1], my, (myPID - (mx*my) * (myPID / (mx*my)) / mx), ny, shifty);
      Utils::getSubdomainData(dims[2], mz, myPID / (mx*my), nz, shiftz);

      nodes        .resize((nx+1)*(ny+1)*(nz+1));
      dirichlet_   .resize((nx+1)*(ny+1)*(nz+1), 0);
      local2Global_.resize((nx+1)*(ny+1)*(nz+1));
      elements     .resize(nx*ny*nz);

#define NODE(i,j,k) ((k)*(ny+1)*(nx+1) + (j)*(nx+1) + (i))
#define CELL(i,j,k) ((k)*ny*nx         + (j)*nx     + (i))
      for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
          for (int i = 0; i <= nx; i++) {
            int ii = shiftx+i, jj = shifty+j, kk = shiftz+k;
            nodes[NODE(i,j,k)] = Point((ii+1)*hx, (jj+1)*hy, (kk+1)*hz);
            local2Global_[NODE(i,j,k)] = kk*nx_*ny_ + jj*nx_ + ii;

            if ((ii == 0   && (this->DirichletBC_ & DIR_LEFT))   ||
                (ii == nx  && (this->DirichletBC_ & DIR_RIGHT))  ||
                (jj == 0   && (this->DirichletBC_ & DIR_FRONT))  ||
                (jj == ny  && (this->DirichletBC_ & DIR_BACK))   ||
                (kk == 0   && (this->DirichletBC_ & DIR_BOTTOM)) ||
                (kk == nz  && (this->DirichletBC_ & DIR_TOP)))
              dirichlet_[NODE(i,j,k)] = 1;
          }

      for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
          for (int i = 0; i < nx; i++) {
            std::vector<int>& element = elements[CELL(i,j,k)];
            element.resize(8);
            element[0] = NODE(i,  j,   k  );
            element[1] = NODE(i+1,j,   k  );
            element[2] = NODE(i+1,j+1, k  );
            element[3] = NODE(i,  j+1, k  );
            element[4] = NODE(i,  j,   k+1);
            element[5] = NODE(i+1,j,   k+1);
            element[6] = NODE(i+1,j+1, k+1);
            element[7] = NODE(i,  j+1, k+1);
          }
#undef NODE
#undef CELL
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildMaterialMatrix(Teuchos::SerialDenseMatrix<LocalOrdinal,Scalar>& D) {
      D.shape(6,6);
      SC c = E / (1 + nu) / (1 - 2*nu);
      D(0,0) = c*(1-nu);      D(0,1) = c*nu;      D(0,2) = c*nu;
      D(1,0) = c*nu;          D(1,1) = c*(1-nu);  D(1,2) = c*nu;
      D(2,0) = c*nu;          D(2,1) = c*nu;      D(2,2) = c*(1-nu);
      D(3,3) = D(4,4) = D(5,5) = c*(1-2*nu)/2;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::BuildReferencePoints(size_t& numRefPoints, std::vector<Point>& refPoints, size_t& numGaussPoints, std::vector<Point>& gaussPoints) {
      numRefPoints   = 8;
      numGaussPoints = 8;
      refPoints  .resize(numRefPoints);
      gaussPoints.resize(numGaussPoints);

      refPoints[0] = Point(-1,-1,-1);
      refPoints[1] = Point( 1,-1,-1);
      refPoints[2] = Point( 1, 1,-1);
      refPoints[3] = Point(-1, 1,-1);
      refPoints[4] = Point(-1,-1, 1);
      refPoints[5] = Point( 1,-1, 1);
      refPoints[6] = Point( 1, 1, 1);
      refPoints[7] = Point(-1, 1, 1);

      // Gauss points (reference)
      SC sq3 = 1.0/sqrt(3);
      gaussPoints[0] = Point( sq3, sq3, sq3);
      gaussPoints[1] = Point( sq3,-sq3, sq3);
      gaussPoints[2] = Point(-sq3, sq3, sq3);
      gaussPoints[3] = Point(-sq3,-sq3, sq3);
      gaussPoints[4] = Point( sq3, sq3,-sq3);
      gaussPoints[5] = Point( sq3,-sq3,-sq3);
      gaussPoints[6] = Point(-sq3, sq3,-sq3);
      gaussPoints[7] = Point(-sq3,-sq3,-sq3);
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::EvalDxi(const std::vector<Point>& refPoints, Point& gaussPoint, SC * dxi) {
      for (size_t j = 0; j < refPoints.size(); j++)
        dxi[j] = refPoints[j].x * (1 + refPoints[j].y*gaussPoint.y) * (1 + refPoints[j].z*gaussPoint.z) / 8;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::EvalDeta(const std::vector<Point>& refPoints, Point& gaussPoint, SC * deta) {
      for (size_t j = 0; j < refPoints.size(); j++)
        deta[j] = (1 + refPoints[j].x*gaussPoint.x) * refPoints[j].y * (1 + refPoints[j].z*gaussPoint.z) / 8;
    }

    template <typename Scalar, typename LocalOrdinal, typename GlobalOrdinal, typename Map, typename Matrix, typename MultiVector>
    void Elasticity3DProblem<Scalar,LocalOrdinal,GlobalOrdinal,Map,Matrix,MultiVector>::EvalDzeta(const std::vector<Point>& refPoints, Point& gaussPoint, SC * dzeta) {
      for (size_t j = 0; j < refPoints.size(); j++)
        dzeta[j] = (1 + refPoints[j].x*gaussPoint.x) * (1 + refPoints[j].y*gaussPoint.y) * refPoints[j].z / 8;
    }

  } // namespace Xpetra

} // namespace Galeri

#endif // GALERI_ELASTICITY3DPROBLEM_HPP