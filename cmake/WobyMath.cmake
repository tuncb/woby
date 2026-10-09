include_guard(GLOBAL)

add_library(woby_math STATIC "${PROJECT_SOURCE_DIR}/src/math_utils.cpp")
target_compile_features(woby_math PUBLIC cxx_std_20)
target_include_directories(woby_math PUBLIC "${PROJECT_SOURCE_DIR}/src")
woby_enable_project_warnings(woby_math)
