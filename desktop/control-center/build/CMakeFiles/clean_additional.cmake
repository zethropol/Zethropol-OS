# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "CMakeFiles/zethropol-control-center_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/zethropol-control-center_autogen.dir/ParseCache.txt"
  "zethropol-control-center_autogen"
  )
endif()
