cmake_minimum_required(VERSION 3.10)
project(idge)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_SYSTEM_NAME "Linux") 

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -pg") 
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -pg") 


if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()


find_package(Qt5 COMPONENTS Core Gui Sql OpenGL Qml Quick Charts Widgets LinguistTools Network Sql REQUIRED)
find_package(Threads REQUIRED)
find_package(SDL2)

# opencv
set(OpenCV_DIR ${CMAKE_SOURCE_DIR}/3rdparty/opencv4.10/lib/cmake/opencv4)
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

# 添加 /usr/include 到包含路径
include_directories(
    /usr/include
    /usr/include/libdrm
    ${OpenCV_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS} 
    ${GLES2_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/3rdparty/opencv/include/opencv4
    ${CMAKE_SOURCE_DIR}/3rdparty/librga/include
    ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/include
    ${CMAKE_SOURCE_DIR}/3rdparty/mpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/include
    ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/include
    ${CMAKE_SOURCE_DIR}/3rdparty/openssl1.1.0/include
    ${CMAKE_SOURCE_DIR}/3rdparty/timer
    ${CMAKE_SOURCE_DIR}/3rdparty/stb
    ${CMAKE_SOURCE_DIR}/3rdparty/jpeg_turbo/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/*
    ${CMAKE_SOURCE_DIR}/src/config
    ${CMAKE_SOURCE_DIR}/src/draw
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/model
    ${CMAKE_SOURCE_DIR}/src/process
    ${CMAKE_SOURCE_DIR}/src/rkmedia
    ${CMAKE_SOURCE_DIR}/src/rkmedia/utils
    ${CMAKE_SOURCE_DIR}/src/task
    ${CMAKE_SOURCE_DIR}/src/RtspWorker
    ${CMAKE_SOURCE_DIR}/src/stream
    ${CMAKE_SOURCE_DIR}/src/threadPool
    ${CMAKE_SOURCE_DIR}/src/types
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/context
    ${CMAKE_SOURCE_DIR}/src/counter
    ${CMAKE_SOURCE_DIR}/src/videoplay_gl
    ${CMAKE_SOURCE_DIR}/src/reader
    ${CMAKE_SOURCE_DIR}/src/yolo11
    ${CMAKE_SOURCE_DIR}/src/yolo8
    ${CMAKE_SOURCE_DIR}/src/yolo8_pose
    ${CMAKE_SOURCE_DIR}/src/yolo8_seg
    ${CMAKE_SOURCE_DIR}/src/base
    ${CMAKE_SOURCE_DIR}/src/task
    ${CMAKE_SOURCE_DIR}/src/queue
    ${CMAKE_SOURCE_DIR}/src/form
    ${CMAKE_SOURCE_DIR}/src/VideoPlay
    ${CMAKE_SOURCE_DIR}/src/core_helper
    ${CMAKE_SOURCE_DIR}/src/core_qss
    ${CMAKE_SOURCE_DIR}/src/mpp
    ${CMAKE_SOURCE_DIR}/src/rga
    ${CMAKE_SOURCE_DIR}/src/infer
    ${CMAKE_SOURCE_DIR}/src/player
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/rknn
    ${CMAKE_SOURCE_DIR}/src/yolo
    ${CMAKE_SOURCE_DIR}/src/buffer
    ${CMAKE_SOURCE_DIR}/src/threadpool
    ${CMAKE_SOURCE_DIR}/src/model

)


# 设置动态链接库目录

set(RKNNRT_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64) 
link_directories(${RKNNRT_LIB_PATH}) #使用 link_directories 明确指定 rknnrt 库所在的目录


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()


# opencv
# set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/opencv/opencv-linux-aarch64/share/OpenCV)

find_package(OpenMP REQUIRED)

#dma
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma)


# RGA2 only support under 4G memory
if (ENABLE_DMA32 STREQUAL "TRUE")
    add_definitions(-DDMA_ALLOC_DMA32)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ 3rdparty.out)
#add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS 
               
                ${CMAKE_SOURCE_DIR}/res/*.qrc
                ${CMAKE_SOURCE_DIR}/src/*.cpp 
                ${CMAKE_SOURCE_DIR}/src/*/*.cpp 
                ${CMAKE_SOURCE_DIR}/src/*/*/*.cpp 
                ${CMAKE_SOURCE_DIR}/src/*.ui
                ${CMAKE_SOURCE_DIR}/src/*/*.ui
)

qt5_add_big_resources(CORE_HELPER_SOURCES_QRC
        ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/font.qrc
        ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/image.qrc
        ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/qm.qrc
        ${CMAKE_SOURCE_DIR}/src/core_helper/qrc/wasm.qrc)

qt5_add_big_resources(CORE_QRC
        ${CMAKE_SOURCE_DIR}/src/core_qss/qss.qrc)


set(TS_FILES ${CMAKE_SOURCE_DIR}/src/idge_zh_CN.ts)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/utils ${STB_INCLUDES} ${LIBJPEG_INCLUDES})
include_directories(${LIBRKNNRT_INCLUDES})

message(STATUS ${LIBRKNNRT})

add_executable(${PROJECT_NAME} 
    ${PROJECT_SOURCES}
    ${CORE_HELPER_SOURCES_QRC}
    ${CORE_QRC}
    ${TS_FILES}
)

target_link_directories(${PROJECT_NAME} PUBLIC 
                                ${CMAKE_SOURCE_DIR}/3rdparty/libyuv/lib
                                ${CMAKE_SOURCE_DIR}/3rdparty/zlmediakit/aarch64
                                ${CMAKE_SOURCE_DIR}/3rdparty/librga/lib
                                ${CMAKE_SOURCE_DIR}/3rdparty/rknpu2/Linux/aarch64
                                ${CMAKE_SOURCE_DIR}/3rdparty/mpp/lib
                                ${CMAKE_SOURCE_DIR}/3rdparty/jsoncpp/lib
                                ) 
                                
target_link_libraries(${PROJECT_NAME}
    Qt5::Gui 
    Qt5::Network 
    Qt5::Core 
    Qt5::Charts 
    Qt5::OpenGL 
    Qt5::Sql 
    avformat
    avcodec
    avutil
    swscale 
    avfilter
    avdevice
    avutil
    swscale
    swresample
    postproc
    pthread
    rockchip_mpp
    mk_api
    jsoncpp
    rknnrt
    rga
    crypto
    ssl
    dl
    drm
    ${OpenCV_LIBS} 
    ${GLES2_LIBRARIES}
    ${EGL_LIBRARIES}
    ${SDL2_LIBRARIES}
    Threads::Threads
    ${LIBJPEG}
    ${LIBRGA}
    rknnrt
    dl)
target_compile_definitions(${PROJECT_NAME} PRIVATE ZERO_COPY)

# yolo11_videocapture_demo
# add_executable(yolo11_videocapture_demo 
#     yolo11_videocapture_demo.cc 
#     postprocess.cc 
#     rknpu2/yolo11_zero_copy.cc)
    
# target_link_libraries(yolo11_videocapture_demo 
#     #imageutils 
#     #fileutils 
#     #imagedrawing 
#     Threads::Threads
#     ${OpenCV_LIBS}
#     rknnrt
#     dl)
# target_compile_definitions(yolo11_videocapture_demo PRIVATE ZERO_COPY)


#install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/model/bus.jpg DESTINATION model)
#install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/model/coco_80_labels_list.txt DESTINATION model)
#file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.rknn")
#install(FILES ${RKNN_FILES} DESTINATION model)
