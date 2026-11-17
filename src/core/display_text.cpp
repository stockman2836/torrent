#include "torrent/core/utils.h"

namespace torrent::utils {

std::string displayText(const std::string& value, size_t maximum_bytes) {
    std::string result;
    for (size_t i = 0; i < value.size() && result.size() < maximum_bytes;) {
        const auto first = static_cast<unsigned char>(value[i]);
        size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = length && length <= value.size() - i;
        for (size_t j = 1; valid && j < length; ++j)
            valid = (static_cast<unsigned char>(value[i + j]) & 0xc0) == 0x80;
        if (valid && length >= 3) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            valid = !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
        }
        if (!valid) { result += '?'; ++i; continue; }
        if (length > maximum_bytes - result.size()) break;
        if (first < 0x20 || first == 0x7f) result += ' ';
        else result.append(value, i, length);
        i += length;
    }
    const auto first = result.find_first_not_of(' ');
    return first == std::string::npos ? "" : result.substr(first, result.find_last_not_of(' ') - first + 1);
}

} // namespace torrent::utils
