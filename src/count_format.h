#pragma once

#include <cstdint>
#include <string>

namespace woby {

// Use the same digit groups on all systems, independent of the current locale.
[[nodiscard]] inline std::string formatCount(uint64_t count)
{
    auto text = std::to_string(count);
    for (size_t end = text.size(); end > 3; end -= 3) {
        text.insert(end - 3, 1, ',');
    }
    return text;
}

} // namespace woby
