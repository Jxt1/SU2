﻿/*!
 * \file CNEMOEulerSolver.cpp
 * \brief Headers of the CNEMOEulerSolver class
 * \author C. Garbacz, W. Maier, S.R. Copeland.
 * \version 7.0.6 "Blackbird"
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

#include "../../../Common/include/toolboxes/printing_toolbox.hpp"
#include "../../include/solvers/CNEMOEulerSolver.hpp"
#include "../../include/fluid/CMutationTCLib.hpp"
#include "../../include/fluid/CUserDefinedTCLib.hpp"
#include "../../include/variables/CNEMONSVariable.hpp"

CNEMOEulerSolver::CNEMOEulerSolver(CGeometry *geometry, CConfig *config,
                                   unsigned short iMesh, const bool navier_stokes) :
  CFVMFlowSolverBase<CNEMOEulerVariable, COMPRESSIBLE>() {

  /*--- Based on the navier_stokes boolean, determine if this constructor is
   *    being called by itself, or by its derived class CNEMONSSolver. ---*/

  string description;
  if (navier_stokes) {
    description = "Navier-Stokes";
  }
  else {
    description = "Euler";
  }

  unsigned long iPoint, counter_local, counter_global = 0;
  unsigned short iDim, iMarker, iSpecies, nLineLets;
  unsigned short nZone = geometry->GetnZone();
  bool restart   = (config->GetRestart() || config->GetRestart_Flow());
  bool rans = (config->GetKind_Turb_Model() != NONE);
  unsigned short direct_diff = config->GetDirectDiff();
  int Unst_RestartIter = 0;
  bool dual_time = ((config->GetTime_Marching() == DT_STEPPING_1ST) ||
                    (config->GetTime_Marching() == DT_STEPPING_2ND));
  bool time_stepping = config->GetTime_Marching() == TIME_STEPPING;
  bool adjoint = config->GetDiscrete_Adjoint();
  string filename_ = "flow";
  su2double *Mvec_Inf;
  su2double Alpha, Beta;
  bool check_infty, nonPhys;
  su2double Soundspeed_Inf, sqvel;
  vector<su2double> Energies_Inf;

  /*--- Store the multigrid level. ---*/

  MGLevel = iMesh;

  /*--- Check for a restart file to evaluate if there is a change in the AoA
  before non-dimensionalizing ---*/

  if (!(!restart || (iMesh != MESH_0) || nZone >1 )) {

    /*--- Modify file name for a dual-time unsteady restart ---*/

    if (dual_time) {
      if (adjoint) Unst_RestartIter = SU2_TYPE::Int(config->GetUnst_AdjointIter())-1;
      else if (config->GetTime_Marching() == DT_STEPPING_1ST)
        Unst_RestartIter = SU2_TYPE::Int(config->GetRestart_Iter())-1;
      else Unst_RestartIter = SU2_TYPE::Int(config->GetRestart_Iter())-2;
    }

    /*--- Modify file name for a time stepping unsteady restart ---*/

    if (time_stepping) {
      if (adjoint) Unst_RestartIter = SU2_TYPE::Int(config->GetUnst_AdjointIter())-1;
      else Unst_RestartIter = SU2_TYPE::Int(config->GetRestart_Iter())-1;
    }

    filename_ = config->GetFilename(filename_, ".meta", Unst_RestartIter);

    /*--- Read and store the restart metadata ---*/

    Read_SU2_Restart_Metadata(geometry, config, false, filename_);

  }

  //todo this is nt used (i think)

  LowMach_Precontioner = nullptr;

  /*--- Set the gamma value ---*/
  //TODO compute this?

  Gamma = config->GetGamma();
  Gamma_Minus_One = Gamma - 1.0;

  /*--- Define geometric constants in the solver structure ---*/

  nSpecies     = config->GetnSpecies();
  nMarker      = config->GetnMarker_All();
  nDim         = geometry->GetnDim();
  nPoint       = geometry->GetnPoint();
  nPointDomain = geometry->GetnPointDomain();

  /*--- Set size of the conserved and primitive vectors ---*/
  //     U: [rho1, ..., rhoNs, rhou, rhov, rhow, rhoe, rhoeve]^T
  //     V: [rho1, ..., rhoNs, T, Tve, u, v, w, P, rho, h, a, rhoCvtr, rhoCvve]^T
  // GradV: [rho1, ..., rhoNs, T, Tve, u, v, w, P, rho, h, a, rhoCvtr, rhoCvve]^T
  // Viscous: append [mu, mu_t]^T

  nVar         = nSpecies + nDim + 2;
  if (navier_stokes) {
    nPrimVar   = nSpecies + nDim + 10;
  } else {
    nPrimVar   = nSpecies +nDim +8;
  }
  nPrimVarGrad = nSpecies + nDim + 8;
  //todo, need to clean up?

  /*--- Initialize nVarGrad for deallocation ---*/

  nVarGrad     = nPrimVarGrad;

  /*--- Store the number of vertices on each marker for deallocation ---*/

  nVertex = new unsigned long[nMarker];
  for (iMarker = 0; iMarker < nMarker; iMarker++)
    nVertex[iMarker] = geometry->nVertex[iMarker];

  MassFrac_Inf = config->GetGas_Composition();

  /*--- Perform the non-dimensionalization for the flow equations using the
    specified reference values. ---*/

  SetNondimensionalization(config, iMesh);

  /// TODO: This type of variables will be replaced.

  AllocateTerribleLegacyTemporaryVariables();

  /*--- Allocate base class members. ---*/

  Allocate(*config);

  /*--- Allocate Jacobians for implicit time-stepping ---*/
  if (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT) {

    /*--- Jacobians and vector  structures for implicit computations ---*/
    if (rank == MASTER_NODE) cout << "Initialize Jacobian structure (" << description << "). MG level: " << iMesh <<"." << endl;
    Jacobian.Initialize(nPoint, nPointDomain, nVar, nVar, true, geometry, config);

    if (config->GetKind_Linear_Solver_Prec() == LINELET) {
      nLineLets = Jacobian.BuildLineletPreconditioner(geometry, config);
      if (rank == MASTER_NODE) cout << "Compute linelet structure. " << nLineLets << " elements in each line (average)." << endl;
    }
  }
  else {
    if (rank == MASTER_NODE)  cout<< "Explicit Scheme. No Jacobian structure (" << description << "). MG level: " << iMesh <<"."<<endl;
  }

  /*--- Read farfield conditions from the config file ---*/

  Mach_Inf            = config->GetMach();
  Density_Inf         = config->GetDensity_FreeStreamND();
  Pressure_Inf        = config->GetPressure_FreeStreamND();
  Velocity_Inf        = config->GetVelocity_FreeStreamND();
  Temperature_Inf     = config->GetTemperature_FreeStreamND();
  Temperature_ve_Inf  = config->GetTemperature_ve_FreeStreamND();

  /*--- Initialize the secondary values for direct derivative approxiations ---*/

  switch(direct_diff) {
  case NO_DERIVATIVE:
    /*--- Default ---*/
    break;
  case D_DENSITY:
    SU2_TYPE::SetDerivative(Density_Inf, 1.0);
    break;
  case D_PRESSURE:
    SU2_TYPE::SetDerivative(Pressure_Inf, 1.0);
    break;
  case D_TEMPERATURE:
    SU2_TYPE::SetDerivative(Temperature_Inf, 1.0);
    break;
  case D_MACH: case D_AOA:
  case D_SIDESLIP: case D_REYNOLDS:
  case D_TURB2LAM: case D_DESIGN:
    /*--- Already done in postprocessing of config ---*/
    break;
  default:
    break;
  }

  /*--- Vectorize free stream Mach number based on AoA & AoS ---*/
  Mvec_Inf = new su2double[nDim];
  Alpha    = config->GetAoA()*PI_NUMBER/180.0;
  Beta     = config->GetAoS()*PI_NUMBER/180.0;
  if (nDim == 2) {
    Mvec_Inf[0] = cos(Alpha)*Mach_Inf;
    Mvec_Inf[1] = sin(Alpha)*Mach_Inf;
  }
  if (nDim == 3) {
    Mvec_Inf[0] = cos(Alpha)*cos(Beta)*Mach_Inf;
    Mvec_Inf[1] = sin(Beta)*Mach_Inf;
    Mvec_Inf[2] = sin(Alpha)*cos(Beta)*Mach_Inf;
  }

  /*--- Initialize the solution to the far-field state everywhere. ---*/

  if (navier_stokes) {
    nodes      = new CNEMONSVariable    (Pressure_Inf, MassFrac_Inf, Mvec_Inf,
                                         Temperature_Inf, Temperature_ve_Inf,
                                         nPoint, nDim, nVar, nPrimVar, nPrimVarGrad,
                                         config, FluidModel);
    node_infty = new CNEMONSVariable    (Pressure_Inf, MassFrac_Inf, Mvec_Inf,
                                         Temperature_Inf, Temperature_ve_Inf,
                                         1, nDim, nVar, nPrimVar, nPrimVarGrad,
                                         config, FluidModel);
  } else {
    nodes      = new CNEMOEulerVariable(Pressure_Inf, MassFrac_Inf, Mvec_Inf,
                                        Temperature_Inf, Temperature_ve_Inf,
                                        nPoint, nDim, nVar, nPrimVar, nPrimVarGrad,
                                        config, FluidModel);
    node_infty = new CNEMOEulerVariable(Pressure_Inf, MassFrac_Inf, Mvec_Inf,
                                        Temperature_Inf, Temperature_ve_Inf,
                                        1, nDim, nVar, nPrimVar, nPrimVarGrad,
                                        config, FluidModel);
  }
  SetBaseClassPointerToNodes();
  SetBaseClassPointerToNodeInfty();

  check_infty = node_infty->SetPrimVar(0, FluidModel);

  /*--- Check that the initial solution is physical, report any non-physical nodes ---*/

  counter_local = 0;
  for (iPoint = 0; iPoint < nPoint; iPoint++) {

    nonPhys = nodes->SetPrimVar(iPoint, FluidModel);

    /*--- Set mixture state ---*/

    FluidModel->SetTDStatePTTv(Pressure_Inf, MassFrac_Inf, Temperature_Inf, Temperature_ve_Inf);

    /*--- Compute other freestream quantities ---*/

    Density_Inf    = FluidModel->GetDensity();
    Soundspeed_Inf = FluidModel->GetSoundSpeed();

    sqvel = 0.0;
    for (iDim = 0; iDim < nDim; iDim++){
      sqvel += Mvec_Inf[iDim]*Soundspeed_Inf * Mvec_Inf[iDim]*Soundspeed_Inf;
    }
    Energies_Inf = FluidModel->GetMixtureEnergies();

    /*--- Initialize Solution & Solution_Old vectors ---*/

    for (iSpecies = 0; iSpecies < nSpecies; iSpecies++) {
      Solution[iSpecies]      = Density_Inf*MassFrac_Inf[iSpecies];
    }
    for (iDim = 0; iDim < nDim; iDim++) {
      Solution[nSpecies+iDim] = Density_Inf*Mvec_Inf[iDim]*Soundspeed_Inf;
    }
    Solution[nSpecies+nDim]     = Density_Inf*(Energies_Inf[0] + 0.5*sqvel);
    Solution[nSpecies+nDim+1]   = Density_Inf*Energies_Inf[1];
    nodes->SetSolution(iPoint,Solution);
    nodes->SetSolution_Old(iPoint,Solution);

    if(nonPhys)
      counter_local++;
  }

  /*--- Warning message about non-physical points ---*/

  if (config->GetComm_Level() == COMM_FULL) {

    SU2_MPI::Reduce(&counter_local, &counter_global, 1, MPI_UNSIGNED_LONG, MPI_SUM, MASTER_NODE, MPI_COMM_WORLD);

    if ((rank == MASTER_NODE) && (counter_global != 0))
      cout << "Warning. The original solution contains "<< counter_global << " points that are not physical." << endl;
  }

  /*--- Initial comms. ---*/

  CommunicateInitialState(geometry, config);

  /*--- Add the solver name (max 8 characters) ---*/

  SolverName = "NEMO.FLOW";

  /*--- Finally, check that the static arrays will be large enough (keep this
   *    check at the bottom to make sure we consider the "final" values). ---*/

  if((nDim > MAXNDIM) || (nPrimVar > MAXNVAR))
    SU2_MPI::Error("Oops! The CNEMOEulerSolver static array sizes are not large enough.",CURRENT_FUNCTION);

  /*--- Deallocate arrays ---*/

  delete [] Mvec_Inf;

}

CNEMOEulerSolver::~CNEMOEulerSolver(void) {
  unsigned short iVar;

  if (LowMach_Precontioner != nullptr) {
    for (iVar = 0; iVar < nVar; iVar ++)
      delete [] LowMach_Precontioner[iVar];
    delete [] LowMach_Precontioner;
  }

  delete node_infty;
  delete FluidModel;
}

