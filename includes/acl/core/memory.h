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

#include "acl/core/error.h"

#include <malloc.h>
#include <stdint.h>
#include <type_traits>
#include <limits>
#include <memory>

namespace acl
{
	constexpr bool is_power_of_two(size_t input)
	{
		return input != 0 && (input & (input - 1)) == 0;
	}

	template<typename Type>
	constexpr bool is_alignment_valid(size_t alignment)
	{
		return is_power_of_two(alignment) && alignment >= alignof(Type);
	}

	//////////////////////////////////////////////////////////////////////////

	class Allocator
	{
	public:
		static constexpr size_t DEFAULT_ALIGNMENT = 16;

		Allocator() {}
		virtual ~Allocator() {}

		Allocator(const Allocator&) = delete;
		Allocator& operator=(const Allocator&) = delete;

		virtual void* allocate(size_t size, size_t alignment = DEFAULT_ALIGNMENT)
		{
			return _aligned_malloc(size, alignment);
		}

		virtual void deallocate(void* ptr, size_t size)
		{
			if (ptr == nullptr)
				return;

			_aligned_free(ptr);
		}
	};

	template<typename AllocatedType, typename... Args>
	AllocatedType* allocate_type(Allocator& allocator, Args&&... args)
	{
		AllocatedType* ptr = reinterpret_cast<AllocatedType*>(allocator.allocate(sizeof(AllocatedType), alignof(AllocatedType)));
		if (std::is_trivially_default_constructible<AllocatedType>::value)
			return ptr;
		return new(ptr) AllocatedType(std::forward<Args>(args)...);
	}

	template<typename AllocatedType, typename... Args>
	AllocatedType* allocate_type(Allocator& allocator, size_t alignment, Args&&... args)
	{
		ACL_ENSURE(is_alignment_valid<AllocatedType>(alignment), "Invalid alignment: %u. Expected a power of two at least equal to %u", alignment, alignof(AllocatedType));
		AllocatedType* ptr = reinterpret_cast<AllocatedType*>(allocator.allocate(sizeof(AllocatedType), alignment));
		if (std::is_trivially_default_constructible<AllocatedType>::value)
			return ptr;
		return new(ptr) AllocatedType(std::forward<Args>(args)...);
	}

	template<typename AllocatedType>
	void deallocate_type(Allocator& allocator, AllocatedType* ptr)
	{
		if (ptr == nullptr)
			return;

		if (!std::is_trivially_destructible<AllocatedType>::value)
			ptr->~AllocatedType();

		allocator.deallocate(ptr, sizeof(AllocatedType));
	}

	template<typename AllocatedType, typename... Args>
	AllocatedType* allocate_type_array(Allocator& allocator, size_t num_elements, Args&&... args)
	{
		AllocatedType* ptr = reinterpret_cast<AllocatedType*>(allocator.allocate(sizeof(AllocatedType) * num_elements, alignof(AllocatedType)));
		if (std::is_trivially_default_constructible<AllocatedType>::value)
			return ptr;
		for (size_t element_index = 0; element_index < num_elements; ++element_index)
			new(&ptr[element_index]) AllocatedType(std::forward<Args>(args)...);
		return ptr;
	}

	template<typename AllocatedType, typename... Args>
	AllocatedType* allocate_type_array(Allocator& allocator, size_t num_elements, size_t alignment, Args&&... args)
	{
		ACL_ENSURE(is_alignment_valid<AllocatedType>(alignment), "Invalid alignment: %u. Expected a power of two at least equal to %u", alignment, alignof(AllocatedType));
		AllocatedType* ptr = reinterpret_cast<AllocatedType*>(allocator.allocate(sizeof(AllocatedType) * num_elements, alignment));
		if (std::is_trivially_default_constructible<AllocatedType>::value)
			return ptr;
		for (size_t element_index = 0; element_index < num_elements; ++element_index)
			new(&ptr[element_index]) AllocatedType(std::forward<Args>(args)...);
		return ptr;
	}

	template<typename AllocatedType>
	void deallocate_type_array(Allocator& allocator, AllocatedType* elements, size_t num_elements)
	{
		if (elements == nullptr)
			return;

		if (!std::is_trivially_destructible<AllocatedType>::value)
		{
			for (size_t element_index = 0; element_index < num_elements; ++element_index)
				elements[element_index].~AllocatedType();
		}

		allocator.deallocate(elements, sizeof(AllocatedType) * num_elements);
	}

	//////////////////////////////////////////////////////////////////////////

	template<typename AllocatedType>
	class Deleter
	{
	public:
		Deleter() : m_allocator(nullptr) {}
		Deleter(Allocator& allocator) : m_allocator(&allocator) {}
		Deleter(const Deleter& deleter) : m_allocator(deleter.m_allocator) {}

		void operator()(AllocatedType* ptr)
		{
			if (ptr == nullptr)
				return;

			if (!std::is_trivially_destructible<AllocatedType>::value)
				ptr->~AllocatedType();

			m_allocator->deallocate(ptr, sizeof(AllocatedType));
		}

	private:
		Allocator* m_allocator;
	};

	template<typename AllocatedType, typename... Args>
	std::unique_ptr<AllocatedType, Deleter<AllocatedType>> make_unique(Allocator& allocator, Args&&... args)
	{
		return std::unique_ptr<AllocatedType, Deleter<AllocatedType>>(
			allocate_type<AllocatedType>(allocator, std::forward<Args>(args)...),
			Deleter<AllocatedType>(allocator));
	}

