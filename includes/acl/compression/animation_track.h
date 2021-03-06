#pragma once

////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2017 Nicholas Frechette & Animation Compression Library contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "acl/core/memory.h"
#include "acl/core/error.h"
#include "acl/core/utils.h"
#include "acl/math/quat_64.h"
#include "acl/math/vector4_64.h"

#include <stdint.h>
#include <utility>

namespace acl
{
	class AnimationTrack
	{
	public:
		bool is_initialized() const { return m_allocator != nullptr; }

		uint32_t get_num_samples() const { return m_num_samples; }

	protected:
		enum class AnimationTrackType8 : uint8_t
		{
			Rotation = 0,
			Translation = 1,
			// TODO: Scale
		};

		AnimationTrack()
			: m_allocator(nullptr)
			, m_sample_data(nullptr)
			, m_num_samples(0)
			, m_sample_rate(0)
			, m_type(AnimationTrackType8::Rotation)
		{}

		AnimationTrack(AnimationTrack&& track)
			: m_allocator(track.m_allocator)
			, m_sample_data(track.m_sample_data)
			, m_num_samples(track.m_num_samples)
			, m_sample_rate(track.m_sample_rate)
			, m_type(track.m_type)
		{}

		AnimationTrack(Allocator& allocator, uint32_t num_samples, uint32_t sample_rate, AnimationTrackType8 type)
			: m_allocator(&allocator)
			, m_sample_data(allocate_type_array<double>(allocator, num_samples * get_animation_track_sample_size(type)))
			, m_num_samples(num_samples)
			, m_sample_rate(sample_rate)
			, m_type(type)
		{}

		~AnimationTrack()
		{
			if (is_initialized())
			{
				deallocate_type_array(*m_allocator, m_sample_data, m_num_samples * get_animation_track_sample_size(m_type));
			}
		}

		AnimationTrack& operator=(AnimationTrack&& track)
		{
			std::swap(m_allocator, track.m_allocator);
			std::swap(m_sample_data, track.m_sample_data);
			std::swap(m_num_samples, track.m_num_samples);
			std::swap(m_sample_rate, track.m_sample_rate);
			std::swap(m_type, track.m_type);
			return *this;
		}

		AnimationTrack(const AnimationTrack&) = delete;
		AnimationTrack& operator=(const AnimationTrack&) = delete;

		// TODO: constexpr
		// Returns the number of values per sample
		static inline size_t get_animation_track_sample_size(AnimationTrackType8 type)
		{
			switch (type)
			{
			default:
			case AnimationTrackType8::Rotation:		return 4;
			case AnimationTrackType8::Translation:	return 3;
			}
		}

		Allocator*						m_allocator;
		double*							m_sample_data;

		uint32_t						m_num_samples;
		uint32_t						m_sample_rate;

		AnimationTrackType8				m_type;

		// TODO: Support different sampling methods: linear, cubic
	};

	class AnimationRotationTrack : public AnimationTrack
	{
	public:
		AnimationRotationTrack()
			: AnimationTrack()
		{}

		AnimationRotationTrack(Allocator& allocator, uint32_t num_samples, uint32_t sample_rate)
			: AnimationTrack(allocator, num_samples, sample_rate, AnimationTrackType8::Rotation)
		{}

		AnimationRotationTrack(AnimationRotationTrack&& track)
			: AnimationTrack(std::forward<AnimationTrack>(track))
		{}

		AnimationRotationTrack& operator=(AnimationRotationTrack&& track)
		{
			AnimationTrack::operator=(std::forward<AnimationTrack>(track));
			return *this;
		}

		void set_sample(uint32_t sample_index, const Quat_64& rotation)
		{
			ACL_ENSURE(is_initialized(), "Track is not initialized");
			ACL_ENSURE(sample_index < m_num_samples, "Invalid sample index. %u >= %u", sample_index, m_num_samples);
			ACL_ENSURE(quat_is_valid(rotation), "Invalid rotation: [%f, %f, %f, %f]", quat_get_x(rotation), quat_get_y(rotation), quat_get_z(rotation), quat_get_w(rotation));
			ACL_ENSURE(quat_is_normalized(rotation), "Rotation not normalized: [%f, %f, %f, %f]", quat_get_x(rotation), quat_get_y(rotation), quat_get_z(rotation), quat_get_w(rotation));

			size_t sample_size = get_animation_track_sample_size(m_type);
			ACL_ENSURE(sample_size == 4, "Invalid sample size. %u != 4", sample_size);

			double* sample = &m_sample_data[sample_index * sample_size];
			sample[0] = quat_get_x(rotation);
			sample[1] = quat_get_y(rotation);
			sample[2] = quat_get_z(rotation);
			sample[3] = quat_get_w(rotation);
		}

