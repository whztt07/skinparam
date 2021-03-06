
/*
    Copyright(c) 2013-2014 Yifan Wu.

    This file is part of SkinParam.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

/**
 * Sum of Gaussians param calculation
 */

#include "StdAfx.h"

#include "GaussianParams.h"
#include <algorithm>
#include <fstream>
#include <string>
#include <sstream>
#include "D3DHelper.h"
#include "ProfileFit/GaussianFitTask.h"
#include "PbrtUtils/rng.h"
#include <condition_variable>

using namespace std;
using namespace Skin;
using namespace Utils;
using namespace Parallel;
using namespace ProfileFit;

namespace Skin {

	static istream& getLine(istream& is, string& out) {
		out.clear();

		// The characters in the stream are read one-by-one using a std::streambuf.
		// That is faster than reading them one-by-one using the std::istream.
		// Code that uses streambuf this way must be guarded by a sentry object.
		// The sentry object performs various tasks,
		// such as thread synchronization and updating the stream state.

		std::istream::sentry se(is, true);
		std::streambuf* sb = is.rdbuf();

		for(;;) {
			int c = sb->sbumpc();
			switch (c) {
			case '\n':
				return is;
			case '\r':
				if(sb->sgetc() == '\n')
					sb->sbumpc();
				return is;
			case EOF:
				// Also handle the case when the last line has no line ending
				if(out.empty())
					is.setstate(std::ios::eofbit);
				return is;
			default:
				out.push_back((char)c);
			}
		}
	}
}

GaussianParamsCalculator::GaussianParamsCalculator(const TString& filename) {
	parseFile(filename);
}

void GaussianParamsCalculator::parseFile(const TString& filename) {
	// Load from filename and store profiles into psp
	ifstream in(filename, ios::in);
	if (!in) {
		D3DHelper::checkFailure(E_FAIL, _T("Failed to open: ") + filename);
	}

	vector<SamplePoints>& sps = psp.paramSamplePoints;
	vector<RGBProfile>& profiles = psp.profiles;
	vector<float>& sigmas = psp.sigmas;
	sps.clear();
	string line;
	// The parser state
	struct {
		bool wantSamplePoints;
		int rgbColumnIndex;
		int minColumns;
		int totalProfiles;
	} state = {
		true, 0, 0, 0
	};

	while (getLine(in, line)) {
		istringstream iss(line);
		// most vexing parse?
		vector<string> tokens((istream_iterator<string>(iss)),
			istream_iterator<string>());
		if (state.wantSamplePoints) {
			if (!tokens.size() || tokens[0] == "Param")
				continue;
			if (tokens[0] == "ID") {
				state.wantSamplePoints = false;
				// parse sigmas
				int nParams = (int)sps.size();
				state.rgbColumnIndex = nParams + 1;
				if (tokens.size() < (size_t)state.rgbColumnIndex + 2) {
					D3DHelper::checkFailure(E_FAIL, _T("Ill-formed coeffs file: ") + filename);
				}
				sigmas.clear();
				for (size_t sigmaId = state.rgbColumnIndex + 1;
					sigmaId < tokens.size(); sigmaId++)
				{
					const string& token = tokens[sigmaId];
					if (token == "Error") break;
					sigmas.push_back((float)atof(token.c_str()));
				}
				state.minColumns = (int)(state.rgbColumnIndex + 1 + psp.sigmas.size());
				// prepare the sample points array
				int multiplier = 1;
				for (auto& riter = sps.rbegin(); riter != sps.rend(); ++riter) {
					riter->idMultiplier = multiplier;
					multiplier *= (int)riter->points.size();
				}
				// prepare profile storage
				profiles.resize(multiplier);
				state.totalProfiles = multiplier;
			} else {
				if (tokens.size() < 2)
					continue;
				// parse sample points
				int nPoints = atoi(tokens[1].c_str());
				if (tokens.size() - 2 < (size_t)nPoints)
					continue;
				SamplePoints points;
				for (int ptId = 0; ptId < nPoints; ptId++) {
					points.points.push_back(
						(float)atof(tokens[ptId + 2].c_str()));
				}
				sps.push_back(points);
			}
		} else {
			// parse individual profiles
			if (tokens.size() < (size_t)state.minColumns)
				continue;
			string rgb = tokens[state.rgbColumnIndex];
			if (rgb != "R" && rgb != "G" && rgb != "B")
				continue;
			int id = atoi(tokens[0].c_str());
			if (id < state.totalProfiles && id >= 0) {
				RGBProfile& p = profiles[id];
				SingleProfile& sp = rgb == "R" ? p.red 
					: (rgb == "G" ? p.green : p.blue);
				sp.clear();
				for (int i = state.rgbColumnIndex + 1;
					i < state.minColumns; i++)
				{
					sp.push_back((float)atof(tokens[i].c_str()));
				}
			}
		}
	}

	// Check if all profiles have been filled
	for (RGBProfile& profile : profiles) {
		if (!profile.red.size() || !profile.green.size()
			|| !profile.blue.size())
		{
			D3DHelper::checkFailure(E_FAIL,
				_T("Insufficient data: ") + filename);
		}
	}

	in.close();
}

