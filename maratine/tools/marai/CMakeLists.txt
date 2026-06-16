set(MARAI_SOURCES
  Marai.cpp
  MaraiABI.cpp
  MaraiAudit.cpp
  MaraiInstall.cpp
)

add_executable(marai ${MARAI_SOURCES})

target_include_directories(marai PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(marai PRIVATE
  LLVMCore
  LLVMSupport
  LLVMDemangle
)
