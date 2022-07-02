cmake_minimum_required(VERSION 3.23)

# Name the project.
project(smash_memcard_reader)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_CXX_STANDARD_REQUIRED True)

list(APPEND CMAKE_MODULE_PATH
  ${CMAKE_SOURCE_DIR}/CMake
)

# Support functions
include(CheckAndAddFlag)
include(CheckCCompilerFlag)
include(CheckVendoringApproved)
include(DolphinCompileDefinitions)
include(DolphinDisableWarningsMSVC)
include(RemoveCompileFlag)

if(APPLE)
  option(MACOS_USE_DEFAULT_SEARCH_PATH "Don't prioritize system library paths" OFF)
  option(SKIP_POSTPROCESS_BUNDLE "Skip postprocessing bundle for redistributability" OFF)
  # Enable adhoc code signing by default (otherwise makefile builds on ARM will not work)
  option(MACOS_CODE_SIGNING "Enable codesigning" ON)
  option(USE_BUNDLED_MOLTENVK "Build MoltenVK from Externals with Dolphin-specific patches" ON)
  set(MACOS_CODE_SIGNING_IDENTITY "-" CACHE STRING "The identity used for codesigning.")
  set(MACOS_CODE_SIGNING_IDENTITY_UPDATER "-" CACHE STRING "The identity used for codesigning, for the updater.")
endif()

# Detect 64bit or 32bit
# CMake doesn't provide a simple way to determine 32bit or 64bit
# If we ever support a architecture that is 64bit with 32bit pointers then this'll break
# Of course the chances of that are slim(x32?) so who cares
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_ARCH_64 1)
  add_definitions(-D_ARCH_64=1)
else()
  set(_ARCH_32 1)
  add_definitions(-D_ARCH_32=1)
endif()

if(ENABLE_GENERIC)
  message(STATUS "Warning! Building generic build!")
  set(_M_GENERIC 1)
  add_definitions(-D_M_GENERIC=1)
elseif(_ARCH_64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(_M_X86 1)
  set(_M_X86_64 1)
  add_definitions(-D_M_X86=1)
  add_definitions(-D_M_X86_64=1)
  check_and_add_flag(HAVE_SSE2 -msse2)
elseif(_ARCH_64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_M_ARM_64 1)
  add_definitions(-D_M_ARM_64=1)
  # CRC instruction set is used in the CRC32 hash function
  check_and_add_flag(HAVE_ARCH_ARMV8 -march=armv8-a+crc)
else()
  message(FATAL_ERROR "You're building on an unsupported platform: "
      "'${CMAKE_SYSTEM_PROCESSOR}' with ${CMAKE_SIZEOF_VOID_P}-byte pointers."
      " Enable generic build if you really want a JIT-less binary.")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  if(NOT MACOS_USE_DEFAULT_SEARCH_PATH)
    # Hack up the path to prioritize the path to built-in OS libraries to
    # increase the chance of not depending on a bunch of copies of them
    # installed by MacPorts, Fink, Homebrew, etc, and ending up copying
    # them into the bundle.  Since we optionally depend on libraries which
    # are not part of OS X (ffmpeg, etc.), however, don't remove the default
    # path entirely as was done in a previous version of this file.  This is
    # still kinda evil, since it defeats the user's path settings...
    # See http://www.cmake.org/cmake/help/v3.0/command/find_program.html
    list(APPEND CMAKE_PREFIX_PATH "/usr")
  endif()

  # Prevents Xcode from overriding the -fno-strict-aliasing flag
  set(CMAKE_XCODE_ATTRIBUTE_GCC_STRICT_ALIASING NO)

  # Specify target CPUs.
  if(_ARCH_64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    check_and_add_flag(HAVE_MSSSE3 -mssse3)
    check_and_add_flag(HAVE_ARCH_CORE2 -march=core2)
  endif()
  # Linker flags.
  # Drop unreachable code and data.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip,-dead_strip_dylibs")

  # Set FMT_EXCEPTIONS = 0, for consistency with -fno-exceptions earlier.
  # If we set only -fno-exceptions, fmt fails to compile when included from
  # Objective-C++ because fmt tries try to use throw because __EXCEPTIONS is defined.
  add_definitions(-DFMT_EXCEPTIONS=0)

  find_library(APPKIT_LIBRARY AppKit)
  find_library(APPSERV_LIBRARY ApplicationServices)
  find_library(CARBON_LIBRARY Carbon)
  find_library(COCOA_LIBRARY Cocoa)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  find_library(CORESERV_LIBRARY CoreServices)
  find_library(FORCEFEEDBACK_LIBRARY ForceFeedback)
  find_library(FOUNDATION_LIBRARY Foundation)
  find_library(IOB_LIBRARY IOBluetooth)
  find_library(IOK_LIBRARY IOKit)
  find_library(OPENGL_LIBRARY OpenGL)

  # We don't want to use shared libpng.
  set(USE_SHARED_LIBPNG OFF)
endif()

set(MBEDTLS_LIBRARIES mbedtls mbedcrypto mbedx509)
set(CURL_LIBRARIES curl)
set(ICONV_LIBRARIES iconv)

add_compile_options(-I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/libiconv-1.14/include -I${CMAKE_SOURCE_DIR}/mbedtls/include -I${CMAKE_SOURCE_DIR}/enet/include -I${CMAKE_SOURCE_DIR}/SFML/include -I${CMAKE_SOURCE_DIR}/libusb/libusb/libusb/ -I${CMAKE_SOURCE_DIR}/picojson -I${CMAKE_SOURCE_DIR}/soundtouch -I${CMAKE_SOURCE_DIR}/LZO)

add_subdirectory(fmt)
add_subdirectory(zlib)
add_subdirectory(mbedtls)
add_subdirectory(rangeset)
add_subdirectory(enet)
add_subdirectory(libpng)
add_subdirectory(SFML)
add_subdirectory(minizip)
add_subdirectory(curl)
add_subdirectory(libiconv-1.14)
add_subdirectory(liblzma)
add_subdirectory(bzip2)
add_subdirectory(xxhash)
add_subdirectory(imgui)
add_subdirectory(libusb)
add_subdirectory(cubeb)
add_subdirectory(soundtouch)
add_subdirectory(FreeSurround)
add_subdirectory(LZO)
add_subdirectory(cpp-optparse)
add_subdirectory(pugixml)
add_subdirectory(zstd)
add_subdirectory(Common)
add_subdirectory(DiscIO)
add_subdirectory(AudioCommon)
add_subdirectory(InputCommon)
add_subdirectory(UICommon)
add_subdirectory(VideoBackends)
add_subdirectory(VideoCommon)
add_subdirectory(Core)

add_executable(smashcardloader smashcardloader.cc)
target_link_libraries(smashcardloader core common)
