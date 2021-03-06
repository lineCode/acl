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
#include "acl/compression/skeleton.h"
#include "acl/compression/animation_clip.h"
#include "acl/io/clip_reader.h"
#include "acl/compression/skeleton_error_metric.h"

#include "acl/algorithm/uniformly_sampled/algorithm.h"

#include <Windows.h>
#include <conio.h>

#include <cstring>
#include <cstdio>
#include <fstream>
#include <streambuf>
#include <sstream>
#include <string>
#include <memory>

//#define ACL_RUN_UNIT_TESTS

struct OutputWriterImpl : public acl::OutputWriter
{
	OutputWriterImpl(acl::Allocator& allocator, uint16_t num_bones)
		: m_allocator(allocator)
		, m_transforms(acl::allocate_type_array<acl::Transform_64>(allocator, num_bones))
		, m_num_bones(num_bones)
	{}

	~OutputWriterImpl()
	{
		deallocate_type_array(m_allocator, m_transforms, m_num_bones);
	}

	void write_bone_rotation(uint32_t bone_index, const acl::Quat_32& rotation)
	{
		ACL_ENSURE(bone_index < m_num_bones, "Invalid bone index. %u >= %u", bone_index, m_num_bones);
		m_transforms[bone_index].rotation = acl::quat_cast(rotation);
	}

	void write_bone_translation(uint32_t bone_index, const acl::Vector4_32& translation)
	{
		ACL_ENSURE(bone_index < m_num_bones, "Invalid bone index. %u >= %u", bone_index, m_num_bones);
		m_transforms[bone_index].translation = acl::vector_cast(translation);
	}

	acl::Allocator& m_allocator;
	acl::Transform_64* m_transforms;
	uint16_t m_num_bones;
};

struct RawOutputWriterImpl : public acl::OutputWriter
{
	RawOutputWriterImpl(acl::Allocator& allocator, uint16_t num_bones)
		: m_allocator(allocator)
		, m_transforms(acl::allocate_type_array<acl::Transform_64>(allocator, num_bones))
		, m_num_bones(num_bones)
	{}

	~RawOutputWriterImpl()
	{
		deallocate_type_array(m_allocator, m_transforms, m_num_bones);
	}

	void write_bone_rotation(uint32_t bone_index, const acl::Quat_64& rotation)
	{
		ACL_ENSURE(bone_index < m_num_bones, "Invalid bone index. %u >= %u", bone_index, m_num_bones);
		m_transforms[bone_index].rotation = rotation;
	}

	void write_bone_translation(uint32_t bone_index, const acl::Vector4_64& translation)
	{
		ACL_ENSURE(bone_index < m_num_bones, "Invalid bone index. %u >= %u", bone_index, m_num_bones);
		m_transforms[bone_index].translation = translation;
	}

	acl::Allocator& m_allocator;
	acl::Transform_64* m_transforms;
	uint16_t m_num_bones;
};

struct Options
{
	const char*		input_filename;
	bool			output_stats;
	const char*		output_stats_filename;

	//////////////////////////////////////////////////////////////////////////

	std::FILE*		output_stats_file;

	Options()
		: input_filename(nullptr),
		  output_stats(false)
		, output_stats_filename(nullptr)
		, output_stats_file(nullptr)
	{}

	Options(Options&& other)
		: output_stats(other.output_stats)
		, output_stats_filename(other.output_stats_filename)
		, output_stats_file(other.output_stats_file)
	{
		new (&other) Options();
	}

	~Options()
	{
		if (output_stats_file != nullptr && output_stats_file != stdout)
			std::fclose(output_stats_file);
	}

	Options& operator=(Options&& rhs)
	{
		std::swap(output_stats, rhs.output_stats);
		std::swap(output_stats_filename, rhs.output_stats_filename);
		std::swap(output_stats_file, rhs.output_stats_file);
	}

	Options(const Options&) = delete;
	Options& operator=(const Options&) = delete;

