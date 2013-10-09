/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2013 Stanford University and the Authors.           *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "CpuKernels.h"
#include "openmm/Context.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/internal/NonbondedForceImpl.h"
#include "RealVec.h"

using namespace OpenMM;
using namespace std;

static vector<RealVec>& extractPositions(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *((vector<RealVec>*) data->positions);
}

static vector<RealVec>& extractVelocities(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *((vector<RealVec>*) data->velocities);
}

static vector<RealVec>& extractForces(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *((vector<RealVec>*) data->forces);
}

static RealVec& extractBoxSize(ContextImpl& context) {
    ReferencePlatform::PlatformData* data = reinterpret_cast<ReferencePlatform::PlatformData*>(context.getPlatformData());
    return *(RealVec*) data->periodicBoxSize;
}

CpuCalcNonbondedForceKernel::~CpuCalcNonbondedForceKernel() {
}

void CpuCalcNonbondedForceKernel::initialize(const System& system, const NonbondedForce& force) {

    // Identify which exceptions are 1-4 interactions.

    numParticles = force.getNumParticles();
    exclusions.resize(numParticles);
    vector<int> nb14s;
    for (int i = 0; i < force.getNumExceptions(); i++) {
        int particle1, particle2;
        double chargeProd, sigma, epsilon;
        force.getExceptionParameters(i, particle1, particle2, chargeProd, sigma, epsilon);
        exclusions[particle1].insert(particle2);
        exclusions[particle2].insert(particle1);
        if (chargeProd != 0.0 || epsilon != 0.0)
            nb14s.push_back(i);
    }

    // Record the particle parameters.

    num14 = nb14s.size();
//    bonded14IndexArray = allocateIntArray(num14, 2);
//    bonded14ParamArray = allocateRealArray(num14, 3);
//    particleParamArray = allocateRealArray(numParticles, 3);
    posq.resize(4*numParticles, 0);
    for (int i = 0; i < numParticles; ++i) {
        double charge, radius, depth;
        force.getParticleParameters(i, charge, radius, depth);
        posq[4*i+3] = (float) charge;
//        particleParamArray[i][0] = static_cast<RealOpenMM>(0.5*radius);
//        particleParamArray[i][1] = static_cast<RealOpenMM>(2.0*sqrt(depth));
//        particleParamArray[i][2] = static_cast<RealOpenMM>(charge);
    }
//    this->exclusions = exclusions;
//    for (int i = 0; i < num14; ++i) {
//        int particle1, particle2;
//        double charge, radius, depth;
//        force.getExceptionParameters(nb14s[i], particle1, particle2, charge, radius, depth);
//        bonded14IndexArray[i][0] = particle1;
//        bonded14IndexArray[i][1] = particle2;
//        bonded14ParamArray[i][0] = static_cast<RealOpenMM>(radius);
//        bonded14ParamArray[i][1] = static_cast<RealOpenMM>(4.0*depth);
//        bonded14ParamArray[i][2] = static_cast<RealOpenMM>(charge);
//    }
    nonbondedMethod = CalcNonbondedForceKernel::NonbondedMethod(force.getNonbondedMethod());
    nonbondedCutoff = force.getCutoffDistance();
    if (nonbondedMethod == NoCutoff)
        useSwitchingFunction = false;
    else {
        useSwitchingFunction = force.getUseSwitchingFunction();
        switchingDistance = force.getSwitchingDistance();
    }
    if (nonbondedMethod == Ewald) {
        double alpha;
        NonbondedForceImpl::calcEwaldParameters(system, force, alpha, kmax[0], kmax[1], kmax[2]);
        ewaldAlpha = alpha;
    }
    else if (nonbondedMethod == PME) {
        double alpha;
        NonbondedForceImpl::calcPMEParameters(system, force, alpha, gridSize[0], gridSize[1], gridSize[2]);
        ewaldAlpha = alpha;
    }
    rfDielectric = force.getReactionFieldDielectric();
    if (force.getUseDispersionCorrection())
        dispersionCoefficient = NonbondedForceImpl::calcDispersionCorrection(system, force);
    else
        dispersionCoefficient = 0.0;
}

double CpuCalcNonbondedForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy, bool includeDirect, bool includeReciprocal) {
    vector<RealVec>& posData = extractPositions(context);
    vector<RealVec>& forceData = extractForces(context);
    RealVec boxSize = extractBoxSize(context);
    float floatBoxSize[3] = {(float) boxSize[0], (float) boxSize[1], (float) boxSize[2]};
    double energy = 0;
//    CpuLJCoulombIxn clj;
    bool periodic = (nonbondedMethod == CutoffPeriodic);
    bool ewald  = (nonbondedMethod == Ewald);
    bool pme  = (nonbondedMethod == PME);
    
    // Convert the positions to single precision.
    
    if (periodic)
        for (int i = 0; i < numParticles; i++)
            for (int j = 0; j < 3; j++) {
                RealOpenMM x = posData[i][j];
                double base = floor(x/boxSize[j]+0.5)*boxSize[j];
                posq[4*i+j] = (float) (x-base);
            }
    else
        for (int i = 0; i < numParticles; i++) {
            posq[4*i] = (float) posData[i][0];
            posq[4*i+1] = (float) posData[i][1];
            posq[4*i+2] = (float) posData[i][2];
        }
    neighborList.computeNeighborList(numParticles, posq, exclusions, floatBoxSize, periodic || ewald || pme, nonbondedCutoff);
//    if (nonbondedMethod != NoCutoff) {
//        computeNeighborListVoxelHash(*neighborList, numParticles, posData, exclusions, extractBoxSize(context), periodic || ewald || pme, nonbondedCutoff, 0.0);
//        clj.setUseCutoff(nonbondedCutoff, *neighborList, rfDielectric);
//    }
//    if (periodic || ewald || pme) {
//        RealVec& box = extractBoxSize(context);
//        double minAllowedSize = 1.999999*nonbondedCutoff;
//        if (box[0] < minAllowedSize || box[1] < minAllowedSize || box[2] < minAllowedSize)
//            throw OpenMMException("The periodic box size has decreased to less than twice the nonbonded cutoff.");
//        clj.setPeriodic(box);
//    }
//    if (ewald)
//        clj.setUseEwald(ewaldAlpha, kmax[0], kmax[1], kmax[2]);
//    if (pme)
//        clj.setUsePME(ewaldAlpha, gridSize);
//    if (useSwitchingFunction)
//        clj.setUseSwitchingFunction(switchingDistance);
//    clj.calculatePairIxn(numParticles, posData, particleParamArray, exclusions, 0, forceData, 0, includeEnergy ? &energy : NULL, includeDirect, includeReciprocal);
//    if (includeDirect) {
//        CpuBondForce refBondForce;
//        CpuLJCoulomb14 nonbonded14;
//        refBondForce.calculateForce(num14, bonded14IndexArray, posData, bonded14ParamArray, forceData, includeEnergy ? &energy : NULL, nonbonded14);
//        if (periodic || ewald || pme) {
//            RealVec& boxSize = extractBoxSize(context);
//            energy += dispersionCoefficient/(boxSize[0]*boxSize[1]*boxSize[2]);
//        }
//    }
//    return energy;
    return 0.0;
}

void CpuCalcNonbondedForceKernel::copyParametersToContext(ContextImpl& context, const NonbondedForce& force) {
//    if (force.getNumParticles() != numParticles)
//        throw OpenMMException("updateParametersInContext: The number of particles has changed");
//    vector<int> nb14s;
//    for (int i = 0; i < force.getNumExceptions(); i++) {
//        int particle1, particle2;
//        double chargeProd, sigma, epsilon;
//        force.getExceptionParameters(i, particle1, particle2, chargeProd, sigma, epsilon);
//        if (chargeProd != 0.0 || epsilon != 0.0)
//            nb14s.push_back(i);
//    }
//    if (nb14s.size() != num14)
//        throw OpenMMException("updateParametersInContext: The number of non-excluded exceptions has changed");
//
//    // Record the values.
//
//    for (int i = 0; i < numParticles; ++i) {
//        double charge, radius, depth;
//        force.getParticleParameters(i, charge, radius, depth);
//        particleParamArray[i][0] = static_cast<RealOpenMM>(0.5*radius);
//        particleParamArray[i][1] = static_cast<RealOpenMM>(2.0*sqrt(depth));
//        particleParamArray[i][2] = static_cast<RealOpenMM>(charge);
//    }
//    for (int i = 0; i < num14; ++i) {
//        int particle1, particle2;
//        double charge, radius, depth;
//        force.getExceptionParameters(nb14s[i], particle1, particle2, charge, radius, depth);
//        bonded14IndexArray[i][0] = particle1;
//        bonded14IndexArray[i][1] = particle2;
//        bonded14ParamArray[i][0] = static_cast<RealOpenMM>(radius);
//        bonded14ParamArray[i][1] = static_cast<RealOpenMM>(4.0*depth);
//        bonded14ParamArray[i][2] = static_cast<RealOpenMM>(charge);
//    }
//    
//    // Recompute the coefficient for the dispersion correction.
//
//    NonbondedForce::NonbondedMethod method = force.getNonbondedMethod();
//    if (force.getUseDispersionCorrection() && (method == NonbondedForce::CutoffPeriodic || method == NonbondedForce::Ewald || method == NonbondedForce::PME))
//        dispersionCoefficient = NonbondedForceImpl::calcDispersionCorrection(context.getSystem(), force);
}
