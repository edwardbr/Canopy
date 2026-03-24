#
# Copyright (c) 2026 Edward Boggis-Rolfe All rights reserved.
#

cmake_minimum_required(VERSION 3.24)

add_library(streaming_io_uring INTERFACE)
target_include_directories(streaming_io_uring INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(streaming_io_uring INTERFACE streaming canopy_network_config libcoro uring)