	void open_output_stats_file()
	{
		std::FILE* file = nullptr;
		if (output_stats_filename != nullptr)
			fopen_s(&file, output_stats_filename, "w");
		output_stats_file = file != nullptr ? file : stdout;
	}
};

constexpr char* ACL_INPUT_FILE_OPTION = "-acl=";
constexpr char* STATS_OUTPUT_OPTION = "-stats";

static bool parse_options(int argc, char** argv, Options& options)
{
	for (int arg_index = 1; arg_index < argc; ++arg_index)
	{
		const char* argument = argv[arg_index];

		size_t option_length = std::strlen(ACL_INPUT_FILE_OPTION);
		if (std::strncmp(argument, ACL_INPUT_FILE_OPTION, option_length) == 0)
		{
			options.input_filename = argument + option_length;
			continue;
		}

		option_length = std::strlen(STATS_OUTPUT_OPTION);
		if (std::strncmp(argument, STATS_OUTPUT_OPTION, option_length) == 0)
		{
			options.output_stats = true;
			options.output_stats_filename = argument[option_length] == '=' ? argument + option_length + 1 : nullptr;
			options.open_output_stats_file();
			continue;
		}

		printf("Unrecognized option %s\n", argument);
		return false;
	}

	if (options.input_filename == nullptr || std::strlen(options.input_filename) == 0)
	{
		printf("An input file is required.\n");
		return false;
	}

	return true;
}

#ifdef ACL_RUN_UNIT_TESTS
static acl::Vector4_64 quat_rotate_scalar(const acl::Quat_64& rotation, const acl::Vector4_64& vector)
{
	using namespace acl;
	// (q.W*q.W-qv.qv)v + 2(qv.v)qv + 2 q.W (qv x v)
	Vector4_64 qv = vector_set(quat_get_x(rotation), quat_get_y(rotation), quat_get_z(rotation));
	Vector4_64 vOut = vector_mul(vector_cross3(qv, vector), 2.0 * quat_get_w(rotation));
	vOut = vector_add(vOut, vector_mul(vector, (quat_get_w(rotation) * quat_get_w(rotation)) - vector_dot(qv, qv)));
	vOut = vector_add(vOut, vector_mul(qv, 2.0 * vector_dot(qv, vector)));
	return vOut;
}

static acl::Quat_64 quat_mul_scalar(const acl::Quat_64& lhs, const acl::Quat_64& rhs)
{
	using namespace acl;
	double lhs_raw[4] = { quat_get_x(lhs), quat_get_y(lhs), quat_get_z(lhs), quat_get_w(lhs) };
	double rhs_raw[4] = { quat_get_x(rhs), quat_get_y(rhs), quat_get_z(rhs), quat_get_w(rhs) };

	double x = (rhs_raw[3] * lhs_raw[0]) + (rhs_raw[0] * lhs_raw[3]) + (rhs_raw[1] * lhs_raw[2]) - (rhs_raw[2] * lhs_raw[1]);
	double y = (rhs_raw[3] * lhs_raw[1]) - (rhs_raw[0] * lhs_raw[2]) + (rhs_raw[1] * lhs_raw[3]) + (rhs_raw[2] * lhs_raw[0]);
	double z = (rhs_raw[3] * lhs_raw[2]) + (rhs_raw[0] * lhs_raw[1]) - (rhs_raw[1] * lhs_raw[0]) + (rhs_raw[2] * lhs_raw[3]);
	double w = (rhs_raw[3] * lhs_raw[3]) - (rhs_raw[0] * lhs_raw[0]) - (rhs_raw[1] * lhs_raw[1]) - (rhs_raw[2] * lhs_raw[2]);

	return quat_set(x, y, z, w);
}

