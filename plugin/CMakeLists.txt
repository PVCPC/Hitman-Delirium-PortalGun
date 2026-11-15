cmake_minimum_required(VERSION 3.15)
project(HitmanPortalMod CXX)

set(CMAKE_CXX_STANDARD 23)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(ZHM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../HitmanPortalMod/sdk/DevPkg" CACHE PATH "ZHMModSDK DevPkg folder (from the SDK release).")
set(GAME_INSTALL_PATH "F:/SteamLibrary/steamapps/common/HITMAN 3" CACHE PATH "Path to Hitman 3 installation folder.")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8 /Zc:preprocessor)

include(FetchContent)
FetchContent_Declare(DirectXHeaders
        GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
        GIT_TAG v1.614.1
        GIT_SHALLOW TRUE)
set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(DirectXHeaders)

# MinHook: to keep the game's own death ragdoll off characters Delirium is driving.
FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.3
        GIT_SHALLOW TRUE)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()
enable_language(C)
file(GLOB MINHOOK_SRC ${minhook_SOURCE_DIR}/src/*.c ${minhook_SOURCE_DIR}/src/hde/hde64.c)
add_library(minhook STATIC ${MINHOOK_SRC})
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_subdirectory(${ZHM_SDK_DIR} ZHMModSDK)
target_link_libraries(ZHMModSDK INTERFACE Microsoft::DirectX-Headers implot)
include(${ZHM_SDK_DIR}/cmake/zhm-mod.cmake)

add_subdirectory(PortalGun)
