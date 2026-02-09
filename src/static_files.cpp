#include "ehs/static_files.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ehs {

namespace {

constexpr std::size_t kBlockSize = 16384;

struct MimeEntry {
    std::string_view extension;
    std::string_view type;
};

constexpr std::array<MimeEntry, 18> kMimeTypes{{
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".txt", "text/plain; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},
    {".woff2", "font/woff2"},
    {".wasm", "application/wasm"},
    {".pdf", "application/pdf"},
    {".xml", "application/xml"},
    {".gz", "application/gzip"},
}};

[[nodiscard]] bool safe_relative(std::string_view path) noexcept {
    if (path.find('\0') != std::string_view::npos || path.find('\\') != std::string_view::npos) {
        return false;
    }
    while (!path.empty()) {
        const auto slash = path.find('/');
        if (path.substr(0, slash) == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        path.remove_prefix(slash + 1);
    }
    return true;
}

[[nodiscard]] bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end() || *r != *c) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string to_hex(std::uint64_t value) {
    std::array<char, 20> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, 16);
    static_cast<void>(ec);
    return std::string(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
}

}  // namespace

std::string_view mime_type(std::string_view extension) noexcept {
    for (const auto& entry : kMimeTypes) {
        if (iequals(entry.extension, extension)) {
            return entry.type;
        }
    }
    return "application/octet-stream";
}

StaticFiles::StaticFiles(const std::filesystem::path& root, std::string index_file)
    : index_file_(std::move(index_file)) {
    std::error_code ec;
    root_ = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(root_, ec)) {
        throw std::invalid_argument("static root is not a directory: " + root.string());
    }
}

Response StaticFiles::serve(const Request& request, std::string_view relative_path) const {
    if (request.method != Method::Get && request.method != Method::Head) {
        auto response = Response::error(Status::MethodNotAllowed);
        response.headers.set("Allow", "GET, HEAD");
        return response;
    }
    if (!safe_relative(relative_path)) {
        return Response::error(Status::Forbidden);
    }
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(root_ / std::filesystem::path(relative_path), ec);
    if (ec || !is_within(root_, path)) {
        return Response::error(Status::Forbidden);
    }
    if (std::filesystem::is_directory(path, ec)) {
        path /= index_file_;
        path = std::filesystem::weakly_canonical(path, ec);
        if (ec || !is_within(root_, path)) {
            return Response::error(Status::Forbidden);
        }
    }
    if (!std::filesystem::is_regular_file(path, ec)) {
        return Response::error(Status::NotFound);
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Response::error(Status::NotFound);
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
    const std::string etag = "\"" + to_hex(size) + "-" + to_hex(ticks) + "\"";

    if (const auto match = request.headers.get("If-None-Match"); match && (*match == etag || *match == "*")) {
        Response response;
        response.status = Status::NotModified;
        response.headers.set("ETag", etag);
        return response;
    }

    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!file->is_open()) {
        return Response::error(Status::Forbidden);
    }
    Response response;
    response.headers.set("Content-Type", std::string(mime_type(path.extension().string())));
    response.headers.set("ETag", etag);
    response.headers.set("Cache-Control", "no-cache");
    response.headers.set("X-Content-Type-Options", "nosniff");
    response.content_length = size;
    response.producer = [file]() -> std::optional<std::string> {
        std::string block(kBlockSize, '\0');
        file->read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto got = file->gcount();
        if (got <= 0) {
            return std::nullopt;
        }
        block.resize(static_cast<std::size_t>(got));
        return block;
    };
    return response;
}

}  // namespace ehs