static void run_unit_tests()
{
	using namespace acl;

	constexpr double threshold = 1e-6;

	{
		Quat_64 quat0 = quat_from_euler(deg2rad(30.0), deg2rad(-45.0), deg2rad(90.0));
		Quat_64 quat1 = quat_from_euler(deg2rad(45.0), deg2rad(60.0), deg2rad(120.0));
		Quat_64 result = quat_mul(quat0, quat1);
		Quat_64 result_ref = quat_mul_scalar(quat0, quat1);
		ACL_ENSURE(quat_near_equal(result, result_ref, threshold), "quat_mul unit test failure");

		quat0 = quat_set(0.39564531008956383, 0.044254239301713752, 0.22768840967675355, 0.88863059760894492);
		quat1 = quat_set(1.0, 0.0, 0.0, 0.0);
		result = quat_mul(quat0, quat1);
		result_ref = quat_mul_scalar(quat0, quat1);
		ACL_ENSURE(quat_near_equal(result, result_ref, threshold), "quat_mul unit test failure");
	}

	{
		const Quat_64 test_rotations[] = {
			quat_identity_64(),
			quat_from_euler(deg2rad(30.0), deg2rad(-45.0), deg2rad(90.0)),
			quat_from_euler(deg2rad(45.0), deg2rad(60.0), deg2rad(120.0)),
			quat_from_euler(deg2rad(0.0), deg2rad(180.0), deg2rad(45.0)),
			quat_from_euler(deg2rad(-120.0), deg2rad(-90.0), deg2rad(0.0)),
			quat_from_euler(deg2rad(-0.01), deg2rad(0.02), deg2rad(-0.03)),
		};

		const Vector4_64 test_vectors[] = {
			vector_zero_64(),
			vector_set(1.0, 0.0, 0.0),
			vector_set(0.0, 1.0, 0.0),
			vector_set(0.0, 0.0, 1.0),
			vector_set(45.0, -60.0, 120.0),
			vector_set(-45.0, 60.0, -120.0),
			vector_set(0.57735026918962576451, 0.57735026918962576451, 0.57735026918962576451),
			vector_set(-1.0, 0.0, 0.0),
		};

		for (size_t quat_index = 0; quat_index < (sizeof(test_rotations) / sizeof(Quat_64)); ++quat_index)
		{
			const Quat_64& rotation = test_rotations[quat_index];
			for (size_t vector_index = 0; vector_index < (sizeof(test_vectors) / sizeof(Vector4_64)); ++vector_index)
			{
				const Vector4_64& vector = test_vectors[vector_index];
				Vector4_64 result = quat_rotate(rotation, vector);
				Vector4_64 result_ref = quat_rotate_scalar(rotation, vector);
				ACL_ENSURE(vector_near_equal(result, result_ref, threshold), "quat_rotate unit test failure");
			}
		}
	}

	{
		Quat_64 rotation = quat_set(0.39564531008956383, 0.044254239301713752, 0.22768840967675355, 0.88863059760894492);
		Vector4_64 axis_ref = vector_set(1.0, 0.0, 0.0);
		axis_ref = quat_rotate(rotation, axis_ref);
		double angle_ref = deg2rad(57.0);
		Quat_64 result = quat_from_axis_angle(axis_ref, angle_ref);
		Vector4_64 axis;
		double angle;
		quat_to_axis_angle(result, axis, angle);
		ACL_ENSURE(vector_near_equal(axis, axis_ref, threshold), "quat_to_axis_angle unit test failure");
		ACL_ENSURE(scalar_near_equal(angle, angle_ref, threshold), "quat_to_axis_angle unit test failure");
	}
}
#endif

