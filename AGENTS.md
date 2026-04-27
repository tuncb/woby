# Task completion rules

Any code change requires:
- Build woby project in debug mode, the build shoud succeed without any warnings.
- If there is alrady woby open, terminate the process and re-try. 

# Build rules

- For local development, use vs2026-vcpkg preset instead of ninja + vcpkg.
- CI flow should depend on ninja + vcpkg.

# Code style rules

- Use a procedural programming style. Structs + free functions.
- No inheritance, no private or protected members.
