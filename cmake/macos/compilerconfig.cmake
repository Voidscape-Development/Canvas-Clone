# CMake macOS compiler configuration module

include_guard(GLOBAL)

option(ENABLE_COMPILER_TRACE "Enable clang time-trace" OFF)
mark_as_advanced(ENABLE_COMPILER_TRACE)

if(NOT XCODE)
  message(FATAL_ERROR "Building OBS Studio on macOS requires Xcode generator.")
endif()

include(ccache)
include(compiler_common)

add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-fopenmp-simd>")

# Ensure recent enough Xcode and platform SDK
function(check_sdk_requirements)
  set(obs_macos_minimum_sdk 15.0) # Keep in sync with Xcode
  set(obs_macos_minimum_xcode 16.0) # Keep in sync with SDK
  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-platform-version
    OUTPUT_VARIABLE obs_macos_current_sdk
    RESULT_VARIABLE result
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT result EQUAL 0)
    message(
      FATAL_ERROR
      "Failed to fetch macOS SDK version. "
      "Ensure that the macOS SDK is installed and that xcode-select points at the Xcode developer directory."
    )
  endif()
  message(DEBUG "macOS SDK version: ${obs_macos_current_sdk}")
  if(obs_macos_current_sdk VERSION_LESS obs_macos_minimum_sdk)
    message(
      FATAL_ERROR
      "Your macOS SDK version (${obs_macos_current_sdk}) is too low. "
      "The macOS ${obs_macos_minimum_sdk} SDK (Xcode ${obs_macos_minimum_xcode}) is required to build OBS."
    )
  endif()
  execute_process(COMMAND xcrun --find xcodebuild OUTPUT_VARIABLE obs_macos_xcodebuild RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(
      FATAL_ERROR
      "Xcode was not found. "
      "Ensure you have installed Xcode and that xcode-select points at the Xcode developer directory."
    )
  endif()
  message(DEBUG "Path to xcodebuild binary: ${obs_macos_xcodebuild}")
  if(XCODE_VERSION VERSION_LESS obs_macos_minimum_xcode)
    message(
      FATAL_ERROR
      "Your Xcode version (${XCODE_VERSION}) is too low. Xcode ${obs_macos_minimum_xcode} is required to build OBS."
    )
  endif()
endfunction()

check_sdk_requirements()

# Enable dSYM generator for release builds
string(APPEND CMAKE_C_FLAGS_RELEASE " -g")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g")
string(APPEND CMAKE_OBJC_FLAGS_RELEASE " -g")
string(APPEND CMAKE_OBJCXX_FLAGS_RELEASE " -g")

# Default ObjC compiler options used by Xcode:
#
# * -Wno-implicit-atomic-properties
# * -Wno-objc-interface-ivars
# * -Warc-repeated-use-of-weak
# * -Wno-arc-maybe-repeated-use-of-weak
# * -Wimplicit-retain-self
# * -Wduplicate-method-match
# * -Wshadow
# * -Wfloat-conversion
# * -Wobjc-literal-conversion
# * -Wno-selector
# * -Wno-strict-selector-match
# * -Wundeclared-selector
# * -Wdeprecated-implementations
# * -Wprotocol
# * -Werror=block-capture-autoreleasing
# * -Wrange-loop-analysis

# Default ObjC++ compiler options used by Xcode:
#
# * -Wno-non-virtual-dtor

add_compile_definitions(
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:$<$<CONFIG:DEBUG>:DEBUG>>
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:$<$<CONFIG:DEBUG>:_DEBUG>>
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:SIMDE_ENABLE_OPENMP>
)

if(ENABLE_COMPILER_TRACE)
  add_compile_options(
    $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-ftime-trace>
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -debug-time-expression-type-checking>"
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -debug-time-function-bodies>"
  )
  add_link_options(LINKER:-print_statistics)
endif()

# Xcode 26.6 passes several *_DEPLOYMENT_TARGET variables to build script phases. AUTOMOC runs the
# compiler there to collect its predefined macros without naming a target, and clang then refuses
# to choose between them ("conflicting deployment targets"). Naming the macOS target explicitly
# makes clang ignore those variables.
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND CMAKE_CXX_COMPILER_PREDEFINES_COMMAND "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# The macOS 26 SDK no longer has the AGL framework, but the FindWrapOpenGL module of the Qt in the OBS
# dependencies still links it ("framework 'AGL' not found"). When the SDK has no AGL, point that
# lookup at OpenGL, which Qt links anyway, and drop any AGL entry Qt adds regardless.
execute_process(
  COMMAND xcrun --sdk macosx --show-sdk-path
  OUTPUT_VARIABLE _obs_macos_sdk_path
  RESULT_VARIABLE _obs_macos_sdk_result
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(_obs_macos_sdk_result EQUAL 0 AND NOT EXISTS "${_obs_macos_sdk_path}/System/Library/Frameworks/AGL.framework")
  set(
    WrapOpenGL_AGL
    "${_obs_macos_sdk_path}/System/Library/Frameworks/OpenGL.framework"
    CACHE FILEPATH
    "Stand-in for AGL, which the macOS SDK no longer has"
    FORCE
  )

  function(_obs_drop_agl_from_wrapopengl)
    if(TARGET WrapOpenGL::WrapOpenGL)
      get_target_property(_obs_wrapopengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
      if(_obs_wrapopengl_libs)
        list(FILTER _obs_wrapopengl_libs EXCLUDE REGEX "AGL")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_obs_wrapopengl_libs}")
      endif()
    endif()
  endfunction()
  cmake_language(DEFER CALL _obs_drop_agl_from_wrapopengl)
endif()