static void print_stats(const Options& options, const acl::AnimationClip& clip, const acl::CompressedClip& compressed_clip, uint64_t elapsed_cycles, double max_error, acl::IAlgorithm& algorithm)
{
	if (!options.output_stats)
		return;

	uint32_t raw_size = clip.get_total_size();
	uint32_t compressed_size = compressed_clip.get_size();
	double compression_ratio = double(raw_size) / double(compressed_size);

	LARGE_INTEGER frequency_cycles_per_sec;
	QueryPerformanceFrequency(&frequency_cycles_per_sec);
	double elapsed_time_sec = double(elapsed_cycles) / double(frequency_cycles_per_sec.QuadPart);

	std::FILE* file = options.output_stats_file;

	fprintf(file, "Clip algorithm: %s\n", get_algorithm_name(compressed_clip.get_algorithm_type()));
	fprintf(file, "Clip raw size (bytes): %u\n", raw_size);
	fprintf(file, "Clip compressed size (bytes): %u\n", compressed_size);
	fprintf(file, "Clip compression ratio: %.2f : 1\n", compression_ratio);
	fprintf(file, "Clip max error: %.5f\n", max_error);
	fprintf(file, "Clip compression time (s): %.6f\n", elapsed_time_sec);
	fprintf(file, "Clip duration (s): %.3f\n", clip.get_duration());
	//fprintf(file, "Clip num segments: %u\n", 0);		// TODO
	algorithm.print_stats(compressed_clip, file);
	fprintf(file, "\n");
}

static double find_max_error(acl::Allocator& allocator, const acl::AnimationClip& clip, const acl::RigidSkeleton& skeleton, const acl::CompressedClip& compressed_clip, acl::IAlgorithm& algorithm)
{
	using namespace acl;

	uint16_t num_bones = clip.get_num_bones();
	RawOutputWriterImpl raw_output_writer(allocator, num_bones);
	Transform_32* lossy_pose_transforms = allocate_type_array<Transform_32>(allocator, num_bones);

	double max_error = -1.0;
	double sample_time = 0.0;
	double clip_duration = clip.get_duration();
	double sample_increment = 1.0 / clip.get_sample_rate();
	while (sample_time < clip_duration)
	{
		clip.sample_pose(sample_time, raw_output_writer);
		algorithm.decompress_pose(compressed_clip, (float)sample_time, lossy_pose_transforms, num_bones);

		double error = calculate_skeleton_error(allocator, skeleton, raw_output_writer.m_transforms, lossy_pose_transforms);
		max_error = max(max_error, error);

		sample_time += sample_increment;
	}

	// Make sure we test the last sample time possible as well
	{
		clip.sample_pose(clip_duration, raw_output_writer);
		algorithm.decompress_pose(compressed_clip, (float)clip_duration, lossy_pose_transforms, num_bones);

		double error = calculate_skeleton_error(allocator, skeleton, raw_output_writer.m_transforms, lossy_pose_transforms);
		max_error = max(max_error, error);
	}

	// Unit test
	{
		// Validate that the decoder can decode a single bone at a particular time
		// Use the last bone and last sample time to ensure we can seek properly
		uint16_t sample_bone_index = num_bones - 1;
		Quat_32 test_rotation;
		Vector4_32 test_translation;
		algorithm.decompress_bone(compressed_clip, (float)clip_duration, sample_bone_index, &test_rotation, &test_translation);
		ACL_ENSURE(quat_near_equal(test_rotation, lossy_pose_transforms[sample_bone_index].rotation), "Failed to sample bone index: %u", sample_bone_index);
		ACL_ENSURE(vector_near_equal3(test_translation, lossy_pose_transforms[sample_bone_index].translation), "Failed to sample bone index: %u", sample_bone_index);
	}

	deallocate_type_array(allocator, lossy_pose_transforms, num_bones);

	return max_error;
}

static void try_algorithm(const Options& options, acl::Allocator& allocator, const acl::AnimationClip& clip, const acl::RigidSkeleton& skeleton, acl::IAlgorithm &algorithm)
{
	using namespace acl;

	LARGE_INTEGER start_time_cycles;
	QueryPerformanceCounter(&start_time_cycles);

	CompressedClip* compressed_clip = algorithm.compress_clip(allocator, clip, skeleton);

	LARGE_INTEGER end_time_cycles;
	QueryPerformanceCounter(&end_time_cycles);

	ACL_ENSURE(compressed_clip->is_valid(true), "Compressed clip is invalid");

	double max_error = find_max_error(allocator, clip, skeleton, *compressed_clip, algorithm);

	print_stats(options, clip, *compressed_clip, end_time_cycles.QuadPart - start_time_cycles.QuadPart, max_error, algorithm);

	allocator.deallocate(compressed_clip, compressed_clip->get_size());
}

