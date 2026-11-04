#include "pw/rotating_log.hpp"
#include "test_support.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir()
    {
        std::string pattern = (fs::temp_directory_path() / "pw-log-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

pw::LogSpec spec(const fs::path& file, std::uint64_t max_bytes, unsigned max_files, bool timestamps)
{
    pw::LogSpec result;
    result.file = file;
    result.max_bytes = max_bytes;
    result.max_files = max_files;
    result.timestamps = timestamps;
    return result;
}

}  // namespace

TEST_CASE(creates_directories_and_appends)
{
    const TempDir dir;
    const fs::path file = dir.path() / "nested" / "svc.log";
    {
        pw::RotatingLog log(spec(file, 0, 3, false));
        log.write("hello\n");
    }
    pw::RotatingLog reopened(spec(file, 0, 3, false));
    CHECK_EQ(reopened.size(), 6U);
    reopened.write("world\n");
    CHECK_EQ(read_file(file), "hello\nworld\n");
}

TEST_CASE(rotates_and_keeps_max_files)
{
    const TempDir dir;
    const fs::path file = dir.path() / "svc.log";
    pw::RotatingLog log(spec(file, 10, 2, false));
    log.write("aaaaaaaa\n");
    log.write("bbbbbbbb\n");
    log.write("cccccccc\n");
    log.write("dddddddd\n");
    CHECK_EQ(read_file(file), "dddddddd\n");
    CHECK_EQ(read_file(log.rotated_path(1)), "cccccccc\n");
    CHECK_EQ(read_file(log.rotated_path(2)), "bbbbbbbb\n");
    CHECK(!fs::exists(log.rotated_path(3)));
}

TEST_CASE(zero_max_files_truncates_in_place)
{
    const TempDir dir;
    const fs::path file = dir.path() / "svc.log";
    pw::RotatingLog log(spec(file, 10, 0, false));
    log.write("aaaaaaaa\n");
    log.write("bbbbbbbb\n");
    CHECK_EQ(read_file(file), "bbbbbbbb\n");
    CHECK(!fs::exists(log.rotated_path(1)));
}

TEST_CASE(timestamps_prefix_each_line_across_writes)
{
    const TempDir dir;
    const fs::path file = dir.path() / "svc.log";
    pw::RotatingLog log(spec(file, 0, 1, true));
    log.write("one\ntw");
    log.write("o\nthree\n");
    const std::string content = read_file(file);
    std::istringstream lines(content);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        ++count;
        CHECK(line.size() > 24);
        CHECK_EQ(line[4], '-');
        CHECK_EQ(line[10], 'T');
        CHECK_EQ(line[23], ' ');
    }
    CHECK_EQ(count, 3);
    CHECK(content.find(" two\n") != std::string::npos);
}

TEST_CASE(large_chunks_are_split_at_line_boundaries)
{
    const TempDir dir;
    const fs::path file = dir.path() / "svc.log";
    pw::RotatingLog log(spec(file, 10, 5, false));
    log.write("aaaa\nbbbb\ncccc\n0123456789abcdef");
    CHECK_EQ(read_file(log.rotated_path(3)), "aaaa\nbbbb\n");
    CHECK_EQ(read_file(log.rotated_path(2)), "cccc\n");
    CHECK_EQ(read_file(log.rotated_path(1)), "0123456789");
    CHECK_EQ(read_file(file), "abcdef");
}

TEST_CASE(unwritable_path_throws)
{
    CHECK_THROWS(std::exception, pw::RotatingLog(spec("/proc/pw-cannot-write/x.log", 0, 1, false)));
}

TEST_MAIN()
