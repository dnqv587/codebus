cmake_minimum_required(VERSION 3.0)
aux_source_directory(.  SRC_LIST)
aux_source_directory(./base  SRC_LIST)
aux_source_directory(./log  SRC_LIST)
aux_source_directory(./thread  SRC_LIST)
add_library(netbus ${SRC_LIST})

target_link_libraries(netbus pthread)