# For this script to work, you must first install the FBX SKD Python bindings
# Make sure your python version is supported by the bindings as well

import sys
import zipfile
import os
import io
from collections import namedtuple

FBXNode = namedtuple('FBXNode', 'name parent node')
ACLClip = namedtuple('ACLClip', 'name num_samples sample_rate error_threshold duration')
ACLBone = namedtuple('ACLBone', 'name parent vtx_distance bind_rotation bind_translation bind_scale')
ACLTrack = namedtuple('ACLTrack', 'name rotations translations scales')

def parse_clip(scene):
	anim_stack = scene.GetSrcObject(FbxAnimStack.ClassId, 0)

	clip_name = anim_stack.GetName()
	sample_rate = 30
	timespan = anim_stack.GetLocalTimeSpan()
	duration = timespan.GetDuration().GetFramedTime(False).GetSecondDouble()
	num_samples = int(duration * sample_rate) + 1
	error_threshold = 0.01
	return ACLClip(clip_name, num_samples, sample_rate, error_threshold, duration)

def parse_hierarchy(scene):
	nodes = []

	root_node = scene.GetRootNode()
	nodes.append(FBXNode(root_node.GetName(), "", root_node))

	for i in range(root_node.GetChildCount()):
		parse_hierarchy_node(root_node, root_node.GetChild(i), nodes)

	return nodes

def parse_hierarchy_node(parent_node, node, nodes):
	nodes.append(FBXNode(node.GetName(), parent_node.GetName(), node))

	for i in range(node.GetChildCount()):
		parse_hierarchy_node(node, node.GetChild(i), nodes)

def vector3_to_array(vec):
	return [ vec[0], vec[1], vec[2] ]

def quaternion_to_array(vec):
	return [ vec[0], vec[1], vec[2], vec[3] ]

def parse_bind_pose(scene, nodes):
	bones = []

	vtx_distance = 0.1

	translation = FbxVector4()
	rotation = FbxQuaternion()
	shear = FbxVector4()
	scale = FbxVector4()

	for pose_idx in range(scene.GetPoseCount()):
		pose = scene.GetPose(pose_idx)

		if not pose.IsBindPose():
			continue

		for bone_idx in range(pose.GetCount()):
			bone_name = pose.GetNodeName(bone_idx).GetCurrentName()

			matrix = pose.GetMatrix(bone_idx)
			matrix.GetElements(translation, rotation, shear, scale)

			# Convert from FBX types to float arrays
			rotation_array = quaternion_to_array(rotation)
			translation_array = vector3_to_array(translation)
			scale_array = vector3_to_array(scale)

			if bone_idx == 0:
				parent_name = ""
			else:
				bone_node = next(x for x in nodes if x.name == bone_name)
				parent_name = bone_node.parent

			bone = ACLBone(bone_name, parent_name, vtx_distance, rotation_array, translation_array, scale_array)
			bones.append(bone)

	return bones

def is_key_default(key, default_value, error_threshold = 0.000001):
	for channel_index in range(len(default_value)):
		if abs(key[channel_index] - default_value[channel_index]) > error_threshold:
			# Something isn't equal to our default value
			return False

	# Everything is equal, we are a default key
	return True

def is_track_default(track, default_value, error_threshold = 0.000001):
	if len(track) == 0:
		# Empty track is considered a default track
		return True

	num_channels = len(default_value)

	for key in track:
		if not is_key_default(key, default_value, error_threshold):
			return False

	# Everything is equal, we are a default track
	return True

def parse_tracks(scene, clip, bones, nodes):
	tracks = []

	root_node = scene.GetRootNode()
	anim_evaluator = scene.GetAnimationEvaluator()

	time = FbxTime()
	frame_duration = 1.0 / clip.sample_rate
	default_rotation = [ 0.0, 0.0, 0.0, 1.0 ]
	default_translation = [ 0.0, 0.0, 0.0 ]
	default_scale = [ 1.0, 1.0, 1.0 ]

	for bone in bones:
		bone_node = next(x for x in nodes if x.name == bone.name)

		# Extract all our local space transforms
		rotations = []
		translations = []
		scales = []

		for i in range(clip.num_samples):
			time.SetSecondDouble(i * frame_duration)
			matrix = anim_evaluator.GetNodeLocalTransform(bone_node.node, time)

			rotation = matrix.GetQ()
			translation = matrix.GetT()
			scale = matrix.GetS()

			# Convert from FBX types to float arrays
			rotation = quaternion_to_array(rotation)
			translation = vector3_to_array(translation)
			scale = vector3_to_array(scale)

			rotations.append(rotation)
			translations.append(translation)
			scales.append(scale)

		# Clear track if it is constant and default
		if is_track_default(rotations, default_rotation):
			rotations = []

		if is_track_default(translations, default_translation):
			translations = []

		if is_track_default(scales, default_scale):
			scales = []

		track = ACLTrack(bone.name, rotations, translations, scales)
		tracks.append(track)

	return tracks