	template<typename AllocatedType, typename... Args>
	std::unique_ptr<AllocatedType, Deleter<AllocatedType>> make_unique(Allocator& allocator, size_t alignment, Args&&... args)
	{
		return std::unique_ptr<AllocatedType, Deleter<AllocatedType>>(
			allocate_type<AllocatedType>(allocator, alignment, std::forward<Args>(args)...),
			Deleter<AllocatedType>(allocator));
	}

	template<typename AllocatedType, typename... Args>
	std::unique_ptr<AllocatedType, Deleter<AllocatedType>> make_unique_array(Allocator& allocator, size_t num_elements, Args&&... args)
	{
		return std::unique_ptr<AllocatedType, Deleter<AllocatedType>>(
			allocate_type_array<AllocatedType>(allocator, num_elements, std::forward<Args>(args)...),
			Deleter<AllocatedType>(allocator));
	}

	template<typename AllocatedType, typename... Args>
	std::unique_ptr<AllocatedType, Deleter<AllocatedType>> make_unique_array(Allocator& allocator, size_t num_elements, size_t alignment, Args&&... args)
	{
		return std::unique_ptr<AllocatedType, Deleter<AllocatedType>>(
			allocate_type_array<AllocatedType>(allocator, num_elements, alignment, std::forward<Args>(args)...),
			Deleter<AllocatedType>(allocator));
	}

	//////////////////////////////////////////////////////////////////////////

	template<typename PtrType>
	constexpr bool is_aligned_to(PtrType* value, size_t alignment)
	{
		return (reinterpret_cast<uintptr_t>(value) & (alignment - 1)) == 0;
	}

	template<typename IntegralType>
	constexpr bool is_aligned_to(IntegralType value, size_t alignment)
	{
		return (static_cast<size_t>(value) & (alignment - 1)) == 0;
	}

	template<typename PtrType>
	constexpr bool is_aligned(PtrType* value)
	{
		return is_aligned_to(value, alignof(PtrType));
	}

	template<typename PtrType>
	constexpr PtrType* align_to(PtrType* value, size_t alignment)
	{
		return reinterpret_cast<PtrType*>((reinterpret_cast<uintptr_t>(value) + (alignment - 1)) & ~(alignment - 1));
	}

	template<typename IntegralType>
	constexpr IntegralType align_to(IntegralType value, size_t alignment)
	{
		return static_cast<IntegralType>((static_cast<size_t>(value) + (alignment - 1)) & ~(alignment - 1));
	}

	template<typename DestPtrType, typename SrcPtrType>
	inline DestPtrType* safe_ptr_cast(SrcPtrType* input)
	{
		ACL_ENSURE(is_aligned_to(input, alignof(DestPtrType)), "reinterpret_cast would result in an unaligned pointer");
		return reinterpret_cast<DestPtrType*>(input);
	}

	template<typename DestPtrType, typename SrcIntegralType>
	inline DestPtrType* safe_ptr_cast(SrcIntegralType input)
	{
		ACL_ENSURE(is_aligned_to(input, alignof(DestPtrType)), "reinterpret_cast would result in an unaligned pointer");
		return reinterpret_cast<DestPtrType*>(input);
	}

	template<typename DestIntegralType, typename SrcIntegralType>
	inline DestIntegralType safe_static_cast(SrcIntegralType input)
	{
		ACL_ENSURE(input >= std::numeric_limits<DestIntegralType>::min() && input <= std::numeric_limits<DestIntegralType>::max(), "static_cast would result in truncation");
		return static_cast<DestIntegralType>(input);
	}

	template<typename OutputPtrType, typename InputPtrType, typename OffsetType>
	constexpr OutputPtrType* add_offset_to_ptr(InputPtrType* ptr, OffsetType offset)
	{
		return safe_ptr_cast<OutputPtrType>(reinterpret_cast<uintptr_t>(ptr) + offset);
	}

	//////////////////////////////////////////////////////////////////////////

	struct InvalidPtrOffset {};

	template<typename DataType, typename OffsetType>
	class PtrOffset
	{
	public:
		constexpr PtrOffset() : m_value(0) {}
		constexpr PtrOffset(size_t value) : m_value(safe_static_cast<OffsetType>(value)) {}
		constexpr PtrOffset(InvalidPtrOffset) : m_value(std::numeric_limits<OffsetType>::max()) {}

		template<typename BaseType>
		constexpr DataType* add_to(BaseType* ptr) const
		{
			ACL_ENSURE(is_valid(), "Invalid PtrOffset!");
			return add_offset_to_ptr<DataType>(ptr, m_value);
		}

		template<typename BaseType>
		constexpr const DataType* add_to(const BaseType* ptr) const
		{
			ACL_ENSURE(is_valid(), "Invalid PtrOffset!");
			return add_offset_to_ptr<const DataType>(ptr, m_value);
		}

		template<typename BaseType>
		constexpr DataType* safe_add_to(BaseType* ptr) const
		{
			return is_valid() ? add_offset_to_ptr<DataType>(ptr, m_value) : nullptr;
		}

		template<typename BaseType>
		constexpr const DataType* safe_add_to(const BaseType* ptr) const
		{
			return is_valid() ? add_offset_to_ptr<DataType>(ptr, m_value) : nullptr;
		}

		constexpr operator OffsetType() const { return m_value; }

		constexpr bool is_valid() const { return m_value != std::numeric_limits<OffsetType>::max(); }

	private:
		OffsetType m_value;
	};

	template<typename DataType>
	using PtrOffset16 = PtrOffset<DataType, uint16_t>;

	template<typename DataType>
	using PtrOffset32 = PtrOffset<DataType, uint32_t>;
}
