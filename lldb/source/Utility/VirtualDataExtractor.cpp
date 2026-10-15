//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Utility/VirtualDataExtractor.h"
#include <cassert>

using namespace lldb;
using namespace lldb_private;

VirtualDataExtractor::VirtualDataExtractor(const void *data,
                                           offset_t data_length,
                                           ByteOrder byte_order,
                                           uint32_t addr_size,
                                           LookupTable lookup_table)
    : DataExtractor(data, data_length, byte_order, addr_size),
      m_lookup_table(std::move(lookup_table)) {
  m_lookup_table.Sort();
}

VirtualDataExtractor::VirtualDataExtractor(const DataBufferSP &data_sp,
                                           ByteOrder byte_order,
                                           uint32_t addr_size,
                                           LookupTable lookup_table)
    : DataExtractor(data_sp, byte_order, addr_size),
      m_lookup_table(std::move(lookup_table)) {
  m_lookup_table.Sort();
}

VirtualDataExtractor::VirtualDataExtractor(const DataBufferSP &data_sp,
                                           LookupTable lookup_table)
    : DataExtractor(data_sp), m_lookup_table(std::move(lookup_table)) {
  m_lookup_table.Sort();
}

const VirtualDataExtractor::LookupTable::Entry *
VirtualDataExtractor::FindEntry(offset_t virtual_addr) const {
  // Use RangeDataVector's binary search instead of linear search.
  return m_lookup_table.FindEntryThatContains(virtual_addr);
}

bool VirtualDataExtractor::ValidateVirtualRead(offset_t virtual_addr,
                                               offset_t length) const {
  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  if (!entry)
    return false;

  // A malformed or partial mapped image must not cross a lookup region or
  // wrap while translating its virtual range into the backing buffer.
  const offset_t delta = virtual_addr - entry->base;
  if (delta > entry->size || length > entry->size - delta ||
      length > UINT64_MAX - virtual_addr)
    return false;
  const offset_t physical_size = GetPhysicalByteSize();
  return entry->data <= physical_size && delta <= physical_size - entry->data &&
         length <= physical_size - entry->data - delta;
}

const void *VirtualDataExtractor::GetData(offset_t *offset_ptr,
                                          offset_t length) const {
  // Override to treat offset as virtual address.
  if (!offset_ptr)
    return nullptr;

  offset_t virtual_addr = *offset_ptr;

  if (!ValidateVirtualRead(virtual_addr, length))
    return nullptr;

  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  assert(entry && "ValidateVirtualRead should have found an entry");

  offset_t physical_offset = entry->data + (virtual_addr - entry->base);
  // Use base class PeekData directly to avoid recursion.
  const void *result = DataExtractor::PeekData(physical_offset, length);

  if (result) {
    // Advance the virtual offset pointer.
    *offset_ptr += length;
  }

  return result;
}

offset_t VirtualDataExtractor::SetData(const void *bytes, lldb::offset_t length,
                                       lldb::ByteOrder byte_order) {
  // Invoked from the base class ctor.
  if (!m_data_sp || m_start == nullptr)
    return DataExtractor::SetData(bytes, length, byte_order);

  // A no-op SetData that is setting the same data buffer again.
  if (!m_data_sp && m_start == bytes && length == GetVirtualByteSize())
    return GetVirtualByteSize();

  assert("SetData(1) called on VirtualDataExtractor that already had data" &&
         false);

  DataExtractor::SetData(bytes, length, byte_order);
  ResetLookupTableToMatchPhysical();

  return GetVirtualByteSize();
}

offset_t VirtualDataExtractor::SetData(const DataExtractor &data,
                                       lldb::offset_t offset,
                                       lldb::offset_t length) {
  // Invoked from the base class ctor
  if (!m_data_sp || m_start == nullptr)
    return DataExtractor::SetData(data, offset, length);

  // A no-op SetData that is setting the same data buffer again
  if (m_data_sp && data.GetSharedDataBuffer().get() == m_data_sp.get() &&
      offset == 0 && length == GetVirtualByteSize())
    return GetVirtualByteSize();
  assert("SetData(2) called on VirtualDataExtractor that already had data" &&
         false);

  DataExtractor::SetData(data, offset, length);
  ResetLookupTableToMatchPhysical();

  return GetVirtualByteSize();
}

offset_t VirtualDataExtractor::SetData(const lldb::DataBufferSP &data_sp,
                                       lldb::offset_t offset,
                                       lldb::offset_t length) {
  // Invoked from the base class ctor
  if (!m_data_sp || m_start == nullptr)
    return DataExtractor::SetData(data_sp, offset, length);

  // A no-op SetData that is setting the same data buffer again
  if (m_data_sp && data_sp.get() == m_data_sp.get() && offset == 0 &&
      length == GetVirtualByteSize())
    return GetVirtualByteSize();

  assert("SetData(3) called on VirtualDataExtractor that already had data" &&
         false);

  DataExtractor::SetData(data_sp, offset, length);
  ResetLookupTableToMatchPhysical();

  return GetVirtualByteSize();
}