namespace Skin {

	struct LerpStruct {
		int minId, maxId;
		float lerpAmount;
	};

	static LerpStruct searchLerp(const SamplePoints& sps, float sample) {
		LerpStruct out;
		const vector<float>& points = sps.points;
		if (sample <= points[0]) {
			out.minId = out.maxId = 0;
			out.lerpAmount = 0.f;
		} else if (sample >= points.back()) {
			out.minId = out.maxId = points.size() - 1;
			out.lerpAmount = 0.f;
		} else {
			// sample amount is small -- using linear search suffices
			int id = 1;
			while (points[id] < sample) {
				id++;
			}
			out.maxId = id;
			if (points[id] > sample) {
				out.minId = id - 1;
				out.lerpAmount = (sample - points[id - 1]) / (points[id] - points[id - 1]);
			} else {
				out.minId = id;
				out.lerpAmount = 0.f;
			}
		}
		return out;
	}

	static SingleProfile lerp(float amount, const SingleProfile& p1, const SingleProfile& p2) {
		ASSERT(p1.size() == p2.size());
		SingleProfile ret(p1.size());
		for (size_t i = 0; i < p1.size(); i++) {
			ret[i] = (1.f - amount) * p1[i] + amount * p2[i];
		}
		return ret;
	}

	static RGBProfile lerp(float amount, const RGBProfile& p1, const RGBProfile& p2) {
		RGBProfile ret;
		ret.red = lerp(amount, p1.red, p2.red);
		ret.green = lerp(amount, p1.green, p2.green);
		ret.blue = lerp(amount, p1.blue, p2.blue);
		return ret;
	}

	struct SigmaWeight {
		int id;
		float weight;
	};
}

RGBProfile GaussianParamsCalculator::nsample(int baseId, int nDims, int dim, const LerpStruct* lerps) const {
	const LerpStruct& nextLerp = lerps[dim];
	int multiplier = psp.paramSamplePoints[dim].idMultiplier;
	int minId = baseId + nextLerp.minId * multiplier;
	bool leaf = (dim == nDims - 1);
	RGBProfile p1 = leaf ? psp.profiles[minId] : nsample(minId, nDims, dim + 1, lerps);
	if (nextLerp.maxId != nextLerp.minId) {
		int maxId = baseId + nextLerp.maxId * multiplier;
		RGBProfile p2 = leaf ? psp.profiles[maxId] : nsample(maxId, nDims, dim + 1, lerps);
		return lerp(nextLerp.lerpAmount, p1, p2);
	} else {
		return p1;
	}
}


RGBProfile GaussianParamsCalculator::sample(const float* params) const {
	vector<LerpStruct> lerps;
	int dims = psp.paramSamplePoints.size();
	// do n searches to find the param ids to sample
	for (int d = 0; d < dims; d++) {
		lerps.push_back(searchLerp(psp.paramSamplePoints[d], params[d]));
	}
	// call the recursive helper function
	return nsample(0, dims, 0, &lerps[0]);
}

