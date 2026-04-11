/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2024-2025 Red Hat Inc.
 *
 * Auto white balance
 */

#pragma once

#include <optional>
#include <vector>

#include "libcamera/internal/vector.h"

#include "algorithm.h"

namespace libcamera {

namespace ipa::soft::algorithms {

class Awb : public Algorithm
{
public:
	Awb() = default;
	~Awb() = default;

	int init(IPAContext &context, const YamlObject &tuningData) override;
	int configure(IPAContext &context, const IPAConfigInfo &configInfo) override;
	void prepare(IPAContext &context,
		     const uint32_t frame,
		     IPAFrameContext &frameContext,
		     DebayerParams *params) override;
	void process(IPAContext &context,
		     const uint32_t frame,
		     IPAFrameContext &frameContext,
		     const SwIspStats *stats,
		     ControlList &metadata) override;

private:
	Vector<double, 2> projectToDecisionLocus(const Vector<double, 2> &point) const;
	bool isWithinWarmStartRange(const Vector<double, 2> &point) const;

	std::vector<Vector<double, 2>> decisionLocus_;
	std::optional<Vector<double, 2>> previousDecision_;
	float locusBlend_ = 0.0f;
	float warmStartBlend_ = 0.0f;
	unsigned int warmStartFrames_ = 0;
	unsigned int warmStartFrameCount_ = 0;
	float warmStartRgMin_ = 0.0f;
	float warmStartRgMax_ = 0.0f;
	float warmStartBgMin_ = 0.0f;
	float warmStartBgMax_ = 0.0f;
};

} /* namespace ipa::soft::algorithms */

} /* namespace libcamera */
