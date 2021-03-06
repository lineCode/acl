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
#include "acl/core/bitset.h"
#include "acl/core/enum_utils.h"
#include "acl/core/algorithm_types.h"
#include "acl/algorithm/uniformly_sampled/common.h"
#include "acl/compression/compressed_clip_impl.h"
#include "acl/compression/skeleton.h"
#include "acl/compression/animation_clip.h"
#include "acl/compression/stream/track_stream.h"
#include "acl/compression/stream/convert_clip_to_streams.h"
#include "acl/compression/stream/convert_rotation_streams.h"
#include "acl/compression/stream/compact_constant_streams.h"
#include "acl/compression/stream/normalize_streams.h"
#include "acl/compression/stream/quantize_streams.h"
#include "acl/compression/stream/get_num_animated_streams.h"
#include "acl/compression/stream/write_stream_bitsets.h"
#include "acl/compression/stream/write_stream_data.h"
#include "acl/compression/stream/write_range_data.h"

#include <stdint.h>
#include <cstdio>

//////////////////////////////////////////////////////////////////////////
// Full Precision Encoder
//
// The goal of the full precision format is to be used as a reference
// point for compression speed, compressed size, and decompression speed.
// This will not be a raw format in that we will at least drop constant
// or bind pose tracks. As such, it is near-raw but not quite.
//
// This is the highest precision encoder and the fastest to compress.
//
// Data format:
//    TODO: Detail the format
//////////////////////////////////////////////////////////////////////////

namespace acl
{
	namespace uniformly_sampled
	{
		struct CompressionSettings
		{
			RotationFormat8 rotation_format;
			VectorFormat8 translation_format;

			RangeReductionFlags8 range_reduction;

			CompressionSettings()
				: rotation_format(RotationFormat8::Quat_128)
				, translation_format(VectorFormat8::Vector3_96)
				, range_reduction(RangeReductionFlags8::None)
			{}
		};

