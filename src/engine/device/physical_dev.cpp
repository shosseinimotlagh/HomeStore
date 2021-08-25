/*
 * PhysicalDev.cpp
 *
 *  Created on: 05-Aug-2016
 *      Author: hkadayam
 */

#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <system_error>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <boost/utility.hpp>
#include <folly/Exception.h>
#include <iomgr/iomgr.hpp>
#include <utility/thread_factory.hpp>

#include "engine/common/homestore_assert.hpp"
#include "engine/common/homestore_flip.hpp"

#include "device.h"

SDS_LOGGING_DECL(device)

#define drive_iface iomgr::IOManager::instance().default_drive_interface()

namespace homestore {

static std::atomic< uint64_t > glob_phys_dev_offset{0};
static std::atomic< uint32_t > glob_phys_dev_ids{0};

PhysicalDev::~PhysicalDev() {
    LOGINFO("device name {} superblock magic {} product name {} version {}", m_devname, m_super_blk->get_magic(),
            m_super_blk->get_product_name(), m_super_blk->get_version());
    hs_utils::iobuf_free(reinterpret_cast<uint8_t*>(m_super_blk), sisl::buftag::superblk);
}

void PhysicalDev::update(const uint32_t dev_num, const uint64_t dev_offset, const uint32_t first_chunk_id) {
    HS_DEBUG_ASSERT_EQ(m_info_blk.get_dev_num(), INVALID_DEV_ID);
    HS_DEBUG_ASSERT_EQ(m_info_blk.get_first_chunk_id(), INVALID_CHUNK_ID);

    m_info_blk.dev_num = dev_num;
    m_info_blk.dev_offset = dev_offset;
    m_info_blk.first_chunk_id = first_chunk_id;
}

void PhysicalDev::attach_superblock_chunk(PhysicalDevChunk* const chunk) {
    if (!m_superblock_valid) {
        HS_ASSERT_NULL(DEBUG, m_dm_chunk[m_cur_indx]);
        HS_DEBUG_ASSERT_LT(m_cur_indx, 2);
        m_dm_chunk[m_cur_indx++] = chunk;
        return;
    }
    if (chunk->get_chunk_id() == m_super_blk->dm_chunk[0].chunk_id) {
        HS_ASSERT_NULL(DEBUG, m_dm_chunk[0]);
        m_dm_chunk[0] = chunk;
    } else {
        HS_DEBUG_ASSERT_EQ(chunk->get_chunk_id(), m_super_blk->dm_chunk[1].get_chunk_id());
        HS_ASSERT_NULL(DEBUG, m_dm_chunk[1]);
        m_dm_chunk[1] = chunk;
    }
}

PhysicalDev::PhysicalDev(DeviceManager* const mgr, const std::string& devname, const int oflags,
                         const iomgr::iomgr_drive_type drive_type) :
        m_mgr{mgr}, m_devname{devname}, m_metrics{devname} {

    HS_LOG_ASSERT_LE(sizeof(super_block), SUPERBLOCK_SIZE, "Device {} Ondisk Superblock size not enough to hold in-mem",
                     devname);
    auto* const membuf{hs_utils::iobuf_alloc(SUPERBLOCK_SIZE, sisl::buftag::superblk)};
    m_super_blk = new (membuf) super_block{};
    if (sizeof(super_block) < SUPERBLOCK_SIZE) {
        std::memset(membuf + sizeof(super_block), 0, SUPERBLOCK_SIZE - sizeof(super_block));
    }

    m_iodev = drive_iface->open_dev(devname.c_str(), drive_type, oflags);
    read_superblock();
}

PhysicalDev::PhysicalDev(DeviceManager* const mgr, const std::string& devname, const int oflags,
                         const hs_uuid_t& system_uuid, const uint32_t dev_num, const uint64_t dev_offset,
                         const iomgr::iomgr_drive_type drive_type, const bool is_init, const uint64_t dm_info_size,
                         bool* const is_inited) :
        m_mgr{mgr}, m_devname{devname}, m_metrics{devname} {
    /* super block should always be written atomically. */
    HS_LOG_ASSERT_LE(sizeof(super_block), SUPERBLOCK_SIZE, "Device {} Ondisk Superblock size not enough to hold in-mem",
                     devname);
    auto* const membuf{hs_utils::iobuf_alloc(SUPERBLOCK_SIZE, sisl::buftag::superblk)};
    m_super_blk = new (membuf) super_block{};
    if (sizeof(super_block) < SUPERBLOCK_SIZE) {
        std::memset(membuf + sizeof(super_block), 0, SUPERBLOCK_SIZE - sizeof(super_block));
    }

    if (is_init) { m_super_blk->set_system_uuid(system_uuid); }
    m_info_blk.dev_num = dev_num;
    m_info_blk.dev_offset = dev_offset;
    m_info_blk.first_chunk_id = INVALID_CHUNK_ID;

    int oflags_used{oflags};
    if (devname.find("/tmp") == 0) {
        // tmp directory in general does not allow Direct I/O
        oflags_used &= ~O_DIRECT;
    }
    m_iodev = drive_iface->open_dev(devname.c_str(), drive_type, oflags_used);
    if (m_iodev == nullptr
#ifdef _PRERELEASE
        || (homestore_flip->test_flip("device_boot_fail", devname.c_str()))
#endif
    ) {

        hs_utils::iobuf_free(reinterpret_cast< uint8_t* >(m_super_blk), sisl::buftag::superblk);

        HS_LOG(ERROR, device, "device open failed errno {} dev_name {}", errno, devname.c_str());
        throw std::system_error(errno, std::system_category(), "error while opening the device");
    }

    // Get the device size
    try {
        m_devsize = drive_iface->get_size(m_iodev.get());
    } catch (std::exception& e) {
        hs_utils::iobuf_free(reinterpret_cast< uint8_t* >(m_super_blk), sisl::buftag::superblk);
        throw(e);
    }

    if (m_devsize == 0) {
        const auto s{
            fmt::format("Device {} drive_type={} size={} is too small", devname, enum_name(drive_type), m_devsize)};
        HS_ASSERT(LOGMSG, 0, s.c_str());
        throw homestore::homestore_exception(s, homestore_error::min_size_not_avail);
    }

    const auto current_size{m_devsize};
    m_devsize = sisl::round_down(m_devsize, HS_STATIC_CONFIG(drive_attr.phys_page_size));
    if (m_devsize != current_size) {
        LOGWARN("device size is not the multiple of physical page size old size {}", current_size);
    }
    LOGINFO("Device {} opened with dev_id={} size={}", m_devname, m_iodev->dev_id(), m_devsize);
    m_dm_chunk[0] = m_dm_chunk[1] = nullptr;
    if (is_init) {
        /* create a chunk */
        const uint64_t sb_size{sisl::round_up(SUPERBLOCK_SIZE, HS_STATIC_CONFIG(drive_attr.phys_page_size))};
        HS_LOG_ASSERT_EQ(get_size() % HS_STATIC_CONFIG(drive_attr.phys_page_size), 0,
                         "Expected drive size to be aligned with physical page size");
        m_mgr->create_new_chunk(this, sb_size, get_size() - sb_size, nullptr);

        /* check for min size */
        const uint64_t min_size{SUPERBLOCK_SIZE + 2 * dm_info_size};
        if (m_devsize <= min_size) {
            const auto s{fmt::format("Device {} size={} is too small min_size={}", m_devname, m_devsize, min_size)};
            HS_ASSERT(LOGMSG, 0, s.c_str());
            throw homestore::homestore_exception(s, homestore_error::min_size_not_avail);
        }

        /* We create two chunks for super blocks. Since writing a sb chunk is not atomic operation,
         * so at any given point only one SB chunk is valid.
         */
        for (size_t i{0}; i < 2; ++i) {
            const uint64_t align_size{sisl::round_up(dm_info_size, HS_STATIC_CONFIG(drive_attr.phys_page_size))};
            HS_LOG_ASSERT_EQ(align_size, dm_info_size);
            m_dm_chunk[i] = m_mgr->alloc_chunk(this, INVALID_VDEV_ID, align_size, INVALID_CHUNK_ID);
            m_dm_chunk[i]->set_sb_chunk();
        }
        /* super block is written when first DM info block is written. Writing a superblock and making
         * a disk valid before that doesn't make sense as that disk is of no use until DM info is not
         * written.
         */
    } else {
        *is_inited = load_super_block(system_uuid);
        if (*is_inited) {
            /* If it is different then it mean it require upgrade/revert handling */
            HS_LOG_ASSERT_EQ(m_super_blk->dm_chunk[0].get_chunk_size(), dm_info_size);
            HS_LOG_ASSERT_EQ(m_super_blk->dm_chunk[1].get_chunk_size(), dm_info_size);
            if (is_init_done() == false) { init_done(); }
        }
    }
}

size_t PhysicalDev::get_total_cap() {
    return (m_devsize - (SUPERBLOCK_SIZE + m_dm_chunk[0]->get_size() + m_dm_chunk[1]->get_size()));
}

bool PhysicalDev::load_super_block(const hs_uuid_t& system_uuid) {
    read_superblock();

    // Validate if its homestore formatted device

    const bool is_omstore_dev{validate_device()};

    if (!is_omstore_dev) {
        LOGCRITICAL("invalid device name {} found magic {} product name {} version {}", m_devname,
                    m_super_blk->get_magic(), m_super_blk->get_product_name(), m_super_blk->get_version());
        return false;
    }

    if (m_super_blk->get_system_uuid() != system_uuid) {
        std::ostringstream ss;
        ss << "we found the homestore formatted device with a different system UUID";
        const std::string s{ss.str()};
        LOGCRITICAL("{}", s);
        throw homestore::homestore_exception(s, homestore_error::formatted_disk_found);
    }

    m_info_blk.dev_num = m_super_blk->this_dev_info.dev_num;
    m_info_blk.dev_offset = m_super_blk->this_dev_info.dev_offset;
    m_info_blk.first_chunk_id = m_super_blk->this_dev_info.first_chunk_id;
    m_cur_indx = m_super_blk->cur_indx;
    m_superblock_valid = true;

    return true;
}

void PhysicalDev::read_dm_chunk(char* const mem, const uint64_t size) {
    HS_DEBUG_ASSERT_EQ(m_super_blk->dm_chunk[m_cur_indx & s_dm_chunk_mask].get_chunk_size(), size);
    const auto offset{m_super_blk->dm_chunk[m_cur_indx & s_dm_chunk_mask].chunk_start_offset};
    drive_iface->sync_read(m_iodev.get(), mem, size, offset);
}

void PhysicalDev::write_dm_chunk(const uint64_t gen_cnt, const char* const mem, const uint64_t size) {
    const auto offset{m_dm_chunk[(++m_cur_indx) & s_dm_chunk_mask]->get_start_offset()};
    drive_iface->sync_write(m_iodev.get(), mem, size, offset);
    write_super_block(gen_cnt);
}

uint64_t PhysicalDev::sb_gen_cnt() { return m_super_blk->gen_cnt; }

void PhysicalDev::write_super_block(const uint64_t gen_cnt) {
    // Format the super block and this device info structure
    m_super_blk->magic = MAGIC;
    std::strncpy(m_super_blk->product_name, PRODUCT_NAME, super_block::s_product_name_size);
    m_super_blk->product_name[super_block::s_product_name_size - 1] = 0;
    m_super_blk->version = CURRENT_SUPERBLOCK_VERSION;

    HS_DEBUG_ASSERT_NE(m_info_blk.get_dev_num(), INVALID_DEV_ID);
    HS_DEBUG_ASSERT_NE(m_info_blk.get_first_chunk_id(), INVALID_CHUNK_ID);

    m_super_blk->this_dev_info.dev_num = m_info_blk.dev_num;
    m_super_blk->this_dev_info.first_chunk_id = m_info_blk.first_chunk_id;
    m_super_blk->this_dev_info.dev_offset = m_info_blk.dev_offset;
    m_super_blk->gen_cnt = gen_cnt;
    m_super_blk->cur_indx = m_cur_indx;

    for (size_t i{0}; i < super_block::s_num_dm_chunks; ++i) {
        std::memcpy(static_cast< void* >(&(m_super_blk->dm_chunk[i])),
                    static_cast< const void* >(m_dm_chunk[i]->get_chunk_info()), sizeof(chunk_info_block));
    }

    // Write the information to the offset
    write_superblock();
    m_superblock_valid = true;
}

void PhysicalDev::zero_boot_sbs(const std::vector< dev_info >& devices, const iomgr_drive_type drive_type, const int oflags) {
    // alloc re-usable super block
    auto* const membuf{hs_utils::iobuf_alloc(SUPERBLOCK_SIZE, sisl::buftag::superblk)};
    super_block* const super_blk{new (membuf) super_block{}};
    if (sizeof(super_block) < SUPERBLOCK_SIZE) {
        std::memset(membuf + sizeof(super_block), 0, SUPERBLOCK_SIZE - sizeof(super_block));
    }

    for (const auto& dev : devices) {
        HS_LOG_ASSERT_LE(sizeof(super_block), SUPERBLOCK_SIZE,
                         "Device {} Ondisk Superblock size not enough to hold in-mem", dev.dev_names);
        // open device
        auto iodev{drive_iface->open_dev(dev.dev_names.c_str(), drive_type, oflags)};

        // write zeroed sb to disk
        const auto bytes{drive_iface->sync_write(iodev.get(), (const char*)super_blk, SUPERBLOCK_SIZE, 0)};
        if (sisl_unlikely((bytes < 0) || (static_cast< size_t >(bytes) != SUPERBLOCK_SIZE))) {
            LOGINFO("Failed to zeroed superblock of device: {}, errno: {}", dev.dev_names, errno);
            throw std::system_error(errno, std::system_category(), "error while writing a superblock" + dev.dev_names);
        }

        LOGINFO("Successfully zeroed superblock of device: {}", dev.dev_names);

        // close device;
        drive_iface->close_dev(iodev);
    }

    // free super_blk
    hs_utils::iobuf_free(reinterpret_cast< uint8_t* >(super_blk), sisl::buftag::superblk);
}

bool PhysicalDev::has_valid_superblock(hs_uuid_t& out_uuid) {
    read_superblock();

    // Validate if its homestore formatted device
    const bool ret{(validate_device() && is_init_done())};

    if (ret) { out_uuid = m_super_blk->get_system_uuid(); }
    return ret;
}

void PhysicalDev::close_device() { drive_iface->close_dev(m_iodev); }

void PhysicalDev::init_done() {
    m_super_blk->set_init_done(true);
    write_superblock();
}

inline bool PhysicalDev::validate_device() {
    return ((m_super_blk->magic == MAGIC) && (std::strcmp(m_super_blk->product_name, PRODUCT_NAME) == 0) &&
            (m_super_blk->version == CURRENT_SUPERBLOCK_VERSION));
}

inline void PhysicalDev::write_superblock() {
    const auto bytes{drive_iface->sync_write(m_iodev.get(), reinterpret_cast< const char* >(m_super_blk), static_cast<uint32_t>(SUPERBLOCK_SIZE), 0)};
    if (sisl_unlikely((bytes < 0) || (static_cast< size_t >(bytes) != SUPERBLOCK_SIZE))) {
        throw std::system_error(errno, std::system_category(), "error while writing a superblock" + get_devname());
    }
}

inline void PhysicalDev::read_superblock() {
    // std::memset(static_cast< void* >(m_super_blk), 0, SUPERBLOCK_SIZE);
    const auto bytes{drive_iface->sync_read(m_iodev.get(), reinterpret_cast< char* >(m_super_blk), static_cast<uint32_t>(SUPERBLOCK_SIZE), 0)};
    if (sisl_unlikely((bytes < 0) || (static_cast< size_t >(bytes) != SUPERBLOCK_SIZE))) {
        throw std::system_error(errno, std::system_category(), "error while reading a superblock" + get_devname());
    }
}

void PhysicalDev::write(const char* const data, const uint32_t size, const uint64_t offset, uint8_t* const cookie, const bool part_of_batch) {
    HISTOGRAM_OBSERVE(m_metrics, write_io_sizes, (((size - 1) / 1024) + 1));
    drive_iface->async_write(m_iodev.get(), data, size, offset, cookie, part_of_batch);
}

void PhysicalDev::writev(const iovec* const iov, const int iovcnt, const uint32_t size, const uint64_t offset, uint8_t* const cookie,
                         const bool part_of_batch) {
    HISTOGRAM_OBSERVE(m_metrics, write_io_sizes, (((size - 1) / 1024) + 1));
    drive_iface->async_writev(m_iodev.get(), iov, iovcnt, size, offset, cookie, part_of_batch);
}

void PhysicalDev::read(char* const data, const uint32_t size, const uint64_t offset, uint8_t* const cookie, const bool part_of_batch) {
    HISTOGRAM_OBSERVE(m_metrics, read_io_sizes, (((size - 1) / 1024) + 1));
    drive_iface->async_read(m_iodev.get(), data, size, offset, cookie, part_of_batch);
}

void PhysicalDev::readv(iovec* const iov, const int iovcnt, const uint32_t size, const uint64_t offset, uint8_t* const cookie,
                        const bool part_of_batch) {
    HISTOGRAM_OBSERVE(m_metrics, read_io_sizes, (((size - 1) / 1024) + 1));
    drive_iface->async_readv(m_iodev.get(), iov, iovcnt, size, offset, cookie, part_of_batch);
}

ssize_t PhysicalDev::sync_write(const char* const data, const uint32_t size, const uint64_t offset) {
    try {
        HISTOGRAM_OBSERVE(m_metrics, write_io_sizes, (((size - 1) / 1024) + 1));
        return drive_iface->sync_write(m_iodev.get(), data, size, offset);
    } catch (const std::system_error& e) {
        std::ostringstream ss;
        ss << "dev_name " << get_devname() << ":" << e.what() << "\n";
        const std::string s{ss.str()};
        device_manager_mutable()->handle_error(this);
        throw std::system_error(e.code(), s);
    }
}

ssize_t PhysicalDev::sync_writev(const iovec* const iov, const int iovcnt, const uint32_t size, const uint64_t offset) {
    try {
        HISTOGRAM_OBSERVE(m_metrics, write_io_sizes, (((size - 1) / 1024) + 1));
        return drive_iface->sync_writev(m_iodev.get(), iov, iovcnt, size, offset);
    } catch (const std::system_error& e) {
        std::ostringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s{ss.str()};
        device_manager_mutable()->handle_error(this);
        throw std::system_error(e.code(), s);
    }
}

void PhysicalDev::write_zero(const uint64_t size, const uint64_t offset, uint8_t* const cookie) {
    drive_iface->write_zero(m_iodev.get(), size, offset, cookie);
}

ssize_t PhysicalDev::sync_read(char* const data, const uint32_t size, const uint64_t offset) {
    try {
        HISTOGRAM_OBSERVE(m_metrics, read_io_sizes, (((size - 1) / 1024) + 1));
        return drive_iface->sync_read(m_iodev.get(), data, size, offset);
    } catch (const std::system_error& e) {
        std::ostringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s = ss.str();
        device_manager_mutable()->handle_error(this);
        throw std::system_error(e.code(), s);
        return -1;
    }
}

ssize_t PhysicalDev::sync_readv(iovec* const iov, const int iovcnt, const uint32_t size, const uint64_t offset) {
    try {
        HISTOGRAM_OBSERVE(m_metrics, read_io_sizes, (((size - 1) / 1024) + 1));
        return drive_iface->sync_readv(m_iodev.get(), iov, iovcnt, size, offset);
    } catch (const std::system_error& e) {
        std::ostringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s{ss.str()};
        device_manager_mutable()->handle_error(this);
        throw std::system_error(e.code(), s);
        return -1;
    }
}

void PhysicalDev::attach_chunk(PhysicalDevChunk* const chunk, PhysicalDevChunk* const after) {
    if (after) {
        chunk->set_next_chunk(after->get_next_chunk_mutable());
        chunk->set_prev_chunk(after);

        auto* const next{after->get_next_chunk_mutable()};
        if (next) next->set_prev_chunk(chunk);
        after->set_next_chunk(chunk);
    } else {
        HS_DEBUG_ASSERT_EQ(m_info_blk.get_first_chunk_id(), INVALID_CHUNK_ID);
        m_info_blk.first_chunk_id = chunk->get_chunk_id();
    }
}

std::array< uint32_t, 2 > PhysicalDev::merge_free_chunks(PhysicalDevChunk* chunk) {
    std::array< uint32_t, 2 > freed_ids{INVALID_CHUNK_ID, INVALID_CHUNK_ID};
    uint32_t nids{0};

    // Check if previous and next chunk are free, if so make it contiguous chunk
    PhysicalDevChunk* const prev_chunk{chunk->get_prev_chunk_mutable()};
    PhysicalDevChunk* const next_chunk{chunk->get_next_chunk_mutable()};

    if (prev_chunk && !prev_chunk->is_busy()) {
        // We can merge our space to prev_chunk and remove our current chunk.
        prev_chunk->set_size(prev_chunk->get_size() + chunk->get_size());
        prev_chunk->set_next_chunk(chunk->get_next_chunk_mutable());

        // Erase the current chunk entry
        prev_chunk->set_next_chunk(chunk->get_next_chunk_mutable());
        if (next_chunk) next_chunk->set_prev_chunk(prev_chunk);

        freed_ids[nids++] = chunk->get_chunk_id();
        chunk = prev_chunk;
    }

    if (next_chunk && !next_chunk->is_busy()) {
        next_chunk->set_size(chunk->get_size() + next_chunk->get_size());
        next_chunk->set_start_offset(chunk->get_start_offset());

        // Erase the current chunk entry
        next_chunk->set_prev_chunk(chunk->get_prev_chunk_mutable());
        auto* const p{chunk->get_prev_chunk_mutable()};
        if (p) p->set_next_chunk(next_chunk);
        freed_ids[nids++] = chunk->get_chunk_id();
    }
    return freed_ids;
}

pdev_info_block PhysicalDev::get_info_blk() { return m_info_blk; }

PhysicalDevChunk* PhysicalDev::find_free_chunk(const uint64_t req_size) {
    // Get the slot with closest size;
    PhysicalDevChunk* closest_chunk{nullptr};

    PhysicalDevChunk* chunk{device_manager_mutable()->get_chunk_mutable(m_info_blk.first_chunk_id)};
    while (chunk) {
        if (!chunk->is_busy() && (chunk->get_size() >= req_size)) {
            if ((closest_chunk == nullptr) || (chunk->get_size() < closest_chunk->get_size())) {
                closest_chunk = chunk;
            }
        }
        chunk = device_manager_mutable()->get_chunk_mutable(chunk->get_next_chunk_id());
    }

    return closest_chunk;
}

std::string PhysicalDev::to_string() {
    std::ostringstream ss;
    ss << "Device name = " << m_devname << "\n";
    ss << "Device ID = " << m_iodev->dev_id() << "\n";
    ss << "Device size = " << m_devsize << "\n";
    ss << "Super Block :\n";
    ss << "\tMagic = " << m_super_blk->magic << "\n";
    ss << "\tProduct Name = " << m_super_blk->get_product_name() << "\n";
    ss << "\tHeader version = " << m_super_blk->version << "\n";
    ss << "\tPdev Id = " << m_info_blk.dev_num << "\n";
    ss << "\tPdev Offset = " << m_info_blk.dev_offset << "\n";
    ss << "\tFirst chunk id = " << m_info_blk.first_chunk_id << "\n";

    const PhysicalDevChunk* pchunk{device_manager()->get_chunk(m_info_blk.first_chunk_id)};
    while (pchunk) {
        ss << "\t\t" << pchunk->to_string() << "\n";
        pchunk = pchunk->get_next_chunk();
    }

    return ss.str();
}

/********************* PhysicalDevChunk Section ************************/
PhysicalDevChunk::PhysicalDevChunk(PhysicalDev* const pdev, chunk_info_block* const cinfo) {
    m_chunk_info = cinfo;
    m_pdev = pdev;
#if 0
    const std::unique_ptr< PhysicalDev > &p =
            (static_cast<const homeds::sparse_vector< std::unique_ptr< PhysicalDev > > &>(device_manager()->m_pdevs))[cinfo->pdev_id];
    m_pdev = p.get();
#endif
}

PhysicalDevChunk::PhysicalDevChunk(PhysicalDev* const pdev, const uint32_t chunk_id, const uint64_t start_offset, const uint64_t size,
                                   chunk_info_block* const cinfo) {
    m_chunk_info = cinfo;
    // Fill in with new chunk info
    m_chunk_info->chunk_id = chunk_id;
    m_chunk_info->set_slot_allocated(true);
    m_chunk_info->pdev_id = pdev->get_dev_id();
    m_chunk_info->chunk_start_offset = start_offset;
    m_chunk_info->chunk_size = size;
    m_chunk_info->prev_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->next_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->primary_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->vdev_id = INVALID_VDEV_ID;
    m_chunk_info->set_sb_chunk(false);
    m_chunk_info->end_of_chunk_size = static_cast< int64_t >(size);
    m_pdev = pdev;
}

PhysicalDevChunk::~PhysicalDevChunk() {}

const PhysicalDevChunk* PhysicalDevChunk::get_next_chunk() const {
    return device_manager()->get_chunk(get_next_chunk_id());
}

PhysicalDevChunk* PhysicalDevChunk::get_next_chunk_mutable() {
    return device_manager_mutable()->get_chunk_mutable(get_next_chunk_id());
}

const PhysicalDevChunk* PhysicalDevChunk::get_prev_chunk() const {
    return device_manager()->get_chunk(get_prev_chunk_id());
}

PhysicalDevChunk* PhysicalDevChunk::get_prev_chunk_mutable() {
    return device_manager_mutable()->get_chunk_mutable(get_prev_chunk_id());
}

const PhysicalDevChunk* PhysicalDevChunk::get_primary_chunk() const {
    return device_manager()->get_chunk(m_chunk_info->primary_chunk_id);
}

PhysicalDevChunk* PhysicalDevChunk::get_primary_chunk_mutable() {
    return device_manager_mutable()->get_chunk_mutable(m_chunk_info->primary_chunk_id);
}

const DeviceManager* PhysicalDevChunk::device_manager() const { return get_physical_dev()->device_manager(); }

DeviceManager* PhysicalDevChunk::device_manager_mutable() {
    return get_physical_dev_mutable()->device_manager_mutable();
}
} // namespace homestore
