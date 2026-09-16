////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "duckdb/common/vector/immutable_strings.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <algorithm>

namespace duckdb {

namespace {

class CertifiedAuxiliaryDataSetHolder : public AuxiliaryDataSetHolder {
public:
	using AuxiliaryDataSetHolder::AuxiliaryDataSetHolder;

	bool CertifiesImmutablePayloads() const override {
		return true;
	}
};

void CopyImmutable(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
                   idx_t source_offset, idx_t target_offset);

bool CopyImmutableStrings(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_offset,
                          idx_t target_offset, idx_t copy_count) {
	auto &source_buffer = source.GetBufferRef();
	if (!source_buffer) {
		return false;
	}
	auto &source_aux = source_buffer->GetAuxiliaryData();
	if (!source_aux || !ImmutableStrings::Certified(*source_aux)) {
		return false;
	}
	auto &target_validity = FlatVector::ValidityMutable(target);
	target_validity.CopySel(FlatVector::Validity(source), sel, source_offset, target_offset, copy_count);
	auto ldata = FlatVector::GetData<string_t>(source);
	auto tdata = FlatVector::GetDataMutable<string_t>(target);
	if (!sel.IsSet()) {
		memcpy(tdata + target_offset, ldata + source_offset, copy_count * sizeof(string_t));
	} else {
		for (idx_t i = 0; i < copy_count; i++) {
			tdata[target_offset + i] = ldata[sel.get_index(source_offset + i)];
		}
	}
	StringVector::AddAuxiliaryData(target, make_uniq<CertifiedAuxiliaryDataSetHolder>(source_aux));
	return true;
}

void CopyImmutableList(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_offset,
                       idx_t target_offset, idx_t copy_count) {
	auto &target_validity = FlatVector::ValidityMutable(target);
	target_validity.CopySel(FlatVector::Validity(source), sel, source_offset, target_offset, copy_count);
	auto sdata = FlatVector::GetData<list_entry_t>(source);
	auto tdata = FlatVector::GetDataMutable<list_entry_t>(target);
	idx_t child_size = ListVector::GetListSize(target);
	idx_t total = 0;
	idx_t first_offset = 0;
	bool contiguous = true;
	for (idx_t i = 0; i < copy_count; i++) {
		auto target_idx = target_offset + i;
		if (!target_validity.RowIsValid(target_idx)) {
			tdata[target_idx].offset = 0;
			tdata[target_idx].length = 0;
			continue;
		}
		auto &source_entry = sdata[sel.get_index(source_offset + i)];
		if (source_entry.length != 0) {
			if (total == 0) {
				first_offset = source_entry.offset;
			} else if (source_entry.offset != first_offset + total) {
				contiguous = false;
			}
		}
		tdata[target_idx].offset = child_size;
		tdata[target_idx].length = source_entry.length;
		child_size += source_entry.length;
		total += source_entry.length;
	}
	if (total == 0) {
		return;
	}
	ListVector::Reserve(target, child_size);
	auto &source_child = ListVector::GetChild(source);
	auto &target_child = ListVector::GetChildMutable(target);
	if (contiguous) {
		CopyImmutable(source_child, target_child, *FlatVector::IncrementalSelectionVector(), first_offset + total,
		              first_offset, child_size - total);
	} else {
		SelectionVector child_sel(total);
		idx_t child_idx = 0;
		for (idx_t i = 0; i < copy_count; i++) {
			if (!target_validity.RowIsValid(target_offset + i)) {
				continue;
			}
			auto &source_entry = sdata[sel.get_index(source_offset + i)];
			for (idx_t j = 0; j < source_entry.length; j++) {
				child_sel.set_index(child_idx++, source_entry.offset + j);
			}
		}
		CopyImmutable(source_child, target_child, child_sel, total, 0, child_size - total);
	}
	ListVector::SetListSize(target, child_size);
}

void CopyImmutable(const Vector &source_p, Vector &target, const SelectionVector &sel_p, idx_t source_count,
                   idx_t source_offset, idx_t target_offset) {
	const idx_t copy_count = source_count - source_offset;
	if (copy_count == 0) {
		return;
	}
	const_reference<Vector> source_ref(source_p);
	const_reference<SelectionVector> sel_ref(sel_p);
	SelectionVector owned_sel;
	while (source_ref.get().GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		auto &dictionary = source_ref.get();
		auto &dict_sel = DictionaryVector::SelVector(dictionary);
		if (sel_ref.get().IsSet()) {
			auto &sel = sel_ref.get();
			auto composed = make_buffer<SelectionData>(copy_count);
			auto composed_data = reinterpret_cast<sel_t *>(composed->owned_data.get());
			for (idx_t i = 0; i < copy_count; i++) {
				composed_data[i] = UnsafeNumericCast<sel_t>(dict_sel.get_index(sel.get_index(source_offset + i)));
			}
			owned_sel.Initialize(std::move(composed));
			sel_ref = owned_sel;
			source_offset = 0;
			source_count = copy_count;
		} else {
			sel_ref = dict_sel;
		}
		source_ref = DictionaryVector::Child(dictionary);
	}
	auto &source = source_ref.get();
	auto &sel = sel_ref.get();
	if (source.GetVectorType() == VectorType::FLAT_VECTOR) {
		switch (target.GetType().InternalType()) {
		case PhysicalType::VARCHAR:
			if (CopyImmutableStrings(source, target, sel, source_offset, target_offset, copy_count)) {
				return;
			}
			break;
		case PhysicalType::LIST:
			CopyImmutableList(source, target, sel, source_offset, target_offset, copy_count);
			return;
		case PhysicalType::STRUCT: {
			FlatVector::ValidityMutable(target).CopySel(FlatVector::Validity(source), sel, source_offset, target_offset,
			                                            copy_count);
			auto &source_children = StructVector::GetEntries(source);
			auto &target_children = StructVector::GetEntries(target);
			for (idx_t i = 0; i < source_children.size(); i++) {
				CopyImmutable(source_children[i], target_children[i], sel, source_count, source_offset, target_offset);
			}
			return;
		}
		default:
			break;
		}
	}
	VectorOperations::Copy(source, target, sel, source_count, source_offset, target_offset);
}

} // namespace

bool ImmutableStrings::Certified(const AuxiliaryDataSet &set) {
	return std::any_of(set.data.begin(), set.data.end(), [](const unique_ptr<AuxiliaryDataHolder> &holder) {
		return holder->CertifiesImmutablePayloads();
	});
}

unique_ptr<AuxiliaryDataHolder> ImmutableStrings::Reference(buffer_ptr<AuxiliaryDataSet> set) {
	if (set && Certified(*set)) {
		return make_uniq<CertifiedAuxiliaryDataSetHolder>(std::move(set));
	}
	return make_uniq<AuxiliaryDataSetHolder>(std::move(set));
}

void ImmutableStrings::Copy(const Vector &source, Vector &target, idx_t source_count, idx_t source_offset,
                            idx_t target_offset) {
	CopyImmutable(source, target, *FlatVector::IncrementalSelectionVector(), source_count, source_offset,
	              target_offset);
}

void ImmutableStrings::Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
                            idx_t source_offset, idx_t target_offset) {
	CopyImmutable(source, target, sel, source_count, source_offset, target_offset);
}

void ImmutableStrings::Append(Vector &list, const Vector &source, idx_t count) {
	if (count == 0) {
		return;
	}
	const idx_t size = ListVector::GetListSize(list);
	ListVector::Reserve(list, size + count);
	CopyImmutable(source, ListVector::GetChildMutable(list), *FlatVector::IncrementalSelectionVector(), count, 0, size);
	ListVector::SetListSize(list, size + count);
}

} // namespace duckdb
