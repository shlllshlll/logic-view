/*
 * This file is part of the LogicAnalyzer project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2026 Q2H2
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

#include <extdef.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <QDebug>

#include <algorithm>

#include <libsigrok/libsigrok.h>  // For SR_MHZ

#include "analog.hpp"
#include "analogsegment.hpp"

using std::bad_alloc;
using std::lock_guard;
using std::make_pair;
using std::max;
using std::max_element;
using std::min;
using std::min_element;
using std::pair;
using std::recursive_mutex;
using std::unique_ptr;

namespace pv
{
	namespace data
	{

		const int AnalogSegment::EnvelopeScalePower = 4;
		const int AnalogSegment::EnvelopeScaleFactor = 1 << EnvelopeScalePower;
		const float AnalogSegment::LogEnvelopeScaleFactor = logf(EnvelopeScaleFactor);
		// Allocation step, in entries: 64 Ki entries == 512 KiB per step.
		const uint64_t AnalogSegment::EnvelopeDataUnit = 64 * 1024;

		AnalogSegment::AnalogSegment(Analog &owner, uint32_t segment_id, uint64_t samplerate,
									 uint64_t max_samples) :
			Segment(segment_id, samplerate, sizeof(float)),
			owner_(owner),
			min_value_(0),
			max_value_(0)
		{
			lock_guard<recursive_mutex> lock(mutex_);
			memset(envelope_levels_, 0, sizeof(envelope_levels_));
			memset(envelopeAppendLength_, 0, sizeof(envelopeAppendLength_));

			preallocate_envelopes(max_samples);
		}

		AnalogSegment::~AnalogSegment()
		{
			lock_guard<recursive_mutex> lock(mutex_);
			for (Envelope &e : envelope_levels_)
				free(e.samples);
		}

		void AnalogSegment::preallocate_envelopes(uint64_t max_samples)
		{
			if (max_samples == 0)
				return;

			// Reserve the whole envelope pyramid for the expected capture
			// length up front, like LogicSegment does for its mip-maps, so
			// that a running acquisition does not realloc while it streams
			// samples in.
			uint64_t level_samples = max_samples / EnvelopeScaleFactor;
			for (unsigned int level = 0; level < ScaleStepCount; level++)
			{
				const uint64_t needed_entries =
					((level_samples + EnvelopeDataUnit - 1) / EnvelopeDataUnit) *
					EnvelopeDataUnit;
				if (needed_entries == 0)
					break;

				Envelope &e = envelope_levels_[level];
				e.samples = (EnvelopeSample *)malloc(needed_entries * sizeof(EnvelopeSample));
				if (e.samples != nullptr)
				{
					e.data_length = needed_entries;
					envelopeAppendLength_[level] = needed_entries;
				}
				else
				{
					qDebug() << "preallocate_envelopes: malloc failed for level" << level
							 << "requested" << needed_entries * sizeof(EnvelopeSample) << "bytes";
					// Envelope is non-critical, data will still be appended
				}

				level_samples /= EnvelopeScaleFactor;
			}
		}

		void AnalogSegment::append_interleaved_samples(const float *data,
													   size_t sample_count, size_t stride)
		{
			assert(unit_size_ == sizeof(float));

			lock_guard<recursive_mutex> lock(mutex_);

			// Logical sample count (excludes discarded head in rolling mode)
			uint64_t prev_sample_count = get_sample_count();

			if (bad_memory_)
				return;

			if (stride == 1)
			{
				// Single-channel data is already contiguous.
				append_samples(const_cast<float *>(data), sample_count);
			}
			else
			{
				// Deinterleave the samples and add them
				unique_ptr<float[]> deint_data;
				try
				{
					deint_data.reset(new float[sample_count]);
				}
				catch (bad_alloc &)
				{
					qDebug() << "append_interleaved_samples: deinterleave allocation failed";
					bad_memory_ = true;
					Q_EMIT memoryError();
					return;
				}
				float *deint_data_ptr = deint_data.get();
				for (size_t i = 0; i < sample_count; i++)
				{
					*deint_data_ptr = (float)(*data);
					deint_data_ptr++;
					data += stride;
				}

				append_samples(deint_data.get(), sample_count);
			}

			if (bad_memory_)
				return;

			// Generate the first mip-map from the data
			append_payload_to_envelope_levels();

			// Rolling capture: discard old head chunks beyond the ring capacity
			// and drop their envelope entries accordingly.
			const uint64_t discarded = discard_old_chunks();
			if (discarded > 0)
			{
				trim_envelopes(discarded);
				// The whole visible window shifted, keep the reported range valid
				const uint64_t logical_count = get_sample_count();
				prev_sample_count = (logical_count > (uint64_t)sample_count) ? logical_count - sample_count : 0;
			}

			if (sample_count > 1)
				owner_.notify_samples_added(shared_ptr<Segment>(shared_from_this()),
											prev_sample_count + 1, prev_sample_count + 1 + sample_count);
			else
				owner_.notify_samples_added(shared_ptr<Segment>(shared_from_this()),
											prev_sample_count + 1, prev_sample_count + 1);
		}

		float AnalogSegment::get_sample(int64_t sample_num) const
		{
			assert(sample_num >= 0);
			assert(sample_num <= (int64_t)get_sample_count());

			lock_guard<recursive_mutex> lock(mutex_); // Because of free_unused_memory()

			return *((const float *)get_raw_sample(sample_num));
		}

		void AnalogSegment::get_samples(int64_t start_sample, int64_t end_sample,
										float *dest) const
		{
			assert(start_sample >= 0);
			assert(start_sample < (int64_t)get_sample_count());
			assert(end_sample >= 0);
			assert(end_sample <= (int64_t)get_sample_count());
			assert(start_sample <= end_sample);
			assert(dest != nullptr);

			lock_guard<recursive_mutex> lock(mutex_);

			get_raw_samples(start_sample, (end_sample - start_sample), (uint8_t *)dest);
		}

		const pair<float, float> AnalogSegment::get_min_max() const
		{
			return make_pair(min_value_, max_value_);
		}

		float *AnalogSegment::get_iterator_value_ptr(SegmentDataIterator *it)
		{
			assert(it->sample_index <= (sample_count_ - 1));

			return (float *)(it->chunk + it->chunk_offs);
		}

		void AnalogSegment::get_envelope_section(EnvelopeSection &s,
												 uint64_t start, uint64_t end, float min_length) const
		{
			assert(end <= get_sample_count());
			assert(start <= end);
			assert(min_length > 0);

			lock_guard<recursive_mutex> lock(mutex_);

			const unsigned int min_level = max((int)floorf(logf(min_length) /
														   LogEnvelopeScaleFactor) -
												   1,
											   0);
			const unsigned int scale_power = (min_level + 1) *
											 EnvelopeScalePower;
			start >>= scale_power;
			end >>= scale_power;

			s.start = start << scale_power;
			s.scale = 1 << scale_power;
			s.length = end - start;
			s.samples = new EnvelopeSample[s.length];
			memcpy(s.samples,
				   envelope_levels_[min_level].samples +
					   envelope_levels_[min_level].head_offset + start,
				   s.length * sizeof(EnvelopeSample));
		}

		bool AnalogSegment::wch_reallocate_envelope(Envelope &e, unsigned int level)
		{
			lock_guard<recursive_mutex> lock(mutex_);

			uint64_t append_length = envelopeAppendLength_[level];
			if (e.data_length == 0)
				append_length = 0;

			// Level 0 is the hot level: its entries are produced by every
			// append, so give it more slack, and more still at very high
			// sample rates where the producer runs at its fastest.
			int depth = (level == 0) ? 3 : 2;
			if (samplerate_ > SR_MHZ(500))
				depth *= 2;

			const uint64_t new_data_length = ((e.head_offset + e.length + EnvelopeDataUnit - 1) /
											  EnvelopeDataUnit) *
											 EnvelopeDataUnit;

			if (new_data_length > e.data_length)
			{
				if (new_data_length > append_length)
				{
					uint64_t new_capacity = depth * new_data_length;
					EnvelopeSample *new_samples = nullptr;

					while (!(new_samples = (EnvelopeSample *)realloc(e.samples,
						new_capacity * sizeof(EnvelopeSample))))
					{
						depth--;
						if (depth <= 0)
							break;
						new_capacity = depth * new_data_length;
					}

					if (new_samples != nullptr && depth > 0)
					{
						e.samples = new_samples;
						envelopeAppendLength_[level] = new_capacity;
					}

					if (new_samples == nullptr)
					{
						qDebug() << __FILE__ << " :" << __LINE__ << ":realloc failed";
						Q_EMIT memoryError();
						return false;
					}
				}

				e.data_length = new_data_length;
			}

			return true;
		}

		void AnalogSegment::append_payload_to_envelope_levels()
		{
			Envelope &e0 = envelope_levels_[0];
			uint64_t prev_length;
			EnvelopeSample *dest_ptr;
			SegmentDataIterator *it;

			// Expand the data buffer to fit the new samples
			// (logical sample count, excludes discarded head in rolling mode)
			prev_length = e0.length;
			e0.length = get_sample_count() / EnvelopeScaleFactor;

			// Calculate min/max values in case we have too few samples for an envelope
			const float old_min_value = min_value_, old_max_value = max_value_;
			if (get_sample_count() < EnvelopeScaleFactor)
			{
				it = begin_sample_iteration(0);
				for (uint64_t i = 0; i < get_sample_count(); i++)
				{
					const float sample = *get_iterator_value_ptr(it);
					if (sample < min_value_)
						min_value_ = sample;
					if (sample > max_value_)
						max_value_ = sample;
					continue_sample_iteration(it, 1);
				}
				end_sample_iteration(it);
			}

			// Break off if there are no new samples to compute
			if (e0.length == prev_length)
				return;

			// Deferred compaction: if head_offset + new length would exceed the
			// allocated capacity, compact first.
			if (e0.samples && e0.head_offset > 0)
			{
				const uint64_t capacity_entries = max(envelopeAppendLength_[0], e0.data_length);
				if (e0.head_offset + e0.length > capacity_entries)
				{
					memmove(e0.samples,
							e0.samples + e0.head_offset,
							prev_length * sizeof(EnvelopeSample));
					e0.head_offset = 0;
				}
			}

			if (!wch_reallocate_envelope(e0, 0))
			{
				e0.length = prev_length;
				return;
			}

			dest_ptr = e0.samples + e0.head_offset + prev_length;

			// Iterate through the samples to populate the first level mipmap
			uint64_t start_sample = prev_length * EnvelopeScaleFactor;
			uint64_t end_sample = e0.length * EnvelopeScaleFactor;

			it = begin_sample_iteration(start_sample);
			for (uint64_t i = start_sample; i < end_sample; i += EnvelopeScaleFactor)
			{
				const float *samples = get_iterator_value_ptr(it);

				const EnvelopeSample sub_sample = {
					*min_element(samples, samples + EnvelopeScaleFactor),
					*max_element(samples, samples + EnvelopeScaleFactor),
				};

				if (sub_sample.min < min_value_)
					min_value_ = sub_sample.min;
				if (sub_sample.max > max_value_)
					max_value_ = sub_sample.max;

				continue_sample_iteration(it, EnvelopeScaleFactor);
				*dest_ptr++ = sub_sample;
			}
			end_sample_iteration(it);

			// Compute higher level mipmaps
			for (unsigned int level = 1; level < ScaleStepCount; level++)
			{
				Envelope &e = envelope_levels_[level];
				const Envelope &el = envelope_levels_[level - 1];

				// Expand the data buffer to fit the new samples
				prev_length = e.length;
				e.length = el.length / EnvelopeScaleFactor;

				// Break off if there are no more samples to be computed
				if (e.length == prev_length)
					break;

				// Deferred compaction
				if (e.samples && e.head_offset > 0)
				{
				const uint64_t capacity_entries =
					max(envelopeAppendLength_[level], e.data_length);
					if (e.head_offset + e.length > capacity_entries)
					{
						memmove(e.samples,
								e.samples + e.head_offset,
								prev_length * sizeof(EnvelopeSample));
						e.head_offset = 0;
					}
				}

				if (!wch_reallocate_envelope(e, level))
				{
					e.length = prev_length;
					return;
				}

				const EnvelopeSample *src_ptr =
					el.samples + el.head_offset + prev_length * EnvelopeScaleFactor;
				const EnvelopeSample *const end_dest_ptr =
					e.samples + e.head_offset + e.length;

				for (dest_ptr = e.samples + e.head_offset + prev_length;
					 dest_ptr < end_dest_ptr; dest_ptr++)
				{
					const EnvelopeSample *const end_src_ptr =
						src_ptr + EnvelopeScaleFactor;

					EnvelopeSample sub_sample = *src_ptr++;
					while (src_ptr < end_src_ptr)
					{
						sub_sample.min = min(sub_sample.min, src_ptr->min);
						sub_sample.max = max(sub_sample.max, src_ptr->max);
						src_ptr++;
					}

					*dest_ptr = sub_sample;
				}
			}

			// Notify if the min or max value changed
			if ((old_min_value != min_value_) || (old_max_value != max_value_))
				owner_.min_max_changed(min_value_, max_value_);
		}

		void AnalogSegment::trim_envelopes(uint64_t discarded_samples)
		{
			lock_guard<recursive_mutex> lock(mutex_);

			// Discards are chunk-aligned, and chunk sample counts are divisible
			// by EnvelopeScaleFactor (guaranteed by discard_old_chunks()).
			assert(discarded_samples % EnvelopeScaleFactor == 0);

			Envelope &e0 = envelope_levels_[0];
			const uint64_t drop0 = discarded_samples / EnvelopeScaleFactor;

			if (drop0 == 0)
				return;

			// Level 0 is always aligned (discarded_samples % EnvelopeScaleFactor == 0).
			// Deferred compaction: instead of memmove, just advance head_offset.
			if (e0.samples)
			{
				if (drop0 >= e0.length)
				{
					e0.length = 0;
					e0.head_offset = 0;
				}
				else
				{
					e0.head_offset += drop0;
					e0.length -= drop0;
				}
			}

			// Level L (L >= 1) covers EnvelopeScaleFactor^(L+1) raw samples per entry.
			// Advance head_offset for every level whose entry granularity evenly
			// divides the discard count; the first level where it does not (and
			// every coarser level above) must be rebuilt from the level below.
			unsigned int rebuild_start = ScaleStepCount;
			for (unsigned int level = 1; level < ScaleStepCount; level++)
			{
				const uint64_t level_scale =
					(uint64_t)EnvelopeScaleFactor << (EnvelopeScalePower * level);
				if (discarded_samples % level_scale != 0)
				{
					rebuild_start = level;
					break;
				}

				const uint64_t drop = discarded_samples / level_scale;
				Envelope &e = envelope_levels_[level];
				if (e.samples)
				{
					if (drop >= e.length)
					{
						e.length = 0;
						e.head_offset = 0;
					}
					else
					{
						e.head_offset += drop;
						e.length -= drop;
					}
				}
			}

			rebuild_higher_envelope_levels(rebuild_start);

			// Note: min_value_/max_value_ are left as-is. They may remain slightly
			// wider than the true extremes of the retained window, which is a
			// deliberate simplification (only affects vertical scaling hints).
		}

		void AnalogSegment::rebuild_higher_envelope_levels(unsigned int start_level)
		{
			lock_guard<recursive_mutex> lock(mutex_);

			for (unsigned int level = start_level; level < ScaleStepCount; level++)
			{
				Envelope &e = envelope_levels_[level];
				const Envelope &el = envelope_levels_[level - 1];

				e.length = el.length / EnvelopeScaleFactor;
				// Rebuilt levels are compacted to the front (head_offset = 0).
				e.head_offset = 0;

				if (e.length == 0)
					continue;
				if (!el.samples)
				{
					e.length = 0;
					continue;
				}

				if (!wch_reallocate_envelope(e, level))
				{
					e.length = 0;
					continue;
				}

				// Read from the lower level starting at el.head_offset.
				const EnvelopeSample *src_ptr = el.samples + el.head_offset;
				EnvelopeSample *dest_ptr = e.samples;

				for (uint64_t i = 0; i < e.length; i++)
				{
					const EnvelopeSample *const end_src_ptr =
						src_ptr + EnvelopeScaleFactor;

					EnvelopeSample sub_sample = *src_ptr++;
					while (src_ptr < end_src_ptr)
					{
						sub_sample.min = min(sub_sample.min, src_ptr->min);
						sub_sample.max = max(sub_sample.max, src_ptr->max);
						src_ptr++;
					}

					*dest_ptr++ = sub_sample;
				}
			}
		}

	} // namespace data
} // namespace pv