void CNEMOEulerSolver::SetNondimensionalization(CConfig *config, unsigned short iMesh) {

  su2double
      Temperature_FreeStream = 0.0, Temperature_ve_FreeStream = 0.0, Mach2Vel_FreeStream         = 0.0,
      ModVel_FreeStream      = 0.0, Energy_FreeStream         = 0.0, ModVel_FreeStreamND         = 0.0,
      Velocity_Reynolds      = 0.0, Omega_FreeStream          = 0.0, Omega_FreeStreamND          = 0.0,
      Viscosity_FreeStream   = 0.0, Density_FreeStream        = 0.0, Pressure_FreeStream         = 0.0,
      Tke_FreeStream         = 0.0, Length_Ref                = 0.0, Density_Ref                 = 0.0,
      Pressure_Ref           = 0.0, Velocity_Ref              = 0.0, Temperature_Ref             = 0.0,
      Temperature_ve_Ref     = 0.0, Time_Ref                  = 0.0, Omega_Ref                   = 0.0,
      Force_Ref              = 0.0, Gas_Constant_Ref          = 0.0, Viscosity_Ref               = 0.0,
      Conductivity_Ref       = 0.0, Energy_Ref                = 0.0, Pressure_FreeStreamND       = 0.0,
      Energy_FreeStreamND    = 0.0, Temperature_FreeStreamND  = 0.0, Temperature_ve_FreeStreamND = 0.0,
      Gas_ConstantND         = 0.0, Viscosity_FreeStreamND    = 0.0, sqvel                       = 0.0,
      Tke_FreeStreamND       = 0.0, Total_UnstTimeND          = 0.0, Delta_UnstTimeND            = 0.0,
      soundspeed             = 0.0, GasConstant_Inf           = 0.0, Froude                      = 0.0,
      Density_FreeStreamND   = 0.0;

  su2double Velocity_FreeStreamND[3] = {0.0, 0.0, 0.0};
  vector<su2double> energies;

  unsigned short iDim;

  /*--- Local variables ---*/

  su2double Alpha         = config->GetAoA()*PI_NUMBER/180.0;
  su2double Beta          = config->GetAoS()*PI_NUMBER/180.0;
  su2double Mach          = config->GetMach();
  su2double Reynolds      = config->GetReynolds();

  bool unsteady           = (config->GetTime_Marching() != NO);
  bool viscous            = config->GetViscous();
  bool dynamic_grid       = config->GetGrid_Movement();
  bool gravity            = config->GetGravityForce();
  bool turbulent          = (config->GetKind_Turb_Model() != NONE);
  bool tkeNeeded          = (turbulent && (config->GetKind_Turb_Model() == SST || config->GetKind_Turb_Model() == SST_SUST));
  bool reynolds_init      = (config->GetKind_InitOption() == REYNOLDS);

  //TODO/
  //  /*--- Create one final fluid model object per OpenMP thread to be able to use them in parallel.
  //   *    GetFluidModel() should be used to automatically access the "right" object of each thread. ---*/

  //  assert(FluidModel.empty() && "Potential memory leak!");
  //  FluidModel.resize(omp_get_max_threads());
  //  SU2_OMP_PARALLEL
  //    {
  //      const int thread = omp_get_thread_num();

  //      switch (config->GetKind_FluidModel()) {

  //        case MUTATIONPP:
  //          FluidModel[thread] = new CMutationGas(config->GetGasModel(), config->GetKind_TransCoeffModel());
  //          break;

  //        case USER_DEFINED_NONEQ:
  //          FluidModel[thread] = new CUserDefinedTCLib(config, nDim, viscous);
  //          break;
  //  }

  /*--- Instatiate the fluid model ---*/

  switch (config->GetKind_FluidModel()) {
  case MUTATIONPP:
    //FluidModel = new CMutationGas(config->GetGasModel(), config->GetKind_TransCoeffModel());
    break;
  case USER_DEFINED_NONEQ:
    FluidModel = new CUserDefinedTCLib(config, nDim, viscous);
    break;
  }

  /*--- Compute the Free Stream Pressure, Temperatrue, and Density ---*/

  Pressure_FreeStream        = config->GetPressure_FreeStream();
  Temperature_FreeStream     = config->GetTemperature_FreeStream();
  Temperature_ve_FreeStream  = config->GetTemperature_ve_FreeStream();

  /*--- Compressible non dimensionalization ---*/

  /*--- Set mixture state based on pressure, mass fractions and temperatures ---*/

  FluidModel->SetTDStatePTTv(Pressure_FreeStream, MassFrac_Inf, Temperature_FreeStream, Temperature_ve_FreeStream);

  /*--- Compute Gas Constant ---*/

  GasConstant_Inf = FluidModel->GetGasConstant();
  config->SetGas_Constant(GasConstant_Inf);

  /*--- Compute the freestream density, soundspeed ---*/

  Density_FreeStream = FluidModel->GetDensity();
  soundspeed = FluidModel->GetSoundSpeed();

  /*--- Compute the Free Stream velocity, using the Mach number ---*/

  if (nDim == 2) {
    config->GetVelocity_FreeStream()[0] = cos(Alpha)*Mach*soundspeed;
    config->GetVelocity_FreeStream()[1] = sin(Alpha)*Mach*soundspeed;
  }
  if (nDim == 3) {
    config->GetVelocity_FreeStream()[0] = cos(Alpha)*cos(Beta)*Mach*soundspeed;
    config->GetVelocity_FreeStream()[1] = sin(Beta)*Mach*soundspeed;
    config->GetVelocity_FreeStream()[2] = sin(Alpha)*cos(Beta)*Mach*soundspeed;
  }

  /*--- Compute the modulus of the free stream velocity ---*/

  ModVel_FreeStream = 0.0;
  for (iDim = 0; iDim < nDim; iDim++){
    ModVel_FreeStream += config->GetVelocity_FreeStream()[iDim]*config->GetVelocity_FreeStream()[iDim];
  }
  sqvel = ModVel_FreeStream;
  ModVel_FreeStream = sqrt(ModVel_FreeStream); config->SetModVel_FreeStream(ModVel_FreeStream);

  /*--- Calculate energies ---*/

  energies = FluidModel->GetMixtureEnergies();

  /*--- Viscous initialization ---*/

  if (viscous) {

    /*--- The dimensional viscosity is needed to determine the free-stream conditions.
          To accomplish this, simply set the non-dimensional coefficients to the
          dimensional ones. This will be overruled later.---*/

    config->SetMu_RefND(config->GetMu_Ref());
    config->SetMu_Temperature_RefND(config->GetMu_Temperature_Ref());
    config->SetMu_SND(config->GetMu_S());
    config->SetMu_ConstantND(config->GetMu_Constant());

    /*--- Reynolds based initialization ---*/

    if (reynolds_init) {

      /*--- First, check if there is mesh motion. If yes, use the Mach
         number relative to the body to initialize the flow. ---*/

      if (dynamic_grid) Velocity_Reynolds = config->GetMach_Motion()*Mach2Vel_FreeStream;
      else Velocity_Reynolds = ModVel_FreeStream;

      /*--- For viscous flows, pressure will be computed from a density
            that is found from the Reynolds number. The viscosity is computed
            from the dimensional version of Sutherland's law or the constant
            viscosity, depending on the input option.---*/

      // TODO, THIS NEEDS TO BE REVISITED
      Viscosity_FreeStream = 1.853E-5*(pow(Temperature_FreeStream/300.0,3.0/2.0) * (300.0+110.3)/(Temperature_FreeStream+110.3));
      Density_FreeStream   = Reynolds*Viscosity_FreeStream/(Velocity_Reynolds* config->GetLength_Reynolds());
      Pressure_FreeStream  = Density_FreeStream*GasConstant_Inf*Temperature_FreeStream;
      Energy_FreeStream    = Pressure_FreeStream/(Density_FreeStream*Gamma_Minus_One)+0.5*ModVel_FreeStream *ModVel_FreeStream;

      config->SetViscosity_FreeStream(Viscosity_FreeStream);
      config->SetPressure_FreeStream(Pressure_FreeStream);
    }

    else {

      /*--- Thermodynamics quantities based initialization ---*/

      Viscosity_FreeStream = 1.853E-5*(pow(Temperature_FreeStream/300.0,3.0/2.0) * (300.0+110.3)/(Temperature_FreeStream+110.3));
      Density_FreeStream   = Reynolds*Viscosity_FreeStream/(Velocity_Reynolds* config->GetLength_Reynolds());
      Pressure_FreeStream  = Density_FreeStream*GasConstant_Inf*Temperature_FreeStream;
      Energy_FreeStream    = Pressure_FreeStream/(Density_FreeStream*Gamma_Minus_One)+0.5*ModVel_FreeStream *ModVel_FreeStream ;
    }

    /*--- Turbulence kinetic energy ---*/

    Tke_FreeStream  = 3.0/2.0*(ModVel_FreeStream*ModVel_FreeStream*config->GetTurbulenceIntensity_FreeStream()*config->GetTurbulenceIntensity_FreeStream());

  }
  else {

    /*--- For inviscid flow, energy is calculated from the specified
       FreeStream quantities using the proper gas law. ---*/
    Energy_FreeStream    = energies[0] + 0.5*sqvel;
  }

  config->SetDensity_FreeStream(Density_FreeStream);

  /*-- Compute the freestream energy. ---*/

  if (tkeNeeded) { Energy_FreeStream += Tke_FreeStream; }; config->SetEnergy_FreeStream(Energy_FreeStream);

  /*--- Compute non dimensional quantities. By definition,
     Lref is one because we have converted the grid to meters. ---*/

  if (config->GetRef_NonDim() == DIMENSIONAL) {
    Pressure_Ref       = 1.0;
    Density_Ref        = 1.0;
    Temperature_Ref    = 1.0;
    Temperature_ve_Ref = 1.0;
  }
  else if (config->GetRef_NonDim() == FREESTREAM_PRESS_EQ_ONE) {
    Pressure_Ref       = Pressure_FreeStream;       // Pressure_FreeStream = 1.0
    Density_Ref        = Density_FreeStream;        // Density_FreeStream = 1.0
    Temperature_Ref    = Temperature_FreeStream;    // Temperature_FreeStream = 1.0
    Temperature_ve_Ref = Temperature_ve_FreeStream; // Temperature_ve_FreeStream = 1.0
  }
  else if (config->GetRef_NonDim() == FREESTREAM_VEL_EQ_MACH) {
    Pressure_Ref       = Gamma*Pressure_FreeStream; // Pressure_FreeStream = 1.0/Gamma
    Density_Ref        = Density_FreeStream;        // Density_FreeStream = 1.0
    Temperature_Ref    = Temperature_FreeStream;    // Temp_FreeStream = 1.0
    Temperature_ve_Ref = Temperature_ve_FreeStream; // Temp_ve_FreeStream = 1.0
  }
  else if (config->GetRef_NonDim() == FREESTREAM_VEL_EQ_ONE) {
    Pressure_Ref       = Mach*Mach*Gamma*Pressure_FreeStream; // Pressure_FreeStream = 1.0/(Gamma*(M_inf)^2)
    Density_Ref        = Density_FreeStream;                  // Density_FreeStream = 1.0
    Temperature_Ref    = Temperature_FreeStream;              // Temp_FreeStream = 1.0
    Temperature_ve_Ref = Temperature_ve_FreeStream;           // Temp_ve_FreeStream = 1.0
  }
  config->SetPressure_Ref(Pressure_Ref);
  config->SetDensity_Ref(Density_Ref);
  config->SetTemperature_Ref(Temperature_Ref);
  config->SetTemperature_ve_Ref(Temperature_ve_Ref);

  Length_Ref        = 1.0;                                                         config->SetLength_Ref(Length_Ref);
  Velocity_Ref      = sqrt(config->GetPressure_Ref()/config->GetDensity_Ref());    config->SetVelocity_Ref(Velocity_Ref);
  Time_Ref          = Length_Ref/Velocity_Ref;                                     config->SetTime_Ref(Time_Ref);
  Omega_Ref         = Velocity_Ref/Length_Ref;                                     config->SetOmega_Ref(Omega_Ref);
  Force_Ref         = config->GetDensity_Ref()*Velocity_Ref*Velocity_Ref*Length_Ref*Length_Ref; config->SetForce_Ref(Force_Ref);
  Gas_Constant_Ref  = Velocity_Ref*Velocity_Ref/config->GetTemperature_Ref();      config->SetGas_Constant_Ref(Gas_Constant_Ref);
  Viscosity_Ref     = config->GetDensity_Ref()*Velocity_Ref*Length_Ref;            config->SetViscosity_Ref(Viscosity_Ref);
  Conductivity_Ref  = Viscosity_Ref*Gas_Constant_Ref;                              config->SetConductivity_Ref(Conductivity_Ref);
  Froude            = ModVel_FreeStream/sqrt(STANDARD_GRAVITY*Length_Ref);         config->SetFroude(Froude);

  /*--- Divide by reference values, to compute the non-dimensional free-stream values ---*/

  Pressure_FreeStreamND = Pressure_FreeStream/config->GetPressure_Ref();  config->SetPressure_FreeStreamND(Pressure_FreeStreamND);
  Density_FreeStreamND  = Density_FreeStream/config->GetDensity_Ref();    config->SetDensity_FreeStreamND(Density_FreeStreamND);

  for (iDim = 0; iDim < nDim; iDim++) {
    Velocity_FreeStreamND[iDim] = config->GetVelocity_FreeStream()[iDim]/Velocity_Ref; config->SetVelocity_FreeStreamND(Velocity_FreeStreamND[iDim], iDim);
  }

  Temperature_FreeStreamND    = Temperature_FreeStream/config->GetTemperature_Ref();       config->SetTemperature_FreeStreamND(Temperature_FreeStreamND);
  Temperature_ve_FreeStreamND = Temperature_ve_FreeStream/config->GetTemperature_ve_Ref(); config->SetTemperature_ve_FreeStreamND(Temperature_ve_FreeStreamND);
  Gas_ConstantND              = config->GetGas_Constant()/Gas_Constant_Ref;                config->SetGas_ConstantND(Gas_ConstantND);

  ModVel_FreeStreamND = 0.0;
  for (iDim = 0; iDim < nDim; iDim++) ModVel_FreeStreamND += Velocity_FreeStreamND[iDim]*Velocity_FreeStreamND[iDim];
  ModVel_FreeStreamND    = sqrt(ModVel_FreeStreamND); config->SetModVel_FreeStreamND(ModVel_FreeStreamND);

  Viscosity_FreeStreamND = Viscosity_FreeStream / Viscosity_Ref;   config->SetViscosity_FreeStreamND(Viscosity_FreeStreamND);

  Tke_FreeStream  = 3.0/2.0*(ModVel_FreeStream*ModVel_FreeStream*config->GetTurbulenceIntensity_FreeStream()*config->GetTurbulenceIntensity_FreeStream());
  config->SetTke_FreeStream(Tke_FreeStream);

  Tke_FreeStreamND  = 3.0/2.0*(ModVel_FreeStreamND*ModVel_FreeStreamND*config->GetTurbulenceIntensity_FreeStream()*config->GetTurbulenceIntensity_FreeStream());
  config->SetTke_FreeStreamND(Tke_FreeStreamND);

  Omega_FreeStream = Density_FreeStream*Tke_FreeStream/(Viscosity_FreeStream*config->GetTurb2LamViscRatio_FreeStream());
  config->SetOmega_FreeStream(Omega_FreeStream);

  Omega_FreeStreamND = Density_FreeStreamND*Tke_FreeStreamND/(Viscosity_FreeStreamND*config->GetTurb2LamViscRatio_FreeStream());
  config->SetOmega_FreeStreamND(Omega_FreeStreamND);

  /*--- Initialize the dimensionless Fluid Model that will be used to solve the dimensionless problem ---*/

  Energy_FreeStreamND = Pressure_FreeStreamND/(Density_FreeStreamND*Gamma_Minus_One)+0.5*ModVel_FreeStreamND *ModVel_FreeStreamND;

  if (viscous) {

    /*--- Constant viscosity model ---*/

    config->SetMu_ConstantND(config->GetMu_Constant()/Viscosity_Ref);

    /*--- Sutherland's model ---*/

    config->SetMu_RefND(config->GetMu_Ref()/Viscosity_Ref);
    config->SetMu_SND(config->GetMu_S()/config->GetTemperature_Ref());
    config->SetMu_Temperature_RefND(config->GetMu_Temperature_Ref()/config->GetTemperature_Ref());

    /* constant thermal conductivity model */
    config->SetKt_ConstantND(config->GetKt_Constant()/Conductivity_Ref);

  }

  if (tkeNeeded) { Energy_FreeStreamND += Tke_FreeStreamND; };  config->SetEnergy_FreeStreamND(Energy_FreeStreamND);

  Energy_Ref = Energy_FreeStream/Energy_FreeStreamND; config->SetEnergy_Ref(Energy_Ref);

  Total_UnstTimeND = config->GetTotal_UnstTime() / Time_Ref;    config->SetTotal_UnstTimeND(Total_UnstTimeND);
  Delta_UnstTimeND = config->GetDelta_UnstTime() / Time_Ref;    config->SetDelta_UnstTimeND(Delta_UnstTimeND);

  /*--- Write output to the console if this is the master node and first domain ---*/

  if ((rank == MASTER_NODE) && (iMesh == MESH_0)) {

    cout.precision(6);

    if (viscous) {
      cout << "Viscous flow: Computing pressure using the equation of state for multi-species and multi-temperatures." << endl;
      cout << "based on the free-stream temperatures and a density computed" << endl;
      cout << "from the Reynolds number." << endl;
    } else {
      cout << "Inviscid flow: Computing density based on free-stream" << endl;
      cout << "and pressure using the the equation of state for multi-species and multi-temperatures." << endl;
    }

    if (dynamic_grid) cout << "Force coefficients computed using MACH_MOTION." << endl;
    else cout << "Force coefficients computed using free-stream values." << endl;

    stringstream NonDimTableOut, ModelTableOut;
    stringstream Unit;

    cout << endl;
    PrintingToolbox::CTablePrinter ModelTable(&ModelTableOut);
    ModelTableOut <<"-- Models:"<< endl;

    ModelTable.AddColumn("Transport Model", 38);
    ModelTable.AddColumn("Fluid Model", 38);
    ModelTable.SetAlign(PrintingToolbox::CTablePrinter::RIGHT);
    ModelTable.PrintHeader();

    PrintingToolbox::CTablePrinter NonDimTable(&NonDimTableOut);
    NonDimTable.AddColumn("Name", 22);
    NonDimTable.AddColumn("Dim. value", 14);
    NonDimTable.AddColumn("Ref. value", 14);
    NonDimTable.AddColumn("Unit", 10);
    NonDimTable.AddColumn("Non-dim. value", 14);
    NonDimTable.SetAlign(PrintingToolbox::CTablePrinter::RIGHT);

    NonDimTableOut <<"-- Fluid properties:"<< endl;

    NonDimTable.PrintHeader();

    if (viscous) {

      switch(config->GetKind_TransCoeffModel()){
      case WILKE:
        ModelTable << "Wilke-Blottner-Eucken ";
        if (config->GetSystemMeasurements() == SI) cout << " N.s/m^2." << endl;
        else if (config->GetSystemMeasurements() == US) cout << " lbf.s/ft^2." << endl;
        NonDimTable << "Viscosity" << config->GetMu_Constant() << config->GetMu_Constant()/config->GetMu_ConstantND() << Unit.str() << config->GetMu_ConstantND();
        Unit.str("");
        NonDimTable.PrintFooter();
        break;

      case GUPTAYOS:
        ModelTable << "Gupta-Yos";
        if (config->GetSystemMeasurements() == SI) cout << " N.s/m^2." << endl;
        else if (config->GetSystemMeasurements() == US) cout << " lbf.s/ft^2." << endl;
        NonDimTable << "Viscosity" << config->GetMu_Constant() << config->GetMu_Constant()/config->GetMu_ConstantND() << Unit.str() << config->GetMu_ConstantND();
        Unit.str("");
        NonDimTable.PrintFooter();
        break;
      default:
        break;
      }
    } else {
      ModelTable << "-" ;
    }

    if      (config->GetSystemMeasurements() == SI) Unit << "N.m/kg.K";
    else if (config->GetSystemMeasurements() == US) Unit << "lbf.ft/slug.R";
    NonDimTable << "Gas Constant" << config->GetGas_Constant() << config->GetGas_Constant_Ref() << Unit.str() << config->GetGas_ConstantND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "N.m/kg.K";
    else if (config->GetSystemMeasurements() == US) Unit << "lbf.ft/slug.R";
    NonDimTable << "Spec. Heat Ratio" << "-" << "-" << "-" << Gamma;
    Unit.str("");
    switch(config->GetKind_FluidModel()){
    case USER_DEFINED_NONEQ:
      ModelTable << "Park Two-Temperature";
      break;
    case MUTATIONPP:
      ModelTable << "Mutation++ Library";
      break;
    }

    NonDimTable.PrintFooter();
    NonDimTableOut <<"-- Initial and free-stream conditions:"<< endl;
    NonDimTable.PrintHeader();

    NonDimTable << "Mixture" << config->GetGasModel() << "-"<< "-"<< config->GetGasModel();
    Unit.str("");

    if      (config->GetSystemMeasurements() == SI) Unit << "Pa";
    else if (config->GetSystemMeasurements() == US) Unit << "psf";
    NonDimTable << "Static Pressure" << config->GetPressure_FreeStream() << config->GetPressure_Ref() << Unit.str() << config->GetPressure_FreeStreamND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "kg/m^3";
    else if (config->GetSystemMeasurements() == US) Unit << "slug/ft^3";
    NonDimTable << "Density" << config->GetDensity_FreeStream() << config->GetDensity_Ref() << Unit.str() << config->GetDensity_FreeStreamND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "K";
    else if (config->GetSystemMeasurements() == US) Unit << "R";
    NonDimTable << " T-R Temperature" << config->GetTemperature_FreeStream() << config->GetTemperature_Ref() << Unit.str() << config->GetTemperature_FreeStreamND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "K";
    else if (config->GetSystemMeasurements() == US) Unit << "R";
    NonDimTable << " V-E Temperature" << config->GetTemperature_ve_FreeStream() << config->GetTemperature_ve_Ref() << Unit.str() << config->GetTemperature_ve_FreeStreamND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "m^2/s^2";
    else if (config->GetSystemMeasurements() == US) Unit << "ft^2/s^2";
    NonDimTable << "Total Energy" << config->GetEnergy_FreeStream() << config->GetEnergy_Ref() << Unit.str() << config->GetEnergy_FreeStreamND();
    Unit.str("");
    if      (config->GetSystemMeasurements() == SI) Unit << "m/s";
    else if (config->GetSystemMeasurements() == US) Unit << "ft/s";
    NonDimTable << "Velocity-X" << config->GetVelocity_FreeStream()[0] << config->GetVelocity_Ref() << Unit.str() << config->GetVelocity_FreeStreamND()[0];
    NonDimTable << "Velocity-Y" << config->GetVelocity_FreeStream()[1] << config->GetVelocity_Ref() << Unit.str() << config->GetVelocity_FreeStreamND()[1];
    if (nDim == 3){
      NonDimTable << "Velocity-Z" << config->GetVelocity_FreeStream()[2] << config->GetVelocity_Ref() << Unit.str() << config->GetVelocity_FreeStreamND()[2];
    }
    NonDimTable << "Velocity Magnitude" << config->GetModVel_FreeStream() << config->GetVelocity_Ref() << Unit.str() << config->GetModVel_FreeStreamND();
    Unit.str("");

    if (viscous){
      NonDimTable.PrintFooter();
      if      (config->GetSystemMeasurements() == SI) Unit << "N.s/m^2";
      else if (config->GetSystemMeasurements() == US) Unit << "lbf.s/ft^2";
      NonDimTable << "Viscosity" << config->GetViscosity_FreeStream() << config->GetViscosity_Ref() << Unit.str() << config->GetViscosity_FreeStreamND();
      Unit.str("");
      if      (config->GetSystemMeasurements() == SI) Unit << "W/m^2.K";
      else if (config->GetSystemMeasurements() == US) Unit << "lbf/ft.s.R";
      NonDimTable << "Conductivity" << "-" << config->GetConductivity_Ref() << Unit.str() << "-";
      Unit.str("");
      if (turbulent){
        if      (config->GetSystemMeasurements() == SI) Unit << "m^2/s^2";
        else if (config->GetSystemMeasurements() == US) Unit << "ft^2/s^2";
        NonDimTable << "Turb. Kin. Energy" << config->GetTke_FreeStream() << config->GetTke_FreeStream()/config->GetTke_FreeStreamND() << Unit.str() << config->GetTke_FreeStreamND();
        Unit.str("");
        if      (config->GetSystemMeasurements() == SI) Unit << "1/s";
        else if (config->GetSystemMeasurements() == US) Unit << "1/s";
        NonDimTable << "Spec. Dissipation" << config->GetOmega_FreeStream() << config->GetOmega_FreeStream()/config->GetOmega_FreeStreamND() << Unit.str() << config->GetOmega_FreeStreamND();
        Unit.str("");
      }
    }

    NonDimTable.PrintFooter();
    NonDimTable << "Mach Number" << "-" << "-" << "-" << config->GetMach();
    if (viscous){
      NonDimTable << "Reynolds Number" << "-" << "-" << "-" << config->GetReynolds();
    }
    if (gravity) {
      NonDimTable << "Froude Number" << "-" << "-" << "-" << Froude;
      NonDimTable << "Wave Length"   << "-" << "-" << "-" << 2.0*PI_NUMBER*Froude*Froude;
    }
    NonDimTable.PrintFooter();
    ModelTable.PrintFooter();

    if (unsteady){
      NonDimTableOut << "-- Unsteady conditions" << endl;
      NonDimTable.PrintHeader();
      NonDimTable << "Total Time" << config->GetMax_Time() << config->GetTime_Ref() << "s" << config->GetMax_Time()/config->GetTime_Ref();
      Unit.str("");
      NonDimTable << "Time Step" << config->GetTime_Step() << config->GetTime_Ref() << "s" << config->GetDelta_UnstTimeND();
      Unit.str("");
      NonDimTable.PrintFooter();
    }

    cout << ModelTableOut.str();
    cout << NonDimTableOut.str();

  }

}

void CNEMOEulerSolver::SetInitialCondition(CGeometry **geometry, CSolver ***solver_container, CConfig *config, unsigned long TimeIter) {


  bool restart = (config->GetRestart() || config->GetRestart_Flow());
  bool rans = (config->GetKind_Turb_Model() != NONE);
  bool dual_time = ((config->GetTime_Marching() == DT_STEPPING_1ST) ||
                    (config->GetTime_Marching() == DT_STEPPING_2ND));

  /*--- Start OpenMP parallel region. ---*/

  SU2_OMP_PARALLEL
  {
    unsigned long iPoint;
    unsigned short iMesh;

    /*--- Make sure that the solution is well initialized for unsteady
     calculations with dual time-stepping (load additional restarts for 2nd-order). ---*/
    if (dual_time && ((TimeIter == 0) || (restart && (TimeIter == config->GetRestart_Iter()))) ) {
      PushSolutionBackInTime(TimeIter, restart, rans, solver_container, geometry, config);
    }

  } // end SU2_OMP_PARALLEL

}

