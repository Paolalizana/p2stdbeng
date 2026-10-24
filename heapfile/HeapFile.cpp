#include "heapfile/HeapFile.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <filesystem>

// ============================================================================
// HeapFile — STUDENT IMPLEMENTATION FILE
// ============================================================================
//
// Every member method declared in heapfile/HeapFile.h has a blank body
// below; the header documents the contract of each one (the file-level
// comment there explains the page format, the two free-space lists, and
// the pin protocol). Your task: replace the stub bodies with working
// implementations until the check suite (`p2stdbeng`) passes.
//
// What is already provided (do not change): the file-local helpers in the
// anonymous namespace below — the header-page layout constants, the
// HeaderFields struct and its read/write helpers, and the identification
// record writers/checkers. They fix the on-disk format; your member
// methods build on them. The free function erase_file at the bottom is
// also complete.

namespace heapfile {

namespace {

// Releases a pin without disturbing `error`, which on failure paths already
// carries the real cause (unpin clears the string it is given).
void release(bufman::BufferManager& manager, std::size_t frame, bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

// The header page's record area (its slots are never used): magic, the five
// structural fields, then the table name. Fixed-width fields come before
// the variable-length name so every field has a constant offset.
//
//   0   magic "TUP2"
//   4   free_head     first page with a free slot (kNoPage when empty)
//   8   free_tail     last page appended to the free list
//   12  full_head     first page whose every slot is used
//   16  full_tail
//   20  next_page_id  the id the next page allocation will use (starts at 1)
//   24  name_length
//   28  name bytes
struct HeaderFields {
    std::uint32_t free_head = kNoPage;
    std::uint32_t free_tail = kNoPage;
    std::uint32_t full_head = kNoPage;
    std::uint32_t full_tail = kNoPage;
    std::uint32_t next_page_id = 1;
};

constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderFreeHeadOffset = 4;
constexpr std::size_t kHeaderFreeTailOffset = 8;
constexpr std::size_t kHeaderFullHeadOffset = 12;
constexpr std::size_t kHeaderFullTailOffset = 16;
constexpr std::size_t kHeaderNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::size_t head_offset(bool free_list) {
    return free_list ? kHeaderFreeHeadOffset : kHeaderFullHeadOffset;
}

std::size_t tail_offset(bool free_list) {
    return free_list ? kHeaderFreeTailOffset : kHeaderFullTailOffset;
}

HeaderFields read_header_fields(const char* buffer) {
    HeaderFields fields;
    fields.free_head = page_detail::read_u32(buffer, kHeaderFreeHeadOffset);
    fields.free_tail = page_detail::read_u32(buffer, kHeaderFreeTailOffset);
    fields.full_head = page_detail::read_u32(buffer, kHeaderFullHeadOffset);
    fields.full_tail = page_detail::read_u32(buffer, kHeaderFullTailOffset);
    fields.next_page_id = page_detail::read_u32(buffer, kHeaderNextPageIdOffset);
    return fields;
}

void write_header_fields(char* buffer, const HeaderFields& fields) {
    page_detail::write_u32(buffer, kHeaderFreeHeadOffset, fields.free_head);
    page_detail::write_u32(buffer, kHeaderFreeTailOffset, fields.free_tail);
    page_detail::write_u32(buffer, kHeaderFullHeadOffset, fields.full_head);
    page_detail::write_u32(buffer, kHeaderFullTailOffset, fields.full_tail);
    page_detail::write_u32(buffer, kHeaderNextPageIdOffset, fields.next_page_id);
}

// Writes the identification record into a bound header-page buffer: magic,
// empty lists, allocation frontier at 1, table name. Returns false with a
// named error when the table name is too long.
bool write_identification(char* buffer, const schema::TableSchema& schema,
                          std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    page_detail::write_u32(buffer, kMagicOffset, kHeapMagic);
    write_header_fields(buffer, HeaderFields{});
    page_detail::write_u32(buffer, kNameLengthOffset,
                           static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    return true;
}

// Checks the identification record against `schema`. False with a named
// error when the magic is missing or the name differs.
bool check_identification(const char* buffer,
                          const schema::TableSchema& schema,
                          std::string& error) {
    if (page_detail::read_u32(buffer, kMagicOffset) != kHeapMagic) {
        error = "not a tuple heap file";
        return false;
    }
    const std::uint32_t length = page_detail::read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "not a tuple heap file";
        return false;
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "heap file is for table '" + stored + "'";
        return false;
    }
    return true;
}

// The list pointers must name allocated pages (or be empty). A dirty page
// may not be on disk yet, so next_page_id can legitimately exceed the
// on-disk block count by at most the pool's frame count; anything beyond
// that is a corrupt header, not slack.
bool header_fields_in_range(const HeaderFields& fields, std::uint64_t blocks,
                            std::string& error) {
    constexpr std::uint64_t kDirtySlack = 4096; // >= any pool this engine builds
    const bool ok =
        fields.next_page_id >= 1 &&
        fields.next_page_id <= blocks + kDirtySlack &&
        (fields.free_head == kNoPage || fields.free_head < fields.next_page_id) &&
        (fields.free_tail == kNoPage || fields.free_tail < fields.next_page_id) &&
        (fields.full_head == kNoPage || fields.full_head < fields.next_page_id) &&
        (fields.full_tail == kNoPage || fields.full_tail < fields.next_page_id);
    if (!ok) {
        error = "header list pointers are out of range";
    }
    return ok;
}

}

// ---------------------------------------------------------------------------
// Member methods — YOUR WORK STARTS HERE
// ---------------------------------------------------------------------------

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    // TODO(student): report whether a file is currently open.
    return open_;
}

bool HeapFile::require_open(std::string& error) const {
    // TODO(student): fail with "heap file is not open" when closed.
    error.clear();
    if (!open_) {
        error = "heap file is not open";
        return false;
    }
    return true;
}

bool HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema, std::string& error) {
    // TODO(student): format a one-block file: refuse an open HeapFile / a
    // non-empty path, validate the record size, alloc page 0, format it as
    // a SlottedPage, write the identification record, and close (flushing)
    // the file — leaving this HeapFile closed.

    error.clear();
    if (open_) {
        error = "heap file is already open";
        return false;
    }
    std::error_code fs_error;
    if (std::filesystem::exists(path, fs_error) && std::filesystem::file_size(path, fs_error) > 0) {
        error = "heap file \"" + path + "\": already exists";
        return false;
    }
    const schema ::TupleSerializer serializer(schema);
    const std::size_t record_size = serializer.record_size();
    if (record_size == 0) {
        error="schema has no record layout";
        return false;
    }
    if(record_size >= kSlotCountOffset || slot_capacity(record_size) == 0) {
        error="record size " + std::to_string(record_size) + " does not fit a page";
        return false;
    }
    if(schema.table_name().size() > kMaxTableNameLength) {
        error="table name is too long for the header";
        return false;   
    }
    if (!manager_.open_file(path, file_, error)) {
        file_ = bufman::File{};
        return false;
    }
    std::size_t frame=0;
    if(!manager_.alloc_page(file_, 0, frame, error)) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }

    char* buffer = manager_.frame(frame).buffer;
    SlottedPage header;
    header.bind(buffer, record_size);
    header.format();
    if(!write_identification(buffer, schema, error)) {
        release(manager_, frame, false);
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    if(!manager_.unpin(frame,true, error)){
        return false;
    }
    const bool close = manager_.close_file(file_, error);
    file_ = bufman::File{};
  
}

