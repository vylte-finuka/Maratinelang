set(MARABUG_SOURCES
  MaraBug.cpp
  MaraBugSession.cpp
)

add_executable(marabug ${MARABUG_SOURCES})

target_include_directories(marabug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(marabug PRIVATE
  LLVMCore
  LLVMSupport
  LLVMDemangle
  LLVMDebugInfoDWARF
  LLVMObject
  LLVMBinaryFormat
)
