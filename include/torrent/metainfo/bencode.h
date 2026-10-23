#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <stdexcept>

namespace torrent {

// Bencode value types: integer, string, list, dictionary
class BencodeValue {
public:
    using Integer = int64_t;
    using String = std::string;
    using List = std::vector<BencodeValue>;
    using Dictionary = std::map<std::string, BencodeValue>;

    using Value = std::variant<Integer, String, List, Dictionary>;

    BencodeValue() = default;
    BencodeValue(Integer i) : value_(i) {}
    BencodeValue(const String& s) : value_(s) {}
    BencodeValue(String&& s) : value_(std::move(s)) {}
    BencodeValue(const List& l) : value_(l) {}
    BencodeValue(List&& l) : value_(std::move(l)) {}
    BencodeValue(const Dictionary& d) : value_(d) {}
    BencodeValue(Dictionary&& d) : value_(std::move(d)) {}

    // Type checkers
    bool isInteger() const { return std::holds_alternative<Integer>(value_); }
    bool isString() const { return std::holds_alternative<String>(value_); }
    bool isList() const { return std::holds_alternative<List>(value_); }
    bool isDictionary() const { return std::holds_alternative<Dictionary>(value_); }

    // Getters
    Integer getInteger() const { return std::get<Integer>(value_); }
    const String& getString() const { return std::get<String>(value_); }
    const List& getList() const { return std::get<List>(value_); }
    const Dictionary& getDictionary() const { return std::get<Dictionary>(value_); }

    // Mutable getters
    String& getString() { return std::get<String>(value_); }
    List& getList() { return std::get<List>(value_); }
    Dictionary& getDictionary() { return std::get<Dictionary>(value_); }

private:
    Value value_;
};

// Bencode parser
class BencodeParser {
public:
    static BencodeValue parse(const std::string& data);
    static BencodeValue parse(const std::vector<uint8_t>& data);

    static std::string encode(const BencodeValue& value);

private:
    BencodeParser(const std::string& data) : data_(data), pos_(0) {}

    BencodeValue parseValue();
    BencodeValue parseInteger();
    BencodeValue parseString();
    BencodeValue parseList();
    BencodeValue parseDictionary();

    char peek() const;
    char consume();
    bool hasMore() const { return pos_ < data_.size(); }

    std::string data_;
    size_t pos_;
};

class BencodeError : public std::runtime_error {
public:
    explicit BencodeError(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace torrent
