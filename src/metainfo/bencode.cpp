#include "torrent/metainfo/bencode.h"
#include <cctype>
#include <sstream>

namespace torrent {

// Parse from string
BencodeValue BencodeParser::parse(const std::string& data) {
    BencodeParser parser(data);
    auto value = parser.parseValue();
    if (parser.hasMore()) throw BencodeError("Unexpected trailing data");
    return value;
}

// Parse from binary data
BencodeValue BencodeParser::parse(const std::vector<uint8_t>& data) {
    std::string str(data.begin(), data.end());
    return parse(str);
}

std::pair<BencodeValue, size_t> BencodeParser::parsePrefix(const std::vector<uint8_t>& data) {
    BencodeParser parser(std::string(data.begin(), data.end()));
    auto value = parser.parseValue();
    return {std::move(value), parser.pos_};
}

char BencodeParser::peek() const {
    if (!hasMore()) {
        throw BencodeError("Unexpected end of data");
    }
    return data_[pos_];
}

char BencodeParser::consume() {
    char c = peek();
    pos_++;
    return c;
}

BencodeValue BencodeParser::parseValue() {
    char c = peek();

    if (c == 'i') {
        return parseInteger();
    } else if (c == 'l') {
        return parseList();
    } else if (c == 'd') {
        return parseDictionary();
    } else if (c >= '0' && c <= '9') {
        return parseString();
    } else {
        throw BencodeError(std::string("Unexpected character: ") + c);
    }
}

BencodeValue BencodeParser::parseInteger() {
    consume(); // 'i'

    std::string num_str;
    while (peek() != 'e') {
        num_str += consume();
    }
    consume(); // 'e'

    if (num_str.empty()) {
        throw BencodeError("Empty integer");
    }

    // Check for invalid formats
    if (num_str.size() > 1 && num_str[0] == '0') {
        throw BencodeError("Invalid integer format (leading zero)");
    }
    if (num_str.size() >= 2 && num_str[0] == '-' && num_str[1] == '0') {
        throw BencodeError("Invalid integer format (-0)");
    }

    try {
        size_t consumed = 0;
        int64_t value = std::stoll(num_str, &consumed);
        if (consumed != num_str.size()) {
            throw BencodeError("Invalid integer: " + num_str);
        }
        return BencodeValue(value);
    } catch (const std::exception&) {
        throw BencodeError("Invalid integer: " + num_str);
    }
}

BencodeValue BencodeParser::parseString() {
    std::string length_str;
    while (peek() != ':') {
        if (peek() < '0' || peek() > '9') throw BencodeError("Invalid string length");
        length_str += consume();
    }
    consume(); // ':'

    size_t length;
    try {
        length = std::stoull(length_str);
    } catch (const std::exception&) {
        throw BencodeError("Invalid string length: " + length_str);
    }

    if (length > data_.size() - pos_) {
        throw BencodeError("String length exceeds available data");
    }

    std::string value = data_.substr(pos_, length);
    pos_ += length;

    return BencodeValue(std::move(value));
}

BencodeValue BencodeParser::parseList() {
    consume(); // 'l'

    BencodeValue::List list;
    while (peek() != 'e') {
        list.push_back(parseValue());
    }
    consume(); // 'e'

    return BencodeValue(std::move(list));
}

BencodeValue BencodeParser::parseDictionary() {
    consume(); // 'd'

    BencodeValue::Dictionary dict;
    while (peek() != 'e') {
        // Key must be a string
        BencodeValue key_value = parseString();
        if (!key_value.isString()) {
            throw BencodeError("Dictionary key must be a string");
        }
        std::string key = key_value.getString();

        // Value can be any type
        BencodeValue value = parseValue();

        dict[key] = std::move(value);
    }
    consume(); // 'e'

    return BencodeValue(std::move(dict));
}

// Encoding
std::string BencodeParser::encode(const BencodeValue& value) {
    std::ostringstream oss;

    if (value.isInteger()) {
        oss << "i" << value.getInteger() << "e";
    } else if (value.isString()) {
        const std::string& str = value.getString();
        oss << str.length() << ":" << str;
    } else if (value.isList()) {
        oss << "l";
        for (const auto& item : value.getList()) {
            oss << encode(item);
        }
        oss << "e";
    } else if (value.isDictionary()) {
        oss << "d";
        for (const auto& [key, val] : value.getDictionary()) {
            oss << key.length() << ":" << key;
            oss << encode(val);
        }
        oss << "e";
    }

    return oss.str();
}

} // namespace torrent