GaussianParams GaussianParamsCalculator::getParams(const VariableParams& vps) const {
	//GaussianParams gp = {
	//	{ 0, 0.0394384f, 0.0788769f, 0.157754f, 0.315508f, 0.631015f },
	//	{
	//		XMFLOAT3(0, 0, 0),
	//		XMFLOAT3(0.00157342f, -0.0018154f, 0.0199521f),
	//		XMFLOAT3(0.0164841f, 0.0573655f, 0.128642f),
	//		XMFLOAT3(0.162149f, 0.137493f, 0.0911935f),
	//		XMFLOAT3(0.0933545f, 0.0845024f, 0.0851154f),
	//		XMFLOAT3(0.285779f, 0.128577f, -0.0129746f)
	//	}
	//};
	//GaussianParams gp = {
	//	{ 0.0161538f, 0.0323076f, 0.0646152f, 0.12923f, 0.258461f, 0.516922f },
	//	{
	//		XMFLOAT3(0.000685575f, -0.000839641f, 0.00495757f),
	//		XMFLOAT3(-0.00364998f, 0.00761541f, 0.0347051f),
	//		XMFLOAT3(0.0384604f, 0.0541146f, 0.0169463f),
	//		XMFLOAT3(0.101322f, 0.0272875f, -0.00494447f),
	//		XMFLOAT3(0.0131899f, 0.000140161f, -0.000124362f),
	//		XMFLOAT3(0.020218f, -0.00118855f, -0.000260754f)
	//	}
	//};

	vector<float> params;
	params.push_back(vps.f_mel);
	params.push_back(vps.f_eu);
	params.push_back(vps.f_blood);
	params.push_back(vps.f_ohg);
	RGBProfile profile = sample(&params[0]);

	return getParamsFromRGBProfile(profile, psp.sigmas);
}

GaussianParams GaussianParamsCalculator::getParamsFromRGBProfile(const RGBProfile& profile,
	const std::vector<float>& sigmas)
{
	GaussianParams gp;
	if (sigmas.size() > GaussianParams::NUM_GAUSSIANS) {
		// Take most significant 6 sigmas
		vector<SigmaWeight> sws;
		float rtotal = 0, gtotal = 0, btotal = 0;
		for (size_t sid = 0; sid < sigmas.size(); sid++) {
			SigmaWeight sw = { sid, 0.f };
			// For RGB color spaces that use the ITU-R BT.709 primaries
			// (or sRGB, which defines the same primaries), relative luminance
			// can be calculated from linear RGB components:
			// Y = 0.2126 R + 0.7152 G + 0.0722 B
			// @ http://www.w3.org/Graphics/Color/sRGB
			float r = profile.red[sid] * 0.2126f;
			float g = profile.green[sid] * 0.7152f;
			float b = profile.blue[sid] * 0.0722f;
			rtotal += profile.red[sid];
			gtotal += profile.green[sid];
			btotal += profile.blue[sid];
			sw.weight += r * r + g * g + b * b;
			sws.push_back(sw);
		}
		partial_sort(sws.begin(), sws.begin() + GaussianParams::NUM_GAUSSIANS, sws.end(),
			[] (const SigmaWeight& sw1, const SigmaWeight& sw2) {
				if (sw1.weight > sw2.weight)
					return true;
				if (sw1.weight == sw2.weight)
					return sw1.id > sw2.id;
				return false;
			}
		);
		sort(sws.begin(), sws.begin() + GaussianParams::NUM_GAUSSIANS,
			[] (const SigmaWeight& sw1, const SigmaWeight& sw2) {
				return sw1.id < sw2.id;
			}
		);
		// write values into GaussianParams
		for (size_t swid = 0; swid < GaussianParams::NUM_GAUSSIANS; swid++) {
			int sid = sws[swid].id;
			gp.sigmas[swid] = sigmas[sid];
			gp.coeffs[swid].x = profile.red[sid];
			gp.coeffs[swid].y = profile.green[sid];
			gp.coeffs[swid].z = profile.blue[sid];
		}
		// append rest Gaussian weights to the nearest selected sigma
		for (size_t swid = GaussianParams::NUM_GAUSSIANS; swid < sigmas.size(); swid++) {
			int sid = sws[swid].id;
			float sigma = sigmas[sid];
			size_t nearestswid = 0;
			float nearestdiff = abs(sigma - sigmas[sws[0].id]);
			for (size_t selswid = 1; selswid < GaussianParams::NUM_GAUSSIANS; selswid++) {
				float diff = abs(sigma - sigmas[sws[selswid].id]);
				if (diff < nearestdiff) {
					nearestdiff = diff;
					nearestswid = selswid;
				}
			}
			gp.coeffs[nearestswid].x += profile.red[sid];
			gp.coeffs[nearestswid].y += profile.green[sid];
			gp.coeffs[nearestswid].z += profile.blue[sid];
		}
	} else {
		// pad zeros
		int numZeros = GaussianParams::NUM_GAUSSIANS - sigmas.size();
		for (int i = 0; i < numZeros; i++) {
			gp.sigmas[i] = 0.f;
			gp.coeffs[i] = XMFLOAT3(0, 0, 0);
		}
		// write values into GaussianParams
		for (size_t sid = 0; sid < sigmas.size(); sid++) {
			gp.sigmas[numZeros + sid] = sigmas[sid];
			gp.coeffs[numZeros + sid].x = profile.red[sid];
			gp.coeffs[numZeros + sid].y = profile.green[sid];
			gp.coeffs[numZeros + sid].z = profile.blue[sid];
		}
	}

	return gp;
}

