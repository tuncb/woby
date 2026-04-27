# Task completion rules

Any code change requires:
- Build woby project in debug mode, the build shoud succeed without any warnings.
- If there is alrady woby open, terminate the process and re-try. 

# Build rules

- For local development, use visual studio back end instead of ninja.
- CI flow should depend on ninja.

# Code style rules

- Use a procedural programming style. Structs + free functions.
- No inheritance, no private or protected members.
