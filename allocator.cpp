/*******************************************************************************
  Copyright (c) 2016, Manuel Freiberger
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include "allocator.hpp"

#include <weos/type_traits.hpp>

#include <cstdint>

using namespace eule;
using namespace std;


union maximum_aligned_type
{
    long long ll;
    double d;
    long double ld;
    void* v;
};

static constexpr auto max_alignment = alignment_of<maximum_aligned_type>::value;


struct DlAllocator::Header
{
    using size_type = uintptr_t;
    static_assert(sizeof(size_type) == sizeof(void*), "Size mismatch");

    struct used_t {};
    static constexpr used_t used = used_t();

    struct free_t {};
    static constexpr free_t free = free_t();

    static constexpr auto minimum_alloc_size = 2 * sizeof(size_type);
    static constexpr size_type InUse = 1;

    size_type prevSize;
    size_type thisSize;
    Header* nextFree;
    Header** prevFree;


    static
    Header* payloadToHeader(void* payload) noexcept
    {
        return reinterpret_cast<Header*>(static_cast<char*>(payload) - 2 * sizeof(size_type));
    }

    void* toPayload() const noexcept
    {
        return reinterpret_cast<char*>(const_cast<Header*>(this)) + 2 * sizeof(size_type);
    }


    size_type size() const noexcept
    {
        return thisSize & ~InUse;
    }

    size_type previousSize() const noexcept
    {
        return prevSize & ~InUse;
    }

    void setSize(size_type size, used_t) noexcept
    {
        thisSize = size | InUse;
        next()->prevSize = size | InUse;
    }

    void setSize(size_type size, free_t) noexcept
    {
        thisSize = size;
        next()->prevSize = size;
    }


    Header* next() const noexcept
    {
        return reinterpret_cast<Header*>(reinterpret_cast<char*>(
                                             const_cast<Header*>(this)) + size());
    }

    Header* nextIfFree() const noexcept
    {
        auto header = next();
        return (header->thisSize & InUse) ? nullptr : header;
    }

    Header* prevIfFree() const noexcept
    {
        return (prevSize & InUse)
                ? nullptr
                : reinterpret_cast<Header*>(reinterpret_cast<char*>(
                                                const_cast<Header*>(this)) - previousSize());
    }

    void link(Header** first) noexcept
    {
        Header** iter = first;
        while (*iter && (*iter)->size() < size())
            iter = &(*iter)->nextFree;

        prevFree = iter;
        nextFree = *iter;
        *iter = this;
        if (nextFree)
            nextFree->prevFree = &nextFree;
    }

    void unlink() noexcept
    {
        *prevFree = nextFree;
        if (nextFree)
            nextFree->prevFree = prevFree;
    }
};


DlAllocator::DlAllocator(char* begin, char* end) noexcept
    : m_freeList(nullptr)
{
    static_assert(sizeof(Header) == 4 * sizeof(void*), "There is padding");

    begin += 2 * sizeof(Header::size_type);
    begin = reinterpret_cast<char*>(
                (uintptr_t(begin) + max_alignment - 1) & -max_alignment);

    end -= 2 * sizeof(Header::size_type);
    end = reinterpret_cast<char*>(uintptr_t(end) & -max_alignment);

    if (end <= begin)
        return;

    auto header = Header::payloadToHeader(begin);
    header->setSize(end - begin, Header::free);
    header->prevSize = Header::InUse;
    header->next()->thisSize = Header::InUse;
    header->link(&m_freeList);
}

void* DlAllocator::allocate(size_t numBytes) noexcept
{
    if (numBytes <= Header::minimum_alloc_size)
        numBytes = Header::minimum_alloc_size;

    // Add the overhead for the chunk meta data to the allocation request
    // and align it to the maximum possible alignment.
    numBytes += 2 * sizeof(Header::size_type);
    numBytes = (numBytes + max_alignment - 1) & -max_alignment;

    // Find a suitable chunk.
    for (Header* iter = static_cast<Header*>(m_freeList); iter != nullptr;
         iter = iter->nextFree)
    {
        if (iter->size() >= numBytes)
        {
            iter->unlink();

            // If the chunk is big enough, split it into a used and an unused
            // part.
            auto remainingSize = iter->size() - numBytes;
            if (remainingSize >= sizeof(Header))
            {
                iter->setSize(numBytes, Header::used);
                auto next = iter->next();
                next->setSize(remainingSize, Header::free);
                next->link(&m_freeList);
            }
            else
            {
                iter->setSize(iter->size(), Header::used);
            }

            return iter->toPayload();
        }
    }

    return nullptr;
}

void DlAllocator::deallocate(void* ptr) noexcept
{
    auto self = Header::payloadToHeader(ptr);
    auto chunkSize = self->size();

    // If the next chunk is free, merge with it.
    if (auto nextHeader = self->nextIfFree())
    {
        nextHeader->unlink();
        chunkSize += nextHeader->size();
    }

    // If the previous chunk is free, merge with it.
    if (auto prevHeader = self->prevIfFree())
    {
        prevHeader->unlink();
        chunkSize += prevHeader->size();
        self = prevHeader;
    }

    // Write the size of this chunk after merging inside our header and the
    // header of the following chunk.
    self->setSize(chunkSize, Header::free);
    self->link(&m_freeList);
}
