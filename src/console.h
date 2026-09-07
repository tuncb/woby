#pragma once

namespace woby {

// Runtime adapter: attach to an existing terminal without allocating a console.
void initializeConsole();
[[nodiscard]] bool hasStandardError();

} // namespace woby
