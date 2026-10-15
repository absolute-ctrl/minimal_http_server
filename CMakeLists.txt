cmake_minimum_required(VERSION 3.16)

project(meteor_dodge-OpenGL-C LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # compile_commands.json para clangd

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "Este projeto foi feito para Clang, mas o compilador e "
        "${CMAKE_C_COMPILER_ID}. Use um preset (cmake --preset clang-debug) "
        "ou -DCMAKE_C_COMPILER=clang.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
endif()

# ---------------------------------------------------------------------
# Dependencias (pasta external/)
# ---------------------------------------------------------------------

# GLFW: compilado a partir do codigo-fonte, so a biblioteca.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

# glad: carregador das funcoes OpenGL.
add_subdirectory(external/glad)

# stb_image: leitura de PNG (so um header).
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE external/stb)

# ---------------------------------------------------------------------
# Jogo
# ---------------------------------------------------------------------

add_executable(meteor_dodge
    src/main.c
    src/game.c
    src/renderer.c
    src/sprites.c
    src/text.c
    src/stb_image_impl.c
)

target_include_directories(meteor_dodge PRIVATE include)
target_link_libraries(meteor_dodge PRIVATE glfw glad stb)

# math.h (cosf, sinf, fmaxf) precisa de libm no Linux.
if(UNIX AND NOT APPLE)
    target_link_libraries(meteor_dodge PRIVATE m)
endif()

# Caminho da pasta assets no projeto, como plano B se o jogo
# for executado de outra pasta.
target_compile_definitions(meteor_dodge PRIVATE
    ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# Avisos do compilador (clang-cl no Windows usa a sintaxe do MSVC).
# stb_image.h e codigo de terceiros: sem avisos nesse arquivo.
# No Windows, a biblioteca C da Microsoft marca fopen etc. como "inseguros";
# desligamos esse aviso porque usamos so C padrao.
if(WIN32)
    target_compile_definitions(meteor_dodge PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(meteor_dodge PRIVATE /W4)
    set_source_files_properties(src/stb_image_impl.c PROPERTIES COMPILE_OPTIONS "/w")
else()
    target_compile_options(meteor_dodge PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(src/stb_image_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Copia assets/ para o lado do executavel apos cada build.
add_custom_command(TARGET meteor_dodge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:meteor_dodge>/assets
    COMMENT "Copiando assets")