void VirtualDataExtractor::ResetLookupTableToMatchPhysical() {
  // calling SetData on a VirtualDataExtractor that already has a
  // data buffer means the LookupTable needs to be either replaced, or
  // if we assume the buffer is a subset of the original, we need to
  // update all the entries to have correct new offsets into the buffer
  // and remove entries that are outside the new range.
  // For now, zero out the LookupTable and behave as if this is a simple
  // DataExtractor.
  m_lookup_table.Clear();
  m_lookup_table.Append(
      VirtualDataExtractor::LookupTable::Entry{0, GetPhysicalByteSize(), 0});
}

uint64_t VirtualDataExtractor::GetVirtualByteSize() const {
  offset_t lowest = -1ULL;
  offset_t highest = 0;
  for (const auto ent : m_lookup_table) {
    if (ent.size > UINT64_MAX - ent.base)
      return 0;
    lowest = std::min(lowest, ent.base);
    highest = std::max(highest, ent.base + ent.size);
  }
  return lowest == UINT64_MAX ? 0 : highest - lowest;
}

uint64_t VirtualDataExtractor::GetPhysicalByteSize() const {
  return m_end - m_start;
}

offset_t VirtualDataExtractor::VirtualBytesLeft(offset_t virtual_offset) const {
  const offset_t size = GetVirtualByteSize();
  if (size > virtual_offset)
    return size - virtual_offset;
  return 0;
}

offset_t
VirtualDataExtractor::PhysicalBytesLeft(offset_t physical_offset) const {
  const offset_t size = m_end - m_start;
  if (size > physical_offset)
    return size - physical_offset;
  return 0;
}

const uint8_t *VirtualDataExtractor::PeekData(offset_t offset,
                                              offset_t length) const {
  // Override to treat offset as virtual address.
  if (!ValidateVirtualRead(offset, length))
    return nullptr;

  const LookupTable::Entry *entry = FindEntry(offset);
  assert(entry && "ValidateVirtualRead should have found an entry");

  offset_t physical_offset = entry->data + (offset - entry->base);
  // Use the base class PeekData with the physical offset.
  return DataExtractor::PeekData(physical_offset, length);
}

uint8_t VirtualDataExtractor::GetU8_unchecked(offset_t *offset_ptr) const {
  offset_t virtual_addr = *offset_ptr;
  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  assert(entry && "Unchecked methods require valid virtual address");

  offset_t physical_offset = entry->data + (virtual_addr - entry->base);
  uint8_t result = DataExtractor::GetU8_unchecked(&physical_offset);
  *offset_ptr += 1;
  return result;
}

uint16_t VirtualDataExtractor::GetU16_unchecked(offset_t *offset_ptr) const {
  offset_t virtual_addr = *offset_ptr;
  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  assert(entry && "Unchecked methods require valid virtual address");

  offset_t physical_offset = entry->data + (virtual_addr - entry->base);
  uint16_t result = DataExtractor::GetU16_unchecked(&physical_offset);
  *offset_ptr += 2;
  return result;
}

uint32_t VirtualDataExtractor::GetU32_unchecked(offset_t *offset_ptr) const {
  offset_t virtual_addr = *offset_ptr;
  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  assert(entry && "Unchecked methods require valid virtual address");

  offset_t physical_offset = entry->data + (virtual_addr - entry->base);
  uint32_t result = DataExtractor::GetU32_unchecked(&physical_offset);
  *offset_ptr += 4;
  return result;
}

uint64_t VirtualDataExtractor::GetU64_unchecked(offset_t *offset_ptr) const {
  offset_t virtual_addr = *offset_ptr;
  const LookupTable::Entry *entry = FindEntry(virtual_addr);
  assert(entry && "Unchecked methods require valid virtual address");

  offset_t physical_offset = entry->data + (virtual_addr - entry->base);
  uint64_t result = DataExtractor::GetU64_unchecked(&physical_offset);
  *offset_ptr += 8;
  return result;
}

DataExtractorSP
VirtualDataExtractor::GetSubsetExtractorSP(offset_t virtual_offset,
                                           offset_t virtual_length) {
  if (!ValidateVirtualRead(virtual_offset, virtual_length))
    return {};
  const LookupTable::Entry *entry = FindEntry(virtual_offset);
  const offset_t physical_start = entry->data + (virtual_offset - entry->base);
  auto new_sp = std::make_shared<DataExtractor>(
      GetSharedDataBuffer(), GetByteOrder(), GetAddressByteSize());
  new_sp->SetData(GetSharedDataBuffer(), physical_start, virtual_length);
  return new_sp;
}

// Return a DataExtractorSP that contains a single LookupTable's entry; all
// bytes are guaranteed to be readable.
DataExtractorSP
VirtualDataExtractor::GetSubsetExtractorSP(offset_t virtual_offset) {
  const LookupTable::Entry *entry = FindEntry(virtual_offset);
  if (!entry || virtual_offset - entry->base > entry->size)
    return {};
  return GetSubsetExtractorSP(virtual_offset,
                              entry->size - (virtual_offset - entry->base));
}

// Return an ArrayRef to the first contiguous region of the LookupTable
// only.  The LookupTable entries may have gaps of unmapped data, and we
// can't include those in the ArrayRef or something may touch those pages.
llvm::ArrayRef<uint8_t> VirtualDataExtractor::GetData() const {
  const LookupTable::Entry *entry = FindEntry(0);
  assert(entry &&
         "VirtualDataExtractor GetData requires valid virtual address");
  if (!entry)
    return {};
  return {m_start + static_cast<size_t>(entry->data), entry->size};
}
