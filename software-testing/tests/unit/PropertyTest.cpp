// Property-based tests (RapidCheck).
//
// Technique: property-based testing. Instead of one hand-picked input and its
// expected output, each test states a rule that must hold for EVERY input,
// and RapidCheck generates the inputs (100 per property by default). When a
// property fails it shrinks the input to the smallest one that still fails
// and prints it together with a string that replays exactly that run:
//
//   RC_PARAMS="reproduce=<string from the failure>" ./build/unit_tests
//   RC_PARAMS="max_success=5000" ./build/unit_tests --gtest_filter='Property*'
//
// Why this goes beyond the example-based tests: defect D1 (see
// software-testing/docs/03-test-results.md) only showed for one head size
// combined with a few split points, which nobody had thought to write down.
// "The answer does not depend on how the bytes are split" is one line here,
// and the generator tries the sizes and split points for us.
//
// Built only when RapidCheck is installed (apt-get install librapidcheck-dev).

#include "FakeStore.hpp"
#include "miniLB/balancer/Backend.hpp"
#include "miniLB/balancer/LeastConnections.hpp"
#include "miniLB/balancer/RoundRobin.hpp"
#include "miniLB/balancer/WeightedRoundRobin.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/redis/Cache.hpp"
#include "miniLB/redis/RateLimiter.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using minilb::Backend;
using minilb::HttpParser;
using minilb::HttpRequest;
using minilb::ParseStatus;

