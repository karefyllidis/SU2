/*!
 * \file CNewtonUpdateOnSubspace.hpp
 * \brief
 * \note Adopts some general layout from CQuasiNewtonInvLeastSquares.
 * \author O. Burghardt
 * \version 7.1.1 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "CQuasiNewtonInvLeastSquares.hpp"

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/QR>
//typedef Eigen::Matrix<su2double, Eigen::Dynamic, Eigen::Dynamic> EigenMatrix;
//typedef Eigen::Matrix<su2double, Eigen::Dynamic, 1> EigenVector;
//#include <Eigen/LU>
//#include <Eigen/SVD>
//typedef Eigen::JacobiSVD<EigenMatrix> EigenSVD;

/*--- Both classes operate on a window of past corrected solutions (X) and an
 *    data structure (R) of similar size to construct a (quasi-) Newton scheme.
 *    Have to identify a proper common base class. ---*/
template<class Scalar_t>
class CNewtonUpdateOnSubspace : public CQuasiNewtonInvLeastSquares<Scalar_t> {

public:

  using Scalar = Scalar_t;
  using Index = typename su2matrix<Scalar>::Index;

protected:

  using MPI_Wrapper = typename SelectMPIWrapper<Scalar>::W;

  using CQuasiNewtonInvLeastSquares<Scalar_t>::iSample;
  using CQuasiNewtonInvLeastSquares<Scalar_t>::nPtDomain;
  using CQuasiNewtonInvLeastSquares<Scalar_t>::work;        // the uncorrected (input) solutions and intermediate data
  using CQuasiNewtonInvLeastSquares<Scalar_t>::X;           // deltas of corrected solutions, nsample many
  using CQuasiNewtonInvLeastSquares<Scalar_t>::R;           // basis of unstable space, nbasis many

  /*--- Some more storage is needed because of the handling of two separated solution update strategies ---*/
  su2matrix<Scalar> work2;
  su2matrix<Scalar> p;                  // projected solution
  Eigen::VectorXd p_R;                  // projected solution in terms of basis R
  Eigen::VectorXd pn_R;                 // old projected solution in terms of basis R

  Eigen::MatrixXd EigenX;               // X in contiguous memory (Eigen object)
  Eigen::MatrixXd EigenR;               // R in contiguous memory (Eigen object)
  Eigen::MatrixXd DR;                   // Derivatives of basis vectors (needed for projected Jacobian)
  Eigen::MatrixXd ProjectedJacobian;    // Projected Jacobian to construct Newton step matrix
  Eigen::MatrixXd NewtonInverseMatrix;  // p = p_n + NewtonInverseMatrix*(p-p_n)

  Index iBasis = 0;
  Index BasisSize_n = 0;

  void shiftHistoryLeft(std::vector<su2matrix<Scalar>> &history) {
    for (Index i = 1; i < history.size(); ++i) {
      /*--- Swap instead of moving to re-use the memory of the first sample.
       *    This is why X and R are not stored as contiguous blocks of mem. ---*/
      std::swap(history[i-1], history[i]);
    }
  }

  void projectOntoSubspace() {

    /*--- save p_R to pn_R as needed for the Newton step ---*/
    pn_R = p_R;                                           // pn_R: z in original paper

    /*--- Get references ---*/
    Eigen::Map<Eigen::VectorXd> Eigen_work(work.data(),work.size());
    Eigen::Map<Eigen::VectorXd> Eigen_p(p.data(),p.size());

    /* --- Compute projection onto subspace of unstable/slow modes ---*/
    p_R = EigenR.transpose()*Eigen_work;                // p_R: \xi in original paper
    Eigen_p = EigenR*p_R;                               // p addresses: (uncorrected) projected solution in standard basis
  }

  void updateProjectedSolution() {

    if (EigenR.cols() > BasisSize_n) {
      pn_R = p_R;
      BasisSize_n = EigenR.cols();
    }

    /*--- Compute update w.r.t. subspace basis (Eq. (5.6) in original paper of Shroff & Keller). ---*/
    p_R = pn_R + NewtonInverseMatrix * (p_R - pn_R);
//    p_R = pn_R + (p_R - pn_R);                        // linear algebra sanity check

    /*--- Compute unstable part w.r.t. standard basis ---*/
    Eigen::Map<Eigen::VectorXd> Eigen_p(p.data(),p.size());
    Eigen_p = EigenR*p_R;                               // updated projected solution
  }

public:

  /*! \brief Default construction without allocation. */
  CNewtonUpdateOnSubspace() = default;

  /*! \brief Construction with allocation, see "resize". */
  CNewtonUpdateOnSubspace(Index nsample, Index npt, Index nvar, Index nptdomain = 0) {
    resize(nsample, npt, nvar, nptdomain);
  }

  /*!
   * \brief Resize the object.
   * \param[in] nsample - Number of samples used to build the FP history.
   * \param[in] nbasis - Dimension of basis the unstable space on which we apply the Newton update scheme.
   * \param[in] npt - Size of the solution including any halos.
   * \param[in] nvar - Number of solution variables.
   * \param[in] nptdomain - Local size (< npt), if 0 (default), MPI parallelization is skipped.
   */
  void resize(Index nsample, Index nbasis, Index npt, Index nvar, Index nptdomain = 0) {

    if (nptdomain > npt || nsample < 2)
      SU2_MPI::Error("Invalid Newton update parameters", CURRENT_FUNCTION);

    iSample = 0; iBasis = 0;
    nPtDomain = nptdomain? nptdomain : npt;
    work.resize(npt,nvar);
    work2.resize(npt,nvar);
    p.resize(npt,nvar);
    X.clear();                              // role here: store history of delta solutions in stable space
    R.clear();                              // role here: store basis of unstable subspace
    for (Index i = 0; i < nsample; ++i) {
      X.emplace_back(npt,nvar);
    }
    for (Index i = 0; i < nbasis; ++i) {
      R.emplace_back(npt,nvar);
    }
    X[0] = Scalar(0);
    R[0] = Scalar(0);

    EigenX.resize(npt*nvar,nsample);
    EigenR.resize(npt*nvar,1);
    DR.resize(npt*nvar,1);
  }

  /*! \brief Size of the object, the size of the subspace basis. */
  Index size() const { return R.size(); }

  /*! \brief Discard all history, keeping the current sample. */
  void reset() { std::swap(X[0], X[iSample]); iSample = 0; iBasis = 0; }

  /*!
   * \brief Check for new basis vector and eventually append to basis.
   */
  bool checkBasis(su2double KrylovCriterionValue) {

    /*--- Check whether we have collected enough samples, if not, return directly. ---*/
    if (iSample+1 < X.size())
      return false;

    if (iBasis < R.size()) {

      /*--- Create Eigen data structures for QR decomposition via Eigen ---*/
      for (Index i = 0; i < X.size(); ++i) {

        /*--- X is not stored in contiguous memory, copying it to an Eigen object is one
         *    alternative... (If it was, something like Map<MatrixXd> X(data, rows, cols)
         *    should be possible.) ---*/
        EigenX.col(i) = Eigen::VectorXd::Map(X[i].data(),X[0].size());
      }

      /* --- Compute QR decomposition and QR criterion ---*/
      Eigen::HouseholderQR<Eigen::MatrixXd> QR(EigenX.rows(),EigenX.cols());
      QR.compute(EigenX);
      auto Rdiag = QR.matrixQR().diagonal();

      std::vector<su2double> Krylov_Criterion_Quotients;
      Krylov_Criterion_Quotients.resize(X.size()-1);

      for (Index i = 0; i < X.size()-1; i++)
        if (Rdiag(i+1) != 0.0)
          Krylov_Criterion_Quotients[i] = Rdiag(i)/Rdiag(i+1);

      if ((abs(Krylov_Criterion_Quotients[0]) > KrylovCriterionValue) &&
          !(abs(Krylov_Criterion_Quotients[0])!=abs(Krylov_Criterion_Quotients[0]))
          ) {

        cout << "Krylov criterion fulfilled (" << Krylov_Criterion_Quotients[0] << "), appending new basis vector ... ";
        iBasis++;

        /*--- Get reference to new basis vector, extract first column from Q ---*/
        Eigen::Map<Eigen::VectorXd> Eigen_NewR(R[iBasis-1].data(),R[0].size());
        Eigen_NewR = QR.householderQ()*Eigen_NewR.setIdentity();

        for (Index i = 0; i < iBasis-1; ++i) {
          Eigen::Map<Eigen::VectorXd> PrecedingR(R[i].data(),R[0].size());
          Eigen_NewR = Eigen_NewR - Eigen_NewR.dot(PrecedingR)*PrecedingR;      // CHECK: this might be obsolete
        }
        Eigen_NewR.normalize();

        /*--- Update Eigen basis object ---*/
        EigenR.resize(EigenR.rows(),iBasis);
        for (Index i = 0; i < iBasis; ++i) {
          EigenR.col(i) = Eigen::VectorXd::Map(R[i].data(),R[0].size());
        }

        cout << "done." << endl;
        return true;
      }
    }
    else {
      // TODO: Maintain the basis
    }
    return false;
  }

