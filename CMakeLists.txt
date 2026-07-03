cmake_minimum_required(VERSION 3.16)


project(XDAQ-RHX-Debug VERSION 1.4.3 LANGUAGES CXX)

if(MSVC)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(__PRETTY_FUNCTION__=__FUNCSIG__)

    add_compile_options(/Zi)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
elseif(
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
    (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
    # https://gitlab.kitware.com/cmake/cmake/-/issues/25297
    # CMake 3.29
    add_link_options(LINKER:-no_warn_duplicate_libraries)
endif()

SET(CMAKE_MODULE_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake
    ${CMAKE_MODULE_PATH}
)


# Compiler flags and dependencies

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenCL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Multimedia Network Widgets Xml)
find_package(xdaq REQUIRED)
find_package(crashpad REQUIRED)

include(FetchContent)
FetchContent_Declare(
    cpptrace
    GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
    GIT_TAG v1.0.4
)
set(CPPTRACE_UNWIND_WITH_FRAME_POINTER ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpptrace)

# Build
add_subdirectory(Engine)
add_subdirectory(GUI)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(ICON_NAME "xdaq-icon.icns")
else()
    set(ICON_NAME "xdaq-icon.ico")
endif()
set(ICON_PATH ${PROJECT_SOURCE_DIR}/images/${ICON_NAME})

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    enable_language("RC")
    set (WIN32_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/XDAQ-RHX.rc)
endif()

add_executable(
    XDAQ-RHX WIN32 MACOSX_BUNDLE
    IntanRHX.qrc
    main.cpp
    ${EngineSources}
    ${GUISources}
    ${ICON_PATH}
    ${WIN32_RESOURCES}
)

target_compile_definitions(XDAQ-RHX PRIVATE
	XDAQ_RHX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	XDAQ_RHX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	XDAQ_RHX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_include_directories(XDAQ-RHX PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/API/Abstract
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/API/Hardware
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/API/Synthetic
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Processing
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Processing/DataFileReaders
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Processing/SaveManagers
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Processing/XPUInterfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Threads
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Windows
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/GUI/Dialogs
)

target_compile_features(XDAQ-RHX PRIVATE cxx_std_23)
target_compile_options(XDAQ-RHX PRIVATE $<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-deprecated>)
target_compile_options(XDAQ-RHX PRIVATE $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-omit-frame-pointer>)
if(MSVC)
    # /EHsc: enable C++ exception unwinding so std::set_terminate fires for unhandled throws
    target_compile_options(XDAQ-RHX PRIVATE /EHsc)
    # /Zi: emit debug info into a PDB; /FS: serialize PDB writes (required for parallel Ninja builds)
    target_compile_options(XDAQ-RHX PRIVATE $<$<CONFIG:Debug>:/Zi /FS>)
    # /DEBUG: produce a PDB for the linked binary; place it next to the exe
    target_link_options(XDAQ-RHX PRIVATE
        $<$<CONFIG:Debug>:/DEBUG /PDB:$<TARGET_FILE_DIR:XDAQ-RHX>/XDAQ-RHX.pdb>
    )
endif()
target_compile_definitions(XDAQ-RHX
    PUBLIC
        $<$<CONFIG:Debug>:DEBUG>
    PRIVATE
	    $<$<PLATFORM_ID:Linux>:OS_LINUX>
	    $<$<PLATFORM_ID:Windows>:OS_WINDOWS>
	    $<$<PLATFORM_ID:Darwin>:OS_MACOS>
)

option(Sanitize "Enable sanitizers" OFF)
if(Sanitize)
    target_compile_options(XDAQ-RHX PRIVATE -fsanitize=address,undefined)
    target_link_options(XDAQ-RHX PRIVATE -fsanitize=address,undefined)
endif()

target_link_libraries(XDAQ-RHX PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    Qt6::Widgets
    Qt6::Xml
    OpenCL::OpenCL
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    xdaq::xdaq_device
    cpptrace::cpptrace
    crashpad::crashpad
)

# Install
include(deployQt)


if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    install(TARGETS XDAQ-RHX DESTINATION .)
    xdaq_install_core(. .)
    xdaq_install_device_managers(./managers)
    install(FILES
        ${PROJECT_SOURCE_DIR}/kernel.cl
        "$<TARGET_FILE:OpenCL::OpenCL>"
        DESTINATION .
    )

    # Ship the PDB so cpptrace can resolve symbols on the target machine.
    # /DEBUG is set unconditionally above, so a PDB exists in every config.
    install(FILES $<TARGET_PDB_FILE:XDAQ-RHX> DESTINATION .)

    # crashpad_handler.exe is copied into the build dir by conanfile.py generate();
    # StartHandler() launches it at runtime, so it must ship next to XDAQ-RHX.exe.
    install(PROGRAMS ${CMAKE_BINARY_DIR}/crashpad_handler.exe DESTINATION .)

    deployqt($<TARGET_FILE_NAME:XDAQ-RHX>)

    # Dev convenience: mirror xdaq_install_core/xdaq_install_device_managers next to
    # the raw build output, so XDAQ-RHX.exe can run straight out of the build
    # directory without a full `cmake --install` step.
    file(GLOB XDAQ_CORE_DLLS "${XDAQ_ROOT_DIR}/bin/*.dll")
    file(GLOB XDAQ_MANAGER_DLLS "${XDAQ_DEVICE_MANAGER_DIR}/*.dll")
    add_custom_command(TARGET XDAQ-RHX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:XDAQ-RHX>/managers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${XDAQ_CORE_DLLS} "$<TARGET_FILE_DIR:XDAQ-RHX>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${XDAQ_MANAGER_DLLS} "$<TARGET_FILE_DIR:XDAQ-RHX>/managers"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${XDAQ_ROOT_DIR}/lib/xdaq/resources" "$<TARGET_FILE_DIR:XDAQ-RHX>/resources"
        COMMENT "Copying xdaq runtime DLLs next to XDAQ-RHX.exe for local dev runs"
    )
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    install(TARGETS XDAQ-RHX DESTINATION .)
    set_target_properties(XDAQ-RHX PROPERTIES 
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    xdaq_install_core(
        "$<TARGET_FILE_NAME:XDAQ-RHX>.app/Contents/Frameworks"
        "$<TARGET_FILE_NAME:XDAQ-RHX>.app/Contents/Frameworks"
    )
    xdaq_install_device_managers("$<TARGET_FILE_NAME:XDAQ-RHX>.app/Contents/PlugIns/managers")
    install(FILES
        ${PROJECT_SOURCE_DIR}/kernel.cl
        DESTINATION "$<TARGET_FILE_NAME:XDAQ-RHX>.app/Contents/MacOS"
    )
    set_source_files_properties(${ICON_PATH} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(XDAQ-RHX PROPERTIES
        BUNDLE True
        MACOSX_BUNDLE_ICON_FILE "${ICON_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.kontex.XDAQ-RHX"
        MACOSX_BUNDLE_BUNDLE_NAME "XDAQ-RHX"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    deployqt("$<TARGET_FILE_NAME:XDAQ-RHX>.app")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    install(TARGETS XDAQ-RHX DESTINATION bin)
    set_target_properties(XDAQ-RHX PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
    )

    xdaq_install_core(lib lib)
    xdaq_install_device_managers(lib/managers)
    install(FILES
        ${PROJECT_SOURCE_DIR}/kernel.cl
        DESTINATION bin
    )
endif()


# Packaging
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "KonteX Neuroscience")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "IntanRHX Data Acquisition Software for XDAQ")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR}/package)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    include (InstallRequiredSystemLibraries)

    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME ${PROJECT_NAME})
    set(CPACK_NSIS_UNINSTALL_NAME "Uninstall XDAQ-RHX")
    set(CPACK_NSIS_MUI_ICON ${ICON_PATH})
    set(CPACK_NSIS_INSTALLED_ICON_NAME "XDAQ-RHX.exe")
    set(CPACK_NSIS_MENU_LINKS "XDAQ-RHX.exe" "XDAQ-RHX")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
	set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/codesign.cmake")
endif()

include(CPack)
set(CPACK_GENERATORS "NSIS")
set(CPACK_PACKAGE_NAME "XDAQ-RHX-Debug")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")