namespace {

// ------------------------------ generators -----------------------------------

rc::Gen<std::string> textOf(const std::string& alphabet, int minLength, int maxLength) {
    return rc::gen::mapcat(rc::gen::inRange(minLength, maxLength + 1), [alphabet](int length) {
        return rc::gen::container<std::string>(static_cast<std::size_t>(length), rc::gen::elementOf(alphabet));
    });
}

const std::string kLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const std::string kNameChars = kLetters + "0123456789-_";
const std::string kPathChars = kLetters + "0123456789-_.~%";
const std::string kValueChars = kLetters + "0123456789 ,;=/()\"'*+-_.:";

// A syntactically valid HTTP/1.1 request in wire format.
rc::Gen<std::string> validRequest() {
    return rc::gen::exec([] {
        const std::string method =
            *rc::gen::elementOf(std::vector<std::string>{"GET", "HEAD", "POST", "PUT", "DELETE"});
        std::string target = "/" + *textOf(kPathChars, 0, 12);
        if (*rc::gen::inRange(0, 3) == 0) {
            target += "?" + *textOf(kPathChars + "&=", 0, 12);
        }
        std::string wire = method + " " + target + " HTTP/1.1\r\n";

        const int headers = *rc::gen::inRange(0, 6);
        for (int i = 0; i < headers; ++i) {
            // Values are trimmed by the parser, so generate them without
            // spaces at the ends to keep "what was sent" comparable.
            std::string value = *textOf(kValueChars, 0, 20);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            while (!value.empty() && value.back() == ' ') value.pop_back();
            wire += "X-" + *textOf(kNameChars, 1, 10) + ": " + value + "\r\n";
        }

        // Any bytes at all in the body, including ones that look like HTTP.
        const std::string body = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        if (!body.empty() || method == "POST" || method == "PUT") {
            wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        return wire + "\r\n" + body;
    });
}

// Cuts `wire` into pieces at generated positions.
std::vector<std::string> splitRandomly(const std::string& wire) {
    std::vector<std::size_t> cuts = *rc::gen::container<std::vector<std::size_t>>(
        rc::gen::inRange<std::size_t>(0, wire.size() + 1));
    cuts.push_back(0);
    cuts.push_back(wire.size());
    std::sort(cuts.begin(), cuts.end());
    std::vector<std::string> pieces;
    for (std::size_t i = 1; i < cuts.size(); ++i) {
        pieces.push_back(wire.substr(cuts[i - 1], cuts[i] - cuts[i - 1]));
    }
    return pieces;
}

// Everything observable about a parse, for comparing two runs.
struct Outcome {
    ParseStatus status = ParseStatus::NeedMore;
    int errorStatus = 0;
    std::string method, target, body;
    minilb::HeaderList headers;

    bool operator==(const Outcome& other) const {
        return status == other.status && errorStatus == other.errorStatus && method == other.method &&
               target == other.target && body == other.body && headers == other.headers;
    }
};

Outcome parse(const std::vector<std::string>& pieces, HttpParser::Limits limits = {}) {
    HttpParser parser(limits);
    for (const std::string& piece : pieces) {
        parser.feed(piece);
    }
    Outcome outcome;
    outcome.status = parser.feed("");   // the status after all pieces
    if (outcome.status == ParseStatus::Error) {
        outcome.errorStatus = parser.errorStatus();
    } else if (outcome.status == ParseStatus::Complete) {
        outcome.method = parser.request().method;
        outcome.target = parser.request().target;
        outcome.body = parser.request().body;
        outcome.headers = parser.request().headers;
    }
    return outcome;
}

struct Pool {
    std::vector<std::unique_ptr<Backend>> owned;
    std::vector<Backend*> all;

    explicit Pool(const std::vector<int>& weights) {
        for (std::size_t i = 0; i < weights.size(); ++i) {
            owned.push_back(std::make_unique<Backend>("10.0.0." + std::to_string(i + 1), 80, weights[i]));
            all.push_back(owned.back().get());
        }
    }
};

} // namespace

// =============================== HTTP parser =================================

// F14: "the parser accepts input in any fragmentation".
RC_GTEST_PROP(PropertyParser, ValidRequestsParseTheSameHoweverTheyAreSplit, ()) {
    const std::string wire = *validRequest();
    const Outcome whole = parse({wire});
    RC_ASSERT(whole.status == ParseStatus::Complete);
    RC_ASSERT(parse(splitRandomly(wire)) == whole);
}

// The same rule for ANY bytes, valid or not, and with limits small enough
// that generated requests land on both sides of them. This is the property
// that defect D1 violated: the head-size check gave a different answer
// depending on where the bytes were cut.
RC_GTEST_PROP(PropertyParser, AnyInputGetsTheSameVerdictHoweverItIsSplit, ()) {
    const bool startFromValid = *rc::gen::arbitrary<bool>();
    std::string wire = startFromValid ? *validRequest() : *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
    if (startFromValid && !wire.empty() && *rc::gen::arbitrary<bool>()) {
        // Damage one byte of a valid request: most interesting invalid
        // inputs are almost valid.
        wire[*rc::gen::inRange<std::size_t>(0, wire.size())] = *rc::gen::arbitrary<char>();
    }
    HttpParser::Limits limits;
    limits.maxHeaderBytes = *rc::gen::inRange<std::size_t>(1, 300);
    limits.maxBodyBytes = *rc::gen::inRange<std::size_t>(0, 150);

    const Outcome whole = parse({wire}, limits);
    const Outcome pieces = parse(splitRandomly(wire), limits);
    RC_CLASSIFY(whole.status == ParseStatus::Complete, "complete");
    RC_CLASSIFY(whole.status == ParseStatus::Error, "rejected");
    RC_ASSERT(pieces == whole);
}

// Robustness: whatever arrives, the parser ends in one of its three states,
// rejects only with a status miniLB documents, and never changes its mind.
RC_GTEST_PROP(PropertyParser, ArbitraryBytesAreHandledAndTheVerdictIsFinal, (const std::string& bytes, const std::string& more)) {
    HttpParser parser;
    const ParseStatus status = parser.feed(bytes);
    if (status == ParseStatus::Error) {
        const int code = parser.errorStatus();
        RC_ASSERT(code == 400 || code == 413 || code == 431 || code == 501);
        RC_ASSERT(!parser.errorMessage().empty());
    }
    if (status != ParseStatus::NeedMore) {
        RC_ASSERT(parser.feed(more) == status);
    }
}

// What miniLB sends to a backend is the request it parsed: serialising a
// parsed request and parsing it again changes nothing.
RC_GTEST_PROP(PropertyParser, SerialiseThenParseGivesTheSameRequest, ()) {
    const std::string wire = *validRequest();
    HttpParser first;
    RC_ASSERT(first.feed(wire) == ParseStatus::Complete);

    HttpParser second;
    RC_ASSERT(second.feed(first.request().serialize()) == ParseStatus::Complete);
    RC_ASSERT(second.request().method == first.request().method);
    RC_ASSERT(second.request().target == first.request().target);
    RC_ASSERT(second.request().headers == first.request().headers);
    RC_ASSERT(second.request().body == first.request().body);
}

// ============================ Balancing strategies ===========================

// F3: traffic in proportion to weight. Stronger than "roughly proportional":
// in ANY window of totalWeight consecutive picks, every backend is chosen
// exactly `weight` times.
RC_GTEST_PROP(PropertyBalancing, WeightedGivesEachBackendExactlyItsWeightInEveryCycle, ()) {
    const std::vector<int> weights =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(rc::gen::inRange(1, 7)));
    RC_PRE(weights.size() <= 8u);
    Pool pool(weights);
    minilb::WeightedRoundRobin strategy;
    const HttpRequest request;

