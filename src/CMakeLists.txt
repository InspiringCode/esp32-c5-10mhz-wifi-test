# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources})

# Intercept the driver's internal PHY retune so we can land on the off-grid
# 5730 MHz center frequency (channel 146 is not a valid 20 MHz ESP channel).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=phy_change_channel")
