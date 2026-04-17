cmake_minimum_required(VERSION 3.10)

project(mainSoftware
    VERSION 1.0.0
    LANGUAGES CXX
)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/main.cpp
    src/UnitManager.cpp
    src/DeviceManager.cpp
    src/TdkLambdaPsu.cpp
    src/Dtn.cpp
    src/Hsn.cpp
    src/Cmc.cpp
    src/Mmc.cpp
    src/Vmc.cpp
    src/Utils.cpp
    src/SerialPort.cpp
    src/SystemCommand.cpp
    src/Server.cpp
    src/SSHDeployer.cpp
    src/CumulusHelper.cpp
    src/SerialTimeForwarder.cpp
    src/ReportManager.cpp
    src/SafeShutdown.cpp
    src/PsuTelemetryPublisher.cpp
)

# pthread for std::thread (PsuTelemetryPublisher background loop)
find_package(Threads REQUIRED)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# Output setting for build directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
