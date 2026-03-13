# Root ESP-IDF CMake entrypoint.
# These boilerplate lines must stay in this order for ESP-IDF tooling.
cmake_minimum_required(VERSION 3.16)

# Register non-default component locations (we use `src` instead of `main`).
set(EXTRA_COMPONENT_DIRS "src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Tersano_ESP_Challenge)
