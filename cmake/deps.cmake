include(FetchContent)

# Unicorn: static, ARM only.
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        unicorn
        GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
        GIT_TAG 2.1.4
        GIT_SHALLOW TRUE
)

FetchContent_Declare(
        tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
)

FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(unicorn tomlplusplus doctest)
