/*
 * Copyright (C) 2021 Apple Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Assertions.h>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Variant.h>

namespace WTF {

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(LikelyDenseUnsignedIntegerSet);

// This is effectively a Variant<HashSet, Pair<BitVector, IndexType>>
// If it is in BitVector mode, it keeps track of the minimum value in the set, and has the bitVector shifted by the same amount.
// So for example {4000, 4002, 4003} would be represented as the bitVector 1101 with a m_min of 4000.
// It shifts between the two modes whenever that would at least halve its memory usage. So it will never use more than twice the optimal amount of memory, and yet should not ping-pong between the two modes too often.
template<typename IndexType>
class LikelyDenseUnsignedIntegerSet {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(LikelyDenseUnsignedIntegerSet);
    static_assert(std::is_unsigned<IndexType>::value);
    using Set = HashSet<IndexType, WTF::IntHash<IndexType>, WTF::UnsignedWithZeroKeyHashTraits<IndexType> >;
public:
    LikelyDenseUnsignedIntegerSet()
        : m_size(0)
        , m_min(0)
        , m_max(0)
    {
        new (&m_inline.bitVector) BitVector;
    }

    ~LikelyDenseUnsignedIntegerSet()
    {
        if (isBitVector())
            m_inline.bitVector.~BitVector();
        else
            m_inline.hashSet.~Set();
    }

    LikelyDenseUnsignedIntegerSet(LikelyDenseUnsignedIntegerSet&& other)
        : m_size(other.m_size)
        , m_min(other.m_min)
        , m_max(other.m_max)
    {
        if (isBitVector())
            new (&m_inline.bitVector) BitVector(WTFMove(other.m_inline.bitVector));
        else
            new (&m_inline.hashSet) Set(WTFMove(other.m_inline.hashSet));
    }

    bool contains(IndexType value) const
    {
        ASSERT(isValidValue(value));

        if (isBitVector()) {
            if (m_min > value)
                return false;
            return m_inline.bitVector.get(value - m_min);
        }

        return m_inline.hashSet.contains(value);
    }

    // Providing an iterator in this would be possible, but none of our clients need it.
    struct AddResult {
        bool isNewEntry;
    };

    AddResult add(IndexType value)
    {
        ASSERT(isValidValue(value));

        if (!m_size) {
            ASSERT(isBitVector());
            m_min = value;
            m_max = value;
            m_size = 1;
            m_inline.bitVector.quickSet(0);
            return { true };
        }

        if (!isBitVector()) {
            bool isNewEntry = m_inline.hashSet.add(value).isNewEntry;
            if (!isNewEntry)
                return { false };
            m_min = std::min(m_min, value);
            m_max = std::max(m_max, value);
            unsigned hashSetSize = m_inline.hashSet.capacity() * sizeof(IndexType);
            unsigned wouldBeBitVectorSize = (m_max - m_min) / 8;
            if (wouldBeBitVectorSize * 2 < hashSetSize)
                transitionToBitVector();
            return { true };
        }

        if (value >= m_min && value <= m_max) {
            bool isNewEntry = !m_inline.bitVector.quickSet(value - m_min);
            m_size += isNewEntry;
            return { isNewEntry };
        }

        // We are in BitVector mode, and value is not in the bounds: we will definitely insert it as a new entry.
        ++m_size;

        IndexType newMin = std::min(m_min, value);
        IndexType newMax = std::max(m_max, value);
        unsigned bitVectorSize = (newMax - newMin) / 8;
        unsigned hashSetEstimatedOccupancyOverhead = 3; // max occupancy for a large HashSet is 50%
        unsigned wouldBeHashSetCapacity = std::max(8u, m_size) * hashSetEstimatedOccupancyOverhead;
        unsigned wouldBeHashSetSize = wouldBeHashSetCapacity * sizeof(IndexType);
        if (wouldBeHashSetSize * 2 < bitVectorSize) {
            transitionToHashSet();
            auto result = m_inline.hashSet.add(value);
            ASSERT_UNUSED(result, result.isNewEntry);
            m_min = newMin;
            m_max = newMax;
            return { true };
        }

        if (m_min <= value) {
            bool isNewEntry = !m_inline.bitVector.set(value - m_min);
            ASSERT_UNUSED(isNewEntry, isNewEntry);
            ASSERT(newMax == value);
            m_max = value;
            return { true };
        }

        BitVector newBitVector;
        newBitVector.resize(m_max - value + 1);
        IndexType shiftReduction = m_min - value;
        for (IndexType oldIndex : m_inline.bitVector)
            newBitVector.quickSet(oldIndex + shiftReduction);
        newBitVector.quickSet(0);
        m_inline.bitVector = WTFMove(newBitVector);
        ASSERT(newMin == value);
        m_min = value;
        return { true };
    }

    unsigned size() const
    {
        if (isBitVector())
            return m_size;
        return m_inline.hashSet.size();
    }

    class iterator {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        iterator(BitVector::iterator it, IndexType shift)
            : m_underlying({ it })
            , m_shift(shift)
        {
        }
        iterator(typename Set::iterator it, IndexType shift)
            : m_underlying({ it })
            , m_shift(shift)
        {
        }

        iterator& operator++()
        {
            WTF::switchOn(m_underlying,
                [](BitVector::iterator& it) { ++it; },
                [](typename Set::iterator& it) { ++it; });
            return *this;
        }

        IndexType operator*() const
        {
            return WTF::switchOn(m_underlying,
                [&](const BitVector::iterator& it) { return *it + m_shift; },
                [](const typename Set::iterator& it) { return *it; });
        }

        bool operator==(const iterator& other) const
        {
            return m_underlying == other.m_underlying && m_shift == other.m_shift;
        }
        bool operator!=(const iterator& other) const
        {
            return m_underlying != other.m_underlying || m_shift != other.m_shift;
        }

    private:
        WTF::Variant<BitVector::iterator, typename Set::iterator> m_underlying;
        IndexType m_shift;
    };

    iterator begin() const
    {
        if (isBitVector())
            return { m_inline.bitVector.begin(), m_min };
        return { m_inline.hashSet.begin(), m_min };
    }
    iterator end() const
    {
        if (isBitVector())
            return { m_inline.bitVector.end(), m_min };
        return { m_inline.hashSet.end(), m_min };
    }

    unsigned memoryUse() const
    {
        unsigned result = sizeof(LikelyDenseUnsignedIntegerSet);
        if (isBitVector())
            result += m_inline.bitVector.outOfLineMemoryUse();
        else
            result += m_inline.hashSet.capacity() * sizeof(IndexType);
        return result;
    }

    void validate() const
    {
        IndexType min = std::numeric_limits<IndexType>::max(), max = 0;
        if (isBitVector()) {
            unsigned numElements = 0;
            for (IndexType shiftedIndex : m_inline.bitVector) {
                ++numElements;
                IndexType correctedIndex = m_min + shiftedIndex;
                min = std::min(min, correctedIndex);
                max = std::max(max, correctedIndex);
            }
            RELEASE_ASSERT(m_size == numElements);
        } else {
            for (IndexType index : m_inline.hashSet) {
                min = std::min(min, index);
                max = std::max(max, index);
            }
        }
        if (m_size) {
            RELEASE_ASSERT(m_min == min);
            RELEASE_ASSERT(m_max == max);
        }
    }

private:
    bool isBitVector() const { return m_size != std::numeric_limits<unsigned>::max(); }

    bool isValidValue(IndexType value) const
    {
        return value != UnsignedWithZeroKeyHashTraits<IndexType>::emptyValue()
            && !UnsignedWithZeroKeyHashTraits<IndexType>::isDeletedValue(value);
    }

    void transitionToHashSet()
    {
        ASSERT(isBitVector());

        Set newSet;
        newSet.reserveInitialCapacity(m_size + 1);
        for (IndexType oldIndex : m_inline.bitVector)
            newSet.add(oldIndex + m_min);
        m_inline.bitVector.~BitVector();
        new (&m_inline.hashSet) Set(WTFMove(newSet));
        m_size = std::numeric_limits<unsigned>::max();

        ASSERT(!isBitVector());
    }

    void transitionToBitVector()
    {
        ASSERT(!isBitVector());

        BitVector newBitVector;
        newBitVector.ensureSize(m_max - m_min + 1);
        m_size = 0;
        for (IndexType oldValue : m_inline.hashSet) {
            newBitVector.quickSet(oldValue - m_min);
            ++m_size;
        }
        ASSERT(newBitVector.quickGet(0));
        m_inline.hashSet.~Set();
        new (&m_inline.bitVector) BitVector(WTFMove(newBitVector));

        ASSERT(isBitVector());
    }

    union U {
        BitVector bitVector;
        Set hashSet;

        U() { }
        ~U() { }
    } m_inline;
    unsigned m_size; // Only updated in BitVector mode, std::numeric_limits<unsigned>::max() to indicate HashSet mode
    IndexType m_min;
    IndexType m_max;

};

} // namespace WTF

using WTF::LikelyDenseUnsignedIntegerSet;