    int totalWeight = 0;
    for (const int weight : weights) {
        totalWeight += weight;
    }
    const int offset = *rc::gen::inRange(0, 3 * totalWeight);   // start anywhere in the sequence
    for (int i = 0; i < offset; ++i) {
        strategy.select(pool.all, request);
    }

    std::map<Backend*, int> picks;
    for (int i = 0; i < totalWeight; ++i) {
        Backend* chosen = strategy.select(pool.all, request);
        RC_ASSERT(chosen != nullptr);
        ++picks[chosen];
    }
    for (Backend* backend : pool.all) {
        RC_ASSERT(picks[backend] == backend->weight);
    }
}

// F1: strict rotation. Any n consecutive picks visit each of n backends once.
RC_GTEST_PROP(PropertyBalancing, RoundRobinVisitsEveryBackendOncePerRound, ()) {
    const int count = *rc::gen::inRange(1, 9);
    Pool pool(std::vector<int>(static_cast<std::size_t>(count), 1));
    minilb::RoundRobin strategy;
    const HttpRequest request;

    const int offset = *rc::gen::inRange(0, 3 * count);
    for (int i = 0; i < offset; ++i) {
        strategy.select(pool.all, request);
    }
    std::map<Backend*, int> picks;
    for (int i = 0; i < count; ++i) {
        ++picks[strategy.select(pool.all, request)];
    }
    RC_ASSERT(picks.size() == static_cast<std::size_t>(count));
}

// F2: least connections never picks a backend that is busier than another.
RC_GTEST_PROP(PropertyBalancing, LeastConnectionsAlwaysPicksAMinimum, ()) {
    const std::vector<int> loads =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 50)));
    Pool pool(std::vector<int>(loads.size(), 1));
    for (std::size_t i = 0; i < loads.size(); ++i) {
        pool.all[i]->activeConnections = loads[i];
    }
    minilb::LeastConnections strategy;
    const HttpRequest request;

    const int minimum = *std::min_element(loads.begin(), loads.end());
    const int calls = *rc::gen::inRange(1, 10);   // the internal rotation must not matter
    for (int i = 0; i < calls; ++i) {
        Backend* chosen = strategy.select(pool.all, request);
        RC_ASSERT(chosen != nullptr);
        RC_ASSERT(chosen->activeConnections.load() == minimum);
    }
}