def print_clip_header(clip_name, clip):
	file = open('clip_{}.h'.format(clip_name), 'w')
	print('#pragma once', file = file)
	print('#include "acl/compression/skeleton.h"', file = file)
	print('#include "acl/math/quat_64.h"', file = file)
	print('#include "acl/math/vector4_64.h"', file = file)
	print('', file = file)
	print('namespace clip_{}'.format(clip_name), file = file)
	print('{', file = file)
	print('\textern acl::RigidBone bones[];', file = file)
	print('\textern uint16_t num_bones;', file = file)
	print('\tstatic constexpr uint16_t num_samples = {};'.format(clip.num_samples), file = file)
	print('\tstatic constexpr uint16_t sample_rate = {};'.format(clip.sample_rate), file = file)
	print('', file = file)
	print('\textern uint16_t rotation_track_bone_index[];', file = file)
	print('\textern uint32_t num_rotation_tracks;', file = file)
	print('\textern acl::Quat_64 rotation_tracks[][{}];'.format(clip.num_samples), file = file)
	print('', file = file)
	print('\textern uint16_t translation_track_bone_index[];', file = file)
	print('\textern uint32_t num_translation_tracks;', file = file)
	print('\textern acl::Vector4_64 translation_tracks[][{}];'.format(clip.num_samples), file = file)
	print('}', file = file)
	print('', file = file)
	file.close

def quaternion_array_to_cpp_string(quat):
	return 'acl::quat_set({}, {}, {}, {})'.format(quat[0], quat[1], quat[2], quat[3])

def vector3_array_to_cpp_string(vec):
	return 'acl::vector_set({}, {}, {})'.format(vec[0], vec[1], vec[2])

def print_clip_skeleton(clip_name, bones):
	file = open('clip_{}_skeleton.cpp'.format(clip_name), 'w')
	print('#include "acl/compression/skeleton.h"', file = file)
	print('#include "acl/math/quat_64.h"', file = file)
	print('#include "acl/math/vector4_64.h"', file = file)
	print('', file = file)
	print('namespace clip_{}'.format(clip_name), file = file)
	print('{', file = file)
	print('\tacl::RigidBone bones[] =', file = file)
	print('\t{', file = file)
	for bone in bones:
		bone_index = bones.index(bone)
		bone_name = bone.name
		parent_bone_index = "0xFFFF"
		if bone_index != 0:
			parent_bone_index = bones.index(next(x for x in bones if x.name == bone.parent))
		rotation = quaternion_array_to_cpp_string(bone.bind_rotation)
		translation = vector3_array_to_cpp_string(bone.bind_translation)
		vtx_distance = bone.vtx_distance
		print('\t\t/* {} */\t{{ "{}", {}, {}, {}, {} }},'.format(bone_index, bone_name, parent_bone_index, rotation, translation, vtx_distance), file = file)
	print('\t};', file = file)
	print('', file = file)
	print('\tuint16_t num_bones = sizeof(bones) / sizeof(acl::RigidBone);', file = file)
	print('}', file = file)
	print('', file = file)
	file.close()

def print_clip_rotations(clip_name, clip, bones, tracks):
	file = open('clip_{}_rotations.cpp'.format(clip_name), 'w')
	print('#include "acl/math/quat_64.h"', file = file)
	print('#include "acl/math/vector4_64.h"', file = file)
	print('', file = file)
	print('namespace clip_{}'.format(clip_name), file = file)
	print('{', file = file)
	print('\tuint16_t rotation_track_bone_index[] =', file = file)
	print('\t{', file = file)
	for track in tracks:
		if len(track.rotations) == 0:
			continue

		track_bone_index = bones.index(next(x for x in bones if x.name == track.name))
		print('\t\t{},\t\t// "{}"'.format(track_bone_index, track.name), file = file)
	print('\t};', file = file)
	print('', file = file)
	print('\tuint32_t num_rotation_tracks = sizeof(rotation_track_bone_index) / sizeof(uint16_t);', file = file)
	print('', file = file)
	print('\tacl::Quat_64 rotation_tracks[][{}] ='.format(clip.num_samples), file = file)
	print('\t{', file = file)
	for track in tracks:
		if len(track.rotations) == 0:
			continue
		print('\t\t{', file = file)
		for rotation in track.rotations:
			print('\t\t\t{},'.format(quaternion_array_to_cpp_string(rotation)), file = file)
		print('\t\t},', file = file)
	print('\t};', file = file)
	print('}', file = file)
	print('', file = file)
	file.close()

