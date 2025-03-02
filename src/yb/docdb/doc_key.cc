// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/doc_key.h"

#include <memory>
#include <sstream>

#include "yb/util/string_util.h"

#include "yb/common/partition.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/doc_path.h"
#include "yb/docdb/value_type.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/util/enums.h"
#include "yb/util/compare_util.h"

using std::ostringstream;

using strings::Substitute;

using yb::util::CompareVectors;
using yb::util::CompareUsingLessThan;

namespace yb {
namespace docdb {

namespace {

// Checks whether slice starts with primitive value.
// Valid cases are end of group or primitive value starting with value type.
Result<bool> HasPrimitiveValue(Slice* slice, AllowSpecial allow_special) {
  if (PREDICT_FALSE(slice->empty())) {
    return STATUS(Corruption, "Unexpected end of key when decoding document key");
  }
  ValueType current_value_type = static_cast<ValueType>(*slice->data());
  if (current_value_type == ValueType::kGroupEnd) {
    slice->consume_byte();
    return false;
  }

  if (IsPrimitiveValueType(current_value_type)) {
    return true;
  }

  if (allow_special && IsSpecialValueType(current_value_type)) {
    return true;
  }

  return STATUS_FORMAT(Corruption, "Expected a primitive value type, got $0", current_value_type);
}

// Consumes all primitive values from key until group end is found.
// Callback is called for each value and responsible for consuming this single value from slice.
template<class Callback>
Status ConsumePrimitiveValuesFromKey(Slice* slice, AllowSpecial allow_special, Callback callback) {
  const auto initial_slice(*slice);  // For error reporting.
  while (true) {
    if (!VERIFY_RESULT(HasPrimitiveValue(slice, allow_special))) {
      return Status::OK();
    }

    RETURN_NOT_OK_PREPEND(callback(),
        Substitute("while consuming primitive values from $0",
                   initial_slice.ToDebugHexString()));
  }
}

Status ConsumePrimitiveValuesFromKey(Slice* slice, AllowSpecial allow_special,
                                     boost::container::small_vector_base<Slice>* result) {
  return ConsumePrimitiveValuesFromKey(slice, allow_special, [slice, result]() -> Status {
    auto begin = slice->data();
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, /* out */ nullptr));
    if (result) {
      result->emplace_back(begin, slice->data());
    }
    return Status::OK();
  });
}

Status ConsumePrimitiveValuesFromKey(
    Slice* slice, AllowSpecial allow_special, std::vector<PrimitiveValue>* result) {
  return ConsumePrimitiveValuesFromKey(slice, allow_special, [slice, result] {
    result->emplace_back();
    return result->back().DecodeFromKey(slice);
  });
}

} // namespace

Result<bool> ConsumePrimitiveValueFromKey(Slice* slice) {
  if (!VERIFY_RESULT(HasPrimitiveValue(slice, AllowSpecial::kFalse))) {
    return false;
  }
  RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, nullptr /* out */));
  return true;
}

Status ConsumePrimitiveValuesFromKey(Slice* slice, std::vector<PrimitiveValue>* result) {
  return ConsumePrimitiveValuesFromKey(slice, AllowSpecial::kFalse, result);
}

// ------------------------------------------------------------------------------------------------
// DocKey
// ------------------------------------------------------------------------------------------------

DocKey::DocKey() : cotable_id_(boost::uuids::nil_uuid()), hash_present_(false) {
}

DocKey::DocKey(std::vector<PrimitiveValue> range_components)
    : cotable_id_(boost::uuids::nil_uuid()),
      hash_present_(false),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(boost::uuids::nil_uuid()),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Uuid& cotable_id,
               DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(cotable_id),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Uuid& cotable_id)
    : cotable_id_(cotable_id),
      hash_present_(false),
      hash_(0) {
}

DocKey::DocKey(const Schema& schema)
    : cotable_id_(schema.cotable_id()),
      hash_present_(false) {
}

DocKey::DocKey(const Schema& schema, DocKeyHash hash)
    : cotable_id_(schema.cotable_id()),
      hash_present_(true),
      hash_(hash) {
}

DocKey::DocKey(const Schema& schema, std::vector<PrimitiveValue> range_components)
    : cotable_id_(schema.cotable_id()),
      hash_present_(false),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Schema& schema, DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(schema.cotable_id()),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

KeyBytes DocKey::Encode() const {
  KeyBytes result;
  AppendTo(&result);
  return result;
}