static bool read_clip(acl::Allocator& allocator, const char* filename,
					  std::unique_ptr<acl::AnimationClip, acl::Deleter<acl::AnimationClip>>& clip,
					  std::unique_ptr<acl::RigidSkeleton, acl::Deleter<acl::RigidSkeleton>>& skeleton)
{
	printf("Reading ACL input clip...");

	LARGE_INTEGER read_start_time_cycles;
	QueryPerformanceCounter(&read_start_time_cycles);

	std::ifstream t(filename);
	std::stringstream buffer;
	buffer << t.rdbuf();
	std::string str = buffer.str();

	LARGE_INTEGER read_end_time_cycles;
	QueryPerformanceCounter(&read_end_time_cycles);

	uint64_t elapsed_cycles = read_end_time_cycles.QuadPart - read_start_time_cycles.QuadPart;
	LARGE_INTEGER frequency_cycles_per_sec;
	QueryPerformanceFrequency(&frequency_cycles_per_sec);
	double elapsed_time_sec = double(elapsed_cycles) / double(frequency_cycles_per_sec.QuadPart);
	double elapsed_time_ms = elapsed_time_sec * 1000.0;

	printf(" Done in %.1f ms!\n", elapsed_time_ms);
	printf("Parsing ACL input clip...");

	acl::ClipReader reader(allocator, str.c_str(), str.length());

	if (!reader.read(skeleton) || !reader.read(clip, *skeleton))
	{
		acl::ClipReaderError err = reader.get_error();
		printf("\nError on line %d column %d: %s\n", err.line, err.column, err.get_description());
		return false;
	}

	LARGE_INTEGER parse_end_time_cycles;
	QueryPerformanceCounter(&parse_end_time_cycles);

	elapsed_cycles = parse_end_time_cycles.QuadPart - read_end_time_cycles.QuadPart;
	elapsed_time_sec = double(elapsed_cycles) / double(frequency_cycles_per_sec.QuadPart);
	elapsed_time_ms = elapsed_time_sec * 1000.0;

	printf(" Done in %.1f ms!\n", elapsed_time_ms);
	return true;
}

int main(int argc, char** argv)
{
	using namespace acl;

#ifdef ACL_RUN_UNIT_TESTS
	run_unit_tests();
#endif

	Options options;

	if (!parse_options(argc, argv, options))
	{
		return -1;
	}

	Allocator allocator;
	std::unique_ptr<AnimationClip, Deleter<AnimationClip>> clip;
	std::unique_ptr<RigidSkeleton, Deleter<RigidSkeleton>> skeleton;

	if (!read_clip(allocator, options.input_filename, clip, skeleton))
	{
		return -1;
	}

	// Compress & Decompress
	{
		UniformlySampledAlgorithm uniform_tests[] =
		{
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_96, RangeReductionFlags8::None),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_128, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_96, RangeReductionFlags8::None),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_96, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_96, RangeReductionFlags8::None),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_48, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_96, RangeReductionFlags8::None),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_96, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_48, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Translations),
			UniformlySampledAlgorithm(RotationFormat8::Quat_32, VectorFormat8::Vector3_32, RangeReductionFlags8::PerClip | acl::RangeReductionFlags8::Rotations | acl::RangeReductionFlags8::Translations),
		};

		for (UniformlySampledAlgorithm& algorithm : uniform_tests)
			try_algorithm(options, allocator, *clip.get(), *skeleton.get(), algorithm);
	}

	if (IsDebuggerPresent())
	{
		printf("Press any key to continue...\n");
		while (_kbhit() == 0);
	}

	return 0;
}
