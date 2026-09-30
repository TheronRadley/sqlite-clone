#include "journal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "error.hpp"
#include "util.hpp"

namespace sc {
namespace journal {

static const char MAGIC[8] = {'S', 'Q', 'L', 'C', 'J', 'R', 'N', '1'};
static constexpr size_t HEADER_SIZE = 32;

std::string journal_path_for(const std::string& db_path) {
    return db_path + "-journal";
}

bool exists(const std::string& db_path) {
    std::FILE* f = std::fopen(journal_path_for(db_path).c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

static void sync_fd(const char* path, std::FILE* f) {
    if (std::fflush(f) != 0)
        throw DbError::storage(str("cannot flush ", path, ": I/O error"));
    if (::fsync(::fileno(f)) != 0)
        throw DbError::storage(str("cannot fsync ", path, ": ", std::strerror(errno)));
}

void write(const std::string& db_path, const JournalData& data) {
    std::string path = journal_path_for(db_path);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        throw DbError::storage(str("cannot create journal file ", path));

    try {
        // serialize records
        std::vector<uint8_t> records;
        records.reserve(data.pages.size() * (4 + data.page_size));
        for (const auto& [id, image] : data.pages) {
            uint8_t b4[4];
            wr_be32(b4, id);
            records.insert(records.end(), b4, b4 + 4);
            if (image.size() != data.page_size)
                throw DbError::storage("journal page image has wrong size");
            records.insert(records.end(), image.begin(), image.end());
        }

        std::vector<uint8_t> head(HEADER_SIZE);
        std::copy(MAGIC, MAGIC + 8, head.begin());
        wr_be16(head.data() + 8, 1);                          // journal format version
        wr_be16(head.data() + 10, 0);                          // reserved
        wr_be32(head.data() + 12, data.page_size);
        wr_be32(head.data() + 16, data.original_page_count);
        wr_be32(head.data() + 20, uint32_t(data.pages.size()));

        // checksum covers bytes 12..24 (page size, original page count,
        // record count) plus all record bytes; it lives at 24..32
        uint64_t sum = fnv1a64(head.data() + 12, 12) ^ fnv1a64(records.data(), records.size());
        wr_be64(head.data() + 24, sum);

        if (std::fwrite(head.data(), 1, head.size(), f) != head.size() ||
            (!records.empty() &&
             std::fwrite(records.data(), 1, records.size(), f) != records.size()))
            throw DbError::storage(str("cannot write journal file ", path));
        sync_fd(path.c_str(), f);
        std::fclose(f);
    } catch (...) {
        std::fclose(f);
        throw;
    }
}

JournalData read_and_validate(const std::string& db_path) {
    std::string path = journal_path_for(db_path);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        throw DbError::storage(str("cannot open journal file ", path));

    try {
        std::vector<uint8_t> head(HEADER_SIZE);
        if (std::fread(head.data(), 1, head.size(), f) != head.size())
            throw DbError::storage(str("journal ", path, " is truncated (header)"));
        for (int i = 0; i < 8; ++i)
            if (head[size_t(i)] != uint8_t(MAGIC[i]))
                throw DbError::storage(str("journal ", path, " has a bad magic"));
        if (rd_be16(head.data() + 8) != 1)
            throw DbError::storage(str("journal ", path, " has an unsupported version"));

        JournalData data;
        data.page_size = rd_be32(head.data() + 12);
        data.original_page_count = rd_be32(head.data() + 16);
        uint32_t count = rd_be32(head.data() + 20);
        uint64_t stored_sum = rd_be64(head.data() + 24);

        if (data.page_size < 512 || data.page_size > 32768 ||
            (data.page_size & (data.page_size - 1)) != 0)
            throw DbError::storage(str("journal ", path, " has an impossible page size"));
        if (count > (1u << 22))
            throw DbError::storage(str("journal ", path, " has an impossible record count"));

        std::vector<uint8_t> records(size_t(count) * (4 + data.page_size));
        if (std::fread(records.data(), 1, records.size(), f) != records.size())
            throw DbError::storage(str("journal ", path, " is truncated (records)"));

        uint64_t sum = fnv1a64(head.data() + 12, 12) ^ fnv1a64(records.data(), records.size());
        if (sum != stored_sum)
            throw DbError::storage(str("journal ", path,
                                       " failed its checksum: it is corrupt. The database "
                                       "may be in an inconsistent state; refusing to open."));

        size_t off = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = rd_be32(records.data() + off);
            off += 4;
            if (id >= data.original_page_count)
                throw DbError::storage(str("journal ", path, " references page ", id,
                                           " beyond the original page count"));
            data.pages.emplace(id,
                               std::vector<uint8_t>(records.begin() + off,
                                                    records.begin() + off + data.page_size));
            off += data.page_size;
        }
        std::fclose(f);
        return data;
    } catch (...) {
        std::fclose(f);
        throw;
    }
}

void remove(const std::string& db_path) {
    if (std::remove(journal_path_for(db_path).c_str()) != 0) {
        // Removing a journal that does not exist is fine.
        if (exists(db_path))
            throw DbError::storage(str("cannot remove journal file ",
                                       journal_path_for(db_path)));
    }
}

bool recover_if_present(const std::string& db_path) {
    if (!exists(db_path)) return false;

    JournalData data = read_and_validate(db_path);  // throws if corrupt

    std::FILE* f = std::fopen(db_path.c_str(), "r+b");
    if (!f)
        throw DbError::storage(str("journal exists but database file ", db_path,
                                   " cannot be opened"));
    try {
        for (const auto& [id, image] : data.pages) {
            if (std::fseek(f, long(id) * long(data.page_size), SEEK_SET) != 0 ||
                std::fwrite(image.data(), 1, image.size(), f) != image.size())
                throw DbError::storage("recovery: cannot restore page image");
        }
        long want = long(data.original_page_count) * long(data.page_size);
        if (std::fseek(f, 0, SEEK_END) != 0)
            throw DbError::storage("recovery: cannot seek");
        long have = std::ftell(f);
        if (have > want) {
            // truncate back to the original size
            if (::ftruncate(::fileno(f), want) != 0)
                throw DbError::storage("recovery: cannot truncate database file");
        }
        sync_fd(db_path.c_str(), f);
        std::fclose(f);
    } catch (...) {
        std::fclose(f);
        throw;
    }

    remove(db_path);
    return true;
}

} // namespace journal
} // namespace sc
