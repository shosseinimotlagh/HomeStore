/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <homestore/blk.hpp>
#include <bit>
#include "common/homestore_assert.hpp"

namespace homestore {
blk_id::blk_id(uint64_t id_int) {
    s = std::bit_cast< serialized >(id_int);
    DEBUG_ASSERT_EQ(is_multi(), 0, "MultiBlkId is set on BlkId constructor");
}

blk_id::blk_id(blk_num_t blk_num, blk_count_t nblks, chunk_num_t chunk_num) : s{0x0, blk_num, nblks, chunk_num} {}

uint64_t blk_id::to_integer() const { return std::bit_cast< uint64_t >(s); }

sisl::blob blk_id::serialize() const { return sisl::blob{r_cast< uint8_t const* >(&s), sizeof(serialized)}; }

uint32_t blk_id::serialized_size() const { return sizeof(blk_id); }
uint32_t blk_id::expected_serialized_size() { return sizeof(blk_id); }

void blk_id::deserialize(sisl::blob const& b, bool copy) {
    serialized const* other = r_cast< serialized const* >(b.cbytes());
    s = *other;
}

void blk_id::invalidate() { s.m_nblks = 0; }

bool blk_id::is_valid() const { return (blk_count() > 0); }

std::string blk_id::to_string() const {
    return is_valid() ? fmt::format("blk#={} count={} chunk={}", blk_num(), blk_count(), chunk_num()) : "Invalid_Blkid";
}

int blk_id::compare(const blk_id& one, const blk_id& two) {
    if (one.chunk_num() < two.chunk_num()) {
        return -1;
    } else if (one.chunk_num() > two.chunk_num()) {
        return 1;
    }

    if (one.blk_num() < two.blk_num()) {
        return -1;
    } else if (one.blk_num() > two.blk_num()) {
        return 1;
    }

    if (one.blk_count() < two.blk_count()) {
        return -1;
    } else if (one.blk_count() > two.blk_count()) {
        return 1;
    }

    return 0;
}

//////////////////////////////////// multi_blk_id Section //////////////////////////////
multi_blk_id::multi_blk_id() : blk_id::blk_id() { s.m_is_multi = 1; }

multi_blk_id::multi_blk_id(blk_id const& b) : blk_id::blk_id(b) { s.m_is_multi = 1; }

multi_blk_id::multi_blk_id(blk_num_t blk_num, blk_count_t nblks, chunk_num_t chunk_num) :
        blk_id::blk_id{blk_num, nblks, chunk_num} {
    s.m_is_multi = 1;
}

void multi_blk_id::add(blk_num_t blk_num, blk_count_t nblks, chunk_num_t chunk_num) {
    if (blk_id::is_valid()) {
        RELEASE_ASSERT_EQ(s.m_chunk_num, chunk_num, "multi_blk_id has to be all from same chunk");
        RELEASE_ASSERT_LT(n_addln_piece, max_addln_pieces, "multi_blk_id cannot support more than {} pieces",
                          max_addln_pieces + 1);
        addln_pieces[n_addln_piece] = chain_blkid{.m_blk_num = blk_num, .m_nblks = nblks};
        ++n_addln_piece;
    } else {
        s = blk_id::serialized{0x1, blk_num, nblks, chunk_num};
    }
}

void multi_blk_id::add(blk_id const& b) { add(b.blk_num(), b.blk_count(), b.chunk_num()); }

sisl::blob multi_blk_id::serialize() const { return sisl::blob{r_cast< uint8_t const* >(this), serialized_size()}; }

uint32_t multi_blk_id::serialized_size() const {
    uint32_t sz = blk_id::serialized_size();
    if (n_addln_piece != 0) { sz += sizeof(uint16_t) + (n_addln_piece * sizeof(chain_blkid)); }
    return sz;
}

void multi_blk_id::deserialize(sisl::blob const& b, bool copy) {
    multi_blk_id const* other = r_cast< multi_blk_id const* >(b.cbytes());
    s = other->s;
    if (b.size() == sizeof(blk_id)) {
        n_addln_piece = 0;
    } else {
        n_addln_piece = other->n_addln_piece;
        std::copy(other->addln_pieces.begin(), other->addln_pieces.begin() + other->n_addln_piece,
                  addln_pieces.begin());
    }
}

uint32_t multi_blk_id::expected_serialized_size(uint16_t num_pieces) {
    uint32_t sz = blk_id::expected_serialized_size();
    if (num_pieces > 1) { sz += sizeof(uint16_t) + ((num_pieces - 1) * sizeof(chain_blkid)); }
    return sz;
}

uint32_t multi_blk_id::max_serialized_size() { return expected_serialized_size(max_pieces); }

uint16_t multi_blk_id::num_pieces() const { return blk_id::is_valid() ? n_addln_piece + 1 : 0; }

bool multi_blk_id::has_room() const { return (n_addln_piece < max_addln_pieces); }

multi_blk_id::iterator multi_blk_id::iterate() const { return multi_blk_id::iterator{*this}; }

std::string multi_blk_id::to_string() const {
    std::string str = "[";
    auto it = iterate();
    while (auto const b = it.next()) {
        str += "{" + (b->to_string() + "},");
    }
    str += std::string("]");
    return str;
}

blk_count_t multi_blk_id::blk_count() const {
    blk_count_t nblks{0};
    auto it = iterate();
    while (auto b = it.next()) {
        nblks += b->blk_count();
    }
    return nblks;
}

blk_id multi_blk_id::to_single_blkid() const {
    HS_DBG_ASSERT_LE(num_pieces(), 1, "Can only MultiBlkId with one piece to BlkId");
    return blk_id{blk_num(), blk_count(), chunk_num()};
}

int multi_blk_id::compare(multi_blk_id const& left, multi_blk_id const& right) {
    if (left.chunk_num() < right.chunk_num()) {
        return -1;
    } else if (left.chunk_num() > right.chunk_num()) {
        return 1;
    }

    // Shortcut path for simple blk_id search to avoid building icl set
    if ((left.num_pieces() == 1) && (right.num_pieces() == 1)) {
        return blk_id::compare(d_cast< blk_id const& >(left), d_cast< blk_id const& >(right));
    }

    using IntervalSet = boost::icl::interval_set< uint64_t >;
    using Interval = IntervalSet::interval_type;

    IntervalSet lset;
    auto lit = left.iterate();
    while (auto b = lit.next()) {
        lset.insert(Interval::right_open(b->blk_num(), b->blk_num() + b->blk_count()));
    }

    IntervalSet rset;
    auto rit = right.iterate();
    while (auto b = rit.next()) {
        rset.insert(Interval::right_open(b->blk_num(), b->blk_num() + b->blk_count()));
    }

    if (lset < rset) {
        return -1;
    } else if (lset > rset) {
        return 1;
    } else {
        return 0;
    }
}
} // namespace homestore
