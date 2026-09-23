cmake_minimum_required(VERSION 3.10)
project(face_recognition_rknn)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Release)

# RK3588相关路径（根据实际情况修改）
set(RKNN_API_PATH $ENV{HOME}/rknpu2/runtime/RK3588/Linux/librknn_api)
set(RKNN_INCLUDE_DIR ${RKNN_API_PATH}/include)
set(RKNN_LIB_DIR ${RKNN_API_PATH}/aarch64)

# OpenCV
find_package(OpenCV REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RKNN_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

link_directories(
    ${RKNN_LIB_DIR}
)

add_executable(face_recognition main.cpp)

target_link_libraries(face_recognition
    rknnrt
    ${OpenCV_LIBS}
)