namespace {

// Used as cache of allocated memory by EncodeAsRefCntPrefix.
thread_local boost::optional<KeyBytes> thread_local_encode_buffer;

}

RefCntPrefix DocKey::EncodeAsRefCntPrefix() const {
  KeyBytes* encode_buffer = thread_local_encode_buffer.get_ptr();
  if (!encode_buffer) {
    thread_local_encode_buffer.emplace();
    encode_buffer = thread_local_encode_buffer.get_ptr();
  }
  encode_buffer->Clear();
  AppendTo(encode_buffer);
  return RefCntPrefix(encode_buffer->AsStringRef());
}

void DocKey::AppendTo(KeyBytes* out) const {
  DocKeyEncoder(out).CotableId(cotable_id_).Hash(hash_present_, hash_, hashed_group_).
      Range(range_group_);
}

void DocKey::Clear() {
  hash_present_ = false;
  hash_ = 0xdead;
  hashed_group_.clear();
  range_group_.clear();
}

void DocKey::ClearRangeComponents() {
  range_group_.clear();
}

void DocKey::ResizeRangeComponents(int new_size) {
  range_group_.resize(new_size);
}

namespace {

class DecodeDocKeyCallback {
 public:
  explicit DecodeDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return out_;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

 private:
  boost::container::small_vector_base<Slice>* out_;
};

class DummyCallback {
 public:
  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return nullptr;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }
};

class EncodedSizesCallback {
 public:
  explicit EncodedSizesCallback(DocKeyDecoder* decoder) : decoder_(decoder) {}

  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    range_group_start_ = decoder_->left_input().data();
    return nullptr;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }

  const uint8_t* range_group_start() {
    return range_group_start_;
  }

 private:
  DocKeyDecoder* decoder_;
  mutable const uint8_t* range_group_start_ = nullptr;
};

} // namespace

yb::Status DocKey::PartiallyDecode(Slice *slice,
                                   boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  DocKeyDecoder decoder(*slice);
  RETURN_NOT_OK(DoDecode(
      &decoder, DocKeyPart::WHOLE_DOC_KEY, AllowSpecial::kFalse, DecodeDocKeyCallback(out)));
  *slice = decoder.left_input();
  return Status::OK();
}

Result<DocKeyHash> DocKey::DecodeHash(const Slice& slice) {
  DocKeyDecoder decoder(slice);
  RETURN_NOT_OK(decoder.DecodeCotableId());
  uint16_t hash;
  RETURN_NOT_OK(decoder.DecodeHashCode(&hash));
  return hash;
}

Result<size_t> DocKey::EncodedSize(Slice slice, DocKeyPart part, AllowSpecial allow_special) {
  auto initial_begin = slice.cdata();
  DocKeyDecoder decoder(slice);
  RETURN_NOT_OK(DoDecode(&decoder, part, allow_special, DummyCallback()));
  return decoder.left_input().cdata() - initial_begin;
}

Result<std::pair<size_t, size_t>> DocKey::EncodedHashPartAndDocKeySizes(
    Slice slice,
    AllowSpecial allow_special) {
  auto initial_begin = slice.data();
  DocKeyDecoder decoder(slice);
  EncodedSizesCallback callback(&decoder);
  RETURN_NOT_OK(DoDecode(
      &decoder, DocKeyPart::WHOLE_DOC_KEY, allow_special, callback));
  return std::make_pair(callback.range_group_start() - initial_begin,
                        decoder.left_input().data() - initial_begin);
}

class DocKey::DecodeFromCallback {
 public:
  explicit DecodeFromCallback(DocKey* key) : key_(key) {
  }

  std::vector<PrimitiveValue>* hashed_group() const {
    return &key_->hashed_group_;
  }

  std::vector<PrimitiveValue>* range_group() const {
    return &key_->range_group_;
  }

  void SetHash(bool present, DocKeyHash hash = 0) const {
    key_->hash_present_ = present;
    if (present) {
      key_->hash_ = hash;
    }
  }
  void SetCoTableId(const Uuid cotable_id) const {
    key_->cotable_id_ = cotable_id;
  }

 private:
  DocKey* key_;
};

Status DocKey::DecodeFrom(Slice *slice, DocKeyPart part_to_decode, AllowSpecial allow_special) {
  Clear();
  DocKeyDecoder decoder(*slice);
  RETURN_NOT_OK(DoDecode(&decoder, part_to_decode, allow_special, DecodeFromCallback(this)));
  *slice = decoder.left_input();
  return Status::OK();
}

Result<size_t> DocKey::DecodeFrom(
    const Slice& slice, DocKeyPart part_to_decode, AllowSpecial allow_special) {
  Slice copy = slice;
  RETURN_NOT_OK(DecodeFrom(&copy, part_to_decode, allow_special));
  return slice.size() - copy.size();
}

template<class Callback>
yb::Status DocKey::DoDecode(DocKeyDecoder* decoder,
                            DocKeyPart part_to_decode,
                            AllowSpecial allow_special,
                            const Callback& callback) {
  Uuid cotable_id;
  if (VERIFY_RESULT(decoder->DecodeCotableId(&cotable_id))) {
    callback.SetCoTableId(cotable_id);
  }

  uint16_t hash_code;
  if (VERIFY_RESULT(decoder->DecodeHashCode(&hash_code, allow_special))) {
    callback.SetHash(/* present */ true, hash_code);
    RETURN_NOT_OK_PREPEND(
        ConsumePrimitiveValuesFromKey(
            decoder->mutable_input(), allow_special, callback.hashed_group()),
        "Error when decoding hashed components of a document key");
  } else {
    callback.SetHash(/* present */ false);
  }

  switch (part_to_decode) {
    case DocKeyPart::WHOLE_DOC_KEY:
      if (!decoder->left_input().empty()) {
        RETURN_NOT_OK_PREPEND(
            ConsumePrimitiveValuesFromKey(
                decoder->mutable_input(), allow_special, callback.range_group()),
            "Error when decoding range components of a document key");
      }
      return Status::OK();
    case DocKeyPart::HASHED_PART_ONLY:
      return Status::OK();
  }
  FATAL_INVALID_ENUM_VALUE(DocKeyPart, part_to_decode);
}

yb::Status DocKey::FullyDecodeFrom(const rocksdb::Slice& slice) {
  rocksdb::Slice mutable_slice = slice;
  Status status = DecodeFrom(&mutable_slice);
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into DocKey, found $0 extra bytes",
        mutable_slice.size());
  }
  return status;
}

string DocKey::ToString() const {
  string result = "DocKey(";
  if (!cotable_id_.IsNil()) {
    result += "CoTableId=";
    result += cotable_id_.ToString();
    result += ", ";
  }
  if (hash_present_) {
    result += StringPrintf("0x%04x", hash_);
    result += ", ";
  }

  result += rocksdb::VectorToString(hashed_group_);
  result += ", ";
  result += rocksdb::VectorToString(range_group_);
  result.push_back(')');
  return result;
}

bool DocKey::operator ==(const DocKey& other) const {
  return cotable_id_ == other.cotable_id_ &&
         HashedComponentsEqual(other) &&
         range_group_ == other.range_group_;
}

bool DocKey::HashedComponentsEqual(const DocKey& other) const {
  return hash_present_ == other.hash_present_ &&
      // Only compare hashes and hashed groups if the hash presence flag is set.
      (!hash_present_ || (hash_ == other.hash_ && hashed_group_ == other.hashed_group_));
}

void DocKey::AddRangeComponent(const PrimitiveValue& val) {
  range_group_.push_back(val);
}

void DocKey::SetRangeComponent(const PrimitiveValue& val, int idx) {
  DCHECK_LT(idx, range_group_.size());
  range_group_[idx] = val;
}

int DocKey::CompareTo(const DocKey& other) const {
  int result = CompareUsingLessThan(cotable_id_, other.cotable_id_);
  if (result != 0) return result;

  result = CompareUsingLessThan(hash_present_, other.hash_present_);
  if (result != 0) return result;

  if (hash_present_) {
    result = CompareUsingLessThan(hash_, other.hash_);
    if (result != 0) return result;
  }

  result = CompareVectors(hashed_group_, other.hashed_group_);
  if (result != 0) return result;

  return CompareVectors(range_group_, other.range_group_);
}

DocKey DocKey::FromRedisKey(uint16_t hash, const string &key) {
  DocKey new_doc_key;
  new_doc_key.hash_present_ = true;
  new_doc_key.hash_ = hash;
  new_doc_key.hashed_group_.emplace_back(key);
  return new_doc_key;
}

KeyBytes DocKey::EncodedFromRedisKey(uint16_t hash, const std::string &key) {
  KeyBytes result;
  result.AppendValueType(ValueType::kUInt16Hash);
  result.AppendUInt16(hash);
  result.AppendValueType(ValueType::kString);
  result.AppendString(key);
  result.AppendValueType(ValueType::kGroupEnd);
  result.AppendValueType(ValueType::kGroupEnd);
  DCHECK_EQ(result.data(), FromRedisKey(hash, key).Encode().data());
  return result;
}

std::string DocKey::DebugSliceToString(Slice slice) {
  DocKey key;
  auto decoded_size = key.DecodeFrom(slice, DocKeyPart::WHOLE_DOC_KEY, AllowSpecial::kTrue);
  if (!decoded_size.ok()) {
    return decoded_size.status().ToString() + ": " + slice.ToDebugHexString();
  }
  slice.remove_prefix(*decoded_size);
  auto result = key.ToString();
  if (!slice.empty()) {
    result += " + ";
    result += slice.ToDebugHexString();
  }
  return result;
}

// ------------------------------------------------------------------------------------------------
// SubDocKey
// ------------------------------------------------------------------------------------------------

KeyBytes SubDocKey::DoEncode(bool include_hybrid_time) const {
  KeyBytes key_bytes = doc_key_.Encode();
  for (const auto& subkey : subkeys_) {
    subkey.AppendToKey(&key_bytes);
  }
  if (has_hybrid_time() && include_hybrid_time) {
    AppendDocHybridTime(doc_ht_, &key_bytes);
  }
  return key_bytes;
}

namespace {

class DecodeSubDocKeyCallback {
 public:
  explicit DecodeSubDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return DocKey::PartiallyDecode(slice, out_);
  }

  // We don't need subkeys in partial decoding.
  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }

  DocHybridTime& doc_hybrid_time() const {
    return doc_hybrid_time_;
  }

  void DocHybridTimeSlice(Slice slice) const {
    out_->push_back(slice);
  }
 private:
  boost::container::small_vector_base<Slice>* out_;
  mutable DocHybridTime doc_hybrid_time_;
};

} // namespace

Status SubDocKey::PartiallyDecode(Slice* slice, boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  return DoDecode(slice, HybridTimeRequired::kTrue, DecodeSubDocKeyCallback(out));
}

class SubDocKey::DecodeCallback {
 public:
  explicit DecodeCallback(SubDocKey* key) : key_(key) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return key_->doc_key_.DecodeFrom(slice);
  }

  PrimitiveValue* AddSubkey() const {
    key_->subkeys_.emplace_back();
    return &key_->subkeys_.back();
  }

  DocHybridTime& doc_hybrid_time() const {
    return key_->doc_ht_;
  }

  void DocHybridTimeSlice(Slice slice) const {
  }
 private:
  SubDocKey* key_;
};

Status SubDocKey::DecodeFrom(Slice* slice, HybridTimeRequired require_hybrid_time) {
  Clear();
  return DoDecode(slice, require_hybrid_time, DecodeCallback(this));
}

Result<bool> SubDocKey::DecodeSubkey(Slice* slice) {
  return DecodeSubkey(slice, DummyCallback());
}

template<class Callback>
Result<bool> SubDocKey::DecodeSubkey(Slice* slice, const Callback& callback) {
  if (!slice->empty() && *slice->data() != ValueTypeAsChar::kHybridTime) {
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, callback.AddSubkey()));
    return true;
  }
  return false;
}

template<class Callback>
Status SubDocKey::DoDecode(rocksdb::Slice* slice,
                           const HybridTimeRequired require_hybrid_time,
                           const Callback& callback) {
  const rocksdb::Slice original_bytes(*slice);

  RETURN_NOT_OK(callback.DecodeDocKey(slice));
  for (;;) {
    auto decode_result = DecodeSubkey(slice, callback);
    RETURN_NOT_OK_PREPEND(
        decode_result,
        Substitute("While decoding SubDocKey $0", ToShortDebugStr(original_bytes)));
    if (!decode_result.get()) {
      break;
    }
  }
  if (slice->empty()) {
    if (!require_hybrid_time) {
      callback.doc_hybrid_time() = DocHybridTime::kInvalid;
      return Status::OK();
    }
    return STATUS_SUBSTITUTE(
        Corruption,
        "Found too few bytes in the end of a SubDocKey for a type-prefixed hybrid_time: $0",
        ToShortDebugStr(*slice));
  }

  // The reason the following is not handled as a Status is that the logic above (loop + emptiness
  // check) should guarantee this is the only possible case left.
  DCHECK_EQ(ValueType::kHybridTime, DecodeValueType(*slice));
  slice->consume_byte();

  auto begin = slice->data();
  RETURN_NOT_OK(ConsumeHybridTimeFromKey(slice, &callback.doc_hybrid_time()));
  callback.DocHybridTimeSlice(Slice(begin, slice->data()));

  return Status::OK();
}

Status SubDocKey::FullyDecodeFrom(const rocksdb::Slice& slice,
                                  HybridTimeRequired require_hybrid_time) {
  rocksdb::Slice mutable_slice = slice;
  Status status = DecodeFrom(&mutable_slice, require_hybrid_time);
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into SubDocKey, found $0 extra bytes: $1",
        mutable_slice.size(), mutable_slice.ToDebugHexString());
  }
  return status;
}

Status SubDocKey::DecodePrefixLengths(
    Slice slice, boost::container::small_vector_base<size_t>* out) {
  auto begin = slice.data();
  auto hashed_part_size = VERIFY_RESULT(DocKey::EncodedSize(
      slice, DocKeyPart::HASHED_PART_ONLY));
  if (hashed_part_size != 0) {
    slice.remove_prefix(hashed_part_size);
    out->push_back(hashed_part_size);
  }
  while (VERIFY_RESULT(ConsumePrimitiveValueFromKey(&slice))) {
    out->push_back(slice.data() - begin);
  }
  if (!out->empty()) {
    if (begin[out->back()] != ValueTypeAsChar::kGroupEnd) {
      return STATUS_FORMAT(Corruption, "Range keys group end expected at $0 in $1",
                           out->back(), Slice(begin, slice.end()).ToDebugHexString());
    }
    ++out->back(); // Add range key group end to last prefix
  }
  while (VERIFY_RESULT(SubDocKey::DecodeSubkey(&slice))) {
    out->push_back(slice.data() - begin);
  }

  return Status::OK();
}

Status SubDocKey::DecodeDocKeyAndSubKeyEnds(
    Slice slice, boost::container::small_vector_base<size_t>* out) {
  auto begin = slice.data();
  if (out->empty()) {
    auto doc_key_size = VERIFY_RESULT(DocKey::EncodedSize(
        slice, DocKeyPart::WHOLE_DOC_KEY));
    slice.remove_prefix(doc_key_size);
    out->push_back(doc_key_size);
  } else {
    slice.remove_prefix(out->back());
  }
  while (VERIFY_RESULT(SubDocKey::DecodeSubkey(&slice))) {
    out->push_back(slice.data() - begin);
  }

  return Status::OK();
}

std::string SubDocKey::DebugSliceToString(Slice slice) {
  SubDocKey key;
  auto status = key.FullyDecodeFrom(slice, HybridTimeRequired::kFalse);
  if (!status.ok()) {
    return status.ToString();
  }
  return key.ToString();
}

string SubDocKey::ToString() const {
  std::stringstream result;
  result << "SubDocKey(" << doc_key_.ToString() << ", [";

  bool need_comma = false;
  for (const auto& subkey : subkeys_) {
    if (need_comma) {
      result << ", ";
    }
    need_comma = true;
    result << subkey.ToString();
  }

  if (has_hybrid_time()) {
    if (need_comma) {
      result << "; ";
    }
    result << doc_ht_.ToString();
  }
  result << "])";
  return result.str();
}

Status SubDocKey::FromDocPath(const DocPath& doc_path) {
  RETURN_NOT_OK(doc_key_.FullyDecodeFrom(doc_path.encoded_doc_key().AsSlice()));
  subkeys_ = doc_path.subkeys();
  return Status::OK();
}

void SubDocKey::Clear() {
  doc_key_.Clear();
  subkeys_.clear();
  doc_ht_ = DocHybridTime::kInvalid;
}

bool SubDocKey::StartsWith(const SubDocKey& prefix) const {
  return doc_key_ == prefix.doc_key_ &&
         // Subkeys precede the hybrid_time field in the encoded representation, so the hybrid_time
         // either has to be undefined in the prefix, or the entire key must match, including
         // subkeys and the hybrid_time (in this case the prefix is the same as this key).
         (!prefix.has_hybrid_time() ||
          (doc_ht_ == prefix.doc_ht_ && prefix.num_subkeys() == num_subkeys())) &&
         prefix.num_subkeys() <= num_subkeys() &&
         // std::mismatch finds the first difference between two sequences. Prior to C++14, the
         // behavior is undefined if the second range is shorter than the first range, so we make
         // sure the potentially shorter range is first.
         std::mismatch(
             prefix.subkeys_.begin(), prefix.subkeys_.end(), subkeys_.begin()
         ).first == prefix.subkeys_.end();
}

bool SubDocKey::operator==(const SubDocKey& other) const {
  if (doc_key_ != other.doc_key_ ||
      subkeys_ != other.subkeys_)
    return false;

  const bool ht_is_valid = doc_ht_.is_valid();
  const bool other_ht_is_valid = other.doc_ht_.is_valid();
  if (ht_is_valid != other_ht_is_valid)
    return false;
  if (ht_is_valid) {
    return doc_ht_ == other.doc_ht_;
  } else {
    // Both keys don't have a hybrid time.
    return true;
  }
}

int SubDocKey::CompareTo(const SubDocKey& other) const {
  int result = CompareToIgnoreHt(other);
  if (result != 0) return result;

  const bool ht_is_valid = doc_ht_.is_valid();
  const bool other_ht_is_valid = other.doc_ht_.is_valid();
  if (ht_is_valid) {
    if (other_ht_is_valid) {
      // HybridTimes are sorted in reverse order.
      return -doc_ht_.CompareTo(other.doc_ht_);
    } else {
      // This key has a hybrid time and the other one is identical but lacks the hybrid time, so
      // this one is greater.
      return 1;
    }
  } else {
    if (other_ht_is_valid) {
      // This key is a "prefix" of the other key, which has a hybrid time, so this one is less.
      return -1;
    } else {
      // Neither key has a hybrid time.
      return 0;
    }
  }

}

int SubDocKey::CompareToIgnoreHt(const SubDocKey& other) const {
  int result = doc_key_.CompareTo(other.doc_key_);
  if (result != 0) return result;

  result = CompareVectors(subkeys_, other.subkeys_);
  return result;
}

string BestEffortDocDBKeyToStr(const KeyBytes &key_bytes) {
  rocksdb::Slice mutable_slice(key_bytes.AsSlice());
  SubDocKey subdoc_key;
  Status decode_status = subdoc_key.DecodeFrom(&mutable_slice, HybridTimeRequired::kFalse);
  if (decode_status.ok()) {
    ostringstream ss;
    if (!subdoc_key.has_hybrid_time() && subdoc_key.num_subkeys() == 0) {
      // This is really just a DocKey.
      ss << subdoc_key.doc_key().ToString();
    } else {
      ss << subdoc_key.ToString();
    }
    if (mutable_slice.size() > 0) {
      ss << " followed by raw bytes " << FormatRocksDBSliceAsStr(mutable_slice);
      // Can append the above status of why we could not decode a SubDocKey, if needed.
    }
    return ss.str();
  }

  // We could not decode a SubDocKey at all, even without a hybrid_time.
  return key_bytes.ToString();
}

std::string BestEffortDocDBKeyToStr(const rocksdb::Slice& slice) {
  return BestEffortDocDBKeyToStr(KeyBytes(slice));
}

KeyBytes SubDocKey::AdvanceOutOfSubDoc() const {
  KeyBytes subdoc_key_no_ts = EncodeWithoutHt();
  subdoc_key_no_ts.AppendValueType(ValueType::kMaxByte);
  return subdoc_key_no_ts;
}

KeyBytes SubDocKey::AdvanceOutOfDocKeyPrefix() const {
  // To construct key bytes that will seek past this DocKey and DocKeys that have the same hash
  // components but add more range components to it, we will strip the group-end of the range
  // components and append 0xff, which will be lexicographically higher than any key bytes
  // with the same hash and range component prefix. For example,
  //
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd"])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00!
  // Result:  H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00\xff
  // This key will also skip all DocKeys that have additional range components, e.g.
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd", "ee"])
  // (encoded as H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00$ee\x00\00!). That should
  // make no difference to DocRowwiseIterator in a valid database, because all keys actually stored
  // in DocDB will have exactly the same number of range components.
  //
  // Now, suppose there are no range components in the key passed to us (note: that does not
  // necessarily mean there are no range components in the schema, just the doc key being passed to
  // us is a custom-constructed DocKey with no range components because the caller wants a key
  // that will skip pass all doc keys with the same hash components prefix). Example:
  //
  // DocKey(0x1234, ["aa", "bb"], [])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!!
  // Result: H\0x12\0x34$aa\x00\x00$bb\x00\x00!\xff
  KeyBytes doc_key_encoded = doc_key_.Encode();
  doc_key_encoded.RemoveValueTypeSuffix(ValueType::kGroupEnd);
  doc_key_encoded.AppendValueType(ValueType::kMaxByte);
  return doc_key_encoded;
}

// ------------------------------------------------------------------------------------------------
// DocDbAwareFilterPolicy
// ------------------------------------------------------------------------------------------------

namespace {

class HashedComponentsExtractor : public rocksdb::FilterPolicy::KeyTransformer {
 public:
  HashedComponentsExtractor() {}
  HashedComponentsExtractor(const HashedComponentsExtractor&) = delete;
  HashedComponentsExtractor& operator=(const HashedComponentsExtractor&) = delete;

  static HashedComponentsExtractor& GetInstance() {
    static HashedComponentsExtractor instance;
    return instance;
  }

  Slice Transform(Slice key) const override {
    auto size = CHECK_RESULT(DocKey::EncodedSize(key, DocKeyPart::HASHED_PART_ONLY));
    return Slice(key.data(), size);
  }
};

} // namespace


void DocDbAwareFilterPolicy::CreateFilter(
    const rocksdb::Slice* keys, int n, std::string* dst) const {
  CHECK_GT(n, 0);
  return builtin_policy_->CreateFilter(keys, n, dst);
}

bool DocDbAwareFilterPolicy::KeyMayMatch(
    const rocksdb::Slice& key, const rocksdb::Slice& filter) const {
  return builtin_policy_->KeyMayMatch(key, filter);
}

rocksdb::FilterBitsBuilder* DocDbAwareFilterPolicy::GetFilterBitsBuilder() const {
  return builtin_policy_->GetFilterBitsBuilder();
}

rocksdb::FilterBitsReader* DocDbAwareFilterPolicy::GetFilterBitsReader(
    const rocksdb::Slice& contents) const {
  return builtin_policy_->GetFilterBitsReader(contents);
}

rocksdb::FilterPolicy::FilterType DocDbAwareFilterPolicy::GetFilterType() const {
  return builtin_policy_->GetFilterType();
}

const rocksdb::FilterPolicy::KeyTransformer* DocDbAwareFilterPolicy::GetKeyTransformer() const {
  return &HashedComponentsExtractor::GetInstance();
}

DocKeyEncoderAfterCotableIdStep DocKeyEncoder::CotableId(const Uuid& cotable_id) {
  if (!cotable_id.IsNil()) {
    std::string bytes;
    cotable_id.EncodeToComparable(&bytes);
    out_->AppendValueType(ValueType::kTableId);
    out_->AppendRawBytes(bytes);
  }
  return DocKeyEncoderAfterCotableIdStep(out_);
}

Result<bool> DocKeyDecoder::DecodeCotableId(Uuid* uuid) {
  if (input_.empty() || input_[0] != ValueTypeAsChar::kTableId) {
    return false;
  }

  input_.consume_byte();

  if (input_.size() < kUuidSize) {
    return STATUS_FORMAT(
        Corruption, "Not enough bytes for cotable id: $0", input_.ToDebugHexString());
  }

  if (uuid) {
    RETURN_NOT_OK(uuid->DecodeFromComparableSlice(Slice(input_.data(), kUuidSize)));
  }
  input_.remove_prefix(kUuidSize);

  return true;
}

Result<bool> DocKeyDecoder::DecodeHashCode(uint16_t* out, AllowSpecial allow_special) {
  if (input_.empty()) {
    return false;
  }

  auto first_value_type = static_cast<ValueType>(input_[0]);

  auto good_value_type = allow_special ? IsPrimitiveOrSpecialValueType(first_value_type)
                                       : IsPrimitiveValueType(first_value_type);
  if (first_value_type == ValueType::kGroupEnd) {
    return false;
  }

  if (!good_value_type) {
    return STATUS_FORMAT(Corruption,
        "Expected first value type to be primitive or GroupEnd, got $0 in $1",
        first_value_type, input_.ToDebugHexString());
  }

  if (input_.empty() || input_[0] != ValueTypeAsChar::kUInt16Hash) {
    return false;
  }

  if (input_.size() < sizeof(DocKeyHash) + 1) {
    return STATUS_FORMAT(
        Corruption,
        "Could not decode a 16-bit hash component of a document key: only $0 bytes left",
        input_.size());
  }

  // We'll need to update this code if we ever change the size of the hash field.
  static_assert(sizeof(DocKeyHash) == sizeof(uint16_t),
      "It looks like the DocKeyHash's size has changed -- need to update encoder/decoder.");
  if (out) {
    *out = BigEndian::Load16(input_.data() + 1);
  }
  input_.remove_prefix(sizeof(DocKeyHash) + 1);
  return true;
}

Status DocKeyDecoder::DecodePrimitiveValue(PrimitiveValue* out, AllowSpecial allow_special) {
  if (allow_special && !input_.empty()) {
    if (input_[0] == ValueTypeAsChar::kLowest && input_[0] == ValueTypeAsChar::kHighest) {
      input_.consume_byte();
      return Status::OK();
    }
  }
  return PrimitiveValue::DecodeKey(&input_, out);
}

Status DocKeyDecoder::ConsumeGroupEnd() {
  if (input_.empty() || input_[0] != ValueTypeAsChar::kGroupEnd) {
    return STATUS_FORMAT(Corruption, "Group end expected but $0 found", input_.ToDebugHexString());
  }
  input_.consume_byte();
  return Status::OK();
}

bool DocKeyDecoder::GroupEnded() const {
  return input_.empty() || input_[0] == ValueTypeAsChar::kGroupEnd;
}

Result<bool> DocKeyDecoder::HasPrimitiveValue() {
  return docdb::HasPrimitiveValue(&input_, AllowSpecial::kFalse);
}

Status DocKeyDecoder::DecodeToRangeGroup() {
  RETURN_NOT_OK(DecodeCotableId());
  if (VERIFY_RESULT(DecodeHashCode())) {
    while (VERIFY_RESULT(HasPrimitiveValue())) {
      RETURN_NOT_OK(DecodePrimitiveValue());
    }
  }

  return Status::OK();
}

Result<bool> ClearRangeComponents(KeyBytes* out, AllowSpecial allow_special) {
  auto prefix_size = VERIFY_RESULT(
      DocKey::EncodedSize(out->AsSlice(), DocKeyPart::HASHED_PART_ONLY, allow_special));
  auto& str = *out->mutable_data();
  if (str.size() == prefix_size + 1 && str[prefix_size] == ValueTypeAsChar::kGroupEnd) {
    return false;
  }
  if (str.size() > prefix_size) {
    str[prefix_size] = ValueTypeAsChar::kGroupEnd;
    str.resize(prefix_size + 1);
  } else {
    str.push_back(ValueTypeAsChar::kGroupEnd);
  }
  return true;
}

Result<bool> HashedComponentsEqual(const Slice& lhs, const Slice& rhs) {
  DocKeyDecoder lhs_decoder(lhs);
  DocKeyDecoder rhs_decoder(rhs);
  RETURN_NOT_OK(lhs_decoder.DecodeCotableId());
  RETURN_NOT_OK(rhs_decoder.DecodeCotableId());

  bool hash_present = VERIFY_RESULT(lhs_decoder.DecodeHashCode(AllowSpecial::kTrue));
  RETURN_NOT_OK(rhs_decoder.DecodeHashCode(AllowSpecial::kTrue));

  size_t consumed = lhs_decoder.ConsumedSizeFrom(lhs.data());
  if (consumed != rhs_decoder.ConsumedSizeFrom(rhs.data()) ||
      !strings::memeq(lhs.data(), rhs.data(), consumed)) {
    return false;
  }
  if (!hash_present) {
    return true;
  }

  while (!lhs_decoder.GroupEnded()) {
    auto lhs_start = lhs_decoder.left_input().data();
    auto rhs_start = rhs_decoder.left_input().data();
    auto value_type = lhs_start[0];
    if (rhs_decoder.GroupEnded() || rhs_start[0] != value_type) {
      return false;
    }

    if (PREDICT_FALSE(!IsPrimitiveOrSpecialValueType(static_cast<ValueType>(value_type)))) {
      return false;
    }

    RETURN_NOT_OK(lhs_decoder.DecodePrimitiveValue(AllowSpecial::kTrue));
    RETURN_NOT_OK(rhs_decoder.DecodePrimitiveValue(AllowSpecial::kTrue));
    consumed = lhs_decoder.ConsumedSizeFrom(lhs_start);
    if (consumed != rhs_decoder.ConsumedSizeFrom(rhs_start) ||
        !strings::memeq(lhs_start, rhs_start, consumed)) {
      return false;
    }
  }

  return rhs_decoder.GroupEnded();
}

bool DocKeyBelongsTo(Slice doc_key, const Schema& schema) {
  bool has_table_id = !doc_key.empty() && doc_key[0] == ValueTypeAsChar::kTableId;

  if (schema.cotable_id().IsNil()) {
    return !has_table_id;
  }

  if (!has_table_id) {
    return false;
  }

  doc_key.consume_byte();

  std::string bytes;
  schema.cotable_id().EncodeToComparable(&bytes);
  return doc_key.starts_with(bytes);
}

const KeyBounds KeyBounds::kNoBounds;

}  // namespace docdb
}  // namespace yb
