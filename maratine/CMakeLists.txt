cmake_minimum_required(VERSION 3.20)
project(Maratine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------
# 1. Toolchain
#    Windows : passer -DCMAKE_TOOLCHAIN_FILE=../windows-clang-cl.cmake
#    Linux   : clang-19 auto-détecté ci-dessous
# ---------------------------------------------------------------
if(NOT WIN32)
  if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(_CLANG19 clang-19 PATHS /usr/lib/llvm-19/bin /usr/bin)
    set(CMAKE_C_COMPILER "${_CLANG19}")
  endif()
  if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(_CLANGXX19 clang++-19 PATHS /usr/lib/llvm-19/bin /usr/bin)
    set(CMAKE_CXX_COMPILER "${_CLANGXX19}")
  endif()
  find_program(_LLD ld.lld lld PATHS /usr/lib/llvm-19/bin /usr/bin)
  if(_LLD)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
  endif()
endif()

# ---------------------------------------------------------------
# 2. Trouver LLVM
#    Windows  : LLVM officiel installé séparément (le LLVM bundlé VS
#               ne fournit pas LLVMConfig.cmake ni les import libs).
#               Passer -DLLVM_DIR=<install>/lib/cmake/llvm
#    Linux    : system llvm-config / llvm_install local
# ---------------------------------------------------------------
if(NOT DEFINED LLVM_DIR)
  if(WIN32)
    # Chercher dans les emplacements standard d'un LLVM Windows officiel
    foreach(_candidate
        "C:/Program Files/LLVM/lib/cmake/llvm"
        "D:/LLVM/lib/cmake/llvm"
        "D:/llvm-install/lib/cmake/llvm")
      if(EXISTS "${_candidate}/LLVMConfig.cmake")
        set(LLVM_DIR "${_candidate}")
        message(STATUS "Windows LLVM: ${_candidate}")
        break()
      endif()
    endforeach()
    if(NOT LLVM_DIR)
      message(WARNING
        "LLVMConfig.cmake introuvable.\n"
        "Installe LLVM depuis https://github.com/llvm/llvm-project/releases\n"
        "puis passe -DLLVM_DIR=<install>/lib/cmake/llvm à cmake.")
    endif()
  else()
    # Linux : local install puis system llvm-config
    set(_LOCAL_LLVM "$ENV{HOME}/maratinec/llvm_install/lib/cmake/llvm")
    if(EXISTS "${_LOCAL_LLVM}/LLVMConfig.cmake")
      set(LLVM_DIR "${_LOCAL_LLVM}")
    else()
      execute_process(
        COMMAND llvm-config --cmakedir
        OUTPUT_VARIABLE _LLVM_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      if(_LLVM_CMAKE_DIR)
        set(LLVM_DIR "${_LLVM_CMAKE_DIR}")
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------
# 3. Nettoyage WSL : supprimer les chemins /mnt/* (MSYS2/Win)
#    No-op sur Windows natif.
# ---------------------------------------------------------------
if(NOT WIN32)
  foreach(_wsl_var
      CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES
      CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES
      CMAKE_C_IMPLICIT_LINK_DIRECTORIES
      CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES
      CMAKE_SYSTEM_INCLUDE_PATH
      CMAKE_SYSTEM_LIBRARY_PATH
      CMAKE_SYSTEM_PREFIX_PATH)
    if(DEFINED ${_wsl_var})
      list(FILTER ${_wsl_var} EXCLUDE REGEX "^/mnt/")
      set(${_wsl_var} "${${_wsl_var}}" CACHE INTERNAL "" FORCE)
    endif()
  endforeach()

  find_package(ZLIB)
  find_package(zstd)
  find_package(LibEdit)
  find_package(CURL)
  find_package(LibXml2)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} — ${LLVM_LIBRARY_DIRS}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

llvm_map_components_to_libnames(LLVM_LIBS
  Core Support Passes IRReader BitReader BitWriter
  Analysis TransformUtils Target MC BinaryFormat)

# ---------------------------------------------------------------
# 4. Flags de compilation additionnels (après toolchain)
# ---------------------------------------------------------------
if(WIN32)
  add_compile_options(/W3 -Wno-deprecated-declarations -Wno-unknown-pragmas)
else()
  add_compile_options(-Wall -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------
# 5. Projet
# ---------------------------------------------------------------
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(MARATINE_SOURCES
    lib/MaratineLexer.cpp
    lib/MaratineParser.cpp
    lib/MaratineSema.cpp
    lib/MaratineCodeGen.cpp
    lib/MaratineOVC.cpp
)

add_library(maratine_compiler STATIC ${MARATINE_SOURCES})
target_include_directories(maratine_compiler PUBLIC include)
target_link_libraries(maratine_compiler PUBLIC ${LLVM_LIBS})

add_executable(maratine-cc tools/maratine-cc.cpp)
target_link_libraries(maratine-cc maratine_compiler ${LLVM_LIBS})

if(WIN32)
  # DLL runtime Windows nécessaires
  target_link_libraries(maratine-cc Version.lib)
endif()

# ---------------------------------------------------------------
# 6. Outils
# ---------------------------------------------------------------
add_subdirectory(tools/marai)
add_subdirectory(tools/marabug)

# ---------------------------------------------------------------
# 7. Tests & install
# ---------------------------------------------------------------
enable_testing()
add_subdirectory(test)

install(TARGETS maratine-cc marai marabug DESTINATION bin)
install(FILES include/MaratineAST.h include/MaratineCodeGen.h
              include/MaratineLexer.h include/MaratineParser.h
        DESTINATION include/maratine)

message(STATUS "Maratine configured — LLVM ${LLVM_PACKAGE_VERSION} — ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