		// Encoder entry point
		inline CompressedClip* compress_clip(Allocator& allocator, const AnimationClip& clip, const RigidSkeleton& skeleton, const CompressionSettings& settings)
		{
			using namespace impl;

			uint16_t num_bones = clip.get_num_bones();
			uint32_t num_samples = clip.get_num_samples();

			if (ACL_TRY_ASSERT(num_bones > 0, "Clip has no bones!"))
				return nullptr;
			if (ACL_TRY_ASSERT(num_samples > 0, "Clip has no samples!"))
				return nullptr;

			if (settings.translation_format != VectorFormat8::Vector3_96)
			{
				if (ACL_TRY_ASSERT(are_enum_flags_set(settings.range_reduction, RangeReductionFlags8::PerClip | RangeReductionFlags8::Translations), "Translation quantization requires range reduction to be enabled!"))
					return nullptr;
			}

			BoneStreams* bone_streams = convert_clip_to_streams(allocator, clip);
			convert_rotation_streams(allocator, bone_streams, num_bones, settings.rotation_format);
			compact_constant_streams(allocator, bone_streams, num_bones, 0.00001);

			uint32_t clip_range_data_size = 0;
			if (is_enum_flag_set(settings.range_reduction, RangeReductionFlags8::PerClip))
			{
				normalize_rotation_streams(bone_streams, num_bones, settings.range_reduction, settings.rotation_format);
				normalize_translation_streams(bone_streams, num_bones, settings.range_reduction);
				clip_range_data_size = get_stream_range_data_size(bone_streams, num_bones, settings.range_reduction, settings.rotation_format, settings.translation_format);
			}

			quantize_rotation_streams(allocator, bone_streams, num_bones, settings.rotation_format);
			quantize_translation_streams(allocator, bone_streams, num_bones, settings.translation_format);

			uint32_t num_constant_rotation_tracks;
			uint32_t num_constant_translation_tracks;
			uint32_t num_animated_rotation_tracks;
			uint32_t num_animated_translation_tracks;
			get_num_animated_streams(bone_streams, num_bones, num_constant_rotation_tracks, num_constant_translation_tracks, num_animated_rotation_tracks, num_animated_translation_tracks);

			uint32_t rotation_size = get_packed_rotation_size(settings.rotation_format);
			uint32_t translation_size = get_packed_vector_size(settings.translation_format);

			// Constant translation tracks store the remaining sample with full precision
			const uint32_t constant_translation_size = get_packed_vector_size(VectorFormat8::Vector3_96);
			uint32_t constant_data_size = (rotation_size * num_constant_rotation_tracks) + (constant_translation_size * num_constant_translation_tracks);
			uint32_t animated_data_size = (rotation_size * num_animated_rotation_tracks) + (translation_size * num_animated_translation_tracks);

			animated_data_size *= num_samples;

			uint32_t bitset_size = get_bitset_size(num_bones * FullPrecisionConstants::NUM_TRACKS_PER_BONE);

			uint32_t buffer_size = 0;
			buffer_size += sizeof(CompressedClip);
			buffer_size += sizeof(FullPrecisionHeader);
			buffer_size += sizeof(uint32_t) * bitset_size;		// Default tracks bitset
			buffer_size += sizeof(uint32_t) * bitset_size;		// Constant tracks bitset
			buffer_size += constant_data_size;					// Constant track data
			buffer_size = align_to(buffer_size, 4);				// Align range data
			buffer_size += clip_range_data_size;						// Range data
			buffer_size = align_to(buffer_size, 4);				// Align animated data
			buffer_size += animated_data_size;					// Animated track data

			uint8_t* buffer = allocate_type_array<uint8_t>(allocator, buffer_size, 16);

			CompressedClip* compressed_clip = make_compressed_clip(buffer, buffer_size, AlgorithmType8::UniformlySampled);

			FullPrecisionHeader& header = get_full_precision_header(*compressed_clip);
			header.num_bones = num_bones;
			header.rotation_format = settings.rotation_format;
			header.translation_format = settings.translation_format;
			header.range_reduction = settings.range_reduction;
			header.num_samples = num_samples;
			header.sample_rate = clip.get_sample_rate();
			header.num_animated_rotation_tracks = num_animated_rotation_tracks;
			header.num_animated_translation_tracks = num_animated_translation_tracks;
			header.default_tracks_bitset_offset = sizeof(FullPrecisionHeader);
			header.constant_tracks_bitset_offset = header.default_tracks_bitset_offset + (sizeof(uint32_t) * bitset_size);
			header.constant_track_data_offset = header.constant_tracks_bitset_offset + (sizeof(uint32_t) * bitset_size);	// Aligned to 4 bytes
			header.clip_range_data_offset = align_to(header.constant_track_data_offset + constant_data_size, 4);					// Aligned to 4 bytes
			header.track_data_offset = align_to(header.clip_range_data_offset + clip_range_data_size, 4);								// Aligned to 4 bytes

			write_default_track_bitset(bone_streams, num_bones, header.get_default_tracks_bitset(), bitset_size);
			write_constant_track_bitset(bone_streams, num_bones, header.get_constant_tracks_bitset(), bitset_size);

			if (constant_data_size > 0)
				write_constant_track_data(bone_streams, num_bones, header.get_constant_track_data(), constant_data_size);
			else
				header.constant_track_data_offset = InvalidPtrOffset();

			if (animated_data_size > 0)
				write_animated_track_data(bone_streams, num_bones, header.get_track_data(), animated_data_size);
			else
				header.track_data_offset = InvalidPtrOffset();

			if (is_enum_flag_set(settings.range_reduction, RangeReductionFlags8::PerClip))
				write_range_track_data(bone_streams, num_bones, settings.range_reduction, settings.rotation_format, settings.translation_format, header.get_clip_range_data(), clip_range_data_size);
			else
				header.clip_range_data_offset = InvalidPtrOffset();

			finalize_compressed_clip(*compressed_clip);

			deallocate_type_array(allocator, bone_streams, num_bones);

			return compressed_clip;
		}

		inline void print_stats(const CompressedClip& clip, std::FILE* file)
		{
			using namespace impl;

			const FullPrecisionHeader& header = get_full_precision_header(clip);

			uint32_t num_animated_tracks = header.num_animated_rotation_tracks + header.num_animated_translation_tracks;

			fprintf(file, "Clip rotation format: %s\n", get_rotation_format_name(header.rotation_format));
			fprintf(file, "Clip translation format: %s\n", get_vector_format_name(header.translation_format));
			fprintf(file, "Clip range reduction: %s\n", get_range_reduction_name(header.range_reduction));
			fprintf(file, "Clip num animated tracks: %u\n", num_animated_tracks);
		}
	}
}
