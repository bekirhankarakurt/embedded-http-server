// Fuzz-style robustness tests: random fragmentation, mutation and garbage input must never
// crash, hang or violate parser invariants. Deterministic seeds keep failures reproducible.
#include "test.hpp"

#include "ehs/json.hpp"
#include "ehs/parser.hpp"
#include "ehs/session.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

const std::vector<std::string>& corpus() {
    static const std::vector<std::string> seeds = {
        "GET / HTTP/1.1\r\nHost: a\r\n\r\n",
        "POST /api/config HTTP/1.1\r\nHost: a\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n"
        "{\"a\":[1,2,3]}",
        "PUT /x HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n1;x=y\r\nd\r\n0\r\nT: v\r\n\r\n",
        "GET /a%20b?q=%41 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        "OPTIONS * HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n",
    };
    return seeds;
}

std::string mutate(std::string input, std::mt19937& rng) {
    std::uniform_int_distribution<int> op(0, 3);
    std::uniform_int_distribution<int> byte(0, 255);
    const int edits = 1 + static_cast<int>(rng() % 4U);
    for (int i = 0; i < edits && !input.empty(); ++i) {
        const auto pos = static_cast<std::size_t>(rng() % input.size());
        switch (op(rng)) {
            case 0: input[pos] = static_cast<char>(byte(rng)); break;
            case 1: input.erase(pos, 1); break;
            case 2: input.insert(pos, 1, static_cast<char>(byte(rng))); break;
            default: input.insert(pos, input.substr(pos, std::min<std::size_t>(8, input.size() - pos))); break;
        }
    }
    return input;
}

// Feeds @p text in random fragments; returns {complete, failed}.
std::pair<bool, bool> feed_fragmented(std::string_view text, std::mt19937& rng) {
    ehs::RequestParser parser;
    std::size_t pos = 0;
    while (pos < text.size() && !parser.complete() && !parser.failed()) {
        const auto len = std::min<std::size_t>(1 + rng() % 7U, text.size() - pos);
        const auto used = parser.feed(text.substr(pos, len));
        if (used > len) {
            return {false, true};
        }
        pos += used;
        if (used < len && !parser.complete() && !parser.failed()) {
            return {false, true};
        }
    }
    return {parser.complete(), parser.failed()};
}

}  // namespace

TEST(fuzz, fragmentation_is_transparent) {
    std::mt19937 rng(1234);
    for (const auto& seed : corpus()) {
        ehs::RequestParser whole;
        whole.feed(seed);
        for (int i = 0; i < 200; ++i) {
            const auto [complete, failed] = feed_fragmented(seed, rng);
            CHECK_EQ(complete, whole.complete());
            CHECK_EQ(failed, whole.failed());
        }
    }
}

TEST(fuzz, mutated_requests_never_crash) {
    std::mt19937 rng(42);
    ehs::Limits limits;
    limits.max_body = 256;
    for (int i = 0; i < 20000; ++i) {
        const auto& seed = corpus()[static_cast<std::size_t>(i) % corpus().size()];
        const auto input = mutate(seed, rng);
        ehs::RequestParser parser(limits);
        const auto used = parser.feed(input);
        CHECK(used <= input.size());
        CHECK(!(parser.complete() && parser.failed()));
        if (parser.complete()) {
            const auto request = parser.take();
            CHECK(request.body.size() <= limits.max_body);
            CHECK(!request.target.empty());
        }
    }
}

TEST(fuzz, random_garbage_through_session) {
    std::mt19937 rng(7);
    const ehs::Handler handler = [](ehs::Request& r) { return ehs::Response::text(ehs::Status::Ok, r.path); };
    ehs::SessionOptions options;
    options.send_date = false;
    for (int i = 0; i < 2000; ++i) {
        std::string junk(1 + rng() % 512U, '\0');
        for (auto& c : junk) {
            c = static_cast<char>(rng() % 256U);
        }
        ehs::Session session(handler, options);
        session.receive(junk);
        session.receive_eof();
        std::size_t guard = 0;
        while (!session.output().empty() && guard++ < 1000) {
            session.consume(session.output().size());
            session.pump();
        }
        CHECK(session.done());
    }
}

TEST(fuzz, json_mutations_never_crash) {
    std::mt19937 rng(99);
    const std::string seed = R"({"a":[1,-2.5e3,true,false,null],"b":{"c":"\u00e9\ud83d\ude00\n"},"d":""})";
    for (int i = 0; i < 20000; ++i) {
        const auto input = mutate(seed, rng);
        std::string error;
        const auto doc = ehs::Json::parse(input, &error);
        if (doc) {
            const auto again = ehs::Json::parse(doc->dump());
            REQUIRE(again.has_value());
            CHECK(*again == *doc);
        } else {
            CHECK(!error.empty());
        }
    }
}

TEST(fuzz, deep_nesting_is_bounded) {
    const std::string deep(100000, '[');
    std::string error;
    CHECK(!ehs::Json::parse(deep, &error).has_value());
    CHECK(error.find("deep") != std::string::npos);
}
