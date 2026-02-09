/// @file static_files.hpp
/// @brief Streaming static file handler with path-traversal protection and ETags.
#pragma once

#include "ehs/http.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace ehs {

/// Content type for a file extension such as ".html"; "application/octet-stream" if unknown.
[[nodiscard]] std::string_view mime_type(std::string_view extension) noexcept;

/// Serves files below a root directory.
///
/// Paths containing "..", NUL or backslashes are rejected, and the resolved canonical path
/// must stay inside the root (this also defeats symlink escapes). Files are streamed in
/// fixed-size blocks so memory use does not depend on file size. Conditional requests with
/// If-None-Match are answered with 304.
class StaticFiles {
public:
    /// Throws std::invalid_argument when @p root is not a directory.
    explicit StaticFiles(const std::filesystem::path& root, std::string index_file = "index.html");

    /// Serves @p relative_path (as captured by a "*" route) for @p request.
    [[nodiscard]] Response serve(const Request& request, std::string_view relative_path) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    std::string index_file_;
};

}  // namespace ehs
