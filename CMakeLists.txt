cmake_minimum_required(VERSION 3.20)
project(LegoArkham2 CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "LEGO Batman 2 is a 32-bit game, configure with -A Win32")
endif()

add_library(dinput8 SHARED
  src/main.cpp
  src/hook.cpp
  src/borderless.cpp
  src/chase_camera.cpp
  src/frame.cpp
  src/devices.cpp
  src/device_follow.cpp
  src/input_frame.cpp
  src/quit.cpp
  src/flash_jump.cpp
  src/flash_roll.cpp
  src/reticle.cpp
  src/keybinds.cpp
  src/ability_controls.cpp
  src/cutscene.cpp
  src/glide.cpp
  src/glide_anim.cpp
  src/grapple.cpp
  src/climb.cpp
  src/surface_run.cpp
  src/water_run.cpp
  src/cape_sound.cpp
  src/dinput8.def)

target_compile_features(dinput8 PRIVATE cxx_std_17)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dinput8 PRIVATE dxguid winmm)
set_target_properties(dinput8 PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_custom_command(TARGET dinput8 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LegoArkham2.ini $<TARGET_FILE_DIR:dinput8>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/anim $<TARGET_FILE_DIR:dinput8>/anim)

target_link_options(dinput8 PRIVATE /MAP)
