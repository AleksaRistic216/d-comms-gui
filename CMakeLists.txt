cmake_minimum_required(VERSION 3.28)
project(dcomms_addon CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

# All code that ends up in the shared .node file must be position-independent
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# cmake-js sets CMAKE_JS_INC (Node.js headers) and CMAKE_JS_LIB (node library)
include_directories(${CMAKE_JS_INC})

# Find node-addon-api include path
execute_process(
    COMMAND node -p "require('node-addon-api').include"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE NODE_ADDON_API_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX REPLACE "\"" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")

# Download d-comms via FetchContent; EXCLUDE_FROM_ALL avoids building the
# library's test executables as part of our addon build.
include(FetchContent)
FetchContent_Declare(d_comms
    GIT_REPOSITORY https://github.com/AleksaRistic216/d-comms.git
    GIT_TAG        master
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(d_comms)

# Build the N-API native addon
add_library(dcomms_addon SHARED src/addon.cpp)

set_target_properties(dcomms_addon PROPERTIES
    PREFIX ""
    SUFFIX ".node"
)

target_include_directories(dcomms_addon PRIVATE
    ${CMAKE_JS_INC}
    ${NODE_ADDON_API_DIR}
    ${d_comms_SOURCE_DIR}/src
)

target_compile_definitions(dcomms_addon PRIVATE NAPI_VERSION=8)

target_link_libraries(dcomms_addon PRIVATE
    ${CMAKE_JS_LIB}
    dcomms_core
)

# On MinGW, statically link the GCC, C++, and winpthread runtimes into the
# .node shared library so the packaged app has no external DLL dependencies
# (libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll are not present
# on a stock Windows installation).
if(MINGW)
    target_link_options(dcomms_addon PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,-lwinpthread,-Bdynamic
    )
endif()