void CNEMOEulerSolver::CommonPreprocessing(CGeometry *geometry, CSolver **solver_container, CConfig *config, unsigned short iMesh,
                                           unsigned short iRKStep, unsigned short RunTime_EqSystem, bool Output) {


  bool cont_adjoint     = config->GetContinuous_Adjoint();
  bool disc_adjoint     = config->GetDiscrete_Adjoint();
  bool implicit         = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  bool center           = (config->GetKind_ConvNumScheme_Flow() == SPACE_CENTERED) ||
                          (cont_adjoint && config->GetKind_ConvNumScheme_AdjFlow() == SPACE_CENTERED);
  bool center_jst       = (config->GetKind_Centered_Flow() == JST) && (iMesh == MESH_0);
  bool center_jst_ke    = (config->GetKind_Centered_Flow() == JST_KE) && (iMesh == MESH_0);
  unsigned short kind_row_dissipation = config->GetKind_RoeLowDiss();
  bool roe_low_dissipation  = (kind_row_dissipation != NO_ROELOWDISS) &&
                              (config->GetKind_Upwind_Flow() == ROE ||
                               config->GetKind_Upwind_Flow() == SLAU ||
                               config->GetKind_Upwind_Flow() == SLAU2);


  /*--- Set the primitive variables ---*/

  SU2_OMP_MASTER
  ErrorCounter = 0;
  SU2_OMP_BARRIER

  SU2_OMP_ATOMIC
  ErrorCounter += SetPrimitive_Variables(solver_container, config, Output);

  if ((iMesh == MESH_0) && (config->GetComm_Level() == COMM_FULL)) {
     SU2_OMP_BARRIER
     SU2_OMP_MASTER
     {
       unsigned long tmp = ErrorCounter;
       SU2_MPI::Allreduce(&tmp, &ErrorCounter, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
       config->SetNonphysical_Points(ErrorCounter);
     }
     SU2_OMP_BARRIER
  }

  /*--- Artificial dissipation ---*/

  if (center && !Output) {
    SetMax_Eigenvalue(geometry, config);
    if (center_jst) SetUndivided_Laplacian(geometry, config);
    if (center_jst || center_jst_ke) SetCentered_Dissipation_Sensor(geometry, config);
  }

  /*--- Initialize the Jacobian matrix and residual, not needed for the reducer strategy
   *    as we set blocks (including diagonal ones) and completely overwrite. ---*/

  if(!ReducerStrategy && !Output) {
    LinSysRes.SetValZero();
    if (implicit) Jacobian.SetValZero();
    else {SU2_OMP_BARRIER} // because of "nowait" in LinSysRes
  }

}

void CNEMOEulerSolver::Preprocessing(CGeometry *geometry, CSolver **solver_container,
                                     CConfig *config, unsigned short iMesh,
                                     unsigned short iRKStep,
                                     unsigned short RunTime_EqSystem, bool Output) {


  unsigned long InnerIter = config->GetInnerIter();
  bool disc_adjoint     = config->GetDiscrete_Adjoint();
  bool muscl            = config->GetMUSCL_Flow();
  bool limiter          = ((config->GetKind_SlopeLimit_Flow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter()) && !(disc_adjoint && config->GetFrozen_Limiter_Disc()));
  bool center           = config->GetKind_ConvNumScheme_Flow() == SPACE_CENTERED;
  bool van_albada       = config->GetKind_SlopeLimit_Flow() == VAN_ALBADA_EDGE;


  /*--- Common preprocessing steps ---*/

  CommonPreprocessing(geometry, solver_container, config, iMesh, iRKStep, RunTime_EqSystem, Output);

  /*--- Upwind second order reconstruction ---*/

  if ((muscl && !center) && (iMesh == MESH_0) && !Output) {

    /*--- Calculate the gradients ---*/
    if (config->GetKind_Gradient_Method() == GREEN_GAUSS) {
      SetSolution_Gradient_GG(geometry, config, true);
    }
    if (config->GetKind_Gradient_Method() == WEIGHTED_LEAST_SQUARES) {
      SetSolution_Gradient_LS(geometry, config, true);
    }

    /*--- Limiter computation ---*/
    if ((limiter) && (iMesh == MESH_0) && !Output && !van_albada) {
      SetSolution_Limiter(geometry, config);
    }

  }
 }

unsigned long CNEMOEulerSolver::SetPrimitive_Variables(CSolver **solver_container, CConfig *config, bool Output) {

  /*--- Number of non-physical points, local to the thread, needs
   *    further reduction if function is called in parallel ---*/
  unsigned long nonPhysicalPoints = 0;

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint ++) {

    /*--- Compressible flow, primitive variables nSpecies+nDim+8 (Euler),
         [rho1, ..., rhoNs, T, Tve, u, v, w, P, rho, h, a, rhoCvtr, rhoCvve]^T  ---*/

    bool nonphysical = nodes->SetPrimVar(iPoint,FluidModel);

    /* Check for non-realizable states for reporting. */

    if (nonphysical) nonPhysicalPoints++;

    //Delete me???/
    ///*--- Initialize the convective, source and viscous residual vector ---*/
    //if (!Output) LinSysRes.SetBlock_Zero(iPoint);

  }

  return nonPhysicalPoints;
}

void CNEMOEulerSolver::SetTime_Step(CGeometry *geometry, CSolver **solver_container, CConfig *config,
                                    unsigned short iMesh, unsigned long Iteration) {



  const bool viscous       = config->GetViscous();
  const bool implicit      = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool time_stepping = (config->GetTime_Marching() == TIME_STEPPING);
  const bool dual_time     = (config->GetTime_Marching() == DT_STEPPING_1ST) ||
                             (config->GetTime_Marching() == DT_STEPPING_2ND);
  const su2double K_v = 0.25;

  /*--- Init thread-shared variables to compute min/max values.
   *    Critical sections are used for this instead of reduction
   *    clauses for compatibility with OpenMP 2.0 (Windows...). ---*/

  SU2_OMP_MASTER
  {
    Min_Delta_Time = 1e30;
    Max_Delta_Time = 0.0;
    Global_Delta_UnstTimeND = 1e30;
  }
  SU2_OMP_BARRIER

  const su2double *Normal = nullptr;
  su2double Area, Vol, Mean_SoundSpeed, Mean_ProjVel, Lambda, Local_Delta_Time, Local_Delta_Time_Visc;
  su2double Mean_LaminarVisc, Mean_EddyVisc, Mean_ThermalCond, Mean_ThermalCond_ve, Mean_Density, cv, Lambda_1, Lambda_2;
  unsigned long iEdge, iVertex, iPoint, jPoint;
  unsigned short iDim, iMarker;

  /*--- Loop domain points. ---*/

  SU2_OMP_FOR_DYN(omp_chunk_size)
  for (iPoint = 0; iPoint < nPointDomain; ++iPoint) {

    /*--- Set maximum eigenvalues to zero. ---*/

    nodes->SetMax_Lambda_Inv(iPoint,0.0);

    if (viscous)
      nodes->SetMax_Lambda_Visc(iPoint,0.0);

    /*--- Loop over the neighbors of point i. ---*/

    for (unsigned short iNeigh = 0; iNeigh < geometry->nodes->GetnPoint(iPoint); ++iNeigh)
    {
      jPoint = geometry->nodes->GetPoint(iPoint,iNeigh);

      iEdge = geometry->nodes->GetEdge(iPoint,iNeigh);
      Normal = geometry->edges->GetNormal(iEdge);
      Area = 0.0; for (iDim = 0; iDim < nDim; iDim++) Area += pow(Normal[iDim],2); Area = sqrt(Area);

      /*--- Mean Values ---*/

      Mean_ProjVel = 0.5 * (nodes->GetProjVel(iPoint,Normal) + nodes->GetProjVel(jPoint,Normal));
      Mean_SoundSpeed = 0.5 * (nodes->GetSoundSpeed(iPoint) + nodes->GetSoundSpeed(jPoint)) * Area;

      /*--- Adjustment for grid movement ---*/

      if (dynamic_grid) {
        const su2double *GridVel_i = geometry->nodes->GetGridVel(iPoint);
        const su2double *GridVel_j = geometry->nodes->GetGridVel(jPoint);

        for (iDim = 0; iDim < nDim; iDim++)
          Mean_ProjVel -= 0.5 * (GridVel_i[iDim] + GridVel_j[iDim]) * Normal[iDim];
      }

      /*--- Inviscid contribution ---*/

      Lambda = fabs(Mean_ProjVel) + Mean_SoundSpeed ;
      nodes->AddMax_Lambda_Inv(iPoint,Lambda);

      /*--- Viscous contribution ---*/

      if (!viscous) continue;

       Mean_LaminarVisc    = 0.5*(nodes->GetLaminarViscosity(iPoint) + nodes->GetLaminarViscosity(jPoint));
       Mean_EddyVisc       = 0.5*(nodes->GetEddyViscosity(iPoint) + nodes->GetEddyViscosity(jPoint));
       Mean_ThermalCond    = 0.5*(nodes->GetThermalConductivity(iPoint) +
                                  nodes->GetThermalConductivity(jPoint)  );
       Mean_ThermalCond_ve = 0.5*(nodes->GetThermalConductivity_ve(iPoint) +
                                  nodes->GetThermalConductivity_ve(jPoint)  );
       Mean_Density        = 0.5*(nodes->GetDensity(iPoint) + nodes->GetDensity(jPoint));

       cv = 0.5*(nodes->GetRhoCv_tr(iPoint) + nodes->GetRhoCv_ve(iPoint) +
                 nodes->GetRhoCv_tr(jPoint) + nodes->GetRhoCv_ve(jPoint)  )/ Mean_Density;

       Lambda_1 = (4.0/3.0)*(Mean_LaminarVisc + Mean_EddyVisc);
       Lambda_2 = (Mean_ThermalCond+Mean_ThermalCond_ve)/cv; //TODO, SCALE WITH TURB
       Lambda   = (Lambda_1 + Lambda_2)*Area*Area/Mean_Density;

       nodes->AddMax_Lambda_Visc(iPoint, Lambda);
     }

  }

  /*--- Loop boundary edges ---*/

  for (iMarker = 0; iMarker < geometry->GetnMarker(); iMarker++) {
    if ((config->GetMarker_All_KindBC(iMarker) != INTERNAL_BOUNDARY) &&
      (config->GetMarker_All_KindBC(iMarker) != PERIODIC_BOUNDARY)) {

      SU2_OMP_FOR_STAT(OMP_MIN_SIZE)
      for (iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++) {

        /*--- Point identification, Normal vector and area ---*/

        iPoint = geometry->vertex[iMarker][iVertex]->GetNode();

        if (!geometry->nodes->GetDomain(iPoint)) continue;

        Normal = geometry->vertex[iMarker][iVertex]->GetNormal();
        Area = 0.0; for (iDim = 0; iDim < nDim; iDim++) Area += Normal[iDim]*Normal[iDim]; Area = sqrt(Area);

        /*--- Mean Values ---*/

        Mean_ProjVel = nodes->GetProjVel(iPoint,Normal);
        Mean_SoundSpeed = nodes->GetSoundSpeed(iPoint) * Area;

        /*--- Adjustment for grid movement ---*/

        if (dynamic_grid) {
          const su2double *GridVel = geometry->nodes->GetGridVel(iPoint);

          for (iDim = 0; iDim < nDim; iDim++)
            Mean_ProjVel -= GridVel[iDim]*Normal[iDim];
        }

        /*--- Inviscid contribution ---*/

        Lambda = fabs(Mean_ProjVel) + Mean_SoundSpeed;
        nodes->AddMax_Lambda_Inv(iPoint,Lambda);

        /*--- Viscous contribution ---*/

        if (!viscous) continue;

        Mean_LaminarVisc    = nodes->GetLaminarViscosity(iPoint);
        Mean_EddyVisc       = nodes->GetEddyViscosity(iPoint);
        Mean_ThermalCond    = nodes->GetThermalConductivity(iPoint);
        Mean_ThermalCond_ve = nodes->GetThermalConductivity_ve(iPoint);
        Mean_Density        = nodes->GetDensity(iPoint);

        cv = (nodes->GetRhoCv_tr(iPoint) + nodes->GetRhoCv_ve(iPoint)) / Mean_Density;

        Lambda_1 = (4.0/3.0)*(Mean_LaminarVisc + Mean_EddyVisc);
        Lambda_2 = (Mean_ThermalCond+Mean_ThermalCond_ve)/cv; //TODO SCALE WITH TURB?
        Lambda   = (Lambda_1 + Lambda_2)*Area*Area/Mean_Density;


        nodes->AddMax_Lambda_Visc(iPoint, Lambda);

        }
      }
    }

  /*--- Each element uses their own speed, steady state simulation. ---*/
  {
    /*--- Thread-local variables for min/max reduction. ---*/
    su2double minDt = 1e30, maxDt = 0.0;

    SU2_OMP(for schedule(static,omp_chunk_size) nowait)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {

      Vol = geometry->nodes->GetVolume(iPoint);

      if (Vol != 0.0) {
        Local_Delta_Time = nodes->GetLocalCFL(iPoint)*Vol / nodes->GetMax_Lambda_Inv(iPoint);

        if(viscous) {
          Local_Delta_Time_Visc = nodes->GetLocalCFL(iPoint)*K_v*Vol*Vol/ nodes->GetMax_Lambda_Visc(iPoint);
          Local_Delta_Time = min(Local_Delta_Time, Local_Delta_Time_Visc);
        }

        minDt = min(minDt, Local_Delta_Time);
        maxDt = max(maxDt, Local_Delta_Time);

        nodes->SetDelta_Time(iPoint, min(Local_Delta_Time, config->GetMax_DeltaTime()));
      }
      else {
        nodes->SetDelta_Time(iPoint,0.0);
      }
    }
    /*--- Min/max over threads. ---*/
    SU2_OMP_CRITICAL
    {
      Min_Delta_Time = min(Min_Delta_Time, minDt);
      Max_Delta_Time = max(Max_Delta_Time, maxDt);
      Global_Delta_Time = Min_Delta_Time;
    }
    SU2_OMP_BARRIER
  }

  /*--- Compute the min/max dt (in parallel, now over mpi ranks). ---*/

  SU2_OMP_MASTER
  if (config->GetComm_Level() == COMM_FULL) {
    su2double rbuf_time;
    SU2_MPI::Allreduce(&Min_Delta_Time, &rbuf_time, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
    Min_Delta_Time = rbuf_time;

    SU2_MPI::Allreduce(&Max_Delta_Time, &rbuf_time, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    Max_Delta_Time = rbuf_time;
  }
  SU2_OMP_BARRIER

  /*--- For exact time solution use the minimum delta time of the whole mesh. ---*/

  if (time_stepping) {

    /*--- If the unsteady CFL is set to zero, it uses the defined unsteady time step,
     *    otherwise it computes the time step based on the unsteady CFL. ---*/

    SU2_OMP_MASTER
    {
      if (config->GetUnst_CFL() == 0.0) {
        Global_Delta_Time = config->GetDelta_UnstTime();
      }
      else {
        Global_Delta_Time = Min_Delta_Time;
      }
      Max_Delta_Time = Global_Delta_Time;

      config->SetDelta_UnstTimeND(Global_Delta_Time);
    }
    SU2_OMP_BARRIER

    /*--- Sets the regular CFL equal to the unsteady CFL. ---*/

    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {
      nodes->SetLocalCFL(iPoint, config->GetUnst_CFL());
      nodes->SetDelta_Time(iPoint, Global_Delta_Time);
    }
  }

  /*--- Recompute the unsteady time step for the dual time strategy if the unsteady CFL is diferent from 0. ---*/

  if ((dual_time) && (Iteration == 0) && (config->GetUnst_CFL() != 0.0) && (iMesh == MESH_0)) {

    /*--- Thread-local variable for reduction. ---*/
    su2double glbDtND = 1e30;

    SU2_OMP(for schedule(static,omp_chunk_size) nowait)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {
      glbDtND = min(glbDtND, config->GetUnst_CFL()*Global_Delta_Time / nodes->GetLocalCFL(iPoint));
    }
    SU2_OMP_CRITICAL
    Global_Delta_UnstTimeND = min(Global_Delta_UnstTimeND, glbDtND);
    SU2_OMP_BARRIER

    SU2_OMP_MASTER
    {
      SU2_MPI::Allreduce(&Global_Delta_UnstTimeND, &glbDtND, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
      Global_Delta_UnstTimeND = glbDtND;

      config->SetDelta_UnstTimeND(Global_Delta_UnstTimeND);
    }
    SU2_OMP_BARRIER
  }

  /*--- The pseudo local time (explicit integration) cannot be greater than the physical time ---*/

  if (dual_time && !implicit) {
    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {
      Local_Delta_Time = min((2.0/3.0)*config->GetDelta_UnstTimeND(), nodes->GetDelta_Time(iPoint));
      nodes->SetDelta_Time(iPoint, Local_Delta_Time);
    }
  }

}

void CNEMOEulerSolver::Centered_Residual(CGeometry *geometry, CSolver **solver_container, CNumerics **numerics_container,
                                         CConfig *config, unsigned short iMesh, unsigned short iRKStep) {

  const bool implicit = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  //const bool jst_scheme = (config->GetKind_Centered_Flow() == JST) && (iMesh == MESH_0);
  //const bool jst_ke_scheme = (config->GetKind_Centered_Flow() == JST_KE) && (iMesh == MESH_0);

  /*--- Pick one numerics object per thread. ---*/
  CNumerics* numerics = numerics_container[CONV_TERM + omp_get_thread_num()*MAX_TERMS];

  /*--- Loop over edge colors. ---*/
  for (auto color : EdgeColoring)
  {
  /*--- Chunk size is at least OMP_MIN_SIZE and a multiple of the color group size. ---*/
  SU2_OMP_FOR_DYN(nextMultiple(OMP_MIN_SIZE, color.groupSize))
    for(auto k = 0ul; k < color.size; ++k) {

      auto iEdge = color.indices[k];

      /*--- Points in edge, set normal vectors, and number of neighbors ---*/

      auto iPoint = geometry->edges->GetNode(iEdge,0);
      auto jPoint = geometry->edges->GetNode(iEdge,1);

      numerics->SetNormal(geometry->edges->GetNormal(iEdge));
      numerics->SetNeighbor(geometry->nodes->GetnNeighbor(iPoint),
                            geometry->nodes->GetnNeighbor(jPoint));

      /*--- Set conservative & primitive variables w/o reconstruction ---*/

      numerics->SetConservative(nodes->GetSolution(iPoint), nodes->GetSolution(jPoint));
      numerics->SetPrimitive(nodes->GetPrimitive(iPoint), nodes->GetPrimitive(jPoint));

      /*--- Pass supplementary information to CNumerics ---*/

      numerics->SetdPdU(   nodes->GetdPdU(iPoint),   nodes->GetdPdU(jPoint));
      numerics->SetdTdU(   nodes->GetdTdU(iPoint),   nodes->GetdTdU(jPoint));
      numerics->SetdTvedU( nodes->GetdTvedU(iPoint), nodes->GetdTvedU(jPoint));
      numerics->SetEve(    nodes->GetEve(iPoint),    nodes->GetEve(jPoint));
      numerics->SetCvve(   nodes->GetCvve(iPoint),   nodes->GetCvve(jPoint));

      /*--- Set the largest convective eigenvalue ---*/

      numerics->SetLambda(nodes->GetLambda(iPoint), nodes->GetLambda(jPoint));

      /*--- Set undivided laplacian an pressure based sensor ---*/

      //if (jst_scheme) {
      //  numerics->SetUndivided_Laplacian(nodes->GetUndivided_Laplacian(iPoint),
      //                                   nodes->GetUndivided_Laplacian(jPoint));
      //}
      //if (jst_scheme || jst_ke_scheme) {
      //  numerics->SetSensor(nodes->GetSensor(iPoint), nodes->GetSensor(jPoint));
      //}

      /*--- Grid movement ---*/

      if (dynamic_grid) {
        numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint), geometry->nodes->GetGridVel(jPoint));
      }

      /*--- Compute residuals, and Jacobians ---*/

      auto residual = numerics->ComputeResidual(config);

      /*--- Update convective and artificial dissipation residuals. ---*/

      if (ReducerStrategy) {
        EdgeFluxes.SetBlock(iEdge, residual);
        if (implicit)
          Jacobian.SetBlocks(iEdge, residual.jacobian_i, residual.jacobian_j);
      }
      else {
        LinSysRes.AddBlock(iPoint, residual);
        LinSysRes.SubtractBlock(jPoint, residual);
        if (implicit)
          Jacobian.UpdateBlocks(iEdge, iPoint, jPoint, residual.jacobian_i, residual.jacobian_j);
      }

      /*--- Viscous contribution. ---*/

      Viscous_Residual(iEdge, geometry, solver_container,
                       numerics_container[VISC_TERM + omp_get_thread_num()*MAX_TERMS], config);
    }
    } // end color loop

    if (ReducerStrategy) {
      SumEdgeFluxes(geometry);
      if (implicit)
        Jacobian.SetDiagonalAsColumnSum();
    }
}

void CNEMOEulerSolver::Upwind_Residual(CGeometry *geometry, CSolver **solution_container, CNumerics **numerics_container,
                                       CConfig *config, unsigned short iMesh) {
  unsigned long iEdge, iPoint, jPoint;
  unsigned short iDim, iVar;
  su2double *U_i, *U_j, *V_i, *V_j;
  su2double **GradU_i, **GradU_j, ProjGradU_i, ProjGradU_j;
  su2double *Limiter_i, *Limiter_j;
  su2double *Conserved_i, *Conserved_j, *Primitive_i, *Primitive_j;
  su2double *dPdU_i, *dPdU_j, *dTdU_i, *dTdU_j, *dTvedU_i, *dTvedU_j;
  su2double *Eve_i, *Eve_j, *Cvve_i, *Cvve_j;


  su2double lim_i, lim_j, lim_ij;

  unsigned long InnerIter = config->GetInnerIter();

  CNumerics* numerics = numerics_container[CONV_TERM];

  /*--- Set booleans based on config settings ---*/
   /*--- Set booleans based on config settings ---*/
  bool muscl      = (config->GetMUSCL_Flow() && (iMesh == MESH_0));
  bool disc_adjoint = config->GetDiscrete_Adjoint();
  bool limiter      = ((config->GetKind_SlopeLimit_Flow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter()) &&
                       !(disc_adjoint && config->GetFrozen_Limiter_Disc()));
  bool chk_err_i, chk_err_j, err;

  /*--- Allocate arrays ---*/
  Primitive_i = new su2double[nPrimVar];
  Primitive_j = new su2double[nPrimVar];
  Conserved_i = new su2double[nVar];
  Conserved_j = new su2double[nVar];
  dPdU_i      = new su2double[nVar];
  dPdU_j      = new su2double[nVar];
  dTdU_i      = new su2double[nVar];
  dTdU_j      = new su2double[nVar];
  dTvedU_i    = new su2double[nVar];
  dTvedU_j    = new su2double[nVar];
  Eve_i       = new su2double[nSpecies];
  Eve_j       = new su2double[nSpecies];
  Cvve_i      = new su2double[nSpecies];
  Cvve_j      = new su2double[nSpecies];


  /*--- Loop over edges and calculate convective fluxes ---*/
  for(iEdge = 0; iEdge < geometry->GetnEdge(); iEdge++) {


    /*--- Retrieve node numbers and pass edge normal to CNumerics ---*/
    iPoint = geometry->edges->GetNode(iEdge, 0);
    jPoint = geometry->edges->GetNode(iEdge, 1);
    numerics->SetNormal(geometry->edges->GetNormal(iEdge));

    /*--- Get conserved & primitive variables from CVariable ---*/
    U_i = nodes->GetSolution(iPoint);   U_j = nodes->GetSolution(jPoint);
    V_i = nodes->GetPrimitive(iPoint);  V_j = nodes->GetPrimitive(jPoint);

    /*--- High order reconstruction using MUSCL strategy ---*/
    if (muscl) {


      /*--- Assign i-j and j-i to projection vectors ---*/
      for (iDim = 0; iDim < nDim; iDim++) {
        Vector_i[iDim] = 0.5*(geometry->nodes->GetCoord(jPoint, iDim) -
                              geometry->nodes->GetCoord(iPoint, iDim)   );
        Vector_j[iDim] = 0.5*(geometry->nodes->GetCoord(iPoint, iDim) -
                              geometry->nodes->GetCoord(jPoint, iDim)   );
      }


      /*---+++ Conserved variable reconstruction & limiting +++---*/

      /*--- Retrieve gradient information & limiter ---*/
      GradU_i = nodes->GetGradient_Reconstruction(iPoint);
      GradU_j = nodes->GetGradient_Reconstruction(jPoint);

      if (limiter) {
        Limiter_i = nodes->GetLimiter(iPoint);
        Limiter_j = nodes->GetLimiter(jPoint);
        lim_i = 1.0;
        lim_j = 1.0;
        for (iVar = 0; iVar < nVar; iVar++) {
          if (lim_i > Limiter_i[iVar]) lim_i = Limiter_i[iVar];
          if (lim_j > Limiter_j[iVar]) lim_j = Limiter_j[iVar];
        }
        lim_ij = min(lim_i, lim_j);
      }

      /*--- Reconstruct conserved variables at the edge interface ---*/
      for (iVar = 0; iVar < nVar; iVar++) {
        ProjGradU_i = 0.0; ProjGradU_j = 0.0;
        for (iDim = 0; iDim < nDim; iDim++) {
          ProjGradU_i += Vector_i[iDim]*GradU_i[iVar][iDim];
          ProjGradU_j += Vector_j[iDim]*GradU_j[iVar][iDim];
        }
        if (limiter) {
          Conserved_i[iVar] = U_i[iVar] + lim_ij*ProjGradU_i;
          Conserved_j[iVar] = U_j[iVar] + lim_ij*ProjGradU_j;
        }
        else {
          Conserved_i[iVar] = U_i[iVar] + ProjGradU_i;
          Conserved_j[iVar] = U_j[iVar] + ProjGradU_j;
        }
      }

      chk_err_i = nodes->Cons2PrimVar(Conserved_i, Primitive_i,
                                             dPdU_i, dTdU_i, dTvedU_i, Eve_i, Cvve_i);
      chk_err_j = nodes->Cons2PrimVar(Conserved_j, Primitive_j,
                                             dPdU_j, dTdU_j, dTvedU_j, Eve_j, Cvve_j);

       /*--- Check for physical solutions in the reconstructed values ---*/
      // Note: If non-physical, revert to first order
      if ( chk_err_i || chk_err_j) {
        numerics->SetPrimitive   (V_i, V_j);
        numerics->SetConservative(U_i, U_j);
        numerics->SetdPdU  (nodes->GetdPdU(iPoint),   nodes->GetdPdU(jPoint));
        numerics->SetdTdU  (nodes->GetdTdU(iPoint),   nodes->GetdTdU(jPoint));
        numerics->SetdTvedU(nodes->GetdTvedU(iPoint), nodes->GetdTvedU(jPoint));
        numerics->SetEve   (nodes->GetEve(iPoint),    nodes->GetEve(jPoint));
        numerics->SetCvve  (nodes->GetCvve(iPoint),   nodes->GetCvve(jPoint));
      } else {
        numerics->SetConservative(Conserved_i, Conserved_j);
        numerics->SetPrimitive   (Primitive_i, Primitive_j);
        numerics->SetdPdU  (dPdU_i,   dPdU_j  );
        numerics->SetdTdU  (dTdU_i,   dTdU_j  );
        numerics->SetdTvedU(dTvedU_i, dTvedU_j);
        numerics->SetEve   (Eve_i,    Eve_j   );
        numerics->SetCvve  (Cvve_i,   Cvve_j  );
      }
    } else {
      /*--- Set variables without reconstruction ---*/
      numerics->SetPrimitive   (V_i, V_j);
      numerics->SetConservative(U_i, U_j);
      numerics->SetdPdU  (nodes->GetdPdU(iPoint),   nodes->GetdPdU(jPoint));
      numerics->SetdTdU  (nodes->GetdTdU(iPoint),   nodes->GetdTdU(jPoint));
      numerics->SetdTvedU(nodes->GetdTvedU(iPoint), nodes->GetdTvedU(jPoint));
      numerics->SetEve   (nodes->GetEve(iPoint),    nodes->GetEve(jPoint));
      numerics->SetCvve  (nodes->GetCvve(iPoint),   nodes->GetCvve(jPoint));
    }

    /*--- Compute the residual ---*/

    auto residual = numerics->ComputeResidual(config);

    /*--- Check for NaNs before applying the residual to the linear system ---*/
    err = false;
    for (iVar = 0; iVar < nVar; iVar++)
      if (residual[iVar] != residual[iVar]) err = true;
    //if (implicit)
    //  for (iVar = 0; iVar < nVar; iVar++)
    //    for (jVar = 0; jVar < nVar; jVar++)
    //      if ((Jacobian_i[iVar][jVar] != Jacobian_i[iVar][jVar]) ||
    //          (Jacobian_j[iVar][jVar] != Jacobian_j[iVar][jVar])   )
    //        err = true;

    /*--- Update the residual and Jacobian ---*/
    if (!err) {
      LinSysRes.AddBlock(iPoint, residual);
      LinSysRes.SubtractBlock(jPoint, residual);
      //if (implicit) {
      //  Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);
      //  Jacobian.AddBlock(iPoint, jPoint, Jacobian_j);
      //  Jacobian.SubtractBlock(jPoint, iPoint, Jacobian_i);
      //  Jacobian.SubtractBlock(jPoint, jPoint, Jacobian_j);
      //}
    }
  }

  delete [] Conserved_i;
  delete [] Conserved_j;
  delete [] Primitive_i;
  delete [] Primitive_j;
  delete [] dPdU_i;
  delete [] dPdU_j;
  delete [] dTdU_i;
  delete [] dTdU_j;
  delete [] dTvedU_i;
  delete [] dTvedU_j;
  delete [] Eve_i;
  delete [] Eve_j;
  delete [] Cvve_i;
  delete [] Cvve_j;

}
//void CNEMOEulerSolver::Upwind_Residual(CGeometry *geometry, CSolver **solver_container, CNumerics **numerics_container,
//                                       CConfig *config, unsigned short iMesh) {

//  const auto InnerIter        = config->GetInnerIter();
//  const bool implicit         = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);

//  const bool muscl            = (config->GetMUSCL_Flow() && (iMesh == MESH_0));
//  const bool limiter          = (config->GetKind_SlopeLimit_Flow() != NO_LIMITER) &&
//                                (InnerIter <= config->GetLimiterIter());
//  const bool van_albada       = (config->GetKind_SlopeLimit_Flow() == VAN_ALBADA_EDGE);

//  /*--- Non-physical counter. ---*/
//  unsigned long counter_local = 0;
//  SU2_OMP_MASTER
//  ErrorCounter = 0;

//  /*--- Pick one numerics object per thread. ---*/
//  CNumerics* numerics = numerics_container[CONV_TERM + omp_get_thread_num()*MAX_TERMS];

//  /*--- Static arrays of MUSCL-reconstructed primitives and secondaries (thread safety). ---*/
//  //TODO These may need to be pointers?
//  su2double Primitive_i[MAXNVAR] = {0.0}, Primitive_j[MAXNVAR] = {0.0};
//  su2double Secondary_i[MAXNVAR] = {0.0}, Secondary_j[MAXNVAR] = {0.0};
//  su2double Conserved_i[MAXNVAR] = {0.0}, Conserved_j[MAXNVAR] = {0.0};
//  su2double dPdU_i[MAXNVAR]      = {0.0}, dPdU_j[MAXNVAR]      = {0.0};
//  su2double dTdU_i[MAXNVAR]      = {0.0}, dTdU_j[MAXNVAR]      = {0.0};
//  su2double dTvedU_i[MAXNVAR]    = {0.0}, dTvedU_j[MAXNVAR]    = {0.0};
//  su2double Eve_i[MAXNVAR]       = {0.0}, Eve_j[MAXNVAR]       = {0.0};
//  su2double Cvve_i[MAXNVAR]      = {0.0}, Cvve_j[MAXNVAR]      = {0.0};

//  /*--- Loop over edge colors. ---*/
//  for (auto color : EdgeColoring)
//  {
//  /*--- Chunk size is at least OMP_MIN_SIZE and a multiple of the color group size. ---*/
//  SU2_OMP_FOR_DYN(nextMultiple(OMP_MIN_SIZE, color.groupSize))
//  for(auto k = 0ul; k < color.size; ++k) {

//    auto iEdge = color.indices[k];

//    unsigned short iDim, iVar;

//    /*--- Points in edge and normal vectors ---*/

//    auto iPoint = geometry->edges->GetNode(iEdge,0);
//    auto jPoint = geometry->edges->GetNode(iEdge,1);

//    numerics->SetNormal(geometry->edges->GetNormal(iEdge));

//    auto Coord_i = geometry->nodes->GetCoord(iPoint);
//    auto Coord_j = geometry->nodes->GetCoord(jPoint);

//    /*--- Grid movement ---*/

//    if (dynamic_grid) {
//     numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint),
//                          geometry->nodes->GetGridVel(jPoint));
//    }

//    /*--- Get conservative, primitive and secondary variables ---*/

//    auto U_i = nodes->GetSolution(iPoint);  auto U_j = nodes->GetSolution(jPoint);
//    auto V_i = nodes->GetPrimitive(iPoint); auto V_j = nodes->GetPrimitive(jPoint);
//    auto S_i = nodes->GetSecondary(iPoint); auto S_j = nodes->GetSecondary(jPoint);

//    /*--- Set them with or without high order reconstruction using MUSCL strategy. ---*/

//    if (!muscl) {

//      numerics->SetConservative(U_i,U_j);
//      numerics->SetPrimitive   (V_i, V_j);
//      numerics->SetSecondary   (S_i, S_j);
//      numerics->SetdPdU  (nodes->GetdPdU(iPoint),   nodes->GetdPdU(jPoint));
//      numerics->SetdTdU  (nodes->GetdTdU(iPoint),   nodes->GetdTdU(jPoint));
//      numerics->SetdTvedU(nodes->GetdTvedU(iPoint), nodes->GetdTvedU(jPoint));
//      numerics->SetEve   (nodes->GetEve(iPoint),    nodes->GetEve(jPoint));
//      numerics->SetCvve  (nodes->GetCvve(iPoint),   nodes->GetCvve(jPoint));

//    }
//    else {
//      /*--- Reconstruction ---*/

//      su2double Vector_ij[MAXNDIM] = {0.0};
//      for (iDim = 0; iDim < nDim; iDim++) {
//        Vector_ij[iDim] = 0.5*(Coord_j[iDim] - Coord_i[iDim]);
//      }

//      auto Gradient_i = nodes->GetGradient_Reconstruction(iPoint);
//      auto Gradient_j = nodes->GetGradient_Reconstruction(jPoint);

//      su2double *Limiter_i = nullptr, *Limiter_j = nullptr;

//      if (limiter) {
//        Limiter_i = nodes->GetLimiter(iPoint);
//        Limiter_j = nodes->GetLimiter(jPoint);
//      }

//      for (iVar = 0; iVar < nPrimVarGrad; iVar++) {

//        su2double Project_Grad_i = 0.0;
//        su2double Project_Grad_j = 0.0;

//        for (iDim = 0; iDim < nDim; iDim++) {
//          Project_Grad_i += Vector_ij[iDim]*Gradient_i[iVar][iDim];
//          Project_Grad_j -= Vector_ij[iDim]*Gradient_j[iVar][iDim];
//        }

//        if (limiter) {
//          if (van_albada) {
//            su2double U_ij = U_j[iVar] - U_i[iVar];
//            Limiter_i[iVar] = U_ij*( 2.0*Project_Grad_i + U_ij) / (4*pow(Project_Grad_i, 2) + pow(U_ij, 2) + EPS);
//            Limiter_j[iVar] = U_ij*(-2.0*Project_Grad_j + U_ij) / (4*pow(Project_Grad_j, 2) + pow(U_ij, 2) + EPS);
//          }
//          Conserved_i[iVar] = U_i[iVar] + Limiter_i[iVar]*Project_Grad_i;
//          Conserved_j[iVar] = U_j[iVar] + Limiter_j[iVar]*Project_Grad_j;
//        }
//        else {
//          Conserved_i[iVar] = U_i[iVar] + Project_Grad_i;
//          Conserved_j[iVar] = U_j[iVar] + Project_Grad_j;
//        }

//      }

//      /*--- Recompute the reconstructed prmitive quantities in a thermodynamically consistent way. ---*/

//      bool bad_i = nodes->Cons2PrimVar(Conserved_i, Primitive_i, dPdU_i, dTdU_i, dTvedU_i, Eve_i, Cvve_i);
//      bool bad_j = nodes->Cons2PrimVar(Conserved_j, Primitive_j, dPdU_j, dTdU_j, dTvedU_j, Eve_j, Cvve_j);

//      //TODO?
//      //nodes->SetNon_Physical(iPoint, bad_i);
//      //nodes->SetNon_Physical(jPoint, bad_j);
//      /*--- Get updated state, in case the point recovered after the set. ---*/
//      //bad_i = nodes->GetNon_Physical(iPoint);
//      //bad_j = nodes->GetNon_Physical(jPoint);

//      //counter_local += bad_i+bad_j;

//      /*--- Check for physical solutions in the reconstructed values ---*/
//      // Note: If non-physical, revert to first order
//      numerics->SetConservative( bad_i? U_i : Conserved_i , bad_j? U_j : Conserved_j);
//      numerics->SetPrimitive(    bad_i? V_i : Primitive_i,  bad_j? V_j : Primitive_j);
//      numerics->SetSecondary(    bad_i? S_i : Secondary_i,  bad_j? S_j : Secondary_j);

//      numerics->SetdPdU  ( bad_i? nodes->GetdPdU(iPoint)  : dPdU_i,   bad_j? nodes->GetdPdU(jPoint)  : dPdU_j);
//      numerics->SetdTdU  ( bad_i? nodes->GetdTdU(iPoint)  : dTdU_i,   bad_j? nodes->GetdTdU(jPoint)  : dTdU_j);
//      numerics->SetdTvedU( bad_i? nodes->GetdTvedU(iPoint): dTvedU_i, bad_j? nodes->GetdTvedU(jPoint): dTvedU_j);
//      numerics->SetEve   ( bad_i? nodes->GetEve(iPoint)   : Eve_i,    bad_j? nodes->GetEve(jPoint)   : Eve_j);
//      numerics->SetCvve  ( bad_i? nodes->GetCvve(iPoint)  : Cvve_i,   bad_j? nodes->GetCvve(jPoint)  : Cvve_j);


//    }

//    /*--- Compute the residual ---*/

//    auto residual = numerics->ComputeResidual(config);

//    /*--- Update residual value ---*/

//    if (ReducerStrategy) {
//      EdgeFluxes.SetBlock(iEdge, residual);
//      if (implicit)
//        Jacobian.SetBlocks(iEdge, residual.jacobian_i, residual.jacobian_j);
//    }
//    else {
//      LinSysRes.AddBlock(iPoint, residual);
//      LinSysRes.SubtractBlock(jPoint, residual);

//      /*--- Set implicit computation ---*/
//      if (implicit)
//        Jacobian.UpdateBlocks(iEdge, iPoint, jPoint, residual.jacobian_i, residual.jacobian_j);
//    }

//    /*--- Viscous contribution. ---*/

//    Viscous_Residual(iEdge, geometry, solver_container,
//                     numerics_container[VISC_TERM + omp_get_thread_num()*MAX_TERMS], config);
//  }
//  } // end color loop

//  /*--- Update residual value ---*/

//  if (ReducerStrategy) {
//    SumEdgeFluxes(geometry);
//    if (implicit)
//      Jacobian.SetDiagonalAsColumnSum();
//  }

//  /*--- Warning message about non-physical reconstructions. ---*/

//  if ((iMesh == MESH_0) && (config->GetComm_Level() == COMM_FULL)) {
//    /*--- Add counter results for all threads. ---*/
//    SU2_OMP_ATOMIC
//    ErrorCounter += counter_local;
//    SU2_OMP_BARRIER

//    /*--- Add counter results for all ranks. ---*/
//    SU2_OMP_MASTER
//    {
//      counter_local = ErrorCounter;
//      SU2_MPI::Reduce(&counter_local, &ErrorCounter, 1, MPI_UNSIGNED_LONG, MPI_SUM, MASTER_NODE, MPI_COMM_WORLD);
//      config->SetNonphysical_Reconstr(ErrorCounter);
//    }
//    SU2_OMP_BARRIER
//  }
//}

void CNEMOEulerSolver::SumEdgeFluxes(CGeometry* geometry) {

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; ++iPoint) {

    LinSysRes.SetBlock_Zero(iPoint);

    for (unsigned short iNeigh = 0; iNeigh < geometry->nodes->GetnPoint(iPoint); ++iNeigh) {

      auto iEdge = geometry->nodes->GetEdge(iPoint, iNeigh);

      if (iPoint == geometry->edges->GetNode(iEdge,0))
        LinSysRes.AddBlock(iPoint, EdgeFluxes.GetBlock(iEdge));
      else
        LinSysRes.SubtractBlock(iPoint, EdgeFluxes.GetBlock(iEdge));
    }
  }

}