// All three: a strategy only ever returns a backend from the list it was
// given (so an unhealthy backend, which is not in the list, is never chosen).
RC_GTEST_PROP(PropertyBalancing, StrategiesOnlyChooseFromTheHealthyList, ()) {
    const std::vector<int> weights =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(rc::gen::inRange(1, 5)));
    Pool pool(weights);
    std::vector<Backend*> healthy;
    for (Backend* backend : pool.all) {
        if (*rc::gen::arbitrary<bool>()) {
            healthy.push_back(backend);
        }
    }
    minilb::RoundRobin roundRobin;
    minilb::LeastConnections leastConnections;
    minilb::WeightedRoundRobin weighted;
    const HttpRequest request;

    for (minilb::Strategy* strategy :
         std::vector<minilb::Strategy*>{&roundRobin, &leastConnections, &weighted}) {
        for (int i = 0; i < 5; ++i) {
            Backend* chosen = strategy->select(healthy, request);
            if (healthy.empty()) {
                RC_ASSERT(chosen == nullptr);
            } else {
                RC_ASSERT(std::find(healthy.begin(), healthy.end(), chosen) != healthy.end());
            }
        }
    }
}

// =============================== Rate limiter ================================

// F8, against a model: for any limit, window length, set of clients and
// sequence of request times, a request is allowed exactly when it is among
// the first `limit` of its client in its window, and a rejection says how
// long is left in that window.
RC_GTEST_PROP(PropertyRateLimiter, AgreesWithACountPerClientAndWindow, ()) {
    const int limit = *rc::gen::inRange(1, 8);
    const int window = *rc::gen::inRange(1, 90);
    FakeStore store;
    minilb::RateLimiter limiter(store, limit, window);

    std::map<std::pair<std::string, std::int64_t>, int> model;   // (client, window number) -> requests
    std::int64_t now = *rc::gen::inRange<std::int64_t>(0, 100000);
    const int requests = *rc::gen::inRange(1, 120);

    for (int i = 0; i < requests; ++i) {
        now += *rc::gen::inRange(0, window + 1);   // time never runs backwards
        const std::string client =
            *rc::gen::elementOf(std::vector<std::string>{"10.0.0.1", "10.0.0.2", "10.0.0.3"});

        const int seen = ++model[{client, now / window}];
        const minilb::RateLimiter::Decision decision = limiter.allow(client, now);

        RC_ASSERT(decision.allowed == (seen <= limit));
        if (!decision.allowed) {
            RC_ASSERT(decision.retryAfterSeconds == (now / window + 1) * window - now);
            RC_ASSERT(decision.retryAfterSeconds >= 1);
            RC_ASSERT(decision.retryAfterSeconds <= window);
        }
    }
}

// ================================== Cache ====================================

// F6: what comes out of the cache is what went in, for any body (including
// bytes that look like HTTP) and any headers.
RC_GTEST_PROP(PropertyCache, PutThenGetReturnsTheSameResponse, ()) {
    FakeStore store;
    minilb::Config::Cache settings;
    settings.enabled = true;
    settings.ttlSeconds = *rc::gen::inRange(1, 3600);
    settings.maxEntryBytes = 4096;
    minilb::Cache cache(store, settings);

    minilb::HttpResponse response;
    response.version = "HTTP/1.1";
    response.status = 200;
    response.reason = "OK";
    response.body = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
    RC_PRE(response.body.size() <= settings.maxEntryBytes);
    const int headers = *rc::gen::inRange(0, 5);
    for (int i = 0; i < headers; ++i) {
        std::string value = *textOf(kValueChars, 1, 20);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        while (!value.empty() && value.back() == ' ') value.pop_back();
        response.headers.emplace_back("X-" + *textOf(kNameChars, 1, 10), value);
    }
    response.setHeader("Content-Length", std::to_string(response.body.size()));
    RC_PRE(cache.isCacheableResponse(response));

    const std::string key = "cache:GET:h:/" + *textOf(kPathChars, 0, 10);
    cache.put(key, response);
    const std::optional<minilb::HttpResponse> cached = cache.get(key);

    RC_ASSERT(cached.has_value());
    RC_ASSERT(cached->status == 200);
    RC_ASSERT(cached->body == response.body);
    RC_ASSERT(cached->headers == response.headers);
    RC_ASSERT(store.ttls[key] == settings.ttlSeconds);
}