bool HeapFile::open(const std::string& path,
                    const schema::TableSchema& schema, std::string& error) {
    // TODO(student): probe that the path exists, register it with the
    // manager, then validate block 0: identification, SlottedPage attach,
    // and the header fields in range. On any failure unwind completely
    // (close through the manager, reset the handle) and return false; on
    // success bind schema_/serializer_/record_size_ and mark the file open.
    error.clear();
    if (open_) {
        error = "heap file is already open";
        return false;
    }
    std::error_code fs_error;
    if (!std::filesystem::exists(path, fs_error)) {
        error = "heap file \"" + path + "\": does not exist";
        return false;
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t record_size = serializer.record_size();
    if (record_size == 0) {
        error = "schema has no record layout";
        return false;
    }
    if(!manager_.open_file(path, file_, error)) {
        file_ = bufman::File{};
        return false;
    }
    auto unwind = [&]() {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
    };
    std::size_t frame=0;
    if(!manager_.pin(file_, 0, frame, error)) {
        unwind();
        return false;
    }
    char* buffer = manager_.frame(frame).buffer;
    bool ok= check_identification(buffer, schema, error);
    if(!ok) {
        SlottedPage header;
        header.bind(buffer, record_size);
        ok = header.attach(error);
    }
    if(ok) {
        const HeaderFields fields = read_header_fields(buffer);
        const std::uint64_t blocks = bufman::block_count(file_, error);
        ok = error.empty() && header_fields_in_range(fields, blocks, error);
    }
    if(!ok) {
        release(manager_, frame, false);
        unwind();
        return false;
    }
    if(!manager_.unpin(frame, false, error)) {
        unwind();
        return false;
    }
    schema_ = &schema;
    serializer_.emplace(schema);
    record_size_ = record_size;
    open_ = true;
    return true;
   
}
bool HeapFile::close(std::string& error) {
    error.clear();
    if(!require_open(error)) {
        return false;
    }
    if(!manager_.close_file(file_, error)) {
        return false;
    }
    file_ = bufman::File{};
    schema_ = nullptr;
    serializer_.reset();
    record_size_ = 0;
    open_ = false;
    return true;
}

HeapFileIterator HeapFile::scan() {
    // TODO(student): pin the header once, read the two list heads and
    // next_page_id, unpin, and return an iterator seeded with them. A
    // closed file yields an exhausted iterator.
    if(!open_) {
        return HeapFileIterator(*this, kNoPage, kNoPage, 1);
    }
    std::string error;
    std::size_t frame=0;
    if(!manager_.pin(file_, 0, frame, error)) {
        HeapFileIterator failed(*this, kNoPage, kNoPage, 1);
        failed.iterating_=0;
        failed.seen_.assign(error.begin(), error.end());
        return failed;
    }
    const HeaderFields fields = read_header_fields(manager_.frame(frame).buffer);
    release(manager_, frame, false);
    return HeapFileIterator(*this, fields.free_head, fields.full_head, fields.next_page_id);
}
bool HeapFileIterator ::next(tuple::Tuple& tuple, std::string& error) {
    // TODO(student): walk the free list's pages, then the full list's, in
    // list order; on each page scan slots in order and yield every
    // occupied slot's decoded record, remembering the slot to resume from.
    // Exhaustion is false with an EMPTY error; a revisited page
    // (cyclic lists) is false with a named error. The iterator owns the pin
    // on the current page; unpin it before moving to the next page.
    error.clear();
    if(iterating_ == 0) {
        error.assign(seen_.begin(), seen_.end());
        return false;
    }
    auto fail = [&](){
        iterating_ = 0;
        seen_.assign(error.begin(), error.end());
        return false;
    };
    if(!file_->require_open(error)) {
        return fail();
    }
    bufman::BufferManager& manager = file_->manager_;
    while(true){
        if(page_id_ == kNoPage) {
            if(next_list_ == kNoPage) {
                return false;
            }
            page_id_ = next_list_;
            next_list_ = kNoPage;
            slot_ = 0;
            continue;
        }
        if(iterating_ == page_id_) {
            if(page_id_ ==0 || page_id_ >= seen_.size()) {
             error="scan returned to page " + std::to_string(page_id_) + " outside the allocated pages";
             return fail();
        }
        if(seen_[page_id_]) {
            error="scan revisited page " + std::to_string(page_id_) + " :cyclic lists";
            return fail();
        }
        iterating_ = page_id_;
        slot_ = 0;
        }
    std::size_t frame=0;
    if(!manager.pin(file_->file_, page_id_, frame, error)) {
        return fail();
    }
    SlottedPage page;
    page.bind(manager.frame(frame).buffer, file_->record_size_);
    if(!page.attach(error)) {
        release(manager, frame, false);
        return fail();
    }
    while (slot_<page.capacity()) {
        const std::uint16_t slot = slot_;
        std::string slot_error;

        if(!page.is_occupied(slot_, slot_error)) {
            ++slot_;
            continue;
        }
        std::vector<char> record(file_->record_size_);
        if(!page.get_record(slot_, record.data(), record.size(), error)) {
            release(manager, frame, false);
            return fail();
        }
        release(manager, frame, false);
        if(!file_->serializer_->deserialize(record.data(), record.size(), tuple, error)) {
            return fail();
        }
        slot_ =slot_ + 1;
        current_ = RowId{page_id_, slot};
        return true;
    }
    const std::uint32_t forward = page.next_page();
    release(manager, frame, false);
    seen_[page_id_] = 1;
    iterating_ = kNoPage;
    page_id_ = forward;
    slot_ = 0;
}
}


// --- List surgery -----------------------------------------------------------

bool HeapFile::append_fresh_page(std::uint32_t page_id, std::size_t page_frame, std::string& error) {
    // TODO(student): make the just-allocated page the free list's only
    // element (head and tail) and advance next_page_id past it; clear the
    // page's own links. Only called right after an allocation, when the
    // free list is empty by construction — refuse otherwise.
    (void)page_id;
    (void)page_frame;
    (void)error;
    return false;
}

bool HeapFile::move_between_lists(bool to_full, std::uint32_t page_id,
                                  std::size_t page_frame, std::string& error) {
    // TODO(student): detach the pinned page from its source list (its
    // prev/next name the source neighbors) and append it at the
    // destination list's tail. Acquire ALL auxiliary pins first (header +
    // source neighbors + destination tail) in ascending page-id order;
    // after that the surgery is pure memory writes.
    // (void)to_full;
    // (void)page_id;
    // (void)page_frame;
    // (void)error;
    // return false;
    error.clear();
    char* page = manager_.frame(page_frame).buffer;
    const std::uint32_t prev = page_detail::read_u32(page, kPrevPageIdOffset);
    const std::uint32_t next = page_detail::read_u32(page, kNextPageIdOffset);
    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    char* header = manager_.frame(header_frame).buffer;
    HeaderFields fields = read_header_fields(header);
    const bool src_free = to_full;
    const std::uint32_t dest_tail = to_full ? fields.full_tail : fields.free_tail;

    std::vector<std::uint32_t> ids;
    for (const std::uint32_t id : {prev, next, dest_tail}) {
        if (id != kNoPage) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::pair<std::uint32_t, std::size_t>> pinned;
    for (const std::uint32_t id : ids) {
        std::size_t frame = 0;
        if (!manager_.pin(file_, id, frame, error)) {
            for (const auto& entry : pinned) {
                release(manager_, entry.second, false);
            }
            release(manager_, header_frame, false);
            return false;
        }
        pinned.emplace_back(id, frame);
    }
    auto buffer_of = [&](std::uint32_t id) -> char* {
        for (const auto& entry : pinned) {
            if (entry.first == id) {
                return manager_.frame(entry.second).buffer;
            }
        }
        return nullptr;
    };
    std::uint32_t src_head = page_detail::read_u32(header, head_offset(src_free));
    std::uint32_t src_tail = page_detail::read_u32(header, tail_offset(src_free));
    if (prev != kNoPage) {
        page_detail::write_u32(buffer_of(prev), kNextPageIdOffset, next);
    } else {
        src_head = next;
    }
    if (next != kNoPage) {
        page_detail::write_u32(buffer_of(next), kPrevPageIdOffset, prev);
    } else {
        src_tail = prev;
    }
    if (src_free) {
        fields.free_head = src_head;
        fields.free_tail = src_tail;
    } else {
        fields.full_head = src_head;
        fields.full_tail = src_tail;
    }
    page_detail::write_u32(page, kPrevPageIdOffset, dest_tail);
    page_detail::write_u32(page, kNextPageIdOffset, kNoPage);
    std::uint32_t& dest_head = to_full ? fields.full_head : fields.free_head;
    std::uint32_t& dest_tail_ref = to_full ? fields.full_tail : fields.free_tail;
    if (dest_tail != kNoPage) {
        page_detail::write_u32(buffer_of(dest_tail), kNextPageIdOffset, page_id);
    } else {
        dest_head = page_id;
    }
    dest_tail_ref = page_id;
    write_header_fields(header, fields);

    for (const auto& entry : pinned) {
        release(manager_, entry.second, true);
    }
    release(manager_, header_frame, true);
    return true;
}

// --- Row operations ---------------------------------------------------------

bool HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id,
                      std::string& error) {
    // TODO(student): serialize (validates first), take the free list's
    // head as the target page — or allocate a fresh page and append it to
    // the free list when the free list is empty — store the record in the
    // target's first free slot, and move the page to the full list when
    // its last free slot was taken. Report the new row id.
    // (void)tuple;
    // (void)row_id;
    // (void)error;
    // return false;
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    std::vector<char> record(record_size_);
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }
    std::size_t header_frame = 0;
    if (!manager_.pin(file_,0, header_frame, error)) {
        return false;
    }
    const HeaderFields fields = read_header_fields(manager_.frame(header_frame).buffer);
    std:uint32_t target_id = fields.free_head;
    std::size_t frame = 0;
    SlottedPage page;
    std::optional<std::size_t> free_tail;
    if (target_id == kNoPage){
        if (fields.next_page_id == kNoPage) {
            error = "next page id is invalid";
            release(manager_, header_frame, false);
            return false;
        }
        if (slot_capacity(record_size_) == 1 && fields.full_tail != kNoPage) {
            std::size_t tail_frame = 0;
            if (!manager_.pin(file_, fields.full_tail, tail_frame, error)) {
                release(manager_, header_frame, false);
                return false;
            }
            free_tail = tail_frame;
        }
        target_id = fields.next_page_id;
        if (!manager_.alloc_page(file_, target_id, frame, error)) {
            if (free_tail != 0) {
                release(manager_, *free_tail, false);
            }
            release(manager_, header_frame, false);
            return false;
        }
        page.bind(manager_.frame(frame).buffer, record_size_);
        page.format();
        if (!append_fresh_page(target_id, frame, error)) {
            release(manager_, frame, true);
            if (free_tail != 0) {
                release(manager_, *free_tail, false);
            }
            release(manager_, header_frame, false);
            return false;
        }
        release(manager_, frame, false);
    } else {
        release(manager_, header_frame, false);
        if (!manager_.pin(file_, target_id, frame, error)) {
            return false;
        }
        page.bind(manager_.frame(frame).buffer, record_size_);
        if (!page.attach(error)) {
            release(manager_, frame, false);
            return false;
        }
    }

    const auto free_slot = page.first_free_slot();
    if (!free_slot) {
        error = "no free slot found on page " + std::to_string(target_id);
        release(manager_, frame, false);
        if (free_tail != 0) {
            release(manager_, *free_tail, false);
        }
        return false;
    }
    const bool fills = page.occupied_count() + 1 == page.capacity();
    if (fills && !move_between_lists(true, target_id, frame, error)) {
        release(manager_, frame, false);
        if (free_tail != 0) {
            release(manager_, *free_tail, false);
        }
        return false;
    }
    page.put_record(*free_slot, record.data(), record.size(), error);
    if (free_tail) {
        release(manager_, *free_tail, false);
    }
    row_id = RowId{target_id, static_cast<std::uint32_t>(*free_slot)};
    return manager_.unpin(frame, true, error);
}

