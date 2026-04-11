/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2024-2026 Red Hat Inc.
 *
 * Auto white balance
 */

#include "awb.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdint.h>

#include <libcamera/base/log.h>

#include <libcamera/control_ids.h>

#include "libipa/colours.h"
#include "simple/ipa_context.h"

#include "control_ids.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(IPASoftAwb)

namespace ipa::soft::algorithms {

int Awb::init([[maybe_unused]] IPAContext &context, const YamlObject &tuningData)
{
	auto locusBlend = tuningData["locusBlend"].get<float>();
	if (locusBlend.has_value())
		locusBlend_ = std::clamp(locusBlend.value(), 0.0f, 1.0f);

	auto warmStartBlend = tuningData["warmStartBlend"].get<float>();
	if (warmStartBlend.has_value())
		warmStartBlend_ = std::clamp(warmStartBlend.value(), 0.0f, 1.0f);

	auto warmStartFrames = tuningData["warmStartFrames"].get<unsigned int>();
	if (warmStartFrames.has_value())
		warmStartFrames_ = warmStartFrames.value();

	std::optional<std::vector<double>> warmStartRgRange =
		tuningData["warmStartRgRange"].getList<double>();
	if (warmStartRgRange && warmStartRgRange->size() == 2) {
		warmStartRgMin_ = std::min((*warmStartRgRange)[0], (*warmStartRgRange)[1]);
		warmStartRgMax_ = std::max((*warmStartRgRange)[0], (*warmStartRgRange)[1]);
	}

	std::optional<std::vector<double>> warmStartBgRange =
		tuningData["warmStartBgRange"].getList<double>();
	if (warmStartBgRange && warmStartBgRange->size() == 2) {
		warmStartBgMin_ = std::min((*warmStartBgRange)[0], (*warmStartBgRange)[1]);
		warmStartBgMax_ = std::max((*warmStartBgRange)[0], (*warmStartBgRange)[1]);
	}

	for (const YamlObject &entry : tuningData["decisionLocus"].asList()) {
		std::optional<std::vector<double>> point = entry.getList<double>();
		if (!point || point->size() != 2) {
			LOG(IPASoftAwb, Warning)
				<< "Invalid 'decisionLocus' point; expected [ rg, bg ]";
			decisionLocus_.clear();
			break;
		}

		decisionLocus_.push_back({ { (*point)[0], (*point)[1] } });
	}

	return 0;
}

int Awb::configure(IPAContext &context,
		   [[maybe_unused]] const IPAConfigInfo &configInfo)
{
	auto &gains = context.activeState.awb.gains;
	gains = { { 1.0, 1.0, 1.0 } };
	previousDecision_.reset();
	warmStartFrameCount_ = 0;

	return 0;
}

void Awb::prepare(IPAContext &context,
		  [[maybe_unused]] const uint32_t frame,
		  IPAFrameContext &frameContext,
		  DebayerParams *params)
{
	auto &gains = context.activeState.awb.gains;
	Matrix<float, 3, 3> gainMatrix = { { gains.r(), 0, 0,
					     0, gains.g(), 0,
					     0, 0, gains.b() } };
	context.activeState.combinedMatrix =
		context.activeState.combinedMatrix * gainMatrix;

	frameContext.gains.red = gains.r();
	frameContext.gains.blue = gains.b();

	params->gains = gains;
}

void Awb::process(IPAContext &context,
		  [[maybe_unused]] const uint32_t frame,
		  IPAFrameContext &frameContext,
		  const SwIspStats *stats,
		  ControlList &metadata)
{
	const SwIspStats::Histogram &histogram = stats->yHistogram;
	const uint8_t blackLevel = context.activeState.blc.level;

	const float mdGains[] = {
		static_cast<float>(frameContext.gains.red),
		static_cast<float>(frameContext.gains.blue)
	};
	metadata.set(controls::ColourGains, mdGains);

	if (!stats->valid)
		return;

	/*
	 * Black level must be subtracted to get the correct AWB ratios, they
	 * would be off if they were computed from the whole brightness range
	 * rather than from the sensor range.
	 */
	const uint64_t nPixels = std::accumulate(
		histogram.begin(), histogram.end(), uint64_t(0));
	const uint64_t offset = blackLevel * nPixels;
	const uint64_t minValid = 1;
	/*
	 * Make sure the sums are at least minValid, while preventing unsigned
	 * integer underflow.
	 */
	const RGB<uint64_t> sum = stats->sum_.max(offset + minValid) - offset;

	/*
	 * Calculate red and blue gains for AWB.
	 * Clamp max gain at 4.0, this also avoids 0 division.
	 */
	const float measuredRed =
		sum.r() <= sum.g() / 4 ? 4.0f : static_cast<float>(sum.g()) / sum.r();
	const float measuredBlue =
		sum.b() <= sum.g() / 4 ? 4.0f : static_cast<float>(sum.g()) / sum.b();

	float redGain = measuredRed;
	float blueGain = measuredBlue;

	if (decisionLocus_.size() >= 2 && locusBlend_ > 0.0f) {
		Vector<double, 2> measuredDecision = { {
			1.0 / std::max(redGain, 0.001f),
			1.0 / std::max(blueGain, 0.001f),
		} };
		Vector<double, 2> projectedDecision =
			projectToDecisionLocus(measuredDecision);

		measuredDecision[0] = measuredDecision[0] * (1.0f - locusBlend_) +
				     projectedDecision[0] * locusBlend_;
		measuredDecision[1] = measuredDecision[1] * (1.0f - locusBlend_) +
				     projectedDecision[1] * locusBlend_;

		redGain = 1.0f / std::max(static_cast<float>(measuredDecision[0]), 0.001f);
		blueGain = 1.0f / std::max(static_cast<float>(measuredDecision[1]), 0.001f);
	}

	Vector<double, 2> currentDecision = { {
		1.0 / std::max(redGain, 0.001f),
		1.0 / std::max(blueGain, 0.001f),
	} };

	if (previousDecision_ && warmStartBlend_ > 0.0f &&
	    warmStartFrameCount_ < warmStartFrames_ &&
	    isWithinWarmStartRange(*previousDecision_)) {
		currentDecision[0] = currentDecision[0] * (1.0f - warmStartBlend_) +
				     (*previousDecision_)[0] * warmStartBlend_;
		currentDecision[1] = currentDecision[1] * (1.0f - warmStartBlend_) +
				     (*previousDecision_)[1] * warmStartBlend_;

		redGain = 1.0f / std::max(static_cast<float>(currentDecision[0]), 0.001f);
		blueGain = 1.0f / std::max(static_cast<float>(currentDecision[1]), 0.001f);
	}

	auto &gains = context.activeState.awb.gains;
	gains = { { redGain, 1.0, blueGain } };
	previousDecision_ = currentDecision;
	warmStartFrameCount_++;

	RGB<double> rgbGains{ { 1 / gains.r(), 1 / gains.g(), 1 / gains.b() } };
	context.activeState.awb.temperatureK = estimateCCT(rgbGains);
	metadata.set(controls::ColourTemperature, context.activeState.awb.temperatureK);

	LOG(IPASoftAwb, Debug)
		<< "gain R/B: " << gains << "; temperature: "
		<< context.activeState.awb.temperatureK;
}

Vector<double, 2> Awb::projectToDecisionLocus(const Vector<double, 2> &point) const
{
	if (decisionLocus_.size() < 2)
		return point;

	Vector<double, 2> best = decisionLocus_.front();
	double bestDistance = std::numeric_limits<double>::max();

	for (size_t i = 0; i + 1 < decisionLocus_.size(); ++i) {
		const Vector<double, 2> &a = decisionLocus_[i];
		const Vector<double, 2> &b = decisionLocus_[i + 1];
		const double dx = b[0] - a[0];
		const double dy = b[1] - a[1];
		const double len2 = dx * dx + dy * dy;

		double t = 0.0;
		if (len2 > 0.0)
			t = std::clamp(((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / len2,
				       0.0, 1.0);

		Vector<double, 2> candidate = { {
			a[0] + t * dx,
			a[1] + t * dy,
		} };
		const double distance =
			(candidate[0] - point[0]) * (candidate[0] - point[0]) +
			(candidate[1] - point[1]) * (candidate[1] - point[1]);

		if (distance < bestDistance) {
			bestDistance = distance;
			best = candidate;
		}
	}

	return best;
}

bool Awb::isWithinWarmStartRange(const Vector<double, 2> &point) const
{
	if (warmStartRgMin_ == warmStartRgMax_ || warmStartBgMin_ == warmStartBgMax_)
		return false;

	return point[0] >= warmStartRgMin_ && point[0] <= warmStartRgMax_ &&
	       point[1] >= warmStartBgMin_ && point[1] <= warmStartBgMax_;
}

REGISTER_IPA_ALGORITHM(Awb, "Awb")

} /* namespace ipa::soft::algorithms */

} /* namespace libcamera */
