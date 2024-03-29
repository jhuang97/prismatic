// Copyright Alan (AJ) Pryor, Jr. 2017
// Transcribed from MATLAB code by Colin Ophus
// Prismatic is distributed under the GNU General Public License (GPL)
// If you use Prismatic, we kindly ask that you cite the following papers:

// 1. Ophus, C.: A fast image simulation algorithm for scanning
//    transmission electron microscopy. Advanced Structural and
//    Chemical Imaging 3(1), 13 (2017)

// 2. Pryor, Jr., A., Ophus, C., and Miao, J.: A Streaming Multi-GPU
//    Implementation of Image Simulation Algorithms for Scanning
//	  Transmission Electron Microscopy. arXiv:1706.08563 (2017)

#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <numeric>
#include "configure.h"
#include <numeric>
#include "meta.h"
#include "ArrayND.h"
#include "params.h"
#include "utility.h"
#include "fftw3.h"
#include "WorkDispatcher.h"
#include "Multislice_calcOutput.h"
#include "fileIO.h"

namespace Prismatic{
	using namespace std;
	static const PRISMATIC_FLOAT_PRECISION pi = acos(-1);
	static const std::complex<PRISMATIC_FLOAT_PRECISION> i(0, 1);
	mutex fftw_plan_lock; // for synchronizing access to shared FFTW resources
	// mutex HDF5_lock;

	void setupCoordinates_multislice(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){

		// setup coordinates and build propagators
		std::vector<PRISMATIC_FLOAT_PRECISION> xp_d;
		std::vector<PRISMATIC_FLOAT_PRECISION> yp_d;
		if(pars.meta.arbitraryProbes)
		{
			xp_d = pars.meta.probes_x;
			yp_d = pars.meta.probes_y;
			pars.numXprobes = xp_d.size();
			pars.numYprobes = 1;
			pars.numProbes = xp_d.size();
		}
		else
		{
			Array1D<PRISMATIC_FLOAT_PRECISION> xR = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{2}});
			xR[0] = pars.scanWindowXMin * pars.tiledCellDim[2];
			xR[1] = pars.scanWindowXMax * pars.tiledCellDim[2];
			Array1D<PRISMATIC_FLOAT_PRECISION> yR = zeros_ND<1, PRISMATIC_FLOAT_PRECISION>({{2}});
			yR[0] = pars.scanWindowYMin * pars.tiledCellDim[1];
			yR[1] = pars.scanWindowYMax * pars.tiledCellDim[1];

			PRISMATIC_FLOAT_PRECISION probeStepX;
			PRISMATIC_FLOAT_PRECISION probeStepY;
			if(pars.meta.nyquistSampling){
				int numX = nyquistProbes(pars,2); //x is dim 2
				int numY = nyquistProbes(pars,1); //y is dim 1
				probeStepX = pars.tiledCellDim[2]/numX;
				probeStepY = pars.tiledCellDim[1]/numY;
			}else{
				probeStepX = pars.meta.probeStepX;
				probeStepY = pars.meta.probeStepY;
			}
			
			xp_d = vecFromRange(xR[0], probeStepX, xR[1]);
			yp_d = vecFromRange(yR[0], probeStepY, yR[1]);
		
