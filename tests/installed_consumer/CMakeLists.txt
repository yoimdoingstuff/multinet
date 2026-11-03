cmake_minimum_required(VERSION 3.16)
project(MultiNetInstalledConsumer LANGUAGES C)

find_package(MultiNet 0.2 CONFIG REQUIRED)

add_executable(multinet_consumer main.c)
target_link_libraries(multinet_consumer PRIVATE MultiNet::MultiNet)
