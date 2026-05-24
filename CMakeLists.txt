cmake_minimum_required(VERSION 3.14)

project(task_planning_api CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(userver COMPONENTS core postgresql mongo redis REQUIRED)
find_path(RABBITMQ_INCLUDE_DIR amqp.h REQUIRED)
find_library(RABBITMQ_LIBRARY rabbitmq REQUIRED)

add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${RABBITMQ_INCLUDE_DIR})
target_link_libraries(
    ${PROJECT_NAME}
    userver::core
    userver::postgresql
    userver::mongo
    userver::redis
    ${RABBITMQ_LIBRARY}
)
