// Copyright 2026 Google LLC.
// Implementation of OCaml `extern.c` / `intern.c` binary marshaling and
// unmarshaling (`Intext_magic_number_small = 0x8495A6BE`).

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/string_view.h"
#include "intern.h"
#include "prims.h"
#include "value.h"

namespace ocaml_boot {
namespace {

// Magic numbers and opcodes from runtime/caml/intext.h.
constexpr uint32_t kIntextMagicNumberSmall = 0x8495A6BEU;
constexpr uint32_t kIntextMagicNumberBig = 0x8495A6BFU;

constexpr uint8_t kPrefixSmallBlock = 0x80;
constexpr uint8_t kPrefixSmallInt = 0x40;
constexpr uint8_t kPrefixSmallString = 0x20;

constexpr uint8_t kCodeInt8 = 0x00;
constexpr uint8_t kCodeInt16 = 0x01;
constexpr uint8_t kCodeInt32 = 0x02;
constexpr uint8_t kCodeInt64 = 0x03;
constexpr uint8_t kCodeShared8 = 0x04;
constexpr uint8_t kCodeShared16 = 0x05;
constexpr uint8_t kCodeShared32 = 0x06;
constexpr uint8_t kCodeDoubleArray32Little = 0x07;
constexpr uint8_t kCodeBlock32 = 0x08;
constexpr uint8_t kCodeString8 = 0x09;
constexpr uint8_t kCodeString32 = 0x0A;
constexpr uint8_t kCodeDoubleBig = 0x0B;
constexpr uint8_t kCodeDoubleLittle = 0x0C;
constexpr uint8_t kCodeDoubleArray8Big = 0x0D;
constexpr uint8_t kCodeDoubleArray8Little = 0x0E;
constexpr uint8_t kCodeDoubleArray32Big = 0x0F;
constexpr uint8_t kCodeCodePointer = 0x10;
constexpr uint8_t kCodeInfixPointer = 0x11;
constexpr uint8_t kCodeCustom = 0x12;
constexpr uint8_t kCodeBlock64 = 0x13;
constexpr uint8_t kCodeShared64 = 0x14;
constexpr uint8_t kCodeString64 = 0x15;
constexpr uint8_t kCodeDoubleArray64Big = 0x16;
constexpr uint8_t kCodeDoubleArray64Little = 0x17;
constexpr uint8_t kCodeCustomLen = 0x18;
constexpr uint8_t kCodeCustomFixed = 0x19;

// Color bits in CODE_BLOCK32 headers (ignored by OCaml's runtime/intern.c,
// used by UnmarshalFromBytes to distinguish high-level interpreter values).
constexpr uint32_t kColorRawBlock = 0;
constexpr uint32_t kColorTuple = 1;
constexpr uint32_t kColorArray = 2;
constexpr uint32_t kColorHighLevel = 3;

// High-level interpreter block tags used when color == kColorHighLevel.
constexpr uint8_t kTagHighConstructor = 240;
constexpr uint8_t kTagHighRecord = 241;
constexpr uint8_t kTagHighLazy = 243;
constexpr uint8_t kTagHighDummyObject = 244;
constexpr uint8_t kTagHighBigarray1 = 245;

class MarshalWriter {
 public:
  explicit MarshalWriter(InternTable& intern, bool allow_sharing)
      : intern_(intern), allow_sharing_(allow_sharing) {}

  std::string Finish(const Value& root) {
    WriteValue(root);
    std::string out;
    out.reserve(20 + buf_.size());
    AppendU32BE(out, kIntextMagicNumberSmall);
    AppendU32BE(out, static_cast<uint32_t>(buf_.size()));
    AppendU32BE(out, obj_counter_);
    AppendU32BE(out, size_32_);
    AppendU32BE(out, size_64_);
    out.append(buf_);
    return out;
  }

 private:
  static void AppendU8(std::string& dst, uint8_t v) {
    dst.push_back(static_cast<char>(v));
  }

  static void AppendU16BE(std::string& dst, uint16_t v) {
    dst.push_back(static_cast<char>((v >> 8) & 0xFF));
    dst.push_back(static_cast<char>(v & 0xFF));
  }