void CNEMOEulerSolver::Source_Residual(CGeometry *geometry, CSolver **solver_container, CNumerics **numerics_container, CConfig *config, unsigned short iMesh) {
  
  unsigned short iVar, jVar;
  unsigned long iPoint;
  unsigned long eAxi_global, eChm_global, eVib_global;

  /*--- Assign booleans ---*/

  const bool implicit     = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  const bool axisymmetric = config->GetAxisymmetric();
  const bool frozen       = config->GetFrozen();
  bool err = false;

  /*--- Pick one numerics object per thread. ---*/
  CNumerics* numerics = numerics_container[SOURCE_FIRST_TERM + omp_get_thread_num()*MAX_TERMS];

  /*--- Initialize the error counter ---*/

  unsigned long eAxi_local = 0;
  unsigned long eChm_local = 0;
  unsigned long eVib_local = 0;

  /*--- Initialize the source residual to zero ---*/

  for (iVar = 0; iVar < nVar; iVar++) Residual[iVar] = 0.0;
  
  /*--- loop over interior points ---*/

  SU2_OMP_FOR_DYN(omp_chunk_size)
  for (iPoint = 0; iPoint < nPointDomain; iPoint++) {

    /*--- Set conserved & primitive variables  ---*/

    numerics->SetConservative( nodes->GetSolution(iPoint),  nodes->GetSolution(iPoint));
    numerics->SetPrimitive   ( nodes->GetPrimitive(iPoint), nodes->GetPrimitive(iPoint) );

    /*--- Pass supplementary information to CNumerics ---*/

    numerics->SetdPdU(   nodes->GetdPdU(iPoint),   nodes->GetdPdU(iPoint));
    numerics->SetdTdU(   nodes->GetdTdU(iPoint),   nodes->GetdTdU(iPoint));
    numerics->SetdTvedU( nodes->GetdTvedU(iPoint), nodes->GetdTvedU(iPoint));
    numerics->SetEve(    nodes->GetEve(iPoint),    nodes->GetEve(iPoint));
    numerics->SetCvve(   nodes->GetCvve(iPoint),   nodes->GetCvve(iPoint));

    /*--- Set volume of the dual grid cell ---*/

    numerics->SetVolume( geometry->nodes->GetVolume(iPoint));
    numerics->SetCoord(  geometry->nodes->GetCoord(iPoint),
                         geometry->nodes->GetCoord(iPoint) );

    /*--- Compute axisymmetric source terms (if needed) ---*/

    if (axisymmetric) {
      auto residual = numerics->ComputeAxisymmetric(config);

      /*--- Check for errors before applying source to the linear system ---*/
      err = false;
      for (iVar = 0; iVar < nVar; iVar++)
        if (residual[iVar] != residual[iVar]) err = true;
      if (implicit)
        for (iVar = 0; iVar < nVar; iVar++)
          for (jVar = 0; jVar < nVar; jVar++)
            if (Jacobian_i[iVar][jVar] != Jacobian_i[iVar][jVar]) err = true;

      /*--- Apply the update to the linear system ---*/

      if (!err) {
        LinSysRes.AddBlock(iPoint, residual);
        if (implicit)
          Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);
      }
      else
        eAxi_local++;
    }

    /*--- Compute the non-equilibrium chemistry ---*/

    if(!frozen){

      auto residual = numerics->ComputeChemistry(config);

      /*--- Check for errors before applying source to the linear system ---*/
      err = false;
      for (iVar = 0; iVar < nVar; iVar++)
        if (residual[iVar] != residual[iVar]) err = true;
      if (implicit)
        for (iVar = 0; iVar < nVar; iVar++)
          for (jVar = 0; jVar < nVar; jVar++)
            if (Jacobian_i[iVar][jVar] != Jacobian_i[iVar][jVar]) err = true;
      /*--- Apply the chemical sources to the linear system ---*/
      if (!err) {
        LinSysRes.SubtractBlock(iPoint, residual);
        if (implicit)
          Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
      } else
        eChm_local++;
    }

    /*--- Compute vibrational energy relaxation ---*/
    /// NOTE: Jacobians don't account for relaxation time derivatives, TODO

    auto residual = numerics->ComputeVibRelaxation(config);

    /*--- Check for errors before applying source to the linear system ---*/
    err = false;
    for (iVar = 0; iVar < nVar; iVar++)
      if (residual[iVar] != residual[iVar]) err = true;
    if (implicit)
      for (iVar = 0; iVar < nVar; iVar++)
        for (jVar = 0; jVar < nVar; jVar++)
          if (Jacobian_i[iVar][jVar] != Jacobian_i[iVar][jVar]) err = true;

    /*--- Apply the vibrational relaxation terms to the linear system ---*/
    if (!err) {
      LinSysRes.SubtractBlock(iPoint, residual);
      if (implicit)
        Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
    } else
      eVib_local++;
  }

  /*--- Checking for NaN ---*/
  eAxi_global = eAxi_local;
  eChm_global = eChm_local;
  eVib_global = eVib_local;

  //THIS IS NO FUN
  if ((eAxi_global != 0) ||
      (eChm_global != 0) ||
      (eVib_global != 0)) {
    cout << "Warning!! Instances of NaN in the following source terms: " << endl;
    cout << "Axisymmetry: " << eAxi_global << endl;
    cout << "Chemical:    " << eChm_global << endl;
    cout << "Vib. Relax:  " << eVib_global << endl;
  }
}

