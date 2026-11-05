#pragma once

#include "rei/core/error.hpp"

namespace rei {
    // Lets GTest show which error a failed std::expected carried.
    inline auto operator<<(std::ostream& out, const rei::error& error) -> std::ostream& {
        return out << error.code().category().name() << ':' << error.code().value() << " \""
                   << error.code().message() << "\" " << error.detail();
    }
} // namespace rei

// TODO: The has_error helper should probably be here too
