// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/short_key_index.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "common/status.h"
#include "gen_cpp/segment.pb.h"
#include "util/debug_util.h"
#include "util/faststring.h"
#include "util/slice.h"

namespace starrocks {

// In our system, we have more complicated situation.
// First, our keys can be NULL.
// Second, when key columns are not complete we want to distinguish GT and GE. For examle,
// there are two key columns a and b, we have only one condition a > 1. We can only encode
// a prefix key 1, which is less than 1|2. This will make our read more data than
// we actually need. So we want to add more marker.
// a > 1: will be encoded into 1|\xFF
// a >= 1: will be encoded into 1|\x00
// a = 1 and b > 1: will be encoded into 1|\x02|1
// a = 1 and b is null: will be encoded into 1|\x01

// Used to represent minimal value for that field
constexpr uint8_t KEY_MINIMAL_MARKER = 0x00;
// Used to represent a null field, which value is seemed as minimal than other values
constexpr uint8_t KEY_NULL_FIRST_MARKER = 0x01;
// Used to represent a normal field, which content is encoded after this marker
constexpr uint8_t KEY_NORMAL_MARKER = 0x02;
// Used to represent
constexpr uint8_t KEY_NULL_LAST_MARKER = 0xFE;
// Used to represent maximal value for that field
constexpr uint8_t KEY_MAXIMAL_MARKER = 0xFF;

// Encode a segment short key indices to one ShortKeyPage. This version
// only accepts binary key, client should assure that input key is sorted,
// otherwise error could happens. This builder would arrange the page body in the
// following format:
//      ShortKeyPageBody := KeyContent^NumEntry, KeyOffset(vint)^NumEntry
//      NumEntry, KeyBytes, OffsetBytes is stored in ShortKeyFooterPB
// Usage:
//      ShortKeyIndexBuilder builder(segment_id, num_rows_per_block);
//      builder.add_item(key1);
//      ...
//      builder.add_item(keyN);
//      builder.finalize(segment_size, num_rows, &slices);
// TODO(zc):
// 1. If this can leverage binary page to save key and offset data
// 2. Extending this to save in a BTree like struct, which can index full key
//    more than short key
class ShortKeyIndexBuilder {
public:
    ShortKeyIndexBuilder(uint32_t segment_id, uint32_t num_rows_per_block)
            : _segment_id(segment_id), _num_rows_per_block(num_rows_per_block) {}

    Status add_item(const Slice& key);

    uint64_t size() { return _key_buf.size() + _offset_buf.size(); }

    Status finalize(uint32_t num_rows, std::vector<Slice>* body, PageFooterPB* footer);

private:
    uint32_t _segment_id;
    uint32_t _num_rows_per_block;
    uint32_t _num_items{0};

    faststring _key_buf;
    faststring _offset_buf;
};

class ShortKeyIndexDecoder;

// An Iterator to iterate one short key index.
// Client can use this class to iterator all items in this index.
class ShortKeyIndexIterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = Slice;
    using pointer = Slice*;
    using reference = Slice&;
    using difference_type = ssize_t;

    ShortKeyIndexIterator(const ShortKeyIndexDecoder* decoder, uint32_t ordinal = 0)
            : _decoder(decoder), _ordinal(ordinal) {}

    ShortKeyIndexIterator& operator-=(ssize_t step) {
        _ordinal -= step;
        return *this;
    }

    ShortKeyIndexIterator& operator+=(ssize_t step) {
        _ordinal += step;
        return *this;
    }

    ShortKeyIndexIterator& operator++() {
        _ordinal++;
        return *this;
    }

    ShortKeyIndexIterator& operator--() {
        --_ordinal;
        return *this;
    }

    bool operator!=(const ShortKeyIndexIterator& other) {
        return _ordinal != other._ordinal || _decoder != other._decoder;
    }

    bool operator==(const ShortKeyIndexIterator& other) {
        return _ordinal == other._ordinal && _decoder == other._decoder;
    }

    ssize_t operator-(const ShortKeyIndexIterator& other) const { return _ordinal - other._ordinal; }

    inline bool valid() const;

    Slice operator*() const;

    ssize_t ordinal() const { return _ordinal; }

private:
    const ShortKeyIndexDecoder* _decoder;
    ssize_t _ordinal;
};

// Used to decode short key to header and encoded index data.
// Usage:
//      ShortKeyIndexDecoder decoder;
//      decoder.parse(body, footer);
//      auto iter = decoder.lower_bound(key);
class ShortKeyIndexDecoder {
public:
    ShortKeyIndexDecoder() = default;

    // client should assure that body is available when this class is used
    Status parse(const Slice& body, const ShortKeyFooterPB& footer);

    ShortKeyIndexIterator begin() const {
        DCHECK(_parsed);
        return {this, 0};
    }

    ShortKeyIndexIterator end() const {
        DCHECK(_parsed);
        return {this, num_items()};
    }

    // Return an iterator which locates at the first item who is
    // equal with or greater than the given key.
    // NOTE: If one key is the prefix of other key, this funciton thinks
    // that longer key is greater than the shorter key.
    ShortKeyIndexIterator lower_bound(const Slice& key) const {
        DCHECK(_parsed);
        return seek<true>(key);
    }

    // Return the iterator which locates the first item greater than the
    // input key.
    ShortKeyIndexIterator upper_bound(const Slice& key) const {
        DCHECK(_parsed);
        return seek<false>(key);
    }

    uint32_t num_items() const {
        DCHECK(_parsed);
        return _footer.num_items();
    }

    uint32_t num_rows_per_block() const {
        DCHECK(_parsed);
        return _footer.num_rows_per_block();
    }

    Slice key(ssize_t ordinal) const {
        DCHECK(_parsed);
        DCHECK(ordinal >= 0 && ordinal < num_items());
        return {_key_data.data + _offsets[ordinal], _offsets[ordinal + 1] - _offsets[ordinal]};
    }

    int64_t mem_usage() const {
        return sizeof(ShortKeyIndexDecoder) + sizeof(uint32_t) * _offsets.size() + _key_data.size +
               _footer.ByteSizeLong() - sizeof(_footer);
    }

private:
    template <bool lower_bound>
    ShortKeyIndexIterator seek(const Slice& key) const {
        auto comparator = [](const Slice& lhs, const Slice& rhs) { return lhs.compare(rhs) < 0; };
        if (lower_bound) {
            return std::lower_bound(begin(), end(), key, comparator);
        } else {
            return std::upper_bound(begin(), end(), key, comparator);
        }
    }

    bool _parsed{false};

    // All following fields are only valid after parse has been executed successfully
    ShortKeyFooterPB _footer;
    std::vector<uint32_t> _offsets;
    Slice _key_data;
};

inline Slice ShortKeyIndexIterator::operator*() const {
    return _decoder->key(_ordinal);
}

inline bool ShortKeyIndexIterator::valid() const {
    return _ordinal >= 0 && _ordinal < _decoder->num_items();
}

} // namespace starrocks