		Quat_64 get_sample(uint32_t sample_index) const
		{
			ACL_ENSURE(is_initialized(), "Track is not initialized");
			ACL_ENSURE(m_type == AnimationTrackType8::Rotation, "Invalid track type. %u != %u", m_type, AnimationTrackType8::Rotation);
			ACL_ENSURE(sample_index < m_num_samples, "Invalid sample index. %u >= %u", sample_index, m_num_samples);

			size_t sample_size = get_animation_track_sample_size(m_type);

			const double* sample = &m_sample_data[sample_index * sample_size];
			return quat_unaligned_load(sample);
		}

		Quat_64 sample_track(double sample_time) const
		{
			double track_duration = double(m_num_samples - 1) / double(m_sample_rate);

			uint32_t sample_frame0;
			uint32_t sample_frame1;
			double interpolation_alpha;
			calculate_interpolation_keys(m_num_samples, track_duration, sample_time, sample_frame0, sample_frame1, interpolation_alpha);

			Quat_64 sample0 = get_sample(sample_frame0);
			Quat_64 sample1 = get_sample(sample_frame1);
			return quat_lerp(sample0, sample1, interpolation_alpha);
		}

		AnimationRotationTrack(const AnimationRotationTrack&) = delete;
		AnimationRotationTrack& operator=(const AnimationRotationTrack&) = delete;
	};

	class AnimationTranslationTrack : public AnimationTrack
	{
	public:
		AnimationTranslationTrack()
			: AnimationTrack()
		{}

		AnimationTranslationTrack(Allocator& allocator, uint32_t num_samples, uint32_t sample_rate)
			: AnimationTrack(allocator, num_samples, sample_rate, AnimationTrackType8::Translation)
		{}

		AnimationTranslationTrack(AnimationTranslationTrack&& track)
			: AnimationTrack(std::forward<AnimationTrack>(track))
		{}

		AnimationTranslationTrack& operator=(AnimationTranslationTrack&& track)
		{
			AnimationTrack::operator=(std::forward<AnimationTrack>(track));
			return *this;
		}

		void set_sample(uint32_t sample_index, const Vector4_64& translation)
		{
			ACL_ENSURE(is_initialized(), "Track is not initialized");
			ACL_ENSURE(sample_index < m_num_samples, "Invalid sample index. %u >= %u", sample_index, m_num_samples);
			ACL_ENSURE(vector_is_valid3(translation), "Invalid translation: [%f, %f, %f, %f]", vector_get_x(translation), vector_get_y(translation), vector_get_z(translation));

			size_t sample_size = get_animation_track_sample_size(m_type);
			ACL_ENSURE(sample_size == 3, "Invalid sample size. %u != 3", sample_size);

			double* sample = &m_sample_data[sample_index * sample_size];
			sample[0] = vector_get_x(translation);
			sample[1] = vector_get_y(translation);
			sample[2] = vector_get_z(translation);
		}

		Vector4_64 get_sample(uint32_t sample_index) const
		{
			ACL_ENSURE(is_initialized(), "Track is not initialized");
			ACL_ENSURE(m_type == AnimationTrackType8::Translation, "Invalid track type. %u != %u", m_type, AnimationTrackType8::Translation);
			ACL_ENSURE(sample_index < m_num_samples, "Invalid sample index. %u >= %u", sample_index, m_num_samples);

			size_t sample_size = get_animation_track_sample_size(m_type);

			const double* sample = &m_sample_data[sample_index * sample_size];
			return vector_unaligned_load3(sample);
		}

		Vector4_64 sample_track(double sample_time) const
		{
			double track_duration = double(m_num_samples - 1) / double(m_sample_rate);

			uint32_t sample_frame0;
			uint32_t sample_frame1;
			double interpolation_alpha;
			calculate_interpolation_keys(m_num_samples, track_duration, sample_time, sample_frame0, sample_frame1, interpolation_alpha);

			Vector4_64 sample0 = get_sample(sample_frame0);
			Vector4_64 sample1 = get_sample(sample_frame1);
			return vector_lerp(sample0, sample1, interpolation_alpha);
		}

		AnimationTranslationTrack(const AnimationTranslationTrack&) = delete;
		AnimationTranslationTrack& operator=(const AnimationTranslationTrack&) = delete;
	};
}
