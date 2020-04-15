//
// Created by Kadayam, Hari on 31/01/18.
//

#ifndef OMSTORE_BACKING_BTREE_HPP
#define OMSTORE_BACKING_BTREE_HPP

#include "physical_node.hpp"
#include <boost/intrusive_ptr.hpp>
#include <queue>

namespace homeds {
namespace btree {

#define btree_store_t BtreeStore< BtreeStoreType, K, V, InteriorNodeType, LeafNodeType >

template < btree_store_type BtreeStoreType, typename K, typename V, btree_node_type InteriorNodeType,
           btree_node_type LeafNodeType >
class BtreeNode;

#define btree_node_t BtreeNode< BtreeStoreType, K, V, InteriorNodeType, LeafNodeType >
#define BtreeNodePtr boost::intrusive_ptr< btree_node_t >

template < btree_store_type BtreeStoreType, typename K, typename V, btree_node_type InteriorNodeType,
           btree_node_type LeafNodeType >
class BtreeStore {

    struct superblock;

public:
    using HeaderType = homeds::btree::EmptyClass;

    static std::unique_ptr< btree_store_t > init_btree(BtreeConfig& cfg);
    static uint8_t* get_physical(const btree_node_t* bn);
    static uint32_t get_node_area_size(btree_store_t* store);
    static void update_store_sb(btree_store_t* store, btree_store_t::superblock& sb);
    static btree_cp_id_ptr attach_prepare_cp(btree_store_t* store, btree_cp_id_ptr cur_cp_id);
    static void cp_start(btree_store_t* store, btree_cp_id_ptr cp_id, cp_comp_callback cb);
    static void truncate(btree_store_t* store, btree_cp_id_ptr cp_id);
    static void destroy_done(btree_store_t* store);
    static void write_journal_entry(btree_cp_id_ptr cp_id, uint8_t* mem, size_t size);

    static BtreeNodePtr alloc_node(btree_store_t* store, bool is_leaf,
                                   bool& is_new_allocation, // indicates if allocated node is same as copy_from
                                   BtreeNodePtr copy_from = nullptr);
    static BtreeNodePtr read_node(btree_store_t* store, bnodeid_t id);

    static btree_status_t write_node(btree_store_t* store, BtreeNodePtr bn, BtreeNodePtr dependent_bn,
                                     btree_cp_id_ptr cp_id);
    static void free_node(btree_store_t* store, BtreeNodePtr bn, bool mem_only, btree_cp_id_ptr cp_id);
    static btree_status_t refresh_node(btree_store_t* store, BtreeNodePtr bn, bool is_write_modifiable,
                                       btree_cp_id_ptr cp_id);

    static void swap_node(btree_store_t* store, BtreeNodePtr node1, BtreeNodePtr node2);
    static void copy_node(btree_store_t* store, BtreeNodePtr copy_from, BtreeNodePtr copy_to);
    static void ref_node(btree_node_t* bn);
    static bool deref_node(btree_node_t* bn);
    static btree_status_t write_node_sync(btree_store_t* store, BtreeNodePtr bn);
    static void cp_done(trigger_cp_callback cb);
};
} // namespace btree
} // namespace homeds
#endif // OMSTORE_BACKING_BTREE_HPP