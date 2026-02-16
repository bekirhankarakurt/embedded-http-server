#include "test.hpp"

#include "ehs/static_files.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;
    TempTree() : root(fs::temp_directory_path() / ("ehs-static-" + std::to_string(::getpid()))) {
        fs::remove_all(root);
        fs::create_directories(root / "www" / "sub");
        std::ofstream(root / "www" / "index.html") << "<h1>home</h1>";
        std::ofstream(root / "www" / "app.js") << "console.log(1);";
        std::ofstream(root / "www" / "sub" / "index.html") << "sub";
        std::ofstream(root / "secret.txt") << "top secret";
        std::ofstream big(root / "www" / "big.bin", std::ios::binary);
        big << std::string(100000, 'z');
        big.close();
        fs::create_symlink(root / "secret.txt", root / "www" / "link.txt");
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;
};

ehs::Request get(std::string_view path) {
    ehs::Request r;
    r.method = ehs::Method::Get;
    r.target = std::string(path);
    r.path = r.target;
    return r;
}

std::string drain(ehs::Response& res) {
    std::string body;
    while (auto block = res.producer()) {
        body += *block;
    }
    return body;
}

}  // namespace

TEST(static_files, serves_files_with_mime_and_length) {
    const TempTree tree;
    const ehs::StaticFiles files(tree.root / "www");
    auto res = files.serve(get("/app.js"), "app.js");
    CHECK_EQ(ehs::code(res.status), 200);
    CHECK_EQ(res.headers.get("Content-Type").value_or(""), "text/javascript; charset=utf-8");
    REQUIRE(res.content_length.has_value());
    CHECK_EQ(*res.content_length, 15U);
    CHECK_EQ(drain(res), "console.log(1);");

    auto big = files.serve(get("/big.bin"), "big.bin");
    CHECK_EQ(drain(big).size(), 100000U);
}

TEST(static_files, directory_index) {
    const TempTree tree;
    const ehs::StaticFiles files(tree.root / "www");
    auto root = files.serve(get("/"), "");
    CHECK_EQ(drain(root), "<h1>home</h1>");
    auto sub = files.serve(get("/sub/"), "sub/");
    CHECK_EQ(drain(sub), "sub");
}

TEST(static_files, blocks_traversal_and_symlink_escape) {
    const TempTree tree;
    const ehs::StaticFiles files(tree.root / "www");
    CHECK_EQ(ehs::code(files.serve(get("/../secret.txt"), "../secret.txt").status), 403);
    CHECK_EQ(ehs::code(files.serve(get("/sub/../../secret.txt"), "sub/../../secret.txt").status), 403);
    CHECK_EQ(ehs::code(files.serve(get("/a\\b"), "a\\b").status), 403);
    CHECK_EQ(ehs::code(files.serve(get("/link.txt"), "link.txt").status), 403);
    CHECK_EQ(ehs::code(files.serve(get("/missing"), "missing").status), 404);
}

TEST(static_files, etag_conditional_and_methods) {
    const TempTree tree;
    const ehs::StaticFiles files(tree.root / "www");
    const auto first = files.serve(get("/app.js"), "app.js");
    const auto etag = std::string(first.headers.get("ETag").value_or(""));
    CHECK(!etag.empty());
    auto conditional = get("/app.js");
    conditional.headers.add("If-None-Match", etag);
    CHECK_EQ(ehs::code(files.serve(conditional, "app.js").status), 304);

    auto post = get("/app.js");
    post.method = ehs::Method::Post;
    CHECK_EQ(ehs::code(files.serve(post, "app.js").status), 405);
}

TEST(static_files, mime_types_and_bad_root) {
    CHECK_EQ(ehs::mime_type(".HTML"), "text/html; charset=utf-8");
    CHECK_EQ(ehs::mime_type(".unknown"), "application/octet-stream");
    bool threw = false;
    try {
        const ehs::StaticFiles files("/nonexistent/dir/for/ehs");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}
