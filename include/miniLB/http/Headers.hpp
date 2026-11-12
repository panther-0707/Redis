#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minilb {

// HTTP headers as an ordered list of (name, value) pairs.
//
// A vector (rather than a map) keeps the original order and allows repeated
// headers such as Set-Cookie. Messages have few headers, so a linear search
// is fast enough. Header names are case-insensitive (RFC 9110), so all
// lookups below ignore case.
using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive ASCII string comparison.
bool iequals(std::string_view a, std::string_view b);

// Returns a pointer to the first value for `name`, or nullptr if absent.
const std::string* findHeader(const HeaderList& headers, std::string_view name);

// Replaces every existing `name` header with a single one holding `value`.
void setHeader(HeaderList& headers, std::string_view name, std::string_view value);

// Removes every header called `name`.
void removeHeader(HeaderList& headers, std::string_view name);

// Removes "hop-by-hop" headers. These describe a single TCP connection
// (client<->proxy or proxy<->backend), so a proxy must not forward them.
void removeHopByHopHeaders(HeaderList& headers);

} // namespace minilb
