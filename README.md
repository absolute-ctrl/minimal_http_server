# Meteor Dodge

Jogo 2D em C (OpenGL 3.3 core + GLFW + glad). Pilote a aeronave e desvie
dos meteoros por 60 segundos. Cada meteoro desviado vale 1 ponto.

| Tecla                        | Acao                          |
|------------------------------|-------------------------------|
| Enter / Espaco / clique      | Comecar                       |
| Setas / WASD                 | Mover                         |
| R                            | Jogar de novo (fim de jogo)   |
| M                            | Voltar ao menu (fim de jogo)  |
| ESC                          | Sair                          |

## Estrutura

```
meteor_dodge/
├── CMakeLists.txt        build principal
├── CMakePresets.json     presets com Clang (debug/release)
├── include/              headers do jogo
│   ├── config.h          constantes (tamanho da tela, tempo, cores)
│   ├── game.h            estado e regras do jogo
│   ├── renderer.h        desenho 2D com shaders
│   ├── sprites.h         imagens PNG
│   └── text.h            fonte bitmap
├── src/                  codigo-fonte
│   ├── main.c            janela, entrada e loop principal
│   ├── game.c            logica, colisao e telas
│   ├── renderer.c        shaders, VBO/VAO e texturas
│   ├── sprites.c         carrega PNGs e desenha numeros
│   ├── text.c            fonte bitmap 5x7
│   └── stb_image_impl.c  implementacao da stb_image
├── assets/sprites/       Start, Points, Game Over e digitos 0-9
└── external/             bibliotecas de terceiros
    ├── glfw/             GLFW 3.5.1 (codigo-fonte, compilado junto)
    ├── glad/             glad (gl 4.0 core) + CMakeLists proprio
    └── stb/              stb_image.h (leitura de PNG)
```

## Como compilar (Clang + CMake)

Precisa de CMake 3.21+, Clang e Ninja.

```sh
cmake --preset clang-debug
cmake --build --preset clang-debug
./build/clang-debug/meteor_dodge        # Windows: build\clang-debug\meteor_dodge.exe
```

Use `clang-release` no lugar de `clang-debug` para a versao otimizada.

Sem presets/Ninja:

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang
cmake --build build
```

**Linux:** o GLFW e compilado a partir do codigo e precisa dos headers
de X11 e Wayland:

```sh
sudo apt install clang cmake ninja-build libx11-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxi-dev libwayland-dev \
  libxkbcommon-dev wayland-protocols pkg-config
```

**Windows:** instale o LLVM (Clang), CMake e Ninja (por exemplo com
`winget install LLVM.LLVM Kitware.CMake Ninja-build.Ninja`) e rode os
comandos acima num terminal.

**macOS:** `xcode-select --install` e `brew install cmake ninja`.

A pasta `assets/` e copiada para o lado do executavel a cada build.