  /*!
   * \brief Compute new projected subspace Jacobian and the inverse matrix for Newton steps.
   * \note To be used directly after basis dimension has been increased.
   */
  void computeProjectedJacobian(unsigned short iZone, su2matrix<int>& InputIndices, su2matrix<int>& OutputIndices) {

    ProjectedJacobian.resize(iBasis,iBasis);
    NewtonInverseMatrix.resize(iBasis,iBasis);
    DR.resize(DR.rows(),iBasis);

    cout << "Evaluate R^T (dG/du)^T R[i] for i = ";
    for (Index j = 0; j < iBasis; ++j) {

      AD::ClearAdjoints();
      for (Index it = 0; it < R[j].size(); ++it) {
        AD::SetDerivative(OutputIndices.data()[it], SU2_TYPE::GetValue(R[j].data()[it]));
      }
      AD::ComputeAdjoint();                                             // TODO: make this more efficient

      for (Index it = 0; it < R[j].size(); ++it)
        DR.col(j)(it) = AD::GetDerivative(InputIndices.data()[it]);     // extract DR = (dG/du)^T*R[j]
      for (Index i = 0; i < iBasis; ++i)
        ProjectedJacobian(i,j) = EigenR.col(i).transpose()*DR.col(j);   // R^T*DR

      cout << j+1 << ", ";
    }
    cout << "...";
    ProjectedJacobian = NewtonInverseMatrix.setIdentity() - ProjectedJacobian;
    NewtonInverseMatrix = ProjectedJacobian.inverse();
    cout << " done." << endl;
  }

  /*!
   * \brief Compute and return a new approximation.
   * \note To be used after storing the FP result.
   */
  const su2matrix<Scalar>& compute() {

    if (iBasis > 0) {

    /*--- Project solution-to-be-corrected update (loaded at work), store at p, coefficients at p_R. ---*/
      projectOntoSubspace();
//      SU2_OMP_SIMD
      for (Index i = 0; i < work.size(); ++i) {
        work.data()[i] = work.data()[i] - p.data()[i];              // work addresses: q
      }
    } else {
      for (Index i = 0; i < p.size(); ++i)
        p.data()[i] = 0.0;
    }

    /*--- Keep X updated to check for new basis elements. ---*/

    /*--- Check for need to shift left. ---*/
    if (iSample+1 == X.size()) {
      shiftHistoryLeft(X);
      iSample--;                                                    // X[0].data not needed anymore
    }
//    SU2_OMP_SIMD
    for (Index i = 0; i < work.size(); ++i) {
      work2.data()[i] = work.data()[i] - work2.data()[i];           // work2 addresses: delta q
    }
    std::swap(X[++iSample], work2);                                 // X[iSample] addresses: delta q, address under work2 is free
    std::swap(work2, work);                                         // work2 addresses: q

    /*--- Newton correction for the slow/unstable part of the solution update. ---*/
    if (iBasis > 0)
      updateProjectedSolution();

    /*--- Set the corrected new solution in work2---*/
//    SU2_OMP_SIMD
    for (Index i = 0; i < work.size(); ++i) {
      work.data()[i] = work2.data()[i] + p.data()[i];               // work addresses: corrected solution
    }

    return CQuasiNewtonInvLeastSquares<Scalar_t>::FPresult();
  }
};