void CNEMOEulerSolver::SetMax_Eigenvalue(CGeometry *geometry, CConfig *config) {

  /*--- Loop domain points. ---*/

  SU2_OMP_FOR_DYN(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPointDomain; ++iPoint) {

    /*--- Set eigenvalues to zero. ---*/
    nodes->SetLambda(iPoint,0.0);

    /*--- Loop over the neighbors of point i. ---*/
    for (unsigned short iNeigh = 0; iNeigh < geometry->nodes->GetnPoint(iPoint); ++iNeigh)
    {
      auto jPoint = geometry->nodes->GetPoint(iPoint, iNeigh);

      auto iEdge = geometry->nodes->GetEdge(iPoint, iNeigh);
      auto Normal = geometry->edges->GetNormal(iEdge);
      su2double Area = 0.0;
      for (unsigned short iDim = 0; iDim < nDim; iDim++) Area += pow(Normal[iDim],2);
      Area = sqrt(Area);

      /*--- Mean Values ---*/

      su2double Mean_ProjVel = 0.5 * (nodes->GetProjVel(iPoint,Normal) + nodes->GetProjVel(jPoint,Normal));
      su2double Mean_SoundSpeed = 0.5 * (nodes->GetSoundSpeed(iPoint) + nodes->GetSoundSpeed(jPoint)) * Area;

      /*--- Adjustment for grid movement ---*/

      if (dynamic_grid) {
        const su2double *GridVel_i = geometry->nodes->GetGridVel(iPoint);
        const su2double *GridVel_j = geometry->nodes->GetGridVel(jPoint);

        for (unsigned short iDim = 0; iDim < nDim; iDim++)
          Mean_ProjVel -= 0.5 * (GridVel_i[iDim] + GridVel_j[iDim]) * Normal[iDim];
      }

      /*--- Inviscid contribution ---*/

      su2double Lambda = fabs(Mean_ProjVel) + Mean_SoundSpeed ;
      nodes->AddLambda(iPoint, Lambda);
    }
  }

  /*--- Loop boundary edges ---*/

  for (unsigned short iMarker = 0; iMarker < geometry->GetnMarker(); iMarker++) {
    if ((config->GetMarker_All_KindBC(iMarker) != INTERNAL_BOUNDARY) &&
        (config->GetMarker_All_KindBC(iMarker) != PERIODIC_BOUNDARY)) {

    SU2_OMP_FOR_STAT(OMP_MIN_SIZE)
    for (unsigned long iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++) {

      /*--- Point identification, Normal vector and area ---*/

      auto iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
      auto Normal = geometry->vertex[iMarker][iVertex]->GetNormal();
      su2double Area = 0.0;
      for (unsigned short iDim = 0; iDim < nDim; iDim++)
        Area += pow(Normal[iDim],2);
      Area = sqrt(Area);

      /*--- Mean Values ---*/

      su2double Mean_ProjVel = nodes->GetProjVel(iPoint,Normal);
      su2double Mean_SoundSpeed = nodes->GetSoundSpeed(iPoint) * Area;

      /*--- Adjustment for grid movement ---*/

      if (dynamic_grid) {
        auto GridVel = geometry->nodes->GetGridVel(iPoint);
        for (unsigned short iDim = 0; iDim < nDim; iDim++)
          Mean_ProjVel -= GridVel[iDim]*Normal[iDim];
      }

      /*--- Inviscid contribution ---*/

      su2double Lambda = fabs(Mean_ProjVel) + Mean_SoundSpeed;
      if (geometry->nodes->GetDomain(iPoint)) {
        nodes->AddLambda(iPoint,Lambda);
      }
    }
    }
   }


  /*--- Correct the eigenvalue values across any periodic boundaries. ---*/

  for (unsigned short iPeriodic = 1; iPeriodic <= config->GetnMarker_Periodic()/2; iPeriodic++) {
    InitiatePeriodicComms(geometry, config, iPeriodic, PERIODIC_MAX_EIG);
    CompletePeriodicComms(geometry, config, iPeriodic, PERIODIC_MAX_EIG);
  }

  /*--- MPI parallelization ---*/

  InitiateComms(geometry, config, MAX_EIGENVALUE);
  CompleteComms(geometry, config, MAX_EIGENVALUE);
}

template<ENUM_TIME_INT IntegrationType>
void CNEMOEulerSolver::Explicit_Iteration(CGeometry *geometry, CSolver **solver_container,
                                          CConfig *config, unsigned short iRKStep) {

  static_assert(IntegrationType == CLASSICAL_RK4_EXPLICIT ||
                IntegrationType == RUNGE_KUTTA_EXPLICIT ||
                IntegrationType == EULER_EXPLICIT, "");


  const bool adjoint = config->GetContinuous_Adjoint();

  const su2double RK_AlphaCoeff = config->Get_Alpha_RKStep(iRKStep);

  /*--- Hard-coded classical RK4 coefficients. Will be added to config. ---*/
  const su2double RK_FuncCoeff[] = {1.0/6.0, 1.0/3.0, 1.0/3.0, 1.0/6.0};
  const su2double RK_TimeCoeff[] = {0.5, 0.5, 1.0, 1.0};

  /*--- Set shared residual variables to 0 and declare
   *    local ones for current thread to work on. ---*/

  SU2_OMP_MASTER
  for (unsigned short iVar = 0; iVar < nVar; iVar++) {
    SetRes_RMS(iVar, 0.0);
    SetRes_Max(iVar, 0.0, 0);
  }
  SU2_OMP_BARRIER

  su2double resMax[MAXNVAR] = {0.0}, resRMS[MAXNVAR] = {0.0};
  const su2double* coordMax[MAXNVAR] = {nullptr};
  unsigned long idxMax[MAXNVAR] = {0};

  /*--- Update the solution and residuals ---*/

  SU2_OMP(for schedule(static,omp_chunk_size) nowait)
  for (unsigned long iPoint = 0; iPoint < nPointDomain; iPoint++) {

    su2double Vol = geometry->nodes->GetVolume(iPoint) + geometry->nodes->GetPeriodicVolume(iPoint);
    su2double Delta = nodes->GetDelta_Time(iPoint) / Vol;

    const su2double* Res_TruncError = nodes->GetResTruncError(iPoint);
    const su2double* Residual = LinSysRes.GetBlock(iPoint);

    if (!adjoint) {
      for (unsigned short iVar = 0; iVar < nVar; iVar++) {

        su2double Res = Residual[iVar] + Res_TruncError[iVar];

        /*--- "Static" switch which should be optimized at compile time. ---*/
        switch(IntegrationType) {

          case EULER_EXPLICIT:
            nodes->AddSolution(iPoint,iVar, -Res*Delta);
            break;

          case RUNGE_KUTTA_EXPLICIT:
            nodes->AddSolution(iPoint, iVar, -Res*Delta*RK_AlphaCoeff);
            break;

          case CLASSICAL_RK4_EXPLICIT:
          {
            su2double tmp_time = -1.0*RK_TimeCoeff[iRKStep]*Delta;
            su2double tmp_func = -1.0*RK_FuncCoeff[iRKStep]*Delta;

            if (iRKStep < 3) {
              /* Base Solution Update */
              nodes->AddSolution(iPoint,iVar, tmp_time*Res);

              /* New Solution Update */
              nodes->AddSolution_New(iPoint,iVar, tmp_func*Res);
            } else {
              nodes->SetSolution(iPoint, iVar, nodes->GetSolution_New(iPoint, iVar) + tmp_func*Res);
            }
          }
          break;
        }

        /*--- Update residual information for current thread. ---*/
        resRMS[iVar] += Res*Res;
        if (fabs(Res) > resMax[iVar]) {
          resMax[iVar] = fabs(Res);
          idxMax[iVar] = iPoint;
          coordMax[iVar] = geometry->nodes->GetCoord(iPoint);
        }
      }
    }
  }
  if (!adjoint) {
    /*--- Reduce residual information over all threads in this rank. ---*/
    SU2_OMP_CRITICAL
    for (unsigned short iVar = 0; iVar < nVar; iVar++) {
      AddRes_RMS(iVar, resRMS[iVar]);
      AddRes_Max(iVar, resMax[iVar], geometry->nodes->GetGlobalIndex(idxMax[iVar]), coordMax[iVar]);
    }
  }
  SU2_OMP_BARRIER

  /*--- MPI solution ---*/

  InitiateComms(geometry, config, SOLUTION);
  CompleteComms(geometry, config, SOLUTION);

  SU2_OMP_MASTER
  {
    /*--- Compute the root mean square residual ---*/

    SetResidual_RMS(geometry, config);

    /*--- For verification cases, compute the global error metrics. ---*/

    ComputeVerificationError(geometry, config);
  }
  SU2_OMP_BARRIER
}


void CNEMOEulerSolver::ExplicitRK_Iteration(CGeometry *geometry, CSolver **solver_container,
                                        CConfig *config, unsigned short iRKStep) {

  Explicit_Iteration<RUNGE_KUTTA_EXPLICIT>(geometry, solver_container, config, iRKStep);
}

void CNEMOEulerSolver::ExplicitEuler_Iteration(CGeometry *geometry, CSolver **solver_container, CConfig *config) {

  Explicit_Iteration<EULER_EXPLICIT>(geometry, solver_container, config, 0);
}

void CNEMOEulerSolver::ImplicitEuler_Iteration(CGeometry *geometry, CSolver **solver_container, CConfig *config) {

  /*--- Set shared residual variables to 0 and declare
   *    local ones for current thread to work on. ---*/

  SU2_OMP_MASTER
  for (unsigned short iVar = 0; iVar < nVar; iVar++) {
    SetRes_RMS(iVar, 0.0);
    SetRes_Max(iVar, 0.0, 0);
  }
  SU2_OMP_BARRIER

  su2double resMax[MAXNVAR] = {0.0}, resRMS[MAXNVAR] = {0.0};
  const su2double* coordMax[MAXNVAR] = {nullptr};
  unsigned long idxMax[MAXNVAR] = {0};

  /*--- Build implicit system ---*/

  SU2_OMP(for schedule(static,omp_chunk_size) nowait)
  for (unsigned long iPoint = 0; iPoint < nPointDomain; iPoint++) {

    /*--- Read the residual ---*/

    su2double* local_Res_TruncError = nodes->GetResTruncError(iPoint);

    /*--- Read the volume ---*/

    su2double Vol = geometry->nodes->GetVolume(iPoint) + geometry->nodes->GetPeriodicVolume(iPoint);

    /*--- Modify matrix diagonal to assure diagonal dominance ---*/

    if (nodes->GetDelta_Time(iPoint) != 0.0) {

      su2double Delta = Vol / nodes->GetDelta_Time(iPoint);
      Jacobian.AddVal2Diag(iPoint, Delta);
    }
    else {
      Jacobian.SetVal2Diag(iPoint, 1.0);
      for (unsigned short iVar = 0; iVar < nVar; iVar++) {
        LinSysRes(iPoint,iVar) = 0.0;
        local_Res_TruncError[iVar] = 0.0;
      }
    }

    /*--- Right hand side of the system (-Residual) and initial guess (x = 0) ---*/

    for (unsigned short iVar = 0; iVar < nVar; iVar++) {
      unsigned long total_index = iPoint*nVar + iVar;
      LinSysRes[total_index] = - (LinSysRes[total_index] + local_Res_TruncError[iVar]);
      LinSysSol[total_index] = 0.0;

      su2double Res = fabs(LinSysRes[total_index]);
      resRMS[iVar] += Res*Res;
      if (Res > resMax[iVar]) {
        resMax[iVar] = Res;
        idxMax[iVar] = iPoint;
        coordMax[iVar] = geometry->nodes->GetCoord(iPoint);
      }
    }
  }
  SU2_OMP_CRITICAL
  for (unsigned short iVar = 0; iVar < nVar; iVar++) {
    AddRes_RMS(iVar, resRMS[iVar]);
    AddRes_Max(iVar, resMax[iVar], geometry->nodes->GetGlobalIndex(idxMax[iVar]), coordMax[iVar]);
  }

  /*--- Initialize residual and solution at the ghost points ---*/

  SU2_OMP(sections nowait)
  {
    SU2_OMP(section)
    for (unsigned long iPoint = nPointDomain; iPoint < nPoint; iPoint++)
      LinSysRes.SetBlock_Zero(iPoint);

    SU2_OMP(section)
    for (unsigned long iPoint = nPointDomain; iPoint < nPoint; iPoint++)
      LinSysSol.SetBlock_Zero(iPoint);
  }

  /*--- Solve or smooth the linear system. ---*/

  auto iter = System.Solve(Jacobian, LinSysRes, LinSysSol, geometry, config);
  SU2_OMP_MASTER
  {
    SetIterLinSolver(iter);
    SetResLinSolver(System.GetResidual());
  }
  SU2_OMP_BARRIER

  ComputeUnderRelaxationFactor(solver_container, config);

  /*--- Update solution (system written in terms of increments) ---*/

  if (!adjoint) {
    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (unsigned long iPoint = 0; iPoint < nPointDomain; iPoint++) {
      for (unsigned short iVar = 0; iVar < nVar; iVar++) {
        nodes->AddSolution(iPoint, iVar, nodes->GetUnderRelaxation(iPoint)*LinSysSol[iPoint*nVar+iVar]);
      }
    }
  }

  for (unsigned short iPeriodic = 1; iPeriodic <= config->GetnMarker_Periodic()/2; iPeriodic++) {
    InitiatePeriodicComms(geometry, config, iPeriodic, PERIODIC_IMPLICIT);
    CompletePeriodicComms(geometry, config, iPeriodic, PERIODIC_IMPLICIT);
  }

  /*--- MPI solution ---*/

  InitiateComms(geometry, config, SOLUTION);
  CompleteComms(geometry, config, SOLUTION);

  SU2_OMP_MASTER
  {
    /*--- Compute the root mean square residual ---*/

    SetResidual_RMS(geometry, config);

    /*--- For verification cases, compute the global error metrics. ---*/

    ComputeVerificationError(geometry, config);
  }
  SU2_OMP_BARRIER

}

void CNEMOEulerSolver::SetPreconditioner(CConfig *config, unsigned short iPoint) {

  //TODO
  SU2_MPI::Error("SetPrecondition: Not operational in NEMO.", CURRENT_FUNCTION);
  unsigned short iDim, jDim, iVar, jVar;
  su2double local_Mach, rho, enthalpy, soundspeed, sq_vel;
  su2double *U_i = nullptr;
  su2double Beta_max = config->GetmaxTurkelBeta();
  su2double Mach_infty2, Mach_lim2, aux, parameter;

  /*--- Variables to calculate the preconditioner parameter Beta ---*/
  local_Mach = sqrt(nodes->GetVelocity2(iPoint))/nodes->GetSoundSpeed(iPoint);

  /*--- Weiss and Smith Preconditionng ---*/
  Mach_infty2 = pow(config->GetMach(),2.0);
  Mach_lim2   = pow(0.00001,2.0);
  aux         = max(pow(local_Mach,2.0),Mach_lim2);
  parameter   = min(1.0, max(aux,Beta_max*Mach_infty2));


  U_i        = nodes->GetSolution(iPoint);
  rho        = nodes->GetDensity(iPoint);
  enthalpy   = nodes->GetEnthalpy(iPoint);
  soundspeed = nodes->GetSoundSpeed(iPoint);
  sq_vel     = nodes->GetVelocity2(iPoint);

  /*---Calculating the inverse of the preconditioning matrix that multiplies the time derivative  */
  LowMach_Precontioner[0][0] = 0.5*sq_vel;
  LowMach_Precontioner[0][nVar-1] = 1.0;
  for (iDim = 0; iDim < nDim; iDim ++)
    LowMach_Precontioner[0][1+iDim] = -1.0*U_i[iDim+1]/rho;

  for (iDim = 0; iDim < nDim; iDim ++) {
    LowMach_Precontioner[iDim+1][0] = 0.5*sq_vel*U_i[iDim+1]/rho;
    LowMach_Precontioner[iDim+1][nVar-1] = U_i[iDim+1]/rho;
    for (jDim = 0; jDim < nDim; jDim ++) {
      LowMach_Precontioner[iDim+1][1+jDim] = -1.0*U_i[jDim+1]/rho*U_i[iDim+1]/rho;
    }
  }

  LowMach_Precontioner[nVar-1][0] = 0.5*sq_vel*enthalpy;
  LowMach_Precontioner[nVar-1][nVar-1] = enthalpy;
  for (iDim = 0; iDim < nDim; iDim ++)
    LowMach_Precontioner[nVar-1][1+iDim] = -1.0*U_i[iDim+1]/rho*enthalpy;

  for (iVar = 0; iVar < nVar; iVar ++ ) {
    for (jVar = 0; jVar < nVar; jVar ++ ) {
      LowMach_Precontioner[iVar][jVar] = (parameter - 1.0) * (Gamma-1.0)/(soundspeed*soundspeed)*LowMach_Precontioner[iVar][jVar];
      if (iVar == jVar)
        LowMach_Precontioner[iVar][iVar] += 1.0;
    }
  }
}

