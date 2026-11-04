#include "pw/rotating_log.hpp"

#include "pw/logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace pw {

RotatingLog::RotatingLog(LogSpec spec) : spec_(std::move(spec))
{
    if (spec_.file.has_parent_path()) {
        std::filesystem::create_directories(spec_.file.parent_path());
    }
    open_file();
}

std::filesystem::path RotatingLog::rotated_path(unsigned index) const
{
    std::filesystem::path rotated = spec_.file;
    rotated += "." + std::to_string(index);
    return rotated;
}

void RotatingLog::open_file()
{
    UniqueFd fd{::open(spec_.file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd.valid()) {
        throw std::system_error(errno, std::generic_category(), "open " + spec_.file.string());
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        throw std::system_error(errno, std::generic_category(), "fstat " + spec_.file.string());
    }
    fd_ = std::move(fd);
    size_ = static_cast<std::uint64_t>(info.st_size);
}

void RotatingLog::rotate()
{
    if (spec_.max_files == 0) {
        if (::ftruncate(fd_.get(), 0) != 0) {
            throw std::system_error(errno, std::generic_category(), "ftruncate " + spec_.file.string());
        }
        size_ = 0;
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(rotated_path(spec_.max_files), ignored);
    for (unsigned index = spec_.max_files - 1; index >= 1; --index) {
        const std::filesystem::path from = rotated_path(index);
        if (std::filesystem::exists(from, ignored)) {
            std::filesystem::rename(from, rotated_path(index + 1));
        }
    }
    fd_.reset();
    std::filesystem::rename(spec_.file, rotated_path(1));
    open_file();
}

void RotatingLog::write(std::string_view data)
{
    if (data.empty()) {
        return;
    }
    std::string buffer;
    std::string_view payload = data;
    if (spec_.timestamps) {
        const std::string prefix = timestamp_now() + " ";
        buffer.reserve(data.size() + prefix.size());
        for (const char c : data) {
            if (at_line_start_) {
                buffer += prefix;
                at_line_start_ = false;
            }
            buffer.push_back(c);
            if (c == '\n') {
                at_line_start_ = true;
            }
        }
        payload = buffer;
    }
    if (spec_.max_bytes == 0) {
        write_all(payload);
        return;
    }
    // Fill the current file up to the limit, splitting at line boundaries where
    // possible; a single line longer than the limit is split hard.
    while (!payload.empty()) {
        const std::uint64_t room = spec_.max_bytes > size_ ? spec_.max_bytes - size_ : 0;
        if (payload.size() <= room) {
            write_all(payload);
            return;
        }
        const std::size_t newline = room == 0 ? std::string_view::npos : payload.rfind('\n', room - 1);
        if (newline != std::string_view::npos) {
            write_all(payload.substr(0, newline + 1));
            payload.remove_prefix(newline + 1);
        } else if (size_ == 0) {
            const auto chunk = static_cast<std::size_t>(room);
            write_all(payload.substr(0, chunk));
            payload.remove_prefix(chunk);
        }
        rotate();
    }
}

void RotatingLog::write_all(std::string_view data)
{
    while (!data.empty()) {
        const ssize_t written = ::write(fd_.get(), data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "write " + spec_.file.string());
        }
        const auto count = static_cast<std::size_t>(written);
        size_ += count;
        data.remove_prefix(count);
    }
}

}  // namespace pw
