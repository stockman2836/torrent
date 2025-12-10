#include "magnet_uri.h"
#include "utils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace torrent {

bool MagnetURI::isMagnetURI(const std::string& uri) {
    return uri.find("magnet:?") == 0;
}

MagnetURI MagnetURI::parse(const std::string& uri) {
    MagnetURI magnet;

    if (!isMagnetURI(uri)) {
        return magnet; // Invalid
    }

    // Extract query string after "magnet:?"
    std::string query = uri.substr(8); // Skip "magnet:?"

    // Split by '&' to get parameters
    std::istringstream iss(query);
    std::string param;

    while (std::getline(iss, param, '&')) {
        // Find '=' separator
        size_t eq_pos = param.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = param.substr(0, eq_pos);
        std::string value = param.substr(eq_pos + 1);

        // URL decode value
        value = urlDecode(value);

        if (key == "xt") {
            // Exact Topic (info hash)
            // Format: urn:btih:<hash> or urn:btmh:<hash>
            if (value.find("urn:btih:") == 0) {
                std::string hash_str = value.substr(9); // Skip "urn:btih:"
                magnet.info_hash_ = parseInfoHash(hash_str);
            }
        }
        else if (key == "dn") {
            // Display Name
            magnet.display_name_ = value;
        }
        else if (key == "tr") {
            // Tracker URL
            magnet.trackers_.push_back(value);
        }
        else if (key == "as" || key == "ws") {
            // Acceptable Source / Web Seed
            magnet.web_seeds_.push_back(value);
        }
        else if (key == "xl") {
            // Exact Length
            try {
                magnet.exact_length_ = std::stoll(value);
            } catch (...) {
                // Ignore invalid length
            }
        }
    }

    return magnet;
}

std::string MagnetURI::infoHashHex() const {
    return utils::toHex(info_hash_);
}

std::string MagnetURI::toString() const {
    if (!isValid()) {
        return "";
    }

    std::ostringstream oss;
    oss << "magnet:?xt=urn:btih:" << infoHashHex();

    if (!display_name_.empty()) {
        // URL encode display name
        oss << "&dn=";
        for (char c : display_name_) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            } else if (c == ' ') {
                oss << '+';
            } else {
                oss << '%' << std::hex << std::setw(2) << std::setfill('0')
                    << (int)(unsigned char)c;
            }
        }
        oss << std::dec;
    }

    for (const auto& tracker : trackers_) {
        oss << "&tr=" << tracker; // Already URL encoded
    }

    for (const auto& ws : web_seeds_) {
        oss << "&ws=" << ws;
    }

    if (exact_length_.has_value()) {
        oss << "&xl=" << exact_length_.value();
    }

    return oss.str();
}

std::string MagnetURI::urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            // Hex encoded character
            std::string hex = str.substr(i + 1, 2);
            try {
                char decoded = static_cast<char>(std::stoi(hex, nullptr, 16));
                result += decoded;
                i += 2;
            } catch (...) {
                result += str[i];
            }
        }
        else if (str[i] == '+') {
            result += ' ';
        }
        else {
            result += str[i];
        }
    }

    return result;
}

std::vector<uint8_t> MagnetURI::parseInfoHash(const std::string& hash_str) {
    std::vector<uint8_t> hash;

    // Remove any whitespace
    std::string cleaned;
    for (char c : hash_str) {
        if (!std::isspace(c)) {
            cleaned += c;
        }
    }

    if (cleaned.length() == 40) {
        // Hex format (40 characters = 20 bytes)
        try {
            for (size_t i = 0; i < 40; i += 2) {
                std::string byte_str = cleaned.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
                hash.push_back(byte);
            }
        } catch (...) {
            hash.clear();
        }
    }
    else if (cleaned.length() == 32) {
        // Base32 format (32 characters = 20 bytes)
        // Base32 decoding
        static const char base32_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        std::string uppercase = cleaned;
        std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), ::toupper);

        try {
            uint64_t buffer = 0;
            int bits_in_buffer = 0;

            for (char c : uppercase) {
                const char* pos = std::strchr(base32_chars, c);
                if (!pos) {
                    hash.clear();
                    break;
                }

                int value = pos - base32_chars;
                buffer = (buffer << 5) | value;
                bits_in_buffer += 5;

                if (bits_in_buffer >= 8) {
                    bits_in_buffer -= 8;
                    hash.push_back((buffer >> bits_in_buffer) & 0xFF);
                }
            }
        } catch (...) {
            hash.clear();
        }
    }

    return hash;
}

} // namespace torrent