			pars.numXprobes = xp_d.size();
			pars.numYprobes = yp_d.size();
			pars.numProbes = xp_d.size()*yp_d.size();
		}

		Array1D<PRISMATIC_FLOAT_PRECISION> xp(xp_d, {{xp_d.size()}});
		Array1D<PRISMATIC_FLOAT_PRECISION> yp(yp_d, {{yp_d.size()}});

		pars.xp = xp;
		pars.yp = yp;
		pars.imageSize[0] = pars.pot.get_dimj();
		pars.imageSize[1] = pars.pot.get_dimi();
		Array1D<PRISMATIC_FLOAT_PRECISION> qx = makeFourierCoords(pars.imageSize[1], pars.pixelSize[1]);
		Array1D<PRISMATIC_FLOAT_PRECISION> qy = makeFourierCoords(pars.imageSize[0], pars.pixelSize[0]);
		pars.qx = qx;
		pars.qy = qy;

		pair< Array2D<PRISMATIC_FLOAT_PRECISION>, Array2D<PRISMATIC_FLOAT_PRECISION> > mesh = meshgrid(qy,qx);
		pars.qya = mesh.first;
		pars.qxa = mesh.second;
		Array2D<PRISMATIC_FLOAT_PRECISION> q2(pars.qya);
		transform(pars.qxa.begin(), pars.qxa.end(),
		          pars.qya.begin(), q2.begin(), [](const PRISMATIC_FLOAT_PRECISION& a, const PRISMATIC_FLOAT_PRECISION& b){
					return a*a + b*b;
				});
		Array2D<PRISMATIC_FLOAT_PRECISION> q1(q2);
		pars.q2 = q2;
		pars.q1 = q1;
		for (auto& q : pars.q1)q=sqrt(q);

		// get qMax
		long long ncx = (long long) floor((PRISMATIC_FLOAT_PRECISION) pars.imageSize[1] / 2);
		PRISMATIC_FLOAT_PRECISION dpx = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[1] * pars.meta.realspacePixelSize[1]);
		long long ncy = (long long) floor((PRISMATIC_FLOAT_PRECISION) pars.imageSize[0] / 2);
		PRISMATIC_FLOAT_PRECISION dpy = 1.0 / ((PRISMATIC_FLOAT_PRECISION)pars.imageSize[0] * pars.meta.realspacePixelSize[0]);
		pars.qMax = std::min(dpx*(ncx), dpy*(ncy)) / 2;

		pars.qMask = zeros_ND<2, unsigned int>({{pars.imageSize[0], pars.imageSize[1]}});
		{
			long offset_x = pars.qMask.get_dimi()/4;
			long offset_y = pars.qMask.get_dimj()/4;
			long ndimy = (long)pars.qMask.get_dimj();
			long ndimx = (long)pars.qMask.get_dimi();
			for (long y = 0; y < pars.qMask.get_dimj() / 2; ++y) {
				for (long x = 0; x < pars.qMask.get_dimi() / 2; ++x) {
					pars.qMask.at( ((y-offset_y) % ndimy + ndimy) % ndimy,
					               ((x-offset_x) % ndimx + ndimx) % ndimx) = 1;
				}
			}
		}

		// build propagators
		pars.prop     = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION> >({{pars.imageSize[0], pars.imageSize[1]}});
		pars.propBack = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION> >({{pars.imageSize[0], pars.imageSize[1]}});
		for (auto y = 0; y < pars.qMask.get_dimj(); ++y) {
			for (auto x = 0; x < pars.qMask.get_dimi(); ++x) {
				if (pars.qMask.at(y,x)==1)
				{
		//					pars.prop.at(y,x)     = exp(-i * pi * complex<PRISMATIC_FLOAT_PRECISION>(pars.lambda, 0) *
		//					                            complex<PRISMATIC_FLOAT_PRECISION>(pars.meta.sliceThickness, 0) *
		//					                            complex<PRISMATIC_FLOAT_PRECISION>(pars.q2.at(y, x), 0));
		//					pars.propBack.at(y,x) = exp(i * pi * complex<PRISMATIC_FLOAT_PRECISION>(pars.lambda, 0) *
		//					                            complex<PRISMATIC_FLOAT_PRECISION>(pars.tiledCellDim[0], 0) *
		//					                            complex<PRISMATIC_FLOAT_PRECISION>(pars.q2.at(y, x), 0));

					pars.prop.at(y,x)     = exp(-i*pi*complex<PRISMATIC_FLOAT_PRECISION>(pars.lambda, 0) *
												complex<PRISMATIC_FLOAT_PRECISION>(pars.meta.sliceThickness, 0) *
												complex<PRISMATIC_FLOAT_PRECISION>(pars.q2.at(y, x), 0) +
												i * complex<PRISMATIC_FLOAT_PRECISION>(2, 0)*pi *
												complex<PRISMATIC_FLOAT_PRECISION>(pars.meta.sliceThickness, 0) *
												(qx[x] * tan(pars.meta.probeXtilt) + qy[y] * tan(pars.meta.probeYtilt)));

				}
			}
		}

	}

	void setupDetector_multislice(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){
		pars.alphaMax = pars.qMax * pars.lambda;
		vector<PRISMATIC_FLOAT_PRECISION> detectorAngles_d = vecFromRange(pars.meta.detectorAngleStep * 1000 / 2,
																	      pars.meta.detectorAngleStep * 1000,
																	      (pars.alphaMax - pars.meta.detectorAngleStep / 2) * 1000);
		Array1D<PRISMATIC_FLOAT_PRECISION> detectorAngles(detectorAngles_d, {{detectorAngles_d.size()}});
		pars.detectorAngles = detectorAngles;
		pars.Ndet = pars.detectorAngles.size();
		Array2D<PRISMATIC_FLOAT_PRECISION> alpha = pars.q1 * pars.lambda;
		pars.alphaInd = (alpha + pars.meta.detectorAngleStep/2) / pars.meta.detectorAngleStep;
		for (auto& q : pars.alphaInd) q = std::round(q);
		pars.dq = (pars.qxa.at(0, 1) + pars.qya.at(1, 0)) / 2;
	}

	void setupProbes_multislice(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){

		PRISMATIC_FLOAT_PRECISION qProbeMax = pars.meta.probeSemiangle/ pars.lambda; // currently a single semiangle
		pars.psiProbeInit = zeros_ND<2, complex<PRISMATIC_FLOAT_PRECISION> >({{pars.q1.get_dimj(), pars.q1.get_dimi()}});

		// erf probe is deprecated, but keeping the source here in case we ever want to flexibly switch
		// transform(pars.psiProbeInit.begin(), pars.psiProbeInit.end(),
		//           pars.q1.begin(), pars.psiProbeInit.begin(),
		//           [&pars, &qProbeMax](std::complex<PRISMATIC_FLOAT_PRECISION> &a, PRISMATIC_FLOAT_PRECISION &q1_t) {
		// 	          a.real(erf((qProbeMax - q1_t) / (0.5 * pars.dq)) * 0.5 + 0.5);
		// 	          a.imag(0);
		// 	          return a;
		//           });

		PRISMATIC_FLOAT_PRECISION dqx = pars.qxa.at(0,1);
		PRISMATIC_FLOAT_PRECISION dqy = pars.qya.at(1,0);
		for(auto j = 0; j < pars.q1.get_dimj(); j++)
		{
			for(auto i = 0; i < pars.q1.get_dimi(); i++)
			{
				PRISMATIC_FLOAT_PRECISION tmp_val = (qProbeMax*pars.q1.at(j,i) - pars.q2.at(j,i));
				tmp_val /= sqrt(dqx*dqx*pow(pars.qxa.at(j,i),2.0)+dqy*dqy*pow(pars.qya.at(j,i),2.0));					
				tmp_val += 0.5; 
				tmp_val = std::max(tmp_val, (PRISMATIC_FLOAT_PRECISION) 0.0);
				tmp_val = std::min(tmp_val, (PRISMATIC_FLOAT_PRECISION) 1.0);
				pars.psiProbeInit.at(j,i).real(tmp_val);
			}
		}

		pars.psiProbeInit.at(0,0).real(1.0);

		//apply aberrations
		pars.qTheta = pars.q1;
		std::transform(pars.qxa.begin(), pars.qxa.end(),
					   pars.qya.begin(), pars.qTheta.begin(), [](const PRISMATIC_FLOAT_PRECISION&a, const PRISMATIC_FLOAT_PRECISION& b){
						   return atan2(b,a);
					   });
		
		Array2D<std::complex<PRISMATIC_FLOAT_PRECISION>> chi = getChi(pars.q1, pars.qTheta, pars.lambda, pars.meta.aberrations);

		transform(pars.psiProbeInit.begin(), pars.psiProbeInit.end(),
				chi.begin(), pars.psiProbeInit.begin(),
				[](std::complex<PRISMATIC_FLOAT_PRECISION> &a, std::complex<PRISMATIC_FLOAT_PRECISION> &b) {
					a = a * exp(-i * b);
					return a;
				});


		PRISMATIC_FLOAT_PRECISION norm_constant = sqrt(accumulate(pars.psiProbeInit.begin(), pars.psiProbeInit.end(),
		                                                      (PRISMATIC_FLOAT_PRECISION)0.0, [](PRISMATIC_FLOAT_PRECISION accum, std::complex<PRISMATIC_FLOAT_PRECISION> &a) {
					return accum + abs(a) * abs(a);
				})); // make sure to initialize with 0.0 and NOT 0 or it won't be a float and answer will be wrong

		transform(pars.psiProbeInit.begin(), pars.psiProbeInit.end(),
		          pars.psiProbeInit.begin(), [&norm_constant](std::complex<PRISMATIC_FLOAT_PRECISION> &a) {
					return a / norm_constant;
				});

		if(pars.meta.saveProbe && pars.fpFlag == 0)
		{
            setupProbeOutput(pars);
            saveProbe(pars);
		}
	}

	void createTransmission(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){
		pars.transmission = zeros_ND<3, complex<PRISMATIC_FLOAT_PRECISION> >(
				{{pars.pot.get_dimk(), pars.pot.get_dimj(), pars.pot.get_dimi()}});
		{
			auto p = pars.pot.begin();
			for (auto &j:pars.transmission)j = exp(i * pars.sigma * (*p++));
		}
	}

	void createStack(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){
		size_t numLayers = (pars.numPlanes / pars.numSlices) + ((pars.numPlanes) % pars.numSlices != 0);
		if(pars.zStartPlane > 0)  numLayers += ((pars.zStartPlane) % pars.numSlices == 0) - (pars.zStartPlane / pars.numSlices) ;

		size_t firstLayer = (pars.zStartPlane / pars.numSlices) + ((pars.zStartPlane) % pars.numSlices != 0);
		if(pars.zStartPlane == 0) firstLayer = 1;

		cout << "Number of layers: " << numLayers << endl;
		cout << "First output depth is at " << firstLayer * pars.meta.sliceThickness * pars.numSlices << " angstroms with steps of " << pars.numSlices * pars.meta.sliceThickness << " angstroms" << endl;
		//store depths in vector
		std::vector<PRISMATIC_FLOAT_PRECISION> depths(numLayers);
		depths[0] = firstLayer * pars.meta.sliceThickness * pars.numSlices;
		for(auto i = 1; i < numLayers; i++) depths[i] = depths[i-1]+pars.numSlices*pars.meta.sliceThickness;
		pars.depths = depths;
		pars.numLayers = numLayers;
		
		pars.output = zeros_ND<4, PRISMATIC_FLOAT_PRECISION>({{numLayers, pars.numYprobes, pars.numXprobes, pars.Ndet}});

		if(pars.meta.saveDPC_CoM) pars.DPC_CoM = zeros_ND<4, PRISMATIC_FLOAT_PRECISION>({{numLayers,pars.numYprobes, pars.numXprobes,2}});
		if(pars.meta.save4DOutput)
		{
			if(pars.fpFlag == 0 or pars.meta.saveComplexOutputWave) setup4DOutput(pars);

			if(pars.meta.saveComplexOutputWave)
			{
				pars.cbed_buffer_c = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{pars.imageSize[0]/2, pars.imageSize[1]/2}});
				if(pars.meta.crop4DOutput) pars.cbed_buffer_c = cropOutput(pars.cbed_buffer_c, pars);
			}
			else
			{
				pars.cbed_buffer = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.imageSize[0]/2, pars.imageSize[1]/2}});
				if(pars.meta.crop4DOutput) pars.cbed_buffer = cropOutput(pars.cbed_buffer, pars);
			}
			
		}
	}

	void formatOutput_CPU_integrate(Parameters<PRISMATIC_FLOAT_PRECISION>& pars,
	                                       Array2D< complex<PRISMATIC_FLOAT_PRECISION> >& psi,
	                                       const Array2D<PRISMATIC_FLOAT_PRECISION> &alphaInd,
										   const size_t currentSlice,
	                                       const size_t ay,
	                                       const size_t ax){
		Array2D<PRISMATIC_FLOAT_PRECISION> intOutput = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{psi.get_dimj(), psi.get_dimi()}});
		
		auto psi_ptr = psi.begin();

		for (auto& j:intOutput) j = pow(abs(*psi_ptr++),2);


		if (pars.meta.saveDPC_CoM){
			//calculate center of mass; qxa, qya are the fourier coordinates, should have 0 components at boundaries
			for (long y = 0; y < psi.get_dimj(); ++y){
				for (long x = 0; x < psi.get_dimi(); ++x){
					pars.DPC_CoM.at(currentSlice,ay,ax,0) += pars.qxa.at(y,x) * intOutput.at(y,x);
					pars.DPC_CoM.at(currentSlice,ay,ax,1) += pars.qya.at(y,x) * intOutput.at(y,x);
				}
			}
			//divide by sum of intensity
			PRISMATIC_FLOAT_PRECISION intensitySum = 0;
			for (auto iter = intOutput.begin(); iter != intOutput.end(); ++iter){
				intensitySum += *iter;
			}
			pars.DPC_CoM.at(currentSlice,ay,ax,0) /= intensitySum;
			pars.DPC_CoM.at(currentSlice,ay,ax,1) /= intensitySum;
		}

		//update stack -- ax,ay are unique per thread so this write is thread-safe without a lock
		auto idx = alphaInd.begin();
		for (auto counts = intOutput.begin(); counts != intOutput.end(); ++counts){
			if (*idx <= pars.Ndet){
				pars.output.at(currentSlice,ay,ax,(*idx)-1) += *counts;
			}
			++idx;
		};

		//save 4D output if applicable
		if (pars.meta.save4DOutput)
		{

			std::string nameString = "4DSTEM_simulation/data/datacubes/CBED_array_depth" + getDigitString(currentSlice);
			nameString += pars.currentTag;

			PRISMATIC_FLOAT_PRECISION numFP = pars.meta.numFP;
			hsize_t offset[4] = {ax,ay,0,0}; //order by ax, ay so that aligns with py4DSTEM
			hsize_t mdims[4];
			mdims[0] = mdims[1] = {1};
			
			if(pars.meta.saveComplexOutputWave)
			{
				nameString += "_fp" + getDigitString(pars.meta.fpNum);
				Array2D<std::complex<PRISMATIC_FLOAT_PRECISION>> intOutput_small;
				
				if(pars.meta.crop4DOutput)
				{
					intOutput_small = cropOutput(psi, pars);
				}
				else
				{
					intOutput_small = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{psi.get_dimi()/2, psi.get_dimj()/2}});
					{
						long offset_x = psi.get_dimi() / 4;
						long offset_y = psi.get_dimj() / 4;
						long ndimy = (long) psi.get_dimj();
						long ndimx = (long) psi.get_dimi();
						for (long y = 0; y < psi.get_dimj() / 2; ++y) {
							for (long x = 0; x < psi.get_dimi() / 2; ++x) {
								intOutput_small.at(x, y) = psi.at(((y - offset_y) % ndimy + ndimy) % ndimy,
															((x - offset_x) % ndimx + ndimx) % ndimx);
							}
						}
					}
				}

				mdims[2] = {intOutput_small.get_dimj()};
				mdims[3] = {intOutput_small.get_dimi()};
				writeDatacube4D(pars,&intOutput_small[0],&pars.cbed_buffer_c[0],mdims,offset,numFP,nameString.c_str());

			}
			else
			{

				Array2D<PRISMATIC_FLOAT_PRECISION> intOutput_small;
				
				if(pars.meta.crop4DOutput)
				{
					intOutput_small = cropOutput(intOutput,pars);
				}
				else
				{
					intOutput_small = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{psi.get_dimi()/2, psi.get_dimj()/2}});
					{
						long offset_x = psi.get_dimi() / 4;
						long offset_y = psi.get_dimj() / 4;
						long ndimy = (long) psi.get_dimj();
						long ndimx = (long) psi.get_dimi();
						for (long y = 0; y < psi.get_dimj() / 2; ++y) {
							for (long x = 0; x < psi.get_dimi() / 2; ++x) {
								intOutput_small.at(x, y) = intOutput.at(((y - offset_y) % ndimy + ndimy) % ndimy,
															((x - offset_x) % ndimx + ndimx) % ndimx);
							}
						}
					}
				}

				mdims[2] = {intOutput_small.get_dimj()};
				mdims[3] = {intOutput_small.get_dimi()};
				writeDatacube4D(pars,&intOutput_small[0],&pars.cbed_buffer[0],mdims,offset,numFP,nameString.c_str());

			}
		}
	}
	
	void formatOutput_CPU_integrate_batch(Parameters<PRISMATIC_FLOAT_PRECISION>& pars,
	                                      Array1D< complex<PRISMATIC_FLOAT_PRECISION> >& psi_stack,
	                                      const Array2D<PRISMATIC_FLOAT_PRECISION> &alphaInd,
	                                      size_t Nstart,
	                                      const size_t Nstop,
										  const size_t currentSlice){
		int probe_idx = 0;
		while (Nstart < Nstop) {
			//since constant must use ternary operator
			//ay and ax aren't used to calculate, just for IO and data copies
			const size_t ay = (pars.meta.arbitraryProbes) ? 0 : Nstart / pars.numXprobes;
			const size_t ax = (pars.meta.arbitraryProbes) ? Nstart : Nstart % pars.numXprobes;

			//can't just use PSI like in single integrate for complex output
			Array2D<PRISMATIC_FLOAT_PRECISION> intOutput = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.psiProbeInit.get_dimj(), pars.psiProbeInit.get_dimi()}});
			Array2D<std::complex<PRISMATIC_FLOAT_PRECISION>> intOutput_c;
			if(pars.meta.saveComplexOutputWave)
				intOutput_c = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{pars.psiProbeInit.get_dimj(), pars.psiProbeInit.get_dimi()}});
			
			auto psi_ptr = &psi_stack[probe_idx*pars.psiProbeInit.size()];
			for (auto& j:intOutput) j = pow(abs(*psi_ptr++),2);
			if(pars.meta.saveComplexOutputWave)
			{
				//reset pointer
				psi_ptr = &psi_stack[probe_idx*pars.psiProbeInit.size()];
				for (auto& j:intOutput_c) j = *psi_ptr++;
			}

			if (pars.meta.saveDPC_CoM){
				//calculate center of mass; qxa, qya are the fourier coordinates, should have 0 components at boundaries
				for (long y = 0; y < pars.psiProbeInit.get_dimj(); ++y){
					for (long x = 0; x < pars.psiProbeInit.get_dimi(); ++x){
						pars.DPC_CoM.at(currentSlice,ay,ax,0) += pars.qxa.at(y,x) * intOutput.at(y,x);
						pars.DPC_CoM.at(currentSlice,ay,ax,1) += pars.qya.at(y,x) * intOutput.at(y,x);
					}
				}

				//divide by sum of intensity
				PRISMATIC_FLOAT_PRECISION intensitySum = 0;
				for (auto iter = intOutput.begin(); iter != intOutput.end(); ++iter){
					intensitySum += *iter;
				}
				pars.DPC_CoM.at(currentSlice,ay,ax,0) /= intensitySum;
				pars.DPC_CoM.at(currentSlice,ay,ax,1) /= intensitySum;
			}

			//update stack -- ax,ay are unique per thread so this write is thread-safe without a lock
			auto idx = alphaInd.begin();
			for (auto counts = intOutput.begin(); counts != intOutput.end(); ++counts) {
				if (*idx <= pars.Ndet) {
					pars.output.at(currentSlice, ay, ax, (*idx) - 1) += *counts;
				}
				++idx;
			};

			if (pars.meta.save4DOutput)
			{
				std::string nameString = "4DSTEM_simulation/data/datacubes/CBED_array_depth" + getDigitString(currentSlice);
				nameString += pars.currentTag;

				PRISMATIC_FLOAT_PRECISION numFP = pars.meta.numFP;
				hsize_t offset[4] = {ax,ay,0,0}; //order by ax, ay so that aligns with py4DSTEM
				hsize_t mdims[4];
				mdims[0] = mdims[1] = {1};
				
				if(pars.meta.saveComplexOutputWave)
				{
					nameString += "_fp" + getDigitString(pars.meta.fpNum);
					Array2D<std::complex<PRISMATIC_FLOAT_PRECISION>> intOutput_small;
					
					if(pars.meta.crop4DOutput)
					{
						intOutput_small = cropOutput(intOutput_c, pars);
					}
					else
					{
						intOutput_small = zeros_ND<2, std::complex<PRISMATIC_FLOAT_PRECISION>>({{pars.psiProbeInit.get_dimi()/2, pars.psiProbeInit.get_dimj()/2}});
						{
							long offset_x = pars.psiProbeInit.get_dimi() / 4;
							long offset_y = pars.psiProbeInit.get_dimj() / 4;
							long ndimy = (long) pars.psiProbeInit.get_dimj();
							long ndimx = (long) pars.psiProbeInit.get_dimi();
							for (long y = 0; y < pars.psiProbeInit.get_dimj() / 2; ++y) {
								for (long x = 0; x < pars.psiProbeInit.get_dimi() / 2; ++x) {
									intOutput_small.at(x, y) = intOutput_c.at(((y - offset_y) % ndimy + ndimy) % ndimy,
																((x - offset_x) % ndimx + ndimx) % ndimx);
								}
							}
						}
					}

					mdims[2] = {intOutput_small.get_dimj()};
					mdims[3] = {intOutput_small.get_dimi()};
					writeDatacube4D(pars,&intOutput_small[0],&pars.cbed_buffer_c[0], mdims,offset,numFP,nameString.c_str());

				}
				else
				{

					Array2D<PRISMATIC_FLOAT_PRECISION> intOutput_small;
					
					if(pars.meta.crop4DOutput)
					{
						intOutput_small = cropOutput(intOutput,pars);
					}
					else
					{
						intOutput_small = zeros_ND<2, PRISMATIC_FLOAT_PRECISION>({{pars.psiProbeInit.get_dimi()/2, pars.psiProbeInit.get_dimj()/2}});
						{
							long offset_x = pars.psiProbeInit.get_dimi() / 4;
							long offset_y = pars.psiProbeInit.get_dimj() / 4;
							long ndimy = (long) pars.psiProbeInit.get_dimj();
							long ndimx = (long) pars.psiProbeInit.get_dimi();
							for (long y = 0; y < pars.psiProbeInit.get_dimj() / 2; ++y) {
								for (long x = 0; x < pars.psiProbeInit.get_dimi() / 2; ++x) {
									intOutput_small.at(x, y) = intOutput.at(((y - offset_y) % ndimy + ndimy) % ndimy,
																((x - offset_x) % ndimx + ndimx) % ndimx);
								}
							}
						}
					}

					mdims[2] = {intOutput_small.get_dimj()};
					mdims[3] = {intOutput_small.get_dimi()};
					writeDatacube4D(pars,&intOutput_small[0],&pars.cbed_buffer[0],mdims,offset,numFP,nameString.c_str());
				}
			}


			++Nstart;
			++probe_idx;
		}
	}

	std::pair<Prismatic::Array2D< std::complex<PRISMATIC_FLOAT_PRECISION> >, Prismatic::Array2D< std::complex<PRISMATIC_FLOAT_PRECISION> > >
	getSingleMultisliceProbe_CPU(Parameters<PRISMATIC_FLOAT_PRECISION> &pars, const PRISMATIC_FLOAT_PRECISION xp, const PRISMATIC_FLOAT_PRECISION yp){

		Array2D< std::complex<PRISMATIC_FLOAT_PRECISION> > realspace_probe;
		Array2D< std::complex<PRISMATIC_FLOAT_PRECISION> > kspace_probe;
		PRISMATIC_FFTW_INIT_THREADS();
		PRISMATIC_FFTW_PLAN_WITH_NTHREADS(pars.meta.numThreads);
		Array2D<complex<PRISMATIC_FLOAT_PRECISION> > psi(pars.psiProbeInit);
		unique_lock<mutex> gatekeeper(fftw_plan_lock);
		PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		                                                      FFTW_FORWARD, FFTW_ESTIMATE);
		PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		                                                      FFTW_BACKWARD, FFTW_ESTIMATE);
		gatekeeper.unlock();
		{
			auto qxa_ptr = pars.qxa.begin();
			auto qya_ptr = pars.qya.begin();
			for (auto& p:psi)p*=exp(-2 * pi * i * ( (*qxa_ptr++)*xp +
			                                        (*qya_ptr++)*yp));
		}

		for (auto a2 = 0; a2 < pars.numPlanes; ++a2){
			PRISMATIC_FFTW_EXECUTE(plan_inverse);
			complex<PRISMATIC_FLOAT_PRECISION>* t_ptr = &pars.transmission[a2 * pars.transmission.get_dimj() * pars.transmission.get_dimi()];
			for (auto& p:psi)p *= (*t_ptr++); // transmit
			PRISMATIC_FFTW_EXECUTE(plan_forward);
			auto p_ptr = pars.prop.begin();
			for (auto& p:psi)p *= (*p_ptr++); // propagate
			for (auto& p:psi)p /= psi.size(); // scale FFT
		}


		// output the region of the probe not masked by the anti-aliasing filter
		Array2D<complex<PRISMATIC_FLOAT_PRECISION> > psi_small = zeros_ND<2, complex<PRISMATIC_FLOAT_PRECISION> >({{psi.get_dimj()/2, psi.get_dimi()/2}});

		{
			long offset_x = psi.get_dimi() / 4;
			long offset_y = psi.get_dimj() / 4;
			long ndimy = (long) psi.get_dimj();
			long ndimx = (long)psi.get_dimi();
			for (long y = 0; y < psi.get_dimj() / 2; ++y) {
				for (long x = 0; x < psi.get_dimi() / 2; ++x) {
					psi_small.at(y, x) = psi.at(((y - offset_y) % ndimy + ndimy) % ndimy,
					                            ((x - offset_x) % ndimx + ndimx) % ndimx);
				}
			}
		}
		psi_small = fftshift2(psi_small);
		kspace_probe = psi_small;
		gatekeeper.lock();
		PRISMATIC_FFTW_PLAN plan_inverse_small = PRISMATIC_FFTW_PLAN_DFT_2D(psi_small.get_dimj(), psi_small.get_dimi(),
		                                                            reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
		                                                            reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_small[0]),
		                                                            FFTW_BACKWARD, FFTW_ESTIMATE);
		gatekeeper.unlock();
		PRISMATIC_FFTW_EXECUTE(plan_inverse_small);
		realspace_probe = psi_small;

		// cleanup plans
		gatekeeper.lock();
		PRISMATIC_FFTW_DESTROY_PLAN(plan_forward);
		PRISMATIC_FFTW_DESTROY_PLAN(plan_inverse);
		PRISMATIC_FFTW_DESTROY_PLAN(plan_inverse_small);
		PRISMATIC_FFTW_CLEANUP_THREADS();
		return std::make_pair(realspace_probe, kspace_probe);
	};

	void getMultisliceProbe_CPU_batch(Parameters<PRISMATIC_FLOAT_PRECISION>& pars,
	                                  const size_t Nstart,
	                                  const size_t Nstop,
	                                  PRISMATIC_FFTW_PLAN& plan_forward,
	                                  PRISMATIC_FFTW_PLAN& plan_inverse,
	                                  Array1D<complex<PRISMATIC_FLOAT_PRECISION> >& psi_stack){
		{
			auto psi_ptr = psi_stack.begin();
			for (auto batch_num = 0; batch_num < min(pars.meta.batchSizeCPU, Nstop - Nstart); ++batch_num) {
				for (auto i:pars.psiProbeInit)*psi_ptr++ = i;
			}
		}
		auto psi_ptr   = psi_stack.begin();
		for (auto probe_num = Nstart; probe_num < Nstop; ++probe_num) {
			//			for (auto i:pars.psiProbeInit)*psi_ptr++=i;
			// Initialize the probes
			// Determine x/y position from the linear index
			const size_t ay = (pars.meta.arbitraryProbes) ? probe_num : probe_num / pars.xp.size();
			const size_t ax = (pars.meta.arbitraryProbes) ? probe_num : probe_num % pars.xp.size();
			// populates the output stack for Multislice simulation using the CPU. The number of
			// threads used is determined by pars.meta.numThreads
			{
				auto qxa_ptr = pars.qxa.begin();
				auto qya_ptr = pars.qya.begin();
				for (auto jj = 0; jj < pars.qxa.size(); ++jj) {
					*psi_ptr++ *= exp(-2 * pi * i * ((*qxa_ptr++) * pars.xp[ax] +
					                               (*qya_ptr++) * pars.yp[ay]));
				}
			}
		}

		auto scaled_prop = pars.prop;
		for (auto& jj : scaled_prop) jj/=pars.psiProbeInit.size(); // apply FFT scaling factor here once in advance rather than at every plane
		complex<PRISMATIC_FLOAT_PRECISION>* slice_ptr = &pars.transmission[0];
		size_t currentSlice = 0;

			for (auto a2 = 0; a2 < pars.numPlanes; ++a2){
				PRISMATIC_FFTW_EXECUTE(plan_inverse); // batch FFT

				// transmit each of the probes in the batch
				for (auto batch_idx = 0; batch_idx < min(pars.meta.batchSizeCPU, Nstop - Nstart); ++batch_idx){
					auto t_ptr   = slice_ptr; // start at the beginning of the current slice
					auto psi_ptr = &psi_stack[batch_idx * pars.psiProbeInit.size()];
					for (auto jj = 0; jj < pars.psiProbeInit.size(); ++jj){
						*psi_ptr++ *= (*t_ptr++);// transmit
					}
				}
				slice_ptr += pars.psiProbeInit.size(); // advance to point to the beginning of the next potential slice
				PRISMATIC_FFTW_EXECUTE(plan_forward); // batch FFT

				// propagate each of the probes in the batch
				for (auto batch_idx = 0; batch_idx < min(pars.meta.batchSizeCPU, Nstop - Nstart); ++batch_idx){
					auto p_ptr = scaled_prop.begin();
					auto psi_ptr = &psi_stack[batch_idx * pars.psiProbeInit.size()];
					for (auto jj = 0; jj < pars.psiProbeInit.size(); ++jj){
						*psi_ptr++ *= (*p_ptr++);// propagate
					}
				}

				if  ( ( (((a2+1) % pars.numSlices) == 0) && ((a2+1) >= pars.zStartPlane) ) || ((a2+1) == pars.numPlanes) ){
					formatOutput_CPU_integrate_batch(pars, psi_stack, pars.alphaInd, Nstart, Nstop, currentSlice);
					currentSlice++;
				}
			}
	}

	void getMultisliceProbe_CPU(Parameters<PRISMATIC_FLOAT_PRECISION>& pars,
	                            const size_t ay,
	                            const size_t ax,
								PRISMATIC_FFTW_PLAN& plan_forward,
								PRISMATIC_FFTW_PLAN& plan_inverse,
								Array2D<complex<PRISMATIC_FLOAT_PRECISION> >& psi){

		// populates the output stack for Multislice simulation using the CPU. The number of
		// threads used is determined by pars.meta.numThreads
		//		Array2D<complex<PRISMATIC_FLOAT_PRECISION> > psi(pars.psiProbeInit);
        psi = pars.psiProbeInit;
		// fftw_execute is the only thread-safe function in the library, so we need to synchronize access
		// to the plan creation methods
		//		unique_lock<mutex> gatekeeper(fftw_plan_lock);
		//		PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
		//		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		//		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		//		                                                      FFTW_FORWARD, FFTW_ESTIMATE);
		//		PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_2D(psi.get_dimj(), psi.get_dimi(),
		//		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		//		                                                      reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi[0]),
		//		                                                      FFTW_BACKWARD, FFTW_ESTIMATE);
		//		gatekeeper.unlock(); // unlock it so we only block as long as necessary to deal with plans
		{
			auto qxa_ptr = pars.qxa.begin();
			auto qya_ptr = pars.qya.begin();
			for (auto& p:psi)p*=exp(-2 * pi * i * ( (*qxa_ptr++)*pars.xp[ax] +
			                                        (*qya_ptr++)*pars.yp[ay]));
		}

		auto scaled_prop = pars.prop;
		for (auto& i : scaled_prop) i/=psi.size(); // apply FFT scaling factor here once in advance rather than at every plane
		complex<PRISMATIC_FLOAT_PRECISION>* t_ptr = &pars.transmission[0];
		size_t currentSlice = 0;

			for (auto a2 = 0; a2 < pars.numPlanes; ++a2){
				PRISMATIC_FFTW_EXECUTE(plan_inverse);
				for (auto& p:psi)p *= (*t_ptr++); // transmit
				PRISMATIC_FFTW_EXECUTE(plan_forward);
				auto p_ptr = scaled_prop.begin();
				for (auto& p:psi)p *= (*p_ptr++); // propagate

				if ( ( (((a2+1) % pars.numSlices) == 0) && ((a2+1) >= pars.zStartPlane) ) || ((a2+1) == pars.numPlanes) ){
					formatOutput_CPU(pars, psi, pars.alphaInd, currentSlice, ay, ax);
					currentSlice++;
				}
			}
	}

	void buildMultisliceOutput_CPUOnly(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){

#ifdef PRISMATIC_BUILDING_GUI
        pars.progressbar->signalDescriptionMessage("Computing final output (Multislice)");
#endif

		vector<thread> workers;
		workers.reserve(pars.meta.numThreads); // prevents multiple reallocations
		PRISMATIC_FFTW_INIT_THREADS();
		PRISMATIC_FFTW_PLAN_WITH_NTHREADS(pars.meta.numThreads);
		const size_t PRISMATIC_PRINT_FREQUENCY_PROBES = max((size_t)1, pars.numProbes/ 10); // for printing status
		WorkDispatcher dispatcher(0, pars.numProbes);

		// If the batch size is too big, the work won't be spread over the threads, which will usually hurt more than the benefit
		// of batch FFT
		pars.meta.batchSizeCPU = min(pars.meta.batchSizeTargetCPU, max((size_t)1, pars.numProbes / pars.meta.numThreads));
		for (auto t = 0; t < pars.meta.numThreads; ++t){
			cout << "Launching CPU worker #" << t << endl;
			workers.push_back(thread([&pars, &dispatcher, t, &PRISMATIC_PRINT_FREQUENCY_PROBES]() {
				size_t Nstart, Nstop;
                Nstart=Nstop=0;
				if (dispatcher.getWork(Nstart, Nstop, pars.meta.batchSizeCPU)){ // synchronously get work assignment

					// Allocate memory for the propagated probes. These are 2D arrays, but as they will be operated on
					// as a batch FFT they are all stacked together into one linearized array
					Array1D<complex<PRISMATIC_FLOAT_PRECISION> > psi_stack = zeros_ND<1, complex<PRISMATIC_FLOAT_PRECISION> >({{pars.psiProbeInit.size() * pars.meta.batchSizeCPU}});

					// setup batch FFTW parameters
					const int rank    = 2;
					int n[]           = {(int)pars.psiProbeInit.get_dimj(), (int)pars.psiProbeInit.get_dimi()};
					const int howmany = pars.meta.batchSizeCPU;
					int idist         = n[0]*n[1];
					int odist         = n[0]*n[1];
					int istride       = 1;
					int ostride       = 1;
					int *inembed      = n;
					int *onembed      = n;
					unique_lock<mutex> gatekeeper(fftw_plan_lock);

			//					PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_2D(psi_stack.get_dimj(), psi_stack.get_dimi(),
			//																		  reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
			//																		  reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
			//																		  FFTW_FORWARD, FFTW_MEASURE);
			//					PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_2D(psi_stack.get_dimj(), psi_stack.get_dimi(),
			//																		  reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
			//																		  reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]),
			//																		  FFTW_BACKWARD, FFTW_MEASURE);


					PRISMATIC_FFTW_PLAN plan_forward = PRISMATIC_FFTW_PLAN_DFT_BATCH(rank, n, howmany,
					                                                         reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]), inembed,
					                                                         istride, idist,
					                                                         reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]), onembed,
					                                                         ostride, odist,
					                                                         FFTW_FORWARD, FFTW_MEASURE);
					PRISMATIC_FFTW_PLAN plan_inverse = PRISMATIC_FFTW_PLAN_DFT_BATCH(rank, n, howmany,
					                                                         reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]), inembed,
					                                                         istride, idist,
					                                                         reinterpret_cast<PRISMATIC_FFTW_COMPLEX *>(&psi_stack[0]), onembed,
					                                                         ostride, odist,
					                                                         FFTW_BACKWARD, FFTW_MEASURE);

					gatekeeper.unlock();
					// main work loop
                    do {
						while (Nstart < Nstop) {
							if (Nstart % PRISMATIC_PRINT_FREQUENCY_PROBES < pars.meta.batchSizeCPU | Nstart == 100){
								cout << "Computing Probe Position #" << Nstart << "/" << pars.numProbes << endl;
							}
							getMultisliceProbe_CPU_batch(pars, Nstart, Nstop, plan_forward, plan_inverse, psi_stack);
#ifdef PRISMATIC_BUILDING_GUI
                            pars.progressbar->signalOutputUpdate(Nstart, pars.numProbes);
#endif
							Nstart=Nstop;
						}
					} while(dispatcher.getWork(Nstart, Nstop, pars.meta.batchSizeCPU));
					gatekeeper.lock();
					PRISMATIC_FFTW_DESTROY_PLAN(plan_forward);
					PRISMATIC_FFTW_DESTROY_PLAN(plan_inverse);
					gatekeeper.unlock();
				}
				cout << "CPU worker #" << t << " finished\n";
			}));
		}
		for (auto& t:workers)t.join();
		PRISMATIC_FFTW_CLEANUP_THREADS();
	};


	void Multislice_calcOutput(Parameters<PRISMATIC_FLOAT_PRECISION>& pars){

		// setup coordinates and build propagators
		setupCoordinates_multislice(pars);

		// setup detector coordinates and angles
		setupDetector_multislice(pars);

		// create initial probes
		setupProbes_multislice(pars);

		// create transmission array
		createTransmission(pars);

		// initialize output stack
		createStack(pars);

#ifdef PRISMATIC_BUILDING_GUI
        pars.progressbar->signalDescriptionMessage("Computing final output (Multislice)");
        pars.progressbar->signalOutputUpdate(0, pars.numProbes);
#endif

		// create the output
		buildMultisliceOutput(pars);
	}

}