void CNEMOEulerSolver::BC_Far_Field(CGeometry *geometry, CSolver **solver_container,
                                    CNumerics *conv_numerics, CNumerics *visc_numerics,
                                    CConfig *config, unsigned short val_marker) {
  unsigned short iDim;
  unsigned long iVertex, iPoint, Point_Normal;

  su2double *V_infty,  *V_domain;
  su2double *U_domain, *U_infty;

  /*--- Set booleans from configuration parameters ---*/
  bool implicit  = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  bool viscous   = config->GetViscous();
  bool tkeNeeded = (((config->GetKind_Solver() == NEMO_RANS ) ||
                     (config->GetKind_Solver() == DISC_ADJ_NEMO_RANS)) &&
                     (config->GetKind_Turb_Model() == SST));

  su2double *Normal = new su2double[nDim];

  /*--- Loop over all the vertices on this boundary (val_marker) ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/
    if (geometry->nodes->GetDomain(iPoint)) {

      /*--- Retrieve index of the closest interior node ---*/
      Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor(); //only used for implicit

      /*--- Pass boundary node normal to CNumerics ---*/
      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];
      conv_numerics->SetNormal(Normal);

      /*--- Retrieve solution and primitives
       *  at the boundary node & free-stream ---*/
      U_domain = nodes->GetSolution(iPoint);
      V_domain = nodes->GetPrimitive(iPoint);
      U_infty  = node_infty->GetSolution(0);
      V_infty  = node_infty->GetPrimitive(0);

      /*--- Pass conserved & primitive variables to CNumerics ---*/
      conv_numerics->SetConservative( U_domain, U_infty);
      conv_numerics->SetPrimitive(    V_domain, V_infty);

      /*--- Pass supplementary information to CNumerics ---*/
      conv_numerics->SetdPdU(   nodes->GetdPdU(iPoint),   node_infty->GetdPdU(0));
      conv_numerics->SetdTdU(   nodes->GetdTdU(iPoint),   node_infty->GetdTdU(0));
      conv_numerics->SetdTvedU( nodes->GetdTvedU(iPoint), node_infty->GetdTvedU(0));
      conv_numerics->SetEve(    nodes->GetEve(iPoint),    node_infty->GetEve(0));
      conv_numerics->SetCvve(   nodes->GetCvve(iPoint),   node_infty->GetCvve(0));

      /*--- Compute the convective residual (and Jacobian) ---*/
      auto residual = conv_numerics->ComputeResidual(config);

      /*--- Apply contribution to the linear system ---*/
      LinSysRes.AddBlock(iPoint, residual);

      /*--- Convective Jacobian contribution for implicit integration ---*/
      //if (implicit)
      //  Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);

      /*--- Viscous contribution ---*/
      if (viscous) {

        // TODO  CHECK IF THESE ARE NEVCESSARY
        unsigned short LAM_VISC_INDEX  = nodes->GetLamViscIndex();
        unsigned short EDDY_VISC_INDEX = nodes->GetEddyViscIndex();

        /*--- Set viscosities at the infinity ---*/
        V_infty[EDDY_VISC_INDEX] = V_domain[EDDY_VISC_INDEX];
        V_infty[LAM_VISC_INDEX]  = V_domain[LAM_VISC_INDEX];

        /*--- Set the normal vector and the coordinates ---*/
        visc_numerics->SetNormal(Normal);
        visc_numerics->SetCoord(geometry->nodes->GetCoord(iPoint),
                                geometry->nodes->GetCoord(Point_Normal) );

        /*--- Conservatice, primitive variables, and gradients ---*/
        visc_numerics->SetConservative(    nodes->GetSolution(iPoint),
                                           node_infty->GetSolution(0));
        visc_numerics->SetConsVarGradient( nodes->GetGradient(iPoint),
                                           node_infty->GetGradient(0));
        visc_numerics->SetPrimitive(       nodes->GetPrimitive(iPoint),
                                           node_infty->GetPrimitive(0));
        visc_numerics->SetPrimVarGradient( nodes->GetGradient_Primitive(iPoint),
                                           node_infty->GetGradient_Primitive(0));

        /*--- Pass supplementary information to CNumerics ---*/
        visc_numerics->SetdPdU(   nodes->GetdPdU(iPoint),   node_infty->GetdPdU(0));
        visc_numerics->SetdTdU(   nodes->GetdTdU(iPoint),   node_infty->GetdTdU(0));
        visc_numerics->SetdTvedU( nodes->GetdTvedU(iPoint), node_infty->GetdTvedU(0));
        visc_numerics->SetEve(    nodes->GetEve(iPoint),    node_infty->GetEve(0));
        visc_numerics->SetCvve(   nodes->GetCvve(iPoint),   node_infty->GetCvve(0));

        /*--- Species diffusion coefficients ---*/
        visc_numerics->SetDiffusionCoeff(nodes->GetDiffusionCoeff(iPoint),
                                         node_infty->GetDiffusionCoeff(0) );

        /*--- Laminar viscosity ---*/
        visc_numerics->SetLaminarViscosity(nodes->GetLaminarViscosity(iPoint),
                                           node_infty->GetLaminarViscosity(0) );

        /*--- Thermal conductivity ---*/
        visc_numerics->SetThermalConductivity(nodes->GetThermalConductivity(iPoint),
                                              node_infty->GetThermalConductivity(0));

        /*--- Vib-el. thermal conductivity ---*/
        visc_numerics->SetThermalConductivity_ve(nodes->GetThermalConductivity_ve(iPoint),
                                                 node_infty->GetThermalConductivity_ve(0) );

        /*--- Turbulent kinetic energy ---*/
        if ((config->GetKind_Turb_Model() == SST) || (config->GetKind_Turb_Model() == SST_SUST))
          visc_numerics->SetTurbKineticEnergy(solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint,0),
                                              solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint,0));

        /*--- Set the wall shear stress values (wall functions) to -1 (no evaluation using wall functions) ---*/
        visc_numerics->SetTauWall(-1.0, -1.0);

        /*--- Compute and update residual ---*/
        auto residual = visc_numerics->ComputeResidual(config);
        LinSysRes.SubtractBlock(iPoint, residual);

        /*--- Viscous Jacobian contribution for implicit integration ---*/
        //if (implicit) {
        //  Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
        //}
      }
    }
  }

  /*--- Free locally allocated memory ---*/
  delete [] Normal;
}

void CNEMOEulerSolver::BC_Inlet(CGeometry *geometry, CSolver **solver_container,
                                CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  SU2_MPI::Error("BC_INLET: Not operational in NEMO.", CURRENT_FUNCTION);

  unsigned short iVar, iDim, iSpecies, RHO_INDEX, nSpecies;
  unsigned long iVertex, iPoint;
  su2double  T_Total, P_Total, Velocity[3], Velocity2, H_Total, Temperature, Riemann,
      Pressure, Density, Energy, Mach2, SoundSpeed2, SoundSpeed_Total2, Vel_Mag,
      alpha, aa, bb, cc, dd, Area, UnitaryNormal[3];
  const su2double *Flow_Dir;

  bool dynamic_grid         = config->GetGrid_Movement();
  su2double Two_Gamma_M1    = 2.0/Gamma_Minus_One;
  su2double Gas_Constant    = config->GetGas_ConstantND();
  unsigned short Kind_Inlet = config->GetKind_Inlet();
  string Marker_Tag         = config->GetMarker_All_TagBound(val_marker);
  bool tkeNeeded            = (((config->GetKind_Solver() == NEMO_RANS )|| (config->GetKind_Solver() == DISC_ADJ_NEMO_RANS)) &&
                               (config->GetKind_Turb_Model() == SST));
  bool gravity              = (config->GetGravityForce());
  bool viscous              = config->GetViscous();

  su2double *U_domain = new su2double[nVar];      su2double *U_inlet = new su2double[nVar];
  su2double *V_domain = new su2double[nPrimVar];  su2double *V_inlet = new su2double[nPrimVar];
  su2double *Normal   = new su2double[nDim];

  su2double Spec_Density[config->GetnSpecies()];
  nSpecies = config->GetnSpecies();

  RHO_INDEX = nodes->GetRhoIndex();

  /*--- Loop over all the vertices on this boundary marker ---*/
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/
    if (geometry->nodes->GetDomain(iPoint)) {

      /*--- Normal vector for this vertex (negate for outward convention) ---*/
      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];
      conv_numerics->SetNormal(Normal);

      Area = 0.0;
      for (iDim = 0; iDim < nDim; iDim++) Area += Normal[iDim]*Normal[iDim];
      Area = sqrt (Area);

      /*--- Retrieve solution at this boundary node ---*/
      for (iVar = 0; iVar < nVar; iVar++)     U_domain[iVar] = nodes->GetSolution(iPoint, iVar);
      for (iVar = 0; iVar < nPrimVar; iVar++) V_domain[iVar] = nodes->GetPrimitive(iPoint,iVar);

      /*--- Build the fictitious intlet state based on characteristics ---*/

      /*--- Subsonic inflow: there is one outgoing characteristic (u-c),
       therefore we can specify all but one state variable at the inlet.
       The outgoing Riemann invariant provides the final piece of info.
       Adapted from an original implementation in the Stanford University
       multi-block (SUmb) solver in the routine bcSubsonicInflow.f90
       written by Edwin van der Weide, last modified 04-20-2009. ---*/

      switch (Kind_Inlet) {

      /*--- Total properties have been specified at the inlet. ---*/
      case TOTAL_CONDITIONS:

        /*--- Retrieve the specified total conditions for this inlet. ---*/
        P_Total  = config->GetInlet_Ptotal(Marker_Tag);
        T_Total  = config->GetInlet_Ttotal(Marker_Tag);
        Flow_Dir = config->GetInlet_FlowDir(Marker_Tag);

        /*--- Non-dim. the inputs if necessary. ---*/
        P_Total /= config->GetPressure_Ref();
        T_Total /= config->GetTemperature_Ref();

        /*--- Store primitives and set some variables for clarity. ---*/
        Density = V_domain[RHO_INDEX];
        Velocity2 = 0.0;
        for (iDim = 0; iDim < nDim; iDim++) {
          Velocity[iDim] = U_domain[nSpecies+iDim]/Density;
          Velocity2 += Velocity[iDim]*Velocity[iDim];
        }
        Energy      = U_domain[nVar-2]/Density;
        Pressure    = Gamma_Minus_One*Density*(Energy-0.5*Velocity2);
        H_Total     = (Gamma*Gas_Constant/Gamma_Minus_One)*T_Total;
        SoundSpeed2 = Gamma*Pressure/Density;

        /*--- Compute the acoustic Riemann invariant that is extrapolated
           from the domain interior. ---*/
        Riemann   = 2.0*sqrt(SoundSpeed2)/Gamma_Minus_One;
        for (iDim = 0; iDim < nDim; iDim++)
          Riemann += Velocity[iDim]*UnitaryNormal[iDim];

        /*--- Total speed of sound ---*/
        SoundSpeed_Total2 = Gamma_Minus_One*(H_Total - (Energy + Pressure/Density)+0.5*Velocity2) + SoundSpeed2;

        /*--- Dot product of normal and flow direction. This should
           be negative due to outward facing boundary normal convention. ---*/
        alpha = 0.0;
        for (iDim = 0; iDim < nDim; iDim++)
          alpha += UnitaryNormal[iDim]*Flow_Dir[iDim];

        /*--- Coefficients in the quadratic equation for the velocity ---*/
        aa =  1.0 + 0.5*Gamma_Minus_One*alpha*alpha;
        bb = -1.0*Gamma_Minus_One*alpha*Riemann;
        cc =  0.5*Gamma_Minus_One*Riemann*Riemann
            -2.0*SoundSpeed_Total2/Gamma_Minus_One;

        /*--- Solve quadratic equation for velocity magnitude. Value must
           be positive, so the choice of root is clear. ---*/
        dd = bb*bb - 4.0*aa*cc;
        dd = sqrt(max(0.0,dd));
        Vel_Mag   = (-bb + dd)/(2.0*aa);
        Vel_Mag   = max(0.0,Vel_Mag);
        Velocity2 = Vel_Mag*Vel_Mag;

        /*--- Compute speed of sound from total speed of sound eqn. ---*/
        SoundSpeed2 = SoundSpeed_Total2 - 0.5*Gamma_Minus_One*Velocity2;

        /*--- Mach squared (cut between 0-1), use to adapt velocity ---*/
        Mach2 = Velocity2/SoundSpeed2;
        Mach2 = min(1.0,Mach2);
        Velocity2   = Mach2*SoundSpeed2;
        Vel_Mag     = sqrt(Velocity2);
        SoundSpeed2 = SoundSpeed_Total2 - 0.5*Gamma_Minus_One*Velocity2;

        /*--- Compute new velocity vector at the inlet ---*/
        for (iDim = 0; iDim < nDim; iDim++)
          Velocity[iDim] = Vel_Mag*Flow_Dir[iDim];

        /*--- Static temperature from the speed of sound relation ---*/
        Temperature = SoundSpeed2/(Gamma*Gas_Constant);
        //NEED TVE AS WELL

        /*--- Static pressure using isentropic relation at a point ---*/
        Pressure = P_Total*pow((Temperature/T_Total),Gamma/Gamma_Minus_One);

        /*--- Density at the inlet from the gas law ---*/
        Density = Pressure/(Gas_Constant*Temperature);
        //NEED SPECIES DENSITIES

        /*--- Using pressure, density, & velocity, compute the energy ---*/
        Energy = Pressure/(Density*Gamma_Minus_One)+0.5*Velocity2;
        //NEED EVE AS WELL

        /*--- Conservative variables, using the derived quantities ---*/
        for (iSpecies=0; iSpecies<nSpecies; iSpecies++)
          U_inlet[iSpecies] = Spec_Density[iSpecies];
        for (iDim = 0; iDim < nDim; iDim++)
          U_inlet[nSpecies+iDim] = Velocity[iDim]*Density;
        U_inlet[nVar-2] = Energy*Density;
        //U_inlet[nVar-1]=Eve

        /*--- Primitive variables, using the derived quantities ---*/
        for (iSpecies=0; iSpecies<nSpecies; iSpecies++)
          V_inlet[iSpecies] = Spec_Density[iSpecies];
        V_inlet[nSpecies] = Temperature;
        //V_inlet[nSpecies+1] = Tve
        for (iDim = 0; iDim < nDim; iDim++)
          V_inlet[nSpecies+2] = Velocity[iDim];
        V_inlet[nSpecies+nDim+2] = Pressure;
        V_inlet[RHO_INDEX] = Density;
        //V_inlet[H_INDEX] = H;
        //V_inlet[A_INDEX] = A;
        //V_inlet[RHO_CVTR_INDEX] = rcvtr;
        //V_inlet[RHO_CVVE_INDEX] = rcvve;

        break;

        /*--- Mass flow has been specified at the inlet. ---*/
      case MASS_FLOW:

        /*--- Retrieve the specified mass flow for the inlet. ---*/
        Density  = config->GetInlet_Ttotal(Marker_Tag);
        Vel_Mag  = config->GetInlet_Ptotal(Marker_Tag);
        Flow_Dir = config->GetInlet_FlowDir(Marker_Tag);

        /*--- Non-dim. the inputs if necessary. ---*/
        Density /= config->GetDensity_Ref();
        Vel_Mag /= config->GetVelocity_Ref();

        /*--- Get primitives from current inlet state. ---*/
        for (iDim = 0; iDim < nDim; iDim++)
          Velocity[iDim] = nodes->GetVelocity(iPoint, iDim);
        Pressure    = nodes->GetPressure(iPoint);
        SoundSpeed2 = Gamma*Pressure/U_domain[0];

        /*--- Compute the acoustic Riemann invariant that is extrapolated
           from the domain interior. ---*/
        Riemann = Two_Gamma_M1*sqrt(SoundSpeed2);
        for (iDim = 0; iDim < nDim; iDim++)
          Riemann += Velocity[iDim]*UnitaryNormal[iDim];

        /*--- Speed of sound squared for fictitious inlet state ---*/
        SoundSpeed2 = Riemann;
        for (iDim = 0; iDim < nDim; iDim++)
          SoundSpeed2 -= Vel_Mag*Flow_Dir[iDim]*UnitaryNormal[iDim];

        SoundSpeed2 = max(0.0,0.5*Gamma_Minus_One*SoundSpeed2);
        SoundSpeed2 = SoundSpeed2*SoundSpeed2;

        /*--- Pressure for the fictitious inlet state ---*/
        Pressure = SoundSpeed2*Density/Gamma;

        /*--- Energy for the fictitious inlet state ---*/
        Energy = Pressure/(Density*Gamma_Minus_One)+0.5*Vel_Mag*Vel_Mag;

        /*--- Conservative variables, using the derived quantities ---*/
        U_inlet[0] = Density;
        for (iDim = 0; iDim < nDim; iDim++)
          U_inlet[iDim+1] = Vel_Mag*Flow_Dir[iDim]*Density;
        U_inlet[nDim+1] = Energy*Density;

        /*--- Primitive variables, using the derived quantities ---*/
        V_inlet[0] = Pressure / ( Gas_Constant * Density);
        for (iDim = 0; iDim < nDim; iDim++)
          V_inlet[iDim+1] = Vel_Mag*Flow_Dir[iDim];
        V_inlet[nDim+1] = Pressure;
        V_inlet[nDim+2] = Density;

        break;
      }

      /*--- Set various quantities in the solver class ---*/
      conv_numerics->SetConservative(U_domain, U_inlet);

      if (dynamic_grid)
        conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint), geometry->nodes->GetGridVel(iPoint));

      /*--- Compute the residual using an upwind scheme ---*/
      auto residual = conv_numerics->ComputeResidual(config);
      LinSysRes.AddBlock(iPoint, residual);

      /*--- Jacobian contribution for implicit integration ---*/
      //if (implicit) Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);

      //      /*--- Viscous contribution ---*/
      //      if (viscous) {

      //        /*--- Set the normal vector and the coordinates ---*/
      //        visc_numerics->SetNormal(Normal);
      //        visc_numerics->SetCoord(geometry->nodes->GetCoord(), geometry->node[Point_Normal]->GetCoord());

      //        /*--- Primitive variables, and gradient ---*/
      //        visc_numerics->SetPrimitive(V_domain, V_inlet);
      //        visc_numerics->SetPrimVarGradient(nodes->GetGradient_Primitive(), nodes->GetGradient_Primitive());

      //        /*--- Laminar viscosity ---*/
      //        visc_numerics->SetLaminarViscosity(nodes->GetLaminarViscosity(), nodes->GetLaminarViscosity());

      //        /*--- Compute and update residual ---*/
      //        visc_numerics->ComputeResidual(Residual, Jacobian_i, Jacobian_j, config);
      //        LinSysRes.SubtractBlock(iPoint, Residual);

      //        /*--- Jacobian contribution for implicit integration ---*/
      //        if (implicit)
      //          Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
      //      }
    }
  }

  /*--- Free locally allocated memory ---*/
  delete [] U_domain;
  delete [] U_inlet;
  delete [] V_domain;
  delete [] V_inlet;
  delete [] Normal;
}


