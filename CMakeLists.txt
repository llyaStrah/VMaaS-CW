cmake_minimum_required(VERSION 3.20)

project(vmaas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vmaas_domain INTERFACE)

target_include_directories(
	vmaas_domain INTERFACE backend/include)

add_executable(
	vmaas_backend backend/src/main.cpp)

target_link_libraries(
	vmaas_backend PRIVATE vmaas_domain)
