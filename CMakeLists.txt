cmake_minimum_required(VERSION 3.20)
project(ImmersiveTopTaskbar LANGUAGES CXX)

add_executable(ImmersiveTopTaskbar WIN32 src/main.cpp src/app.rc)
target_compile_features(ImmersiveTopTaskbar PRIVATE cxx_std_20)
target_compile_definitions(ImmersiveTopTaskbar PRIVATE UNICODE _UNICODE)
target_include_directories(ImmersiveTopTaskbar PRIVATE src)
target_link_libraries(ImmersiveTopTaskbar PRIVATE user32 gdi32 shell32 dwmapi ole32 oleaut32 advapi32 winhttp gdiplus)

if(MSVC)
  target_compile_options(ImmersiveTopTaskbar PRIVATE /W4 /permissive-)
endif()
