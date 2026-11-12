
# Add source to this project's executable.
add_executable (vkRenderer 
  main.cpp
  vk_engine.cpp
  vk_engine.h
 )

 set_property(TARGET vkRenderer PROPERTY CXX_STANDARD 20)
set_property(TARGET vkRenderer PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:vkRenderer>)

target_include_directories(vkRenderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(vkRenderer vkguide_shared  vkbootstrap)

add_custom_command(TARGET vkRenderer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:vkRenderer> $<TARGET_FILE_DIR:vkRenderer>
  COMMAND_EXPAND_LISTS
  )
