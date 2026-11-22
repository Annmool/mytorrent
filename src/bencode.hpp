#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bt {
inline constexpr std::size_t metadata_limit = 8 * 1024 * 1024;
inline constexpr std::size_t bencode_value_limit = 250000;
struct Value {
    using List = std::vector<Value>;
    using Dict = std::map<std::string, Value, std::less<>>;
    std::variant<std::int64_t, std::string, List, Dict> data;
    std::size_t begin{};
    std::size_t end{};

    const Dict& dict() const;
    const List& list() const;
    const std::string& bytes() const;
    std::int64_t integer() const;
    const Value& at(std::string_view key) const;
};

Value decode(std::string_view input);
std::string encode(const Value& value);
}
