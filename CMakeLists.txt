cmake_minimum_required(VERSION 3.12)

project(obs-pulse-multichannel-input VERSION 0.1.0)

set(PLUGIN_AUTHOR "Norihiro Kamae")

set(MACOS_BUNDLEID "net.nagater.obs-pulse-multichannel-input")
set(MACOS_PACKAGE_UUID "161DB435-A476-4979-95B4-B028D6F3222D")
set(MACOS_INSTALLER_UUID "DE046344-8243-41D2-931D-DC0A11AB775E")
set(ID_PREFIX "net.nagater.obs-pulse-multichannel-input.")

set(LINUX_MAINTAINER_EMAIL "norihiro@nagater.net")

find_package(libobs REQUIRED)
include(cmake/ObsPluginHelpers.cmake)

configure_file(
	src/plugin-macros.h.in
	../src/plugin-macros.generated.h
)

configure_file(
	ci/ci_includes.sh.in
	../ci/ci_includes.generated.sh
)

set(PLUGIN_SOURCES
	src/plugin-main.c
	src/pulse-input-multichannel.c
	src/pulse-wrapper.c
)

add_library(${CMAKE_PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

target_link_libraries(${CMAKE_PROJECT_NAME}
	OBS::libobs
)

if(OS_WINDOWS)
	# Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
	if (MSVC)
		add_definitions(/MP /d2FH4-)
		add_definitions("-D_CRT_SECURE_NO_WARNINGS") # to avoid a warning for `strncpy`
	endif()

	target_link_libraries(${CMAKE_PROJECT_NAME} OBS::w32-pthreads)
endif()

if(OS_LINUX)
	target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

if(APPLE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
endif()

setup_plugin_target(${CMAKE_PROJECT_NAME})
