cmake_minimum_required(VERSION 3.5)
project(starling_offboard_cpp)

# Default to C99
if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 99)
endif()

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 14)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_SKIP_CHECKING_BUILD_SYSTEM TRUE)

#set(CMAKE_MODULE_PATH "/usr/share/cmake/geographiclib" ${CMAKE_MODULE_PATH})
#set(CMAKE_MODULE_PATH "/usr/local/cmake/GeographicLib" ${CMAKE_MODULE_PATH})

#list(APPEND CMAKE_MODULE_PATH "/workspace/GeographicLib-2.4/cmake")
#
## Set the include directory for GeographicLib
#set(GeographicLib_INCLUDE_DIRS "/usr/include/GeographicLib")
#
## Set the library directory for GeographicLib
#set(GeographicLib_LIBRARY_DIRS "/usr/lib/aarch64-linux-gnu")
#
## Specify the actual library file to link against
#set(GeographicLib_LIBRARIES "Geographic")

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(example_interfaces REQUIRED)
find_package(px4_msgs REQUIRED)
find_package(eigen3_cmake_module REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(GeographicLib REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(rosbag2_cpp REQUIRED)
find_package(message_filters REQUIRED)

include_directories(${EIGEN3_INCLUDE_DIR})

#target_include_directories(starling_offboard_cpp PRIVATE ${GeographicLib_INCLUDE_DIRS})
#target_link_directories(starling_offboard_cpp PRIVATE ${GeographicLib_LIBRARY_DIRS})
#target_link_libraries(starling_offboard_cpp PRIVATE ${GeographicLib_LIBRARIES})

# executables
add_executable(starling_offboard_cpp src/starling_offboard.cpp)
ament_target_dependencies(starling_offboard_cpp 
    rclcpp 
    px4_msgs 
    Eigen3 
    eigen3_cmake_module 
    GeographicLib
    geometry_msgs 
    nav_msgs
    sensor_msgs 
    rosbag2_cpp
)
install(TARGETS starling_offboard_cpp DESTINATION lib/${PROJECT_NAME})

add_executable(starling_transform_test_cpp src/starling_transform_test.cpp)
ament_target_dependencies(starling_transform_test_cpp
    rclcpp 
    px4_msgs 
    Eigen3 
    eigen3_cmake_module 
    GeographicLib
    geometry_msgs 
    nav_msgs
    sensor_msgs 
    rosbag2_cpp
)
install(TARGETS starling_transform_test_cpp DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY
  launch
  DESTINATION share/${PROJECT_NAME}/
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights
  # uncomment the line when a copyright and license is not present in all source files
  #set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # uncomment the line when this package is not in a git repo
  #set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