bool HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame, std::string& error) {
    // TODO(student): shared path for find/update/erase — reject the header
    // page and pages beyond the file's extent (resident pages count as
    // existing even before write-back), pin, attach, and require the slot
    // occupied. The caller owns the pin on success.
    // (void)row_id;
    // (void)page;
    // (void)frame;
    // (void)error;
    // return false;
    error.clear();
    if (row_id.page_id == 0) {
        error = "row id names the header page";
        return false;
    }
    if (!manager_.resident(file_, row_id.page_id)) {
        const std::uint64_t blocks = bufman::block_count(file_, error);
        if (!error.empty()) {
            return false;
        }
        if (row_id.page_id >= blocks) {
            error = "row id names a page beyond the end of the file";
            return false;
        }
    }
    if (!manager_.pin(file_, row_id.page_id, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    if (!page.is_occupied(row_id.slot, error)) {
        release(manager_, frame, false);
        if (error.empty()) {
            error = "row id names a free slot";
        }
        return false;
    }
    return true;
}

bool HeapFile::find(const RowId row_id, tuple::Tuple& tuple,
                    std::string& error) {
    // TODO(student): pin the record page, copy the record out, unpin, and
    // deserialize into `tuple`.
    // (void)row_id;
    // (void)tuple;
    // (void)error;
    // return false;
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    SlottedPage page;
    std::size_t frame = 0;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    std::vector<char> buffer(record_size_);
    if (!page.get_record(row_id.slot, buffer.data(), buffer.size(), error)) {
        release(manager_, frame, false);
        return false;
    }
    if (!manager_.unpin(frame, false, error)) {
        return false;
    }
    return serializer_->deserialize(buffer.data(), buffer.size(), tuple, error);
}

bool HeapFile::update(const RowId row_id, const tuple::Tuple& tuple,
                      std::string& error) {
    // TODO(student): serialize, pin the record page, overwrite the record
    // in place, and unpin. Occupancy never changes, so no list move.
    // (void)row_id;
    // (void)tuple;
    // (void)error;
    // return false;
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    std::vector<char> record(record_size_);
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }
    SlottedPage page;
    std::size_t frame = 0;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    if (!page.put_record(row_id.slot, record.data(), record.size(), error)) {
        release(manager_, frame, false);
        return false;
    }
    return manager_.unpin(frame, true, error);
}

bool HeapFile::erase(const RowId row_id, std::string& error) {
    // TODO(student): pin the record page, note whether the page WAS full,
    // clear the slot's occupancy byte, and move a just-unfilled page back
    // to the free list (at the tail).
    // (void)row_id;
    // (void)error;
    // return false;
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    SlottedPage page;
    std::size_t frame = 0;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool was_full = !page.has_free_slot();
    if (was_full && !move_between_lists(false, row_id.page_id, frame, error)) {
        release(manager_, frame, false);
        return false;
    }
    page.erase(row_id.slot, error);
    return manager_.unpin(frame, true, error);
}

// --- Structure self-check ---------------------------------------------------

bool HeapFile::check_structure(std::string& error) {
    // TODO(student): walk both lists one pin at a time and verify every
    // invariant the header documents: heads/tails agree with the walk,
    // every allocated page reachable exactly once, membership matches
    // occupancy, page 0 on no list, the union covering exactly pages
    // 1..next_page_id-1, and every on-disk data page reachable.
    error.clear();
    if(!require_open(error)) {
        return false;
    }
    std::size_t header_frame=0;
    if(!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }

    const HeaderFields fields = read_header_fields(manager_.frame(header_frame).buffer);
    release(manager_, header_frame, false);

    const std::uint64_t blocks = bufman::block_count(file_, error);
    if(!error.empty()) {
        return false;
    }

    if(!header_fields_in_range(fields, blocks, error)) {
        return false;
    }

    std::vector<char> seen(fields.next_page_id, 0);

    auto walk =[&](bool free_list, std::uint32_t head, std::uint32_t tail) -> bool {
        const std::string name = free_list ? "free list" : "full list";
        if((head == kNoPage )!= (tail == kNoPage)) {
            error = name + " head and tail disagree on emptiness";
            return false;
        }
        std::uint32_t prev = kNoPage;
        std::uint32_t current = head;
        while (current !=kNoPage){
           if(current ==0){
            error= name+ ": the header page appears on the list";
                return false;
           }
           if(current>= fields.next_page_id){
            error = name + ": page "+ std::to_string(current)+ "is beyond the allocated pages";
                return false;
           }
           if (seen[current]) {
            error = name + ": page " + std::to_string(current) + " visited twice (cyclic list or page on both lists)";
                 return false;
            }
            seen[current]=1;
            std::size_t frame=0;
            if(!manager_.pin(file_, current, frame, error)) {
                return false;
            }
            SlottedPage page;
            page.bind(manager_.frame(frame).buffer, record_size_);
            if (!page.attach(error)) {
                release(manager_, frame, false);
                return false;
            }
            const std::uint32_t back = page.prev_page();
            const std::uint32_t forward = page.next_page();
            const bool full = page.occupied_count() == page.capacity();
            release(manager_, frame, false);
            if (back != prev) {
                error = name + ": page " + std::to_string(current) +
                        " has a broken prev link";
                return false;
            }
            if (full == free_list) {
                error = name + ": page " + std::to_string(current) +
                        (full ? " is full" : " has a free slot") +
                        " and contradicts its list";
                return false;
            }
            prev = current;
            current = forward;
        }
        if (prev != tail) {
            error = name + ": tail pointer disagrees with the walk";
            return false;
        }
        return true;
    };

    if (!walk(true, fields.free_head, fields.free_tail) ||
        !walk(false, fields.full_head, fields.full_tail)) {
        return false;
    }
    for (std::uint32_t id = 1; id < fields.next_page_id; ++id) {
        if (!seen[id]) {
            error = "page " + std::to_string(id) + " is on no list";
            return false;
        }
    }
    if (blocks > fields.next_page_id) {
        error = "on-disk page " + std::to_string(fields.next_page_id) +
                " is unreachable from the lists";
        return false;
    }
    return true;
}

// --- Lifecycle --------------------------------------------------------------

bool HeapFile::resident(std::uint32_t page_id) const {
    // TODO(student): observe the manager's pool.
    // (void)page_id;
    // return false;
    return open_ && manager_.resident(file_, page_id);
}

bool HeapFile::close(std::string& error) {
    // TODO(student): close the file through the manager; reset this
    // HeapFile's state only when that succeeded, so a failed close can be
    // retried.
    (void)error;
    return false;
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                                   std::uint32_t full_head,
                                   std::uint32_t allocated)
    : file_(&file),
      page_id_(free_head),
      next_list_(full_head),
      slot_(0),
      seen_(allocated, 0) {}

bool HeapFileIterator::next(tuple::Tuple& tuple, std::string& error) {
    // TODO(student): walk the free list's pages, then the full list's, in
    // list order; on each page scan slots in order and yield every
    // occupied slot's decoded record, remembering the slot to resume from.
    // Exhaustion is false with an EMPTY error; a revisited page means a
    // cyclic list and is a named failure.
    (void)tuple;
    (void)error;
    return false;
}

// --- Free functions ---------------------------------------------------------

bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error) {
    error.clear();
    if (manager.is_open(path)) {
        error = "cannot erase \"" + path + "\": the file is open";
        return false;
    }
    if (std::remove(path.c_str()) != 0) {
        error = "cannot erase \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    return true;
}

}
