#include "bencode.hpp"

#include <charconv>
#include <stdexcept>
#include <type_traits>

namespace bt {
namespace {
template<class T>
const T& as(const Value& value) {
    if (const auto* result = std::get_if<T>(&value.data)) return *result;
    throw std::runtime_error("Unexpected bencode value type");
}

class Parser {
    std::string_view input;
    std::size_t pos{};
    std::size_t nodes{};

    [[noreturn]] void fail() const {
        throw std::runtime_error("Invalid bencode at byte " + std::to_string(pos));
    }

    char peek() const {
        if (pos == input.size()) fail();
        return input[pos];
    }

    template<class T>
    T number(char delimiter) {
        const auto end = input.find(delimiter, pos);
        if (end == input.npos) fail();
        const auto token = input.substr(pos, end - pos);
        const auto digits = token.starts_with('-') ? token.substr(1) : token;
        if (digits.empty() || (digits.starts_with('0') && token != "0")) fail();
        T result{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) fail();
        pos = end + 1;
        return result;
    }

    std::string bytes() {
        const auto size = number<std::size_t>(':');
        if (size > input.size() - pos) fail();
        auto result = std::string(input.substr(pos, size));
        pos += size;
        return result;
    }

    Value parse(unsigned depth) {
        if (depth > 48 || ++nodes > 250000) fail();
        Value value;
        value.begin = pos;
        const char type = peek();
        if (type == 'i') {
            ++pos;
            value.data = number<std::int64_t>('e');
        } else if (type == 'l') {
            ++pos;
            Value::List list;
            while (peek() != 'e') list.push_back(parse(depth + 1));
            ++pos;
            value.data = std::move(list);
        } else if (type == 'd') {
            ++pos;
            Value::Dict dict;
            while (peek() != 'e') {
                auto key = bytes();
                if (!dict.empty() && dict.rbegin()->first >= key) fail();
                auto entry = parse(depth + 1);
                dict.emplace(std::move(key), std::move(entry));
            }
            ++pos;
            value.data = std::move(dict);
        } else {
            value.data = bytes();
        }
        value.end = pos;
        return value;
    }

public:
    explicit Parser(std::string_view source) : input(source) {}
    Value run() {
        if (input.size() > metadata_limit) fail();
        auto result = parse(0);
        if (pos != input.size()) fail();
        return result;
    }
};

void append_bytes(std::string& out, std::string_view bytes) {
    out += std::to_string(bytes.size());
    out += ':';
    out += bytes;
}

void append(std::string& out, const Value& value) {
    std::visit([&out](const auto& data) {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
            out += 'i';
            out += std::to_string(data);
            out += 'e';
        } else if constexpr (std::is_same_v<T, std::string>) {
            append_bytes(out, data);
        } else if constexpr (std::is_same_v<T, Value::List>) {
            out += 'l';
            for (const auto& item : data) append(out, item);
            out += 'e';
        } else {
            out += 'd';
            for (const auto& [key, item] : data) {
                append_bytes(out, key);
                append(out, item);
            }
            out += 'e';
        }
    }, value.data);
}
}

const Value::Dict& Value::dict() const { return as<Dict>(*this); }
const Value::List& Value::list() const { return as<List>(*this); }
const std::string& Value::bytes() const { return as<std::string>(*this); }
std::int64_t Value::integer() const { return as<std::int64_t>(*this); }
const Value& Value::at(std::string_view key) const {
    const auto& fields = dict();
    const auto found = fields.find(key);
    if (found == fields.end()) throw std::runtime_error("Missing field: " + std::string(key));
    return found->second;
}

Value decode(std::string_view input) { return Parser(input).run(); }
std::string encode(const Value& value) {
    std::string out;
    append(out, value);
    return out;
}
}