GaussianParamsCalculator::GaussianFuture
	GaussianParamsCalculator::getLiveFitParams(const VariableParams& vps) const
{
	static bool firstCall = true;
	if (firstCall) {
		SampledSpectrum::Init();
		firstCall = false;
	}

	shared_ptr<TaskQueue> tq(new TaskQueue);
	shared_ptr<mutex> pDelayMutex(new mutex);
	shared_ptr<condition_variable_any> pDelayCondition(new condition_variable_any);
	shared_ptr<bool> pCancel(new bool(false));

	future<GaussianParams> future =	std::async([vps, tq, pDelayMutex, pDelayCondition, pCancel, this] () {
		SkinCoefficients skinCoeffs(vps.f_mel, vps.f_eu, vps.f_blood, vps.f_ohg, 0, 0, 0);
		SpectralGaussianCoeffs spectralGaussianCoeffs;

		{
			chrono::system_clock::time_point targetTime = chrono::system_clock::now() + chrono::seconds(2);
			lock_guard<mutex> lock(*pDelayMutex);
			do {
				pDelayCondition->wait_until(*pDelayMutex, targetTime);
			}
			while (!*pCancel && chrono::system_clock::now() < targetTime);
			if (*pCancel)
				return GaussianParams();
		}

		vector<Task*> tasks = CreateGaussianFitTasks(skinCoeffs, psp.sigmas, spectralGaussianCoeffs);
		tq->EnqueueTasks(tasks);
		tq->WaitForAllTasks();

		DestroyGaussianTasks(tasks);
		ClearGaussianTasksCache();

		// Our coeffs should be ready here
		RGBProfile profile;
		for (size_t sid = 0; sid < spectralGaussianCoeffs.sigmas.size(); sid++) {
			float rgb[3];
			spectralGaussianCoeffs.coeffs[sid].ToRGB(rgb);
			profile.red.push_back(rgb[0]);
			profile.green.push_back(rgb[1]);
			profile.blue.push_back(rgb[2]);
		}
		return getParamsFromRGBProfile(profile, spectralGaussianCoeffs.sigmas);
	});

	weak_ptr<TaskQueue> weak_tq(tq);
	weak_ptr<mutex> weak_pDelayMutex(pDelayMutex);
	weak_ptr<condition_variable_any> weak_pDelayCondition(pDelayCondition);
	weak_ptr<bool> weak_pCancel(pCancel);
	return GaussianFuture(std::move(future), [weak_tq, weak_pDelayMutex, weak_pDelayCondition, weak_pCancel] {
		if (auto tq = weak_tq.lock())
			tq->Abort();
		if (auto pDelayMutex = weak_pDelayMutex.lock()) {
			if (auto pCancel = weak_pCancel.lock()) {
				lock_guard<mutex> lock(*pDelayMutex);
				*pCancel = true;
			}
		}
		if (auto pDelayCondition = weak_pDelayCondition.lock())
			pDelayCondition->notify_one();
	}, [weak_tq] {
		if (auto tq = weak_tq.lock())
			return tq->Progress();
		return 1.;
	});
}

chrono::microseconds GaussianParamsCalculator::perf() const {
	RNG rng(31);
	vector<VariableParams> vps;
	// initialize bunch of VariableParams
	const UINT32 NUM = 100000;
	for (UINT32 i = 0; i < NUM; i++) {
		float r1 = rng.RandomFloat();
		float r2 = rng.RandomFloat();
		float r3 = rng.RandomFloat();
		float r4 = rng.RandomFloat();
		float f_mel = r1 * r1 * 0.5f;
		float f_eu = r2;
		float f_blood = r3 * r3 * 0.1f;
		float f_ohg = r4;
		vps.push_back(VariableParams(f_mel, f_eu, f_blood, f_ohg));
	}
	// test
	chrono::high_resolution_clock::time_point startTime = chrono::high_resolution_clock::now();
	for (const VariableParams& vp : vps) {
		volatile GaussianParams gp = getParams(vp);
	}
	chrono::high_resolution_clock::time_point endTime = chrono::high_resolution_clock::now();
	return chrono::duration_cast<chrono::microseconds>((endTime - startTime) / NUM);
}

