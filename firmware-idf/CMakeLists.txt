# DesktopWallE — ESP-IDF spike for esp-sr VAD on M5 CoreS3 + StackChan.
#
# Run:
#   . ~/esp/esp-idf/export.sh
#   cd firmware-idf
#   idf.py set-target esp32s3
#   idf.py build
#   idf.py -p /dev/cu.usbmodem11301 flash monitor
cmake_minimum_required(VERSION 3.16)

# Allow overriding the esp-sr location with an env var. Defaults to the
# local clone that already exists on this machine — works around the
# component registry being unreliable from behind the local proxy
# (downloads of the ~hundreds-of-MB esp-sr archive truncate ~10% in).
if(DEFINED ENV{ESP_SR_PATH})
    set(_ESP_SR_DIR "$ENV{ESP_SR_PATH}")
else()
    set(_ESP_SR_DIR "/Volumes/Other/LabRoot/GitHub_Analyze/esp-sr")
endif()
if(EXISTS "${_ESP_SR_DIR}/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${_ESP_SR_DIR}")
    message(STATUS "Using local esp-sr from: ${_ESP_SR_DIR}")
else()
    message(WARNING
        "Local esp-sr not found at ${_ESP_SR_DIR}; falling back to "
        "the component manager. Override with ESP_SR_PATH=/path/to/esp-sr "
        "if your local checkout lives elsewhere.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(desktopwalle_idf_spike)
