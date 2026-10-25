# Shared by the player and the independent network/UI test project.
if(NOT TARGET veyra_bilibili)
  get_filename_component(VEYRA_BILIBILI_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  add_library(veyra_bilibili STATIC
    ${VEYRA_BILIBILI_ROOT}/src/bilibili/Playback.cpp
    ${VEYRA_BILIBILI_ROOT}/src/bilibili/StreamServer.cpp
    ${VEYRA_BILIBILI_ROOT}/src/ui/BilibiliModel.cpp
    ${VEYRA_BILIBILI_ROOT}/include/veyra/ui/BilibiliModel.h
    ${VEYRA_BILIBILI_ROOT}/third_party/qrcodegen/qrcodegen.cpp)
  set_target_properties(veyra_bilibili PROPERTIES AUTOMOC ON)
  target_compile_features(veyra_bilibili PUBLIC cxx_std_20)
  target_include_directories(veyra_bilibili PUBLIC ${VEYRA_BILIBILI_ROOT}/include
    PRIVATE ${VEYRA_BILIBILI_ROOT}/third_party/qrcodegen)
  target_link_libraries(veyra_bilibili PUBLIC Qt6::Core Qt6::Gui Qt6::Network)
  if(WIN32)
    target_compile_definitions(veyra_bilibili PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(veyra_bilibili PRIVATE crypt32)
  endif()
  if(MSVC)
    target_compile_options(veyra_bilibili PRIVATE /utf-8 /W4)
  else()
    target_compile_options(veyra_bilibili PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