def print_clip_translations(clip_name, clip, bones, tracks):
	file = open('clip_{}_translations.cpp'.format(clip_name), 'w')
	print('#include "acl/math/quat_64.h"', file = file)
	print('#include "acl/math/vector4_64.h"', file = file)
	print('', file = file)
	print('namespace clip_{}'.format(clip_name), file = file)
	print('{', file = file)
	print('\tuint16_t translation_track_bone_index[] =', file = file)
	print('\t{', file = file)
	for track in tracks:
		if len(track.translations) == 0:
			continue

		track_bone_index = bones.index(next(x for x in bones if x.name == track.name))
		print('\t\t{},\t\t// "{}"'.format(track_bone_index, track.name), file = file)
	print('\t};', file = file)
	print('', file = file)
	print('\tuint32_t num_translation_tracks = sizeof(translation_track_bone_index) / sizeof(uint16_t);', file = file)
	print('', file = file)
	print('\tacl::Vector4_64 translation_tracks[][{}] ='.format(clip.num_samples), file = file)
	print('\t{', file = file)
	for track in tracks:
		if len(track.translations) == 0:
			continue
		print('\t\t{', file = file)
		for translation in track.translations:
			print('\t\t\t{},'.format(vector3_array_to_cpp_string(translation)), file = file)
		print('\t\t},', file = file)
	print('\t};', file = file)
	print('}', file = file)
	print('', file = file)
	file.close()

def parse_argv():
	options = {}
	options['fbx'] = ""

	for i in range(1, len(sys.argv)):
		value = sys.argv[i]

		# TODO: Strip trailing '/' or '\'
		if value.startswith('-fbx='):
			options['fbx'] = value[5:].replace('"', '')

	return options

def convert_file(fbx_filename):
	# Prepare the FBX SDK.
	sdk_manager, scene = InitializeSdkObjects()

	print('Loading FBX: {}'.format(fbx_filename))
	result = LoadScene(sdk_manager, scene, fbx_filename)

	if not result:
		print('An error occurred while loading the scene!')
		return False
	else:
		print('Parsing FBX...')
		# TODO: Ensure we only have 1 anim stack
		# TODO: Ensure we only have 1 anim layer
		clip = parse_clip(scene)
		nodes = parse_hierarchy(scene)
		bones = parse_bind_pose(scene, nodes)
		tracks = parse_tracks(scene, clip, bones, nodes)

		# Output to cpp files
		clip_name = os.path.basename(fbx_filename).replace('.fbx', '')
		print('Writing cpp files...')
		print_clip_header(clip_name, clip)
		print_clip_skeleton(clip_name, bones)
		print_clip_rotations(clip_name, clip, bones, tracks)
		print_clip_translations(clip_name, clip, bones, tracks)

	# Destroy all objects created by the FBX SDK.
	sdk_manager.Destroy()

	return True

if __name__ == "__main__":
	try:
		from FbxCommon import *
	except ImportError:
		import platform
		msg = 'You need to copy the content in compatible subfolder under /lib/python<version> into your python install folder such as '
		if platform.system() == 'Windows' or platform.system() == 'Microsoft':
			msg += '"Python26/Lib/site-packages"'
		elif platform.system() == 'Linux':
			msg += '"/usr/local/lib/python2.6/site-packages"'
		elif platform.system() == 'Darwin':
			msg += '"/Library/Frameworks/Python.framework/Versions/2.6/lib/python2.6/site-packages"'        
		msg += ' folder.'
		print(msg) 
		sys.exit(1)

	options = parse_argv()

	fbx_filename = options['fbx']
	if len(fbx_filename) == 0:
		print('Usage: fbx2cpp -fbx=<FBX file name>')
		sys.exit(1)

	if not os.path.exists(fbx_filename):
		print('FBX input not found: {}'.format(fbx_filename))
		sys.exit(1)

	# Convert a single file
	result = convert_file(fbx_filename)

	if result:
		sys.exit(0)
	else:
		sys.exit(1)