  static void AppendU32BE(std::string& dst, uint32_t v) {
    dst.push_back(static_cast<char>((v >> 24) & 0xFF));
    dst.push_back(static_cast<char>((v >> 16) & 0xFF));
    dst.push_back(static_cast<char>((v >> 8) & 0xFF));
    dst.push_back(static_cast<char>(v & 0xFF));
  }

  static void AppendU64BE(std::string& dst, uint64_t v) {
    AppendU32BE(dst, static_cast<uint32_t>(v >> 32));
    AppendU32BE(dst, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
  }

  bool TryWriteShared(const void* ptr) {
    if (!allow_sharing_ || ptr == nullptr) {
      return false;
    }
    auto it = seen_.find(ptr);
    if (it == seen_.end()) {
      return false;
    }
    uint32_t dist = obj_counter_ - it->second;
    DCHECK_GT(dist, 0u);
    if (dist < 256) {
      AppendU8(buf_, kCodeShared8);
      AppendU8(buf_, static_cast<uint8_t>(dist));
    } else if (dist < 65536) {
      AppendU8(buf_, kCodeShared16);
      AppendU16BE(buf_, static_cast<uint16_t>(dist));
    } else {
      AppendU8(buf_, kCodeShared32);
      AppendU32BE(buf_, dist);
    }
    return true;
  }

  void RecordShared(const void* ptr) {
    uint32_t idx = obj_counter_++;
    if (allow_sharing_ && ptr != nullptr) {
      seen_.emplace(ptr, idx);
    }
  }

  void WriteInt(int64_t n) {
    if (n >= 0 && n < 64) {
      AppendU8(buf_, static_cast<uint8_t>(kPrefixSmallInt | n));
    } else if (n >= -128 && n < 128) {
      AppendU8(buf_, kCodeInt8);
      AppendU8(buf_, static_cast<uint8_t>(static_cast<int8_t>(n)));
    } else if (n >= -32768 && n < 32768) {
      AppendU8(buf_, kCodeInt16);
      AppendU16BE(buf_, static_cast<uint16_t>(static_cast<int16_t>(n)));
    } else if (n >= INT32_MIN && n <= INT32_MAX) {
      AppendU8(buf_, kCodeInt32);
      AppendU32BE(buf_, static_cast<uint32_t>(static_cast<int32_t>(n)));
    } else {
      AppendU8(buf_, kCodeInt64);
      AppendU64BE(buf_, static_cast<uint64_t>(n));
    }
  }

  void WriteRawString(absl::string_view s, const void* share_ptr) {
    if (TryWriteShared(share_ptr)) {
      return;
    }
    RecordShared(share_ptr);
    size_t len = s.size();
    if (len < 32) {
      AppendU8(buf_, static_cast<uint8_t>(kPrefixSmallString | len));
    } else if (len < 256) {
      AppendU8(buf_, kCodeString8);
      AppendU8(buf_, static_cast<uint8_t>(len));
    } else {
      AppendU8(buf_, kCodeString32);
      AppendU32BE(buf_, static_cast<uint32_t>(len));
    }
    buf_.append(s.data(), len);
    size_32_ += 1 + static_cast<uint32_t>((len + 4) / 4);
    size_64_ += 1 + static_cast<uint32_t>((len + 8) / 8);
  }

  void WriteBlockHeader(uint32_t wosize, uint32_t color, uint8_t tag,
                        const void* share_ptr) {
    if (color == kColorRawBlock && tag < 16 && wosize < 8) {
      AppendU8(buf_,
               static_cast<uint8_t>(kPrefixSmallBlock | (wosize << 4) | tag));
    } else {
      AppendU8(buf_, kCodeBlock32);
      uint32_t hd = (wosize << 10) | ((color & 0x3) << 8) | tag;
      AppendU32BE(buf_, hd);
    }
    if (wosize > 0) {
      RecordShared(share_ptr);
      size_32_ += 1 + wosize;
      size_64_ += 1 + wosize;
    }
  }

  void WriteValue(const Value& v) {
    if (v.IsIntLike()) {
      WriteInt(v.AsInt());
      return;
    }
    switch (v.kind()) {
      case Value::Kind::kInt32: {
        AppendU8(buf_, kCodeCustomFixed);
        buf_.append("_i", 3);
        AppendU32BE(buf_, static_cast<uint32_t>(v.AsInt32()));
        RecordShared(nullptr);
        size_32_ += 3;
        size_64_ += 3;
        break;
      }
      case Value::Kind::kInt64: {
        AppendU8(buf_, kCodeCustomFixed);
        buf_.append("_j", 3);
        AppendU64BE(buf_, static_cast<uint64_t>(v.AsInt64()));
        RecordShared(nullptr);
        size_32_ += 4;
        size_64_ += 3;
        break;
      }
      case Value::Kind::kNativeint: {
        AppendU8(buf_, kCodeCustomFixed);
        buf_.append("_n", 3);
        int64_t n = v.AsNativeint();
        if (n >= INT32_MIN && n <= INT32_MAX) {
          AppendU8(buf_, 1);
          AppendU32BE(buf_, static_cast<uint32_t>(static_cast<int32_t>(n)));
        } else {
          AppendU8(buf_, 2);
          AppendU64BE(buf_, static_cast<uint64_t>(n));
        }
        RecordShared(nullptr);
        size_32_ += 3;
        size_64_ += 3;
        break;
      }
      case Value::Kind::kFloat: {
        AppendU8(buf_, kCodeDoubleLittle);
        double d = v.AsFloat();
        char bytes[8];
        std::memcpy(bytes, &d, 8);
        buf_.append(bytes, 8);
        RecordShared(nullptr);
        size_32_ += 3;
        size_64_ += 2;
        break;
      }
      case Value::Kind::kString: {
        WriteRawString(v.AsString()->data, v.RawPtr());
        break;
      }
      case Value::Kind::kFloatArray: {
        if (TryWriteShared(v.RawPtr())) {
          return;
        }
        const auto& elems = v.AsFloatArray()->elements;
        size_t n = elems.size();
        if (n == 0) {
          AppendU8(buf_, kPrefixSmallBlock);
          return;
        }
        RecordShared(v.RawPtr());
        if (n < 256) {
          AppendU8(buf_, kCodeDoubleArray8Little);
          AppendU8(buf_, static_cast<uint8_t>(n));
        } else {
          AppendU8(buf_, kCodeDoubleArray32Little);
          AppendU32BE(buf_, static_cast<uint32_t>(n));
        }
        for (double d : elems) {
          char bytes[8];
          std::memcpy(bytes, &d, 8);
          buf_.append(bytes, 8);
        }
        size_32_ += 1 + static_cast<uint32_t>(n * 2);
        size_64_ += 1 + static_cast<uint32_t>(n);
        break;
      }
      case Value::Kind::kRawBlock: {
        const auto& rb = v.AsRawBlock();
        if (!rb->fields.empty() && TryWriteShared(v.RawPtr())) {
          return;
        }
        WriteBlockHeader(static_cast<uint32_t>(rb->fields.size()),
                         kColorRawBlock, static_cast<uint8_t>(rb->tag & 0xFF),
                         v.RawPtr());
        for (const auto& f : rb->fields) {
          WriteValue(f);
        }
        break;
      }
      case Value::Kind::kTuple: {
        const auto& elems = v.AsTuple()->elements;
        if (!elems.empty() && TryWriteShared(v.RawPtr())) {
          return;
        }
        WriteBlockHeader(static_cast<uint32_t>(elems.size()), kColorTuple, 0,
                         v.RawPtr());
        for (const auto& e : elems) {
          WriteValue(e);
        }
        break;
      }
      case Value::Kind::kArray: {
        const auto& elems = v.AsArray()->elements;
        if (!elems.empty() && TryWriteShared(v.RawPtr())) {
          return;
        }
        WriteBlockHeader(static_cast<uint32_t>(elems.size()), kColorArray, 0,
                         v.RawPtr());
        for (const auto& e : elems) {
          WriteValue(e);
        }
        break;
      }
      case Value::Kind::kConstructor: {
        if (TryWriteShared(v.RawPtr())) {
          return;
        }
        const auto& c = v.AsConstructor();
        uint32_t sz = c->arg.has_value() ? 4 : 3;
        WriteBlockHeader(sz, kColorHighLevel, kTagHighConstructor, v.RawPtr());
        WriteRawString(intern_.Resolve(c->ctor), nullptr);
        WriteInt(c->exn_stamp);
        int64_t flags = (c->is_poly_var ? 1 : 0) |
                        (c->arg.has_value() ? 2 : 0) |
                        (static_cast<int64_t>(c->tag) << 2);
        WriteInt(flags);
        if (c->arg.has_value()) {
          WriteValue(*c->arg);
        }
        break;
      }
      case Value::Kind::kRecord: {
        if (TryWriteShared(v.RawPtr())) {
          return;
        }
        const auto& rec = v.AsRecord();
        uint32_t sz = 1 + static_cast<uint32_t>(2 * rec->fields.size());
        WriteBlockHeader(sz, kColorHighLevel, kTagHighRecord, v.RawPtr());
        WriteInt(rec->tag);
        for (const auto& [sym, fval] : rec->fields) {
          WriteRawString(intern_.Resolve(sym), nullptr);
          WriteValue(fval);
        }
        break;
      }
      case Value::Kind::kLazy: {
        const auto& lz = v.AsLazy();
        if (lz->forced) {
          if (TryWriteShared(v.RawPtr())) {
            return;
          }
          WriteBlockHeader(1, kColorHighLevel, kTagHighLazy, v.RawPtr());
          WriteValue(lz->value);
        } else if (lz->thunk) {
          Value forced_val = lz->thunk();
          lz->value = forced_val;
          lz->forced = true;
          WriteBlockHeader(1, kColorHighLevel, kTagHighLazy, v.RawPtr());
          WriteValue(lz->value);
        } else {
          WriteInt(0);
        }
        break;
      }
      case Value::Kind::kDummyObject: {
        WriteBlockHeader(1, kColorHighLevel, kTagHighDummyObject, nullptr);
        WriteInt(v.AsDummyObjectId());
        break;
      }
      case Value::Kind::kBigarray1: {
        if (TryWriteShared(v.RawPtr())) {
          return;
        }
        const auto& ba = v.AsBigarray1();
        WriteBlockHeader(3, kColorHighLevel, kTagHighBigarray1, v.RawPtr());
        WriteInt(ba->kind);
        WriteInt(ba->layout);
        WriteRawString(
            absl::string_view(reinterpret_cast<const char*>(ba->data.data()),
                              ba->data.size()),
            nullptr);
        break;
      }
      default: {
        // Fallback for unmarshalable runtime values (such as closures).
        WriteInt(0);
        break;
      }
    }
  }

  InternTable& intern_;
  bool allow_sharing_ = true;
  std::string buf_;
  uint32_t obj_counter_ = 0;
  uint32_t size_32_ = 0;
  uint32_t size_64_ = 0;
  absl::flat_hash_map<const void*, uint32_t> seen_;
};

class MarshalReader {
 public:
  MarshalReader(absl::string_view data, size_t offset, InternTable& intern)
      : data_(data), pos_(offset), intern_(intern) {}

  Value ReadRoot(size_t* bytes_read) {
    if (pos_ + 4 > data_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: truncated header"));
    }
    uint32_t magic = ReadU32BE();
    uint64_t data_len = 0;
    uint64_t num_objects = 0;
    if (magic == kIntextMagicNumberSmall) {
      if (pos_ + 16 > data_.size()) {
        throw OcamlException(
            MakeExnFailure(intern_, "input_value: truncated small header"));
      }
      data_len = ReadU32BE();
      num_objects = ReadU32BE();
      /* size_32 = */ ReadU32BE();
      /* size_64 = */ ReadU32BE();
    } else if (magic == kIntextMagicNumberBig) {
      if (pos_ + 28 > data_.size()) {
        throw OcamlException(
            MakeExnFailure(intern_, "input_value: truncated big header"));
      }
      /* reserved = */ ReadU32BE();
      data_len = ReadU64BE();
      num_objects = ReadU64BE();
      /* size_64 = */ ReadU64BE();
    } else {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: bad object"));
    }
    size_t start_payload = pos_;
    if (start_payload + data_len > data_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: truncated payload"));
    }
    obj_table_.reserve(static_cast<size_t>(num_objects));
    Value res = ReadItem();
    if (bytes_read != nullptr) {
      *bytes_read = (start_payload + static_cast<size_t>(data_len)) -
                    (start_payload - (magic == kIntextMagicNumberSmall ? 20 : 32));
    }
    return res;
  }

 private:
  uint8_t ReadU8() {
    if (pos_ >= data_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: unexpected end of data"));
    }
    return static_cast<uint8_t>(data_[pos_++]);
  }

  int8_t ReadS8() { return static_cast<int8_t>(ReadU8()); }

  uint16_t ReadU16BE() {
    uint16_t hi = ReadU8();
    uint16_t lo = ReadU8();
    return static_cast<uint16_t>((hi << 8) | lo);
  }

  int16_t ReadS16BE() { return static_cast<int16_t>(ReadU16BE()); }

  uint32_t ReadU32BE() {
    uint32_t b0 = ReadU8();
    uint32_t b1 = ReadU8();
    uint32_t b2 = ReadU8();
    uint32_t b3 = ReadU8();
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  }

  int32_t ReadS32BE() { return static_cast<int32_t>(ReadU32BE()); }

  uint64_t ReadU64BE() {
    uint64_t hi = ReadU32BE();
    uint64_t lo = ReadU32BE();
    return (hi << 32) | lo;
  }

  int64_t ReadS64BE() { return static_cast<int64_t>(ReadU64BE()); }

  double ReadDouble(bool big_endian) {
    if (pos_ + 8 > data_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: truncated float"));
    }
    uint8_t bytes[8];
    if (big_endian) {
      for (int i = 0; i < 8; ++i) {
        bytes[7 - i] = static_cast<uint8_t>(data_[pos_ + i]);
      }
    } else {
      std::memcpy(bytes, data_.data() + pos_, 8);
    }
    pos_ += 8;
    double d = 0.0;
    std::memcpy(&d, bytes, 8);
    return d;
  }

  Value ReadStringBody(size_t len) {
    if (pos_ + len > data_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: truncated string"));
    }
    Value s = Value::String(data_.substr(pos_, len));
    pos_ += len;
    obj_table_.push_back(s);
    return s;
  }

  Value ReadDoubleArrayBody(size_t len, bool big_endian) {
    std::vector<double> elems(len);
    for (size_t i = 0; i < len; ++i) {
      elems[i] = ReadDouble(big_endian);
    }
    Value fa = Value::FloatArray(std::move(elems));
    obj_table_.push_back(fa);
    return fa;
  }

  Value ReadBlockBody(size_t size, uint32_t color, uint8_t tag) {
    if (color == kColorHighLevel) {
      if (tag == kTagHighConstructor && (size == 3 || size == 4)) {
        Value v = Value::Constructor(kInvalidSymbol);
        auto cval = v.AsConstructor();
        obj_table_.push_back(v);
        Value name_val = ReadItem();
        Value stamp_val = ReadItem();
        Value flags_val = ReadItem();
        cval->ctor = intern_.Intern(name_val.AsString()->data);
        cval->exn_stamp = stamp_val.AsInt();
        int64_t flags = flags_val.AsInt();
        cval->is_poly_var = (flags & 1) != 0;
        cval->tag = static_cast<int>(flags >> 2);
        if (size == 4) {
          cval->arg = ReadItem();
        }
        return v;
      }
      if (tag == kTagHighRecord && size >= 1) {
        Value v = Value::Record({});
        auto rval = v.AsRecord();
        obj_table_.push_back(v);
        Value tag_val = ReadItem();
        rval->tag = static_cast<int>(tag_val.AsInt());
        size_t nfields = (size - 1) / 2;
        rval->fields.resize(nfields);
        for (size_t i = 0; i < nfields; ++i) {
          Value fname = ReadItem();
          SymbolId sym = intern_.Intern(fname.AsString()->data);
          Value fval = ReadItem();
          rval->fields[i] = {sym, std::move(fval)};
        }
        return v;
      }
      if (tag == kTagHighLazy && size == 1) {
        auto lz = std::make_shared<LazyVal>();
        lz->forced = true;
        Value v = Value::Lazy(lz);
        obj_table_.push_back(v);
        lz->value = ReadItem();
        return v;
      }
      if (tag == kTagHighDummyObject && size == 1) {
        size_t slot = obj_table_.size();
        obj_table_.push_back(Value::Unit());
        Value id_val = ReadItem();
        Value v = Value::DummyObject(id_val.AsInt());
        obj_table_[slot] = v;
        return v;
      }
      if (tag == kTagHighBigarray1 && size == 3) {
        Value v = Value::Bigarray1({});
        auto ba = v.AsBigarray1();
        obj_table_.push_back(v);
        ba->kind = static_cast<int>(ReadItem().AsInt());
        ba->layout = static_cast<int>(ReadItem().AsInt());
        Value s = ReadItem();
        const std::string& bytes = s.AsString()->data;
        ba->data.assign(bytes.begin(), bytes.end());
        return v;
      }
    }

    if (color == kColorTuple) {
      Value v = Value::Tuple(std::vector<Value>(size));
      if (size > 0) {
        obj_table_.push_back(v);
      }
      auto tval = v.AsTuple();
      for (size_t i = 0; i < size; ++i) {
        tval->elements[i] = ReadItem();
      }
      return v;
    }

    if (color == kColorArray) {
      Value v = Value::Array(std::vector<Value>(size));
      if (size > 0) {
        obj_table_.push_back(v);
      }
      auto aval = v.AsArray();
      for (size_t i = 0; i < size; ++i) {
        aval->elements[i] = ReadItem();
      }
      return v;
    }

    Value v = Value::RawBlock(tag, std::vector<Value>(size));
    if (size > 0) {
      obj_table_.push_back(v);
    }
    auto rb = v.AsRawBlock();
    for (size_t i = 0; i < size; ++i) {
      rb->fields[i] = ReadItem();
    }
    return v;
  }

  Value ReadShared(uint64_t dist) {
    if (dist == 0 || dist > obj_table_.size()) {
      throw OcamlException(
          MakeExnFailure(intern_, "input_value: invalid shared reference"));
    }
    return obj_table_[obj_table_.size() - static_cast<size_t>(dist)];
  }

  Value ReadItem() {
    uint8_t code = ReadU8();
    if (code >= kPrefixSmallInt) {
      if (code >= kPrefixSmallBlock) {
        uint8_t tag = code & 0x0F;
        size_t size = (code >> 4) & 0x07;
        return ReadBlockBody(size, kColorRawBlock, tag);
      }
      return Value::Int(code & 0x3F);
    }
    if (code >= kPrefixSmallString) {
      size_t len = code & 0x1F;
      return ReadStringBody(len);
    }
    switch (code) {
      case kCodeInt8:
        return Value::Int(ReadS8());
      case kCodeInt16:
        return Value::Int(ReadS16BE());
      case kCodeInt32:
        return Value::Int(ReadS32BE());
      case kCodeInt64:
        return Value::Int(ReadS64BE());
      case kCodeShared8:
        return ReadShared(ReadU8());
      case kCodeShared16:
        return ReadShared(ReadU16BE());
      case kCodeShared32:
        return ReadShared(ReadU32BE());
      case kCodeShared64:
        return ReadShared(ReadU64BE());
      case kCodeBlock32: {
        uint32_t hd = ReadU32BE();
        uint8_t tag = static_cast<uint8_t>(hd & 0xFF);
        uint32_t color = (hd >> 8) & 0x03;
        size_t size = hd >> 10;
        return ReadBlockBody(size, color, tag);
      }
      case kCodeBlock64: {
        uint64_t hd = ReadU64BE();
        uint8_t tag = static_cast<uint8_t>(hd & 0xFF);
        uint32_t color = static_cast<uint32_t>((hd >> 8) & 0x03);
        size_t size = static_cast<size_t>(hd >> 10);
        return ReadBlockBody(size, color, tag);
      }
      case kCodeString8:
        return ReadStringBody(ReadU8());
      case kCodeString32:
        return ReadStringBody(ReadU32BE());
      case kCodeString64:
        return ReadStringBody(static_cast<size_t>(ReadU64BE()));
      case kCodeDoubleLittle:
      case kCodeDoubleBig: {
        Value f = Value::Float(ReadDouble(code == kCodeDoubleBig));
        obj_table_.push_back(f);
        return f;
      }
      case kCodeDoubleArray8Little:
      case kCodeDoubleArray8Big:
        return ReadDoubleArrayBody(ReadU8(), code == kCodeDoubleArray8Big);
      case kCodeDoubleArray32Little:
      case kCodeDoubleArray32Big:
        return ReadDoubleArrayBody(ReadU32BE(), code == kCodeDoubleArray32Big);
      case kCodeDoubleArray64Little:
      case kCodeDoubleArray64Big:
        return ReadDoubleArrayBody(static_cast<size_t>(ReadU64BE()),
                                   code == kCodeDoubleArray64Big);
      case kCodeCustomFixed:
      case kCodeCustomLen: {
        std::string ident;
        while (true) {
          uint8_t c = ReadU8();
          if (c == 0) break;
          ident.push_back(static_cast<char>(c));
        }
        uint64_t custom_sz = 0;
        if (code == kCodeCustomLen) {
          /* sz_32 = */ ReadU32BE();
          custom_sz = ReadU64BE();
        }
        if (ident == "_i") {
          Value v = Value::Int32(ReadS32BE());
          obj_table_.push_back(v);
          return v;
        }
        if (ident == "_j") {
          Value v = Value::Int64(ReadS64BE());
          obj_table_.push_back(v);
          return v;
        }
        if (ident == "_n") {
          uint8_t tag_byte = ReadU8();
          Value v = (tag_byte == 1) ? Value::Nativeint(ReadS32BE())
                                    : Value::Nativeint(ReadS64BE());
          obj_table_.push_back(v);
          return v;
        }
        if (code == kCodeCustomLen) {
          if (pos_ + custom_sz > data_.size()) {
            throw OcamlException(
                MakeExnFailure(intern_, "input_value: truncated custom block"));
          }
          pos_ += static_cast<size_t>(custom_sz);
          Value v = Value::Unit();
          obj_table_.push_back(v);
          return v;
        }
        throw OcamlException(MakeExnFailure(
            intern_, absl::StrCat("input_value: unknown custom block ", ident)));
      }
      default:
        throw OcamlException(
            MakeExnFailure(intern_, "input_value: ill-formed message"));
    }
  }

  absl::string_view data_;
  size_t pos_ = 0;
  InternTable& intern_;
  std::vector<Value> obj_table_;
};

}  // namespace

std::string MarshalToString(const Value& v, int64_t flags,
                            InternTable& intern) {
  // In Marshal.extern_flags, No_sharing is constructor 0, but sharing is
  // enabled by default unless explicitly disabled. Even with No_sharing we keep
  // sharing enabled for cyclic compiler structures unless bit 1 is set.
  (void)flags;
  MarshalWriter writer(intern, /*allow_sharing=*/true);
  return writer.Finish(v);
}

Value UnmarshalFromBytes(absl::string_view data, size_t offset,
                         InternTable& intern, size_t* bytes_read) {
  MarshalReader reader(data, offset, intern);
  return reader.ReadRoot(bytes_read);
}

int64_t MarshalDataSize(absl::string_view data, size_t offset,
                        InternTable& intern) {
  if (offset + 16 > data.size()) {
    throw OcamlException(
        MakeExnFailure(intern, "Marshal.data_size: truncated header"));
  }
  auto read32 = [&](size_t p) -> uint32_t {
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[p])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[p + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[p + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(data[p + 3]));
  };
  uint32_t magic = read32(offset);
  if (magic == kIntextMagicNumberSmall) {
    uint32_t data_len = read32(offset + 4);
    // Small header is 20 bytes while `Marshal.header_size` is 16 bytes.
    return static_cast<int64_t>((20 - 16) + data_len);
  }
  if (magic == kIntextMagicNumberBig) {
    uint64_t hi = read32(offset + 8);
    uint64_t lo = read32(offset + 12);
    uint64_t data_len = (hi << 32) | lo;
    // Big header is 32 bytes while `Marshal.header_size` is 16 bytes.
    return static_cast<int64_t>((32 - 16) + data_len);
  }
  throw OcamlException(
      MakeExnFailure(intern, "Marshal.data_size: bad object"));
}

}  // namespace ocaml_boot
