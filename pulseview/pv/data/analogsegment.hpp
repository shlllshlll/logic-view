/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PULSEVIEW_PV_DATA_ANALOGSEGMENT_HPP
#define PULSEVIEW_PV_DATA_ANALOGSEGMENT_HPP

#include "segment.hpp"

#include <utility>
#include <vector>

#include <QObject>

using std::enable_shared_from_this;
using std::pair;

namespace AnalogSegmentTest {
struct Basic;
}

namespace pv {
namespace data {

class Analog;

class AnalogSegment : public Segment, public enable_shared_from_this<Segment>
{
	Q_OBJECT

public:
	struct EnvelopeSample
	{
		float min;
		float max;
	};

	struct EnvelopeSection
	{
		uint64_t start;
		unsigned int scale;
		uint64_t length;
		EnvelopeSample *samples;
	};

private:
	struct Envelope
	{
		uint64_t length;
		uint64_t data_length;
		EnvelopeSample *samples;
		// Deferred-compaction offset: number of logically-evicted entries at
		// the head that have not been physically moved.  Readers add this to
		// their logical offset to get the physical index.  Non-rolling mode
		// never trims, so head_offset stays 0 (no behaviour change).
		uint64_t head_offset;
	};

private:
	static const unsigned int ScaleStepCount = 10;
	static const int EnvelopeScalePower;
	static const int EnvelopeScaleFactor;
	static const float LogEnvelopeScaleFactor;
	// Allocation step, in entries (512 KiB with an 8-byte EnvelopeSample).
	static const uint64_t EnvelopeDataUnit;

public:
	AnalogSegment(Analog& owner, uint32_t segment_id, uint64_t samplerate,
		uint64_t max_samples = 0);

	virtual ~AnalogSegment();

	void append_interleaved_samples(const float *data,
		size_t sample_count, size_t stride);

	float get_sample(int64_t sample_num) const;
	void get_samples(int64_t start_sample, int64_t end_sample, float* dest) const;

	const pair<float, float> get_min_max() const;

	float* get_iterator_value_ptr(SegmentDataIterator* it);

	void get_envelope_section(EnvelopeSection &s, uint64_t start, uint64_t end, float min_length) const;

private:
	bool wch_reallocate_envelope(Envelope &e, unsigned int level);

	// Pre-allocates the whole envelope pyramid for the expected capture
	// length, so that appending during a sweep needs no realloc at all.
	void preallocate_envelopes(uint64_t max_samples);

	void append_payload_to_envelope_levels();

	// Rolling capture: drop envelope entries for discarded head samples.
	// Aligned levels are shifted left in place; the first level whose entry
	// granularity no longer divides the discard count, and every coarser
	// level above it, are rebuilt from the level below.
	void trim_envelopes(uint64_t discarded_samples);
	void rebuild_higher_envelope_levels(unsigned int start_level = 1);

private:
	Analog& owner_;

	struct Envelope envelope_levels_[ScaleStepCount];

	// Real allocated capacity per level, in entries.  Grows in
	// EnvelopeDataUnit steps with a Depth x slack on top of the high-water
	// mark (see wch_reallocate_envelope()).
	uint64_t envelopeAppendLength_[ScaleStepCount];

	float min_value_, max_value_;

	friend struct AnalogSegmentTest::Basic;
};

} // namespace data
} // namespace pv

#endif // PULSEVIEW_PV_DATA_ANALOGSEGMENT_HPP
