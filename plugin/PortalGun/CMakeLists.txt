cmake_minimum_required(VERSION 3.15)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS Src/*.cpp Src/*.h)

add_library(PortalGun SHARED ${SRC_FILES})
target_include_directories(PortalGun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Src)
target_link_libraries(PortalGun PRIVATE ZHMModSDK minhook)


# Delirium's portal code (Rust): built with cargo, linked in statically.
set(PORTAL_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../portal-core)
set(PORTAL_CORE_LIB ${PORTAL_CORE}/target/release/delirium_portal.lib)
add_custom_target(portal_core COMMAND cargo build --release WORKING_DIRECTORY ${PORTAL_CORE} BYPRODUCTS ${PORTAL_CORE_LIB} USES_TERMINAL)
add_dependencies(PortalGun portal_core)
target_include_directories(PortalGun PRIVATE ${PORTAL_CORE}/include)
target_link_libraries(PortalGun PRIVATE ${PORTAL_CORE_LIB} ws2_32 userenv ntdll bcrypt)

# Delirium (gta4nm.dll, the NM port): delay-loaded from mods/Delirium, so the mod runs without it.
set(DELIRIUM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../delirium-master CACHE PATH "delirium-physics checkout")
target_include_directories(PortalGun PRIVATE ${DELIRIUM_DIR}/rust/crates/gta4-nm-sdk/include)
target_link_libraries(PortalGun PRIVATE ${DELIRIUM_DIR}/rust/target/release/gta4nm.dll.lib delayimp)
target_link_options(PortalGun PRIVATE /DELAYLOAD:gta4nm.dll)
