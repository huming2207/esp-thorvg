# Set a name for libthorvg library
set(TVG_LIB libthorvg)
set(TVG_SUBDIR "${CMAKE_CURRENT_SOURCE_DIR}/thorvg")
set(TVG_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/thorvg_build")
# Kept outside of TVG_BUILD_DIR, which is wiped before every Meson setup
set(TVG_CROSS_CFG "${CMAKE_CURRENT_BINARY_DIR}/cross_file.txt")

set(TVG_LOADERS_OPTION "")

if(CONFIG_THORVG_LOTTIE_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "lottie")
endif()

if(CONFIG_THORVG_SVG_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "svg")
endif()

if(CONFIG_THORVG_TVG_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "tvg")
endif()

if(CONFIG_THORVG_PNG_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "png")
endif()

if(CONFIG_THORVG_JPEG_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "jpg")
endif()

if(CONFIG_THORVG_WEBP_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "webp")
endif()

if(CONFIG_THORVG_TTF_LOADER_SUPPORT)
    list(APPEND TVG_LOADERS_OPTION "ttf")
endif()

list(JOIN TVG_LOADERS_OPTION "," TVG_LOADERS_OPTION)

if(CONFIG_THORVG_LOG_ENABLED)
    set(TVG_LOG "true")
else()
    set(TVG_LOG "false")
endif()

if(CONFIG_THORVG_THREAD_ENABLED)
    set(TVG_THREADS "true")
else()
    set(TVG_THREADS "false")
endif()

# ThorVG detects PIE from the -march that ESP-IDF passes through CMAKE_CXX_FLAGS (@cxxflags).
if(CONFIG_THORVG_SIMD_ENABLED)
    set(TVG_SIMD "true")
else()
    set(TVG_SIMD "false")
endif()

idf_component_register(
    SRCS "dummy.c"
    INCLUDE_DIRS "thorvg/src/bindings/capi" "thorvg/inc"
    PRIV_REQUIRES pthread)


# Convert argument strings to lists, for easier processing
set(linker_args "")
if(CMAKE_EXE_LINKER_FLAGS)
    separate_arguments(linker_args NATIVE_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
endif()
list(FILTER linker_args EXCLUDE REGEX "^$")

set(compiler_args "")
if(CMAKE_CXX_FLAGS)
    separate_arguments(compiler_args NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
endif()
list(FILTER compiler_args EXCLUDE REGEX "^$")

# Add pthread library path to the linker flags. ThorVG's meson.build looks up pthread on every
# non-Windows host even with -Dthreads=false, so Meson setup fails without this.
idf_component_get_property(pthread_lib "pthread" COMPONENT_LIB GENERATOR_EXPRESSION)
set(pthread_lib_path_expr "$<TARGET_FILE_DIR:${pthread_lib}>")
list(APPEND linker_args "-L${pthread_lib_path_expr}")
set(TVG_REQUIRES pthread)

# Add compiler flag to allow type mismatches
list(APPEND compiler_args "-fpermissive")

# Add include directory for sdkconfig.h
list(APPEND compiler_args "-I${CMAKE_BINARY_DIR}/config")

list(APPEND TVG_REQUIRES heap)


message(STATUS "ThorVG: CMAKE_EXE_LINKER_FLAGS: ${linker_args}")
message(STATUS "ThorVG: CMAKE_CXX_FLAGS: ${compiler_args}")

# Convert the list of arguments (arg;arg;arg) to a string "'arg', 'arg', 'arg'"
set(MESON_CXX_FLAGS "")
foreach(arg IN LISTS compiler_args)
    if(MESON_CXX_FLAGS)
        string(APPEND MESON_CXX_FLAGS ", ")
    endif()
    string(APPEND MESON_CXX_FLAGS "'${arg}'")
endforeach()

set(MESON_LINKER_FLAGS "")
foreach(arg IN LISTS linker_args)
    if(MESON_LINKER_FLAGS)
        string(APPEND MESON_LINKER_FLAGS ", ")
    endif()
    string(APPEND MESON_LINKER_FLAGS "'${arg}'")
endforeach()

# Please note, xtensa is not recognized by Meson, but "riscv32" is a safe generic option
# Always use "riscv32" as cpu_family in cross_file.txt to avoid Meson warnings
set(cpu_family "riscv32")

# Two-step generation of cross_file.txt to expand CMake variables...
configure_file(${CMAKE_CURRENT_LIST_DIR}/cross_file.txt.in ${TVG_CROSS_CFG}.tmp)
# ...and then expand generator expressions
file(GENERATE OUTPUT ${TVG_CROSS_CFG} INPUT ${TVG_CROSS_CFG}.tmp)

include(ExternalProject)

ExternalProject_Add(${TVG_LIB}_target
    PREFIX ${CMAKE_CURRENT_BINARY_DIR}/thorvg_ep
    SOURCE_DIR ${TVG_SUBDIR}
    BINARY_DIR ${TVG_BUILD_DIR}
    # We remove the build directory before setup to ensure a clean configuration
    # when Kconfig options change. This is triggered by ExternalProject_Add_StepDependencies.
    # Meson runs from the source directory, as its working directory (the build directory) was just removed.
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E remove_directory ${TVG_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E chdir ${TVG_SUBDIR} meson setup ${TVG_BUILD_DIR} ${TVG_SUBDIR}
    --cross-file ${TVG_CROSS_CFG}
    -Dbindings=capi
    -Dextra=
    -Ddefault_library=static # build static library
    -Db_staticpic=false # no -fPIC
    -Dthreads=${TVG_THREADS}
    -Dsimd=${TVG_SIMD}
    -Dlog=${TVG_LOG} # allow log output
    -Dloaders=${TVG_LOADERS_OPTION}   # Pass the loaders option to Meson
    BUILD_COMMAND ninja -C ${TVG_BUILD_DIR}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${TVG_BUILD_DIR}/src/${TVG_LIB}-1.a
    BUILD_ALWAYS 1
    USES_TERMINAL_CONFIGURE 1
    USES_TERMINAL_BUILD 1
)

# Force re-configure when the cross-file (which tracks Kconfig changes) changes
ExternalProject_Add_StepDependencies(${TVG_LIB}_target configure ${TVG_CROSS_CFG})

add_prebuilt_library(${TVG_LIB} ${TVG_BUILD_DIR}/src/${TVG_LIB}-1.a PRIV_REQUIRES ${TVG_REQUIRES})
add_dependencies(${TVG_LIB} ${TVG_LIB}_target)

add_dependencies(${TVG_LIB}_target idf::pthread)

add_dependencies(${TVG_LIB}_target idf::heap)
add_dependencies(${TVG_LIB}_target idf::esp_common)

target_link_libraries(${COMPONENT_LIB} INTERFACE ${TVG_LIB})