void CNEMOEulerSolver::BC_Outlet(CGeometry *geometry, CSolver **solver_container,
                                 CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  unsigned short iVar, iDim, iSpecies;
  unsigned long iVertex, iPoint;
  su2double Pressure, P_Exit, Velocity[3],
      Temperature, Tve, Velocity2, Entropy, Density,
      Riemann, Vn, SoundSpeed, Mach_Exit, Vn_Exit, Area, UnitNormal[3];
  vector<su2double> rhos, energies;

  rhos.resize(nSpecies,0.0);

  bool implicit           = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  string Marker_Tag       = config->GetMarker_All_TagBound(val_marker);
  bool dynamic_grid       = config->GetGrid_Movement();
  bool gravity            = config->GetGravityForce();
  bool tkeNeeded          = (config->GetKind_Turb_Model() == SST) ||
                            (config->GetKind_Turb_Model() == SST_SUST);


  su2double *U_domain = new su2double[nVar];      su2double *U_outlet = new su2double[nVar];
  su2double *V_domain = new su2double[nPrimVar];  su2double *V_outlet = new su2double[nPrimVar];
  su2double *Normal   = new su2double[nDim];
  su2double *Ys       = new su2double[nSpecies];

  //TODO MAKE SO WE DONT NEED TO CALL EVERYTIME?
  unsigned short T_INDEX       = nodes->GetTIndex();
  unsigned short TVE_INDEX     = nodes->GetTveIndex();
  unsigned short VEL_INDEX     = nodes->GetVelIndex();
  unsigned short PRESS_INDEX   = nodes->GetPIndex();
  unsigned short RHO_INDEX     = nodes->GetRhoIndex();
  unsigned short H_INDEX       = nodes->GetHIndex();
  unsigned short A_INDEX       = nodes->GetAIndex();
  unsigned short RHOCVTR_INDEX = nodes->GetRhoCvtrIndex();
  unsigned short RHOCVVE_INDEX = nodes->GetRhoCvveIndex();


  /*--- Loop over all the vertices on this boundary marker ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/
    if (geometry->nodes->GetDomain(iPoint)) {

      /*--- Normal vector for this vertex (negate for outward convention) ---*/
      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];
      conv_numerics->SetNormal(Normal);

      Area = 0.0;
      for (iDim = 0; iDim < nDim; iDim++) Area += Normal[iDim]*Normal[iDim];
      Area = sqrt (Area);

      for (iDim = 0; iDim < nDim; iDim++)
        UnitNormal[iDim] = Normal[iDim]/Area;

      /*--- Current solution at this boundary node ---*/
      //TODO, could these loops be avoided?
      for (iVar = 0; iVar < nVar; iVar++)     U_domain[iVar] = nodes->GetSolution(iPoint, iVar);
      for (iVar = 0; iVar < nPrimVar; iVar++) V_domain[iVar] = nodes->GetPrimitive(iPoint, iVar);

      /*--- Initialize solution at outlet ---*/
      for (iVar = 0; iVar < nVar; iVar++)     U_outlet[iVar] = 0.0;
      for (iVar = 0; iVar < nPrimVar; iVar++) V_outlet[iVar] = 0.0;

      /*--- Build the fictitious intlet state based on characteristics ---*/

      /*--- Retrieve the specified back pressure for this outlet. ---*/
      if (gravity) P_Exit = config->GetOutlet_Pressure(Marker_Tag) - geometry->nodes->GetCoord(iPoint, nDim-1)*STANDARD_GRAVITY;
      else         P_Exit = config->GetOutlet_Pressure(Marker_Tag);

      /*--- Non-dim. the inputs if necessary. ---*/
      P_Exit = P_Exit/config->GetPressure_Ref();

      /*--- Check whether the flow is supersonic at the exit. The type
       of boundary update depends on this. ---*/
      Density = V_domain[RHO_INDEX];
      Velocity2 = 0.0; Vn = 0.0;
      for (iDim = 0; iDim < nDim; iDim++) {
        Velocity[iDim] = V_domain[VEL_INDEX+iDim];
        Velocity2 += Velocity[iDim]*Velocity[iDim];
        Vn += Velocity[iDim]*UnitNormal[iDim];
      }
      Temperature = V_domain[T_INDEX];
      Tve         = V_domain[TVE_INDEX];
      Pressure    = V_domain[PRESS_INDEX];
      SoundSpeed  = V_domain[A_INDEX];
      Mach_Exit   = sqrt(Velocity2)/SoundSpeed;

      /*--- Compute Species Concentrations ---*/
      //Using partial pressures, maybe not
      for (iSpecies =0; iSpecies<nSpecies;iSpecies++){
        Ys[iSpecies] = V_domain[iSpecies]/Density;
      }

      /*--- Recompute boundary state depending Mach number ---*/
      if (Mach_Exit >= 1.0) {

        /*--- Supersonic exit flow: there are no incoming characteristics,
         so no boundary condition is necessary. Set outlet state to current
         state so that upwinding handles the direction of propagation. ---*/
        for (iVar = 0; iVar < nVar; iVar++) U_outlet[iVar] = U_domain[iVar];
        for (iVar = 0; iVar < nPrimVar; iVar++) V_outlet[iVar] = V_domain[iVar];

      } else {

        /*--- Subsonic exit flow: there is one incoming characteristic,
         therefore one variable can be specified (back pressure) and is used
         to update the conservative variables. Compute the entropy and the
         acoustic Riemann variable. These invariants, as well as the
         tangential velocity components, are extrapolated. The Temperatures
         (T and Tve) and species concentraition are also assumed to be extrapolated.
         ---*/

        Entropy = Pressure*pow(1.0/Density,Gamma);
        Riemann = Vn + 2.0*SoundSpeed/Gamma_Minus_One;

        /*--- Compute the new fictious state at the outlet ---*/
        //     U: [rho1, ..., rhoNs, rhou, rhov, rhow, rhoe, rhoeve]^T
        //     V: [rho1, ..., rhoNs, T, Tve, u, v, w, P, rho, h, a, rhoCvtr, rhoCvve]^T
        Density    = pow(P_Exit/Entropy,1.0/Gamma);
        Pressure   = P_Exit;
        SoundSpeed = sqrt(Gamma*P_Exit/Density);
        Vn_Exit    = Riemann - 2.0*SoundSpeed/Gamma_Minus_One;
        Velocity2  = 0.0;
        for (iDim = 0; iDim < nDim; iDim++) {
          Velocity[iDim] = Velocity[iDim] + (Vn_Exit-Vn)*UnitNormal[iDim];
          Velocity2 += Velocity[iDim]*Velocity[iDim];
        }

        /*--- Primitive variables, using the derived quantities ---*/
        for (iSpecies = 0; iSpecies < nSpecies; iSpecies ++){
          V_outlet[iSpecies] = Ys[iSpecies]*Density;
          rhos[iSpecies]     = V_outlet[iSpecies];
        }

        V_outlet[T_INDEX]     = V_domain[T_INDEX];
        V_outlet[TVE_INDEX]   = V_domain[TVE_INDEX];

        for (iDim = 0; iDim < nDim; iDim++){
          V_outlet[VEL_INDEX+iDim] = Velocity[iDim];
        }

        V_outlet[PRESS_INDEX] = Pressure;
        V_outlet[RHO_INDEX]   = Density;
        V_outlet[A_INDEX]     = SoundSpeed;

        /*--- Set mixture state and compute quantities ---*/
        FluidModel->SetTDStateRhosTTv(rhos, Temperature, Tve);
        V_outlet[RHOCVTR_INDEX] = FluidModel->GetrhoCvtr();
        V_outlet[RHOCVVE_INDEX] = FluidModel->GetrhoCvve();

        energies = FluidModel->GetMixtureEnergies();

        /*--- Conservative variables, using the derived quantities ---*/
        for (iSpecies = 0; iSpecies < nSpecies; iSpecies ++){
          U_outlet[iSpecies] = V_outlet[iSpecies];
        }

        for (iDim = 0; iDim < nDim; iDim++)
          U_outlet[nSpecies+iDim] = Velocity[iDim]*Density;

        U_outlet[nVar-2] = (energies[0] + 0.5*Velocity2) * Density;
        U_outlet[nVar-1] = (energies[1] * Density);

      }

      /*--- Setting Last remaining variables ---*/
      V_outlet[H_INDEX]= (U_outlet[nVar-2]+Pressure)/Density;

      /*--- Set various quantities in the solver class ---*/
      conv_numerics->SetConservative(U_domain, U_outlet);
      conv_numerics->SetPrimitive(V_domain,V_outlet);

      if (dynamic_grid)
        conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint), geometry->nodes->GetGridVel(iPoint));

      /*--- Passing supplementary information to CNumerics ---*/
      conv_numerics->SetdPdU(  nodes->GetdPdU(iPoint),   node_infty->GetdPdU(0));
      conv_numerics->SetdTdU(  nodes->GetdTdU(iPoint),   node_infty->GetdTdU(0));
      conv_numerics->SetdTvedU(nodes->GetdTvedU(iPoint), node_infty->GetdTvedU(0));
      conv_numerics->SetEve(   nodes->GetEve(iPoint),    node_infty->GetEve(0));
      conv_numerics->SetCvve(  nodes->GetCvve(iPoint),   node_infty->GetCvve(0));

      /*--- Compute the residual using an upwind scheme ---*/
      auto residual = conv_numerics->ComputeResidual(config);
      LinSysRes.AddBlock(iPoint, residual);

      /*--- Jacobian contribution for implicit integration ---*/
      //if (implicit)
      //  Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);

      /*--- Viscous contribution ---*/
      //      if (viscous) {

      //        /*--- Set the normal vector and the coordinates ---*/
      //        visc_numerics->SetNormal(Normal);
      //        visc_numerics->SetCoord(geometry->nodes->GetCoord(), geometry->node[Point_Normal]->GetCoord());

      //        /*--- Primitive variables, and gradient ---*/
      //        visc_numerics->SetPrimitive(V_domain, V_outlet);
      //        visc_numerics->SetPrimVarGradient(nodes->GetGradient_Primitive(), nodes->GetGradient_Primitive());

      //        /*--- Conservative variables, and gradient ---*/
      //        visc_numerics->SetConservative(U_domain, U_outlet);
      //        visc_numerics->SetConsVarGradient(nodes->GetGradient(), node_infty->GetGradient() );


      //        /*--- Pass supplementary information to CNumerics ---*/
      //        visc_numerics->SetdPdU(nodes->GetdPdU(), node_infty->GetdPdU());
      //        visc_numerics->SetdTdU(nodes->GetdTdU(), node_infty->GetdTdU());
      //        visc_numerics->SetdTvedU(nodes->GetdTvedU(), node_infty->GetdTvedU());

      //        /*--- Species diffusion coefficients ---*/
      //        visc_numerics->SetDiffusionCoeff(nodes->GetDiffusionCoeff(),
      //                                         node_infty->GetDiffusionCoeff() );

      //        /*--- Laminar viscosity ---*/
      //        visc_numerics->SetLaminarViscosity(nodes->GetLaminarViscosity(),
      //                                           node_infty->GetLaminarViscosity() );

      //        /*--- Thermal conductivity ---*/
      //        visc_numerics->SetThermalConductivity(nodes->GetThermalConductivity(),
      //                                              node_infty->GetThermalConductivity());

      //        /*--- Vib-el. thermal conductivity ---*/
      //        visc_numerics->SetThermalConductivity_ve(nodes->GetThermalConductivity_ve(),
      //                                                 node_infty->GetThermalConductivity_ve() );

      //        /*--- Laminar viscosity ---*/
      //        visc_numerics->SetLaminarViscosity(nodes->GetLaminarViscosity(), nodes->GetLaminarViscosity());

      //        /*--- Compute and update residual ---*/
      //        visc_numerics->ComputeResidual(Residual, Jacobian_i, Jacobian_j, config);
      //        LinSysRes.SubtractBlock(iPoint, Residual);

      //        /*--- Jacobian contribution for implicit integration ---*/
      //        if (implicit)
      //          Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
      //      }
    }
  }

  /*--- Free locally allocated memory ---*/
  delete [] U_domain;
  delete [] U_outlet;
  delete [] V_domain;
  delete [] V_outlet;
  delete [] Normal;
  delete [] Ys;

}

void CNEMOEulerSolver::BC_Supersonic_Inlet(CGeometry *geometry, CSolver **solver_container,
                                           CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  SU2_MPI::Error("BC_SUPERSONIC_INLET: Not operational in NEMO.", CURRENT_FUNCTION);

  //  unsigned short iDim, iVar;
  //  unsigned long iVertex, iPoint, Point_Normal;
  //  su2double Density, Pressure, Temperature, Temperature_ve, Energy, *Velocity, Velocity2, soundspeed;
  //  su2double Gas_Constant = config->GetGas_ConstantND();
  //
  //  bool implicit = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  //  bool dynamic_grid  = config->GetGrid_Movement();
  //  bool viscous              = config->GetViscous();
  //  string Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  //
  //  su2double RuSI  = UNIVERSAL_GAS_CONSTANT;
  //  su2double Ru = 1000.0*RuSI;
  //
  //  su2double *U_inlet = new su2double[nVar];     su2double *U_domain = new su2double[nVar];
  //  su2double *V_inlet = new su2double[nPrimVar]; su2double *V_domain = new su2double[nPrimVar];
  //  su2double *Normal = new su2double[nDim];

  /* ----------------------------------------------------------------------------- */
  /* ----------------------------------------------------------------------------- */
  /* The block of code commented below needs to be updated to use Fluidmodel class */
  /* ----------------------------------------------------------------------------- */
  /* ----------------------------------------------------------------------------- */

  //  /*--- Supersonic inlet flow: there are no outgoing characteristics,
  //   so all flow variables can be imposed at the inlet.
  //   First, retrieve the specified values for the primitive variables. ---*/
  //  //ASSUME TVE = T for the time being
  //  auto Mass_Frac      = config->GetInlet_MassFrac(Marker_Tag);
  //  Temperature    = config->GetInlet_Temperature(Marker_Tag);
  //  Pressure       = config->GetInlet_Pressure(Marker_Tag);
  //  Velocity       = config->GetInlet_Velocity(Marker_Tag);
  //  Temperature_ve = Temperature;
  //
  //  /*--- Compute Density and Species Densities ---*/
  //  for (iSpecies = 0; iSpecies < nHeavy; iSpecies++)
  //    denom += Mass_Frac[iSpecies] * (Ru/Ms[iSpecies]) * Temperature;
  //  for (iSpecies = 0; iSpecies < nEl; iSpecies++)
  //    denom += Mass_Frac[nSpecies-1] * (Ru/Ms[nSpecies-1]) * Temperature_ve;
  //  Density = Pressure / denom;
  //
  //  /*--- Compute Soundspeed and Velocity squared ---*/
  //  for (iSpecies = 0; iSpecies < nHeavy; iSpecies++) {
  //    conc += Mass_Frac[iSpecies]*Density/Ms[iSpecies];
  //    rhoCvtr += Density*Mass_Frac[iSpecies] * (3.0/2.0 + xi[iSpecies]/2.0) * Ru/Ms[iSpecies];
  //  }
  //  soundspeed = sqrt((1.0 + Ru/rhoCvtr*conc) * Pressure/Density);
  //
  //  Velocity2 = 0.0;
  //  for (iDim = 0; iDim < nDim; iDim++)
  //    Velocity2 += Velocity[iDim]*Velocity[iDim];
  //
  //  /*--- Non-dim. the inputs if necessary. ---*/
  //  // Need to update this portion
  //  //Temperature = Temperature/config->GetTemperature_Ref();
  //  //Pressure    = Pressure/config->GetPressure_Ref();
  //  //Density     = Density/config->GetDensity_Ref();
  //  //for (iDim = 0; iDim < nDim; iDim++)
  //  //  Velocity[iDim] = Velocity[iDim]/config->GetVelocity_Ref();
  //
  //  /*--- Compute energy (RRHO) from supplied primitive quanitites ---*/
  //  for (iSpecies = 0; iSpecies < nHeavy; iSpecies++) {
  //
  //    // Species density
  //    rhos = Mass_Frac[iSpecies]*Density;
  //
  //    // Species formation energy
  //    Ef = hf[iSpecies] - Ru/Ms[iSpecies]*Tref[iSpecies];
  //
  //    // Species vibrational energy
  //    if (thetav[iSpecies] != 0.0)
  //      Ev = Ru/Ms[iSpecies] * thetav[iSpecies] / (exp(thetav[iSpecies]/Temperature_ve)-1.0);
  //    else
  //      Ev = 0.0;
  //
  //    // Species electronic energy
  //    num = 0.0;
  //    denom = g[iSpecies][0] * exp(thetae[iSpecies][0]/Temperature_ve);
  //    for (iEl = 1; iEl < nElStates[iSpecies]; iEl++) {
  //      num   += g[iSpecies][iEl] * thetae[iSpecies][iEl] * exp(-thetae[iSpecies][iEl]/Temperature_ve);
  //      denom += g[iSpecies][iEl] * exp(-thetae[iSpecies][iEl]/Temperature_ve);
  //    }
  //    Ee = Ru/Ms[iSpecies] * (num/denom);
  //
  //    // Mixture total energy
  //    rhoE += rhos * ((3.0/2.0+xi[iSpecies]/2.0) * Ru/Ms[iSpecies] * (Temperature-Tref[iSpecies])
  //                    + Ev + Ee + Ef + 0.5*Velocity2);
  //
  //    // Mixture vibrational-electronic energy
  //    rhoEve += rhos * (Ev + Ee);
  //  }
  //
  //  /*--- Setting Conservative Variables ---*/
  //  for (iSpecies = 0; iSpecies < nSpecies; iSpecies++)
  //    U_inlet[iSpecies] = Mass_Frac[iSpecies]*Density;
  //  for (iDim = 0; iDim < nDim; iDim++)
  //    U_inlet[nSpecies+iDim] = Density*Velocity[iDim];
  //  U_inlet[nVar-2] = rhoE;
  //  U_inlet[nVar-1] = rhoEve;
  //
  //  /*--- Setting Primitive Vaariables ---*/
  //  for (iSpecies = 0; iSpecies < nSpecies; iSpecies++)
  //    V_inlet[iSpecies] = Mass_Frac[iSpecies]*Density;
  //  V_inlet[nSpecies] = Temperature;
  //  V_inlet[nSpecies+1] = Temperature_ve;
  //  for (iDim = 0; iDim < nDim; iDim++)
  //    V_inlet[nSpecies+2+iDim] = Velocity[iDim];
  //  V_inlet[nSpecies+2+nDim] = Pressure;
  //  V_inlet[nSpecies+3+nDim] = Density;
  //  V_inlet[nSpecies+4+nDim] = rhoE+Pressure/Density;
  //  V_inlet[nSpecies+5+nDim] = soundspeed;
  //  V_inlet[nSpecies+6+nDim] = rhoCvtr;

  /* ----------------------------------------------------------------------------- */
  /* ----------------------------------------------------------------------------- */
  /* The block of code that needs to be updated to use Fluidmodel class end here   */
  /* ----------------------------------------------------------------------------- */
  /* ----------------------------------------------------------------------------- */

  //  //This requires Newtown Raphson.....So this is not currently operational (See Deathstar)
  //  //V_inlet[nSpecies+7+nDim] = rhoCvve;
  //
  //  /*--- Loop over all the vertices on this boundary marker ---*/
  //  for(iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
  //    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();
  //
  //    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/
  //    if (geometry->nodes->GetDomain(iPoint)) {
  //
  //      /*--- Index of the closest interior node ---*/
  //      Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();
  //
  //      /*--- Current solution at this boundary node ---*/
  //      for (iVar = 0; iVar < nVar; iVar++) U_domain[iVar] = nodes->GetSolution(iPoint,iVar);
  //      for (iVar = 0; iVar < nPrimVar; iVar++) V_domain[iVar] = nodes->GetPrimitive(iPoint,iVar);
  //
  //      /*--- Normal vector for this vertex (negate for outward convention) ---*/
  //      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
  //      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];
  //
  //      su2double Area = 0.0;
  //      for (iDim = 0; iDim < nDim; iDim++)
  //        Area += Normal[iDim]*Normal[iDim];
  //      Area = sqrt (Area);
  //
  //      /*--- Set various quantities in the solver class ---*/
  //      conv_numerics->SetNormal(Normal);
  //      conv_numerics->SetConservative(U_domain, U_inlet);
  //      conv_numerics->SetPrimitive(V_domain, V_inlet);
  //
  //      /*--- Pass supplementary info to CNumerics ---*/
  //      conv_numerics->SetdPdU(nodes->GetdPdU(iPoint), node_infty->GetdPdU(0));
  //      conv_numerics->SetdTdU(nodes->GetdTdU(iPoint), node_infty->GetdTdU(0));
  //      conv_numerics->SetdTvedU(nodes->GetdTvedU(iPoint), node_infty->GetdTvedU(0));
  //      conv_numerics->SetEve(nodes->GetEve(iPoint), node_infty->GetEve(0));
  //      conv_numerics->SetCvve(nodes->GetCvve(iPoint), node_infty->GetCvve(0));
  //
  //      if (dynamic_grid)
  //        conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint),
  //                                  geometry->nodes->GetGridVel(iPoint));
  //
  //      /*--- Compute the residual using an upwind scheme ---*/
  //      auto residual = conv_numerics->ComputeResidual(config);
  //      LinSysRes.AddBlock(iPoint, residual);
  //
  //      /*--- Jacobian contribution for implicit integration ---*/
  //      //if (implicit)
  //      //  Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);
  //
  //      /*--- Viscous contribution ---*/
  //      if (viscous) {
  //
  //        /*--- Set the normal vector and the coordinates ---*/
  //        visc_numerics->SetNormal(Normal);
  //        visc_numerics->SetCoord(geometry->nodes->GetCoord(iPoint), geometry->nodes->GetCoord(Point_Normal));
  //
  //        /*--- Primitive variables, and gradient ---*/
  //        visc_numerics->SetPrimitive(V_domain, V_inlet);
  //        visc_numerics->SetPrimVarGradient(nodes->GetGradient_Primitive(iPoint), nodes->GetGradient_Primitive(iPoint));
  //
  //        /*--- Laminar viscosity ---*/
  //        visc_numerics->SetLaminarViscosity(nodes->GetLaminarViscosity(iPoint), nodes->GetLaminarViscosity(iPoint));
  //
  //        /*--- Compute and update residual ---*/
  //        auto residual = visc_numerics->ComputeResidual(config);
  //        LinSysRes.SubtractBlock(iPoint, residual);
  //
  //        /*--- Jacobian contribution for implicit integration ---*/
  //        //if (implicit)
  //        //  Jacobian.SubtractBlock(iPoint, iPoint, Jacobian_i);
  //      }
  //
  //    }
  //  }
  //
  //  /*--- Free locally allocated memory ---*/
  //  delete [] U_domain;
  //  delete [] U_inlet;
  //  delete [] V_domain;
  //  delete [] V_inlet;
  //  delete [] Normal;

}

void CNEMOEulerSolver::BC_Supersonic_Outlet(CGeometry *geometry, CSolver **solver_container,
                                            CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {
  unsigned short iDim;
  unsigned long iVertex, iPoint;
  su2double *V_outlet, *V_domain;
  su2double *U_outlet, *U_domain;

  bool dynamic_grid  = config->GetGrid_Movement();
  string Marker_Tag = config->GetMarker_All_TagBound(val_marker);

  su2double *Normal = new su2double[nDim];

  /*--- Supersonic outlet flow: there are no ingoing characteristics,
   so all flow variables can should be interpolated from the domain. ---*/

  /*--- Loop over all the vertices on this boundary marker ---*/
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/
    if (geometry->nodes->GetDomain(iPoint)) {

      /*--- Current solution at this boundary node ---*/
      V_domain = nodes->GetPrimitive(iPoint);
      U_domain = nodes->GetSolution(iPoint);

      /*--- Allocate the value at the outlet ---*/
      V_outlet = V_domain;
      U_outlet = U_domain;

      /*--- Normal vector for this vertex (negate for outward convention) ---*/
      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];

      /*--- Set various quantities in the solver class ---*/
      conv_numerics->SetNormal(Normal);
      conv_numerics->SetPrimitive(V_domain, V_outlet);
      conv_numerics->SetConservative(U_domain, U_outlet);

      /*--- Pass supplementary information to CNumerics ---*/
      conv_numerics->SetdPdU(   nodes->GetdPdU(iPoint),   nodes->GetdPdU(iPoint));
      conv_numerics->SetdTdU(   nodes->GetdTdU(iPoint),   nodes->GetdTdU(iPoint));
      conv_numerics->SetdTvedU( nodes->GetdTvedU(iPoint), nodes->GetdTvedU(iPoint));
      conv_numerics->SetEve( nodes->GetEve(iPoint),       nodes->GetEve(iPoint));
      conv_numerics->SetCvve( nodes->GetCvve(iPoint),       nodes->GetCvve(iPoint));

      if (dynamic_grid)
        conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint),
                                  geometry->nodes->GetGridVel(iPoint));

      /*--- Compute the residual using an upwind scheme ---*/
      auto residual = conv_numerics->ComputeResidual(config);
      LinSysRes.AddBlock(iPoint, residual);

      /*--- Jacobian contribution for implicit integration ---*/
      //if (implicit)
      //  Jacobian.AddBlock(iPoint, iPoint, Jacobian_i);
    }
  }

  /*--- Free locally allocated memory ---*/
  delete [] Normal;

}


void CNEMOEulerSolver::BC_Sym_Plane(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                    CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  //TODO, THIS CAN BE UPDATED FURTHER
  unsigned short iDim, jDim, iSpecies, iVar, jVar;
  unsigned long iPoint, iVertex;

  bool implicit     = (config->GetKind_TimeIntScheme_Flow() == EULER_IMPLICIT);
  //bool viscous = config->GetViscous();
  //bool preprocessed = false;

  su2double *Normal = nullptr, Area, UnitNormal[3], *NormalArea,
      **Jacobian_b, **DubDu,
      rho, cs, P, rhoE, rhoEve, conc, *u, *dPdU;

  /*--- Allocate arrays ---*/
  Normal     = new su2double[nDim];
  NormalArea = new su2double[nDim];
  Jacobian_b = new su2double*[nVar];
  DubDu      = new su2double*[nVar];
  u          = new su2double[nDim];

  for (iVar = 0; iVar < nVar; iVar++) {
    Jacobian_b[iVar] = new su2double[nVar];
    DubDu[iVar] = new su2double[nVar];
  }

  /*--- Get species molar mass ---*/
  vector <su2double> Ms = FluidModel->GetMolarMass();

  /*--- Loop over all the vertices on this boundary (val_marker) ---*/
  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/
    if (geometry->nodes->GetDomain(iPoint)) {

      /*--- Normal vector for this vertex (negative for outward convention) ---*/
      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);

      /*--- Calculate parameters from the geometry ---*/
      Area   = 0.0;
      for (iDim = 0; iDim < nDim; iDim++) Area += Normal[iDim]*Normal[iDim];
      Area = sqrt (Area);

      for (iDim = 0; iDim < nDim; iDim++){
        NormalArea[iDim] = -Normal[iDim];
        UnitNormal[iDim] = -Normal[iDim]/Area;
      }

      /*--- Retrieve the pressure on the vertex ---*/
      P   = nodes->GetPressure(iPoint);

      /*--- Apply the flow-tangency b.c. to the convective flux ---*/
      for (iSpecies = 0; iSpecies < nSpecies; iSpecies++)
        Residual[iSpecies] = 0.0;
      for (iDim = 0; iDim < nDim; iDim++)
        Residual[nSpecies+iDim] = P * UnitNormal[iDim] * Area;
      Residual[nSpecies+nDim]   = 0.0;
      Residual[nSpecies+nDim+1] = 0.0;

      /*--- Add value to the residual ---*/
      LinSysRes.AddBlock(iPoint, Residual);

      /*--- If using implicit time-stepping, calculate b.c. contribution to Jacobian ---*/
      if (implicit) {

        /*--- Initialize Jacobian ---*/
        for (iVar = 0; iVar < nVar; iVar++)
          for (jVar = 0; jVar < nVar; jVar++)
            Jacobian_i[iVar][jVar] = 0.0;

        /*--- Calculate state i ---*/
        rho     = nodes->GetDensity(iPoint);
        rhoE    = nodes->GetSolution(iPoint,nSpecies+nDim);
        rhoEve  = nodes->GetSolution(iPoint,nSpecies+nDim+1);
        dPdU    = nodes->GetdPdU(iPoint);
        for (iDim = 0; iDim < nDim; iDim++)
          u[iDim] = nodes->GetVelocity(iPoint,iDim);

        conc = 0.0;
        for (iSpecies = 0; iSpecies < nSpecies; iSpecies++) {
          cs    = nodes->GetMassFraction(iPoint,iSpecies);
          conc += cs * rho/Ms[iSpecies];

          /////// NEW //////
          for (iDim = 0; iDim < nDim; iDim++) {
            Jacobian_i[nSpecies+iDim][iSpecies] = dPdU[iSpecies] * UnitNormal[iDim];
            Jacobian_i[iSpecies][nSpecies+iDim] = cs * UnitNormal[iDim];
          }
        }

        for (iDim = 0; iDim < nDim; iDim++) {
          for (jDim = 0; jDim < nDim; jDim++) {
            Jacobian_i[nSpecies+iDim][nSpecies+jDim] = u[iDim]*UnitNormal[jDim]
                + dPdU[nSpecies+jDim]*UnitNormal[iDim];
          }
          Jacobian_i[nSpecies+iDim][nSpecies+nDim]   = dPdU[nSpecies+nDim]  *UnitNormal[iDim];
          Jacobian_i[nSpecies+iDim][nSpecies+nDim+1] = dPdU[nSpecies+nDim+1]*UnitNormal[iDim];

          Jacobian_i[nSpecies+nDim][nSpecies+iDim]   = (rhoE+P)/rho * UnitNormal[iDim];
          Jacobian_i[nSpecies+nDim+1][nSpecies+iDim] = rhoEve/rho   * UnitNormal[iDim];
        }

        /*--- Integrate over the dual-grid area ---*/
        for (iVar = 0; iVar < nVar; iVar++)
          for (jVar = 0; jVar < nVar; jVar++)
            Jacobian_i[iVar][jVar] = Jacobian_i[iVar][jVar] * Area;

        /*--- Apply the contribution to the system ---*/
        Jacobian.AddBlock(iPoint,iPoint,Jacobian_i);

      }
    }
  }
  delete [] Normal;
  delete [] NormalArea;
  delete [] u;

  for (iVar = 0; iVar < nVar; iVar++) {
    delete [] Jacobian_b[iVar];
    delete [] DubDu[iVar];
  }

  delete [] Jacobian_b;
  delete [] DubDu;
}

void CNEMOEulerSolver::SetResidual_DualTime(CGeometry *geometry,CSolver **solver_container, CConfig *config,
                                            unsigned short iRKStep, unsigned short iMesh, unsigned short RunTime_EqSystem) {

  /*--- Local variables ---*/
  unsigned short iVar, jVar, iMarker, iDim, iNeigh;
  unsigned long iPoint, jPoint, iEdge, iVertex;

  const su2double *U_time_nM1 = nullptr, *U_time_n = nullptr, *U_time_nP1 = nullptr;
  su2double Volume_nM1, Volume_n, Volume_nP1, TimeStep;

  const su2double *Normal = nullptr, *GridVel_i = nullptr, *GridVel_j = nullptr;
  su2double Residual_GCL;

  const bool implicit     = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool first_order  = (config->GetTime_Marching() == DT_STEPPING_1ST);
  const bool second_order = (config->GetTime_Marching() == DT_STEPPING_2ND);

  /*--- Store the physical time step ---*/

  TimeStep = config->GetDelta_UnstTimeND();

  /*--- Compute the dual time-stepping source term for static meshes ---*/

  if (!dynamic_grid) {

    /*--- Loop over all nodes (excluding halos) ---*/

    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {

      /*--- Retrieve the solution at time levels n-1, n, and n+1. Note that
       we are currently iterating on U^n+1 and that U^n & U^n-1 are fixed,
       previous solutions that are stored in memory. ---*/

      U_time_nM1 = nodes->GetSolution_time_n1(iPoint);
      U_time_n   = nodes->GetSolution_time_n(iPoint);
      U_time_nP1 = nodes->GetSolution(iPoint);

      /*--- CV volume at time n+1. As we are on a static mesh, the volume
       of the CV will remained fixed for all time steps. ---*/

      Volume_nP1 = geometry->nodes->GetVolume(iPoint);

      /*--- Compute the dual time-stepping source term based on the chosen
       time discretization scheme (1st- or 2nd-order).---*/

      for (iVar = 0; iVar < nVar; iVar++) {
        if (first_order)
          LinSysRes(iPoint,iVar) += (U_time_nP1[iVar] - U_time_n[iVar])*Volume_nP1 / TimeStep;
        if (second_order)
          LinSysRes(iPoint,iVar) += ( 3.0*U_time_nP1[iVar] - 4.0*U_time_n[iVar]
                                     +1.0*U_time_nM1[iVar])*Volume_nP1 / (2.0*TimeStep);
      }

      /*--- Compute the Jacobian contribution due to the dual time source term. ---*/
      if (implicit) {
        if (first_order) Jacobian.AddVal2Diag(iPoint, Volume_nP1/TimeStep);
        if (second_order) Jacobian.AddVal2Diag(iPoint, (Volume_nP1*3.0)/(2.0*TimeStep));
      }
    }

  }
  else {

    /*--- For unsteady flows on dynamic meshes (rigidly transforming or
     dynamically deforming), the Geometric Conservation Law (GCL) should be
     satisfied in conjunction with the ALE formulation of the governing
     equations. The GCL prevents accuracy issues caused by grid motion, i.e.
     a uniform free-stream should be preserved through a moving grid. First,
     we will loop over the edges and boundaries to compute the GCL component
     of the dual time source term that depends on grid velocities. ---*/

    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < nPointDomain; ++iPoint) {

      GridVel_i = geometry->nodes->GetGridVel(iPoint);
      U_time_n = nodes->GetSolution_time_n(iPoint);

      for (iNeigh = 0; iNeigh < geometry->nodes->GetnPoint(iPoint); iNeigh++) {

        iEdge = geometry->nodes->GetEdge(iPoint, iNeigh);
        Normal = geometry->edges->GetNormal(iEdge);

        jPoint = geometry->nodes->GetPoint(iPoint, iNeigh);
        GridVel_j = geometry->nodes->GetGridVel(jPoint);

        /*--- Determine whether to consider the normal outward or inward. ---*/
        su2double dir = (geometry->edges->GetNode(iEdge,0) == iPoint)? 0.5 : -0.5;

        Residual_GCL = 0.0;
        for (iDim = 0; iDim < nDim; iDim++)
          Residual_GCL += dir*(GridVel_i[iDim]+GridVel_j[iDim])*Normal[iDim];

        for (iVar = 0; iVar < nVar; iVar++)
          LinSysRes(iPoint,iVar) += U_time_n[iVar]*Residual_GCL;
      }
    }

    /*--- Loop over the boundary edges ---*/

    for (iMarker = 0; iMarker < geometry->GetnMarker(); iMarker++) {
      if ((config->GetMarker_All_KindBC(iMarker) != INTERNAL_BOUNDARY)  &&
          (config->GetMarker_All_KindBC(iMarker) != PERIODIC_BOUNDARY)) {

        SU2_OMP_FOR_STAT(OMP_MIN_SIZE)
        for (iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++) {

          /*--- Get the index for node i plus the boundary face normal ---*/

          iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
          Normal = geometry->vertex[iMarker][iVertex]->GetNormal();

          /*--- Grid velocities stored at boundary node i ---*/

          GridVel_i = geometry->nodes->GetGridVel(iPoint);

          /*--- Compute the GCL term by dotting the grid velocity with the face
           normal. The normal is negated to match the boundary convention. ---*/

          Residual_GCL = 0.0;
          for (iDim = 0; iDim < nDim; iDim++)
            Residual_GCL -= 0.5*(GridVel_i[iDim]+GridVel_i[iDim])*Normal[iDim];

          /*--- Compute the GCL component of the source term for node i ---*/

          U_time_n = nodes->GetSolution_time_n(iPoint);
          for (iVar = 0; iVar < nVar; iVar++)
            LinSysRes(iPoint,iVar) += U_time_n[iVar]*Residual_GCL;
        }
      }
    }

    /*--- Loop over all nodes (excluding halos) to compute the remainder
     of the dual time-stepping source term. ---*/

    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < nPointDomain; iPoint++) {

      /*--- Retrieve the solution at time levels n-1, n, and n+1. Note that
       we are currently iterating on U^n+1 and that U^n & U^n-1 are fixed,
       previous solutions that are stored in memory. ---*/

      U_time_nM1 = nodes->GetSolution_time_n1(iPoint);
      U_time_n   = nodes->GetSolution_time_n(iPoint);
      U_time_nP1 = nodes->GetSolution(iPoint);

      /*--- CV volume at time n-1 and n+1. In the case of dynamically deforming
       grids, the volumes will change. On rigidly transforming grids, the
       volumes will remain constant. ---*/

      Volume_nM1 = geometry->nodes->GetVolume_nM1(iPoint);
      Volume_nP1 = geometry->nodes->GetVolume(iPoint);

      /*--- Compute the dual time-stepping source residual. Due to the
       introduction of the GCL term above, the remainder of the source residual
       due to the time discretization has a new form.---*/

      for (iVar = 0; iVar < nVar; iVar++) {
        if (first_order)
          LinSysRes(iPoint,iVar) += (U_time_nP1[iVar] - U_time_n[iVar])*(Volume_nP1/TimeStep);
        if (second_order)
          LinSysRes(iPoint,iVar) += (U_time_nP1[iVar] - U_time_n[iVar])*(3.0*Volume_nP1/(2.0*TimeStep))
                                       + (U_time_nM1[iVar] - U_time_n[iVar])*(Volume_nM1/(2.0*TimeStep));
      }

      /*--- Compute the Jacobian contribution due to the dual time source term. ---*/
      if (implicit) {
        if (first_order) Jacobian.AddVal2Diag(iPoint, Volume_nP1/TimeStep);
        if (second_order) Jacobian.AddVal2Diag(iPoint, (Volume_nP1*3.0)/(2.0*TimeStep));
      }
    }
  }
}

void CNEMOEulerSolver::LoadRestart(CGeometry **geometry, CSolver ***solver, CConfig *config, int val_iter, bool val_update_geo) {

  /*--- Restart the solution from file information ---*/

  unsigned short iDim, iVar, iMesh, iMeshFine;
  unsigned long iPoint, index, iChildren, Point_Fine;
  unsigned short turb_model = config->GetKind_Turb_Model();
  su2double Area_Children, Area_Parent;
  const su2double* Solution_Fine = nullptr;
  const passivedouble* Coord = nullptr;
  bool dual_time      = ((config->GetTime_Marching() == DT_STEPPING_1ST) ||
                         (config->GetTime_Marching() == DT_STEPPING_2ND));
  bool steady_restart = config->GetSteadyRestart();
  bool turbulent      = (config->GetKind_Turb_Model() != NONE);

  string  restart_filename = config->GetFilename(config->GetSolution_FileName(), "", val_iter);

  /*--- To make this routine safe to call in parallel most of it can only be executed by one thread. ---*/
  SU2_OMP_MASTER {

  /*--- Skip coordinates ---*/

  unsigned short skipVars = geometry[MESH_0]->GetnDim();

  /*--- Store the number of variables for the turbulence model
   (that could appear in the restart file before the grid velocities). ---*/

  unsigned short turbVars = 0;
  if (turbulent){
    if ((turb_model == SST) || (turb_model == SST_SUST)) turbVars = 2;
    else turbVars = 1;
  }

  /*--- Read the restart data from either an ASCII or binary SU2 file. ---*/

  if (config->GetRead_Binary_Restart()) {
    Read_SU2_Restart_Binary(geometry[MESH_0], config, restart_filename);
  } else {
    Read_SU2_Restart_ASCII(geometry[MESH_0], config, restart_filename);
  }

  /*--- Load data from the restart into correct containers. ---*/

  unsigned long counter = 0, iPoint_Global = 0;
  for (; iPoint_Global < geometry[MESH_0]->GetGlobal_nPointDomain(); iPoint_Global++) {

    /*--- Retrieve local index. If this node from the restart file lives
     on the current processor, we will load and instantiate the vars. ---*/

    auto iPoint_Local = geometry[MESH_0]->GetGlobal_to_Local_Point(iPoint_Global);

    if (iPoint_Local > -1) {

      /*--- We need to store this point's data, so jump to the correct
        offset in the buffer of data from the restart file and load it. ---*/

      index = counter*Restart_Vars[1] + skipVars;
      for (iVar = 0; iVar < nVar; ++iVar)
        nodes->SetSolution(iPoint_Local, iVar, Restart_Data[index+iVar]);

      /*--- For dynamic meshes, read in and store the
       grid coordinates and grid velocities for each node. ---*/

      if (dynamic_grid && val_update_geo) {

        /*--- Read in the next 2 or 3 variables which are the grid velocities ---*/
        /*--- If we are restarting the solution from a previously computed static calculation (no grid movement) ---*/
        /*--- the grid velocities are set to 0. This is useful for FSI computations ---*/

        /*--- Rewind the index to retrieve the Coords. ---*/
        index = counter*Restart_Vars[1];
        Coord = &Restart_Data[index];

        su2double GridVel[MAXNDIM] = {0.0};
        if (!steady_restart) {
          /*--- Move the index forward to get the grid velocities. ---*/
          index += skipVars + nVar + turbVars;
          for (iDim = 0; iDim < nDim; iDim++) { GridVel[iDim] = Restart_Data[index+iDim]; }
        }

        for (iDim = 0; iDim < nDim; iDim++) {
          geometry[MESH_0]->nodes->SetCoord(iPoint_Local, iDim, Coord[iDim]);
          geometry[MESH_0]->nodes->SetGridVel(iPoint_Local, iDim, GridVel[iDim]);
        }
      }

      /*--- Increment the overall counter for how many points have been loaded. ---*/
      counter++;
    }

 }

  /*--- Detect a wrong solution file ---*/

  if (counter != nPointDomain) {
    SU2_MPI::Error(string("The solution file ") + restart_filename + string(" doesn't match with the mesh file!\n") +
                   string("It could be empty lines at the end of the file."), CURRENT_FUNCTION);
  }
  } // end SU2_OMP_MASTER
  SU2_OMP_BARRIER

  /*--- Update the geometry for flows on deforming meshes ---*/

  if ((dynamic_grid) && val_update_geo) {

    /*--- Communicate the new coordinates and grid velocities at the halos ---*/

    geometry[MESH_0]->InitiateComms(geometry[MESH_0], config, COORDINATES);
    geometry[MESH_0]->CompleteComms(geometry[MESH_0], config, COORDINATES);

    if (dynamic_grid) {
      geometry[MESH_0]->InitiateComms(geometry[MESH_0], config, GRID_VELOCITY);
      geometry[MESH_0]->CompleteComms(geometry[MESH_0], config, GRID_VELOCITY);
    }

    /*--- Recompute the edges and dual mesh control volumes in the
     domain and on the boundaries. ---*/

    geometry[MESH_0]->SetCoord_CG();
    geometry[MESH_0]->SetControlVolume(config, UPDATE);
    geometry[MESH_0]->SetBoundControlVolume(config, UPDATE);
    geometry[MESH_0]->SetMaxLength(config);

    /*--- Update the multigrid structure after setting up the finest grid,
     including computing the grid velocities on the coarser levels. ---*/

    for (iMesh = 1; iMesh <= config->GetnMGLevels(); iMesh++) {
      iMeshFine = iMesh-1;
      geometry[iMesh]->SetControlVolume(config, geometry[iMeshFine], UPDATE);
      geometry[iMesh]->SetBoundControlVolume(config, geometry[iMeshFine],UPDATE);
      geometry[iMesh]->SetCoord(geometry[iMeshFine]);
      if (dynamic_grid) {
        geometry[iMesh]->SetRestricted_GridVelocity(geometry[iMeshFine], config);
      }
      geometry[iMesh]->SetMaxLength(config);
    }
  }

  /*--- Communicate the loaded solution on the fine grid before we transfer
   it down to the coarse levels. We also call the preprocessing routine
   on the fine level in order to have all necessary quantities updated,
   especially if this is a turbulent simulation (eddy viscosity). ---*/

  solver[MESH_0][FLOW_SOL]->InitiateComms(geometry[MESH_0], config, SOLUTION);
  solver[MESH_0][FLOW_SOL]->CompleteComms(geometry[MESH_0], config, SOLUTION);

  /*--- For turbulent simulations the flow preprocessing is done by the turbulence solver
   *    after it loads its variables (they are needed to compute flow primitives). ---*/
  if (!turbulent) {
    solver[MESH_0][FLOW_SOL]->Preprocessing(geometry[MESH_0], solver[MESH_0], config, MESH_0, NO_RK_ITER, RUNTIME_FLOW_SYS, false);
  }

  /*--- Interpolate the solution down to the coarse multigrid levels ---*/

  for (iMesh = 1; iMesh <= config->GetnMGLevels(); iMesh++) {
    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < geometry[iMesh]->GetnPoint(); iPoint++) {
      Area_Parent = geometry[iMesh]->nodes->GetVolume(iPoint);
      su2double Solution_Coarse[MAXNVAR] = {0.0};
      for (iChildren = 0; iChildren < geometry[iMesh]->nodes->GetnChildren_CV(iPoint); iChildren++) {
        Point_Fine = geometry[iMesh]->nodes->GetChildren_CV(iPoint, iChildren);
        Area_Children = geometry[iMesh-1]->nodes->GetVolume(Point_Fine);
        Solution_Fine = solver[iMesh-1][FLOW_SOL]->GetNodes()->GetSolution(Point_Fine);
        for (iVar = 0; iVar < nVar; iVar++) {
          Solution_Coarse[iVar] += Solution_Fine[iVar]*Area_Children/Area_Parent;
        }
      }
      solver[iMesh][FLOW_SOL]->GetNodes()->SetSolution(iPoint,Solution_Coarse);
    }

    solver[iMesh][FLOW_SOL]->InitiateComms(geometry[iMesh], config, SOLUTION);
    solver[iMesh][FLOW_SOL]->CompleteComms(geometry[iMesh], config, SOLUTION);

    if (!turbulent) {
      solver[iMesh][FLOW_SOL]->Preprocessing(geometry[iMesh], solver[iMesh], config, iMesh, NO_RK_ITER, RUNTIME_FLOW_SYS, false);
    }
  }

  /*--- Update the old geometry (coordinates n and n-1) in dual time-stepping strategy. ---*/
  if (dual_time && config->GetGrid_Movement() && !config->GetDeform_Mesh() &&
      (config->GetKind_GridMovement() != RIGID_MOTION)) {
    Restart_OldGeometry(geometry[MESH_0], config);
  }

  /*--- Go back to single threaded execution. ---*/
  SU2_OMP_MASTER
  {
  /*--- Delete the class memory that is used to load the restart. ---*/

  delete [] Restart_Vars; Restart_Vars = nullptr;
  delete [] Restart_Data; Restart_Data = nullptr;

  } // end SU2_OMP_MASTER
  SU2_OMP_BARRIER
}
