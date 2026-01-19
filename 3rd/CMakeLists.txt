# 添加 tomlc17 库
add_library(tomlc17 STATIC
    toml/tomlc17.c
)

target_include_directories(tomlc17 PUBLIC ${PROJECT_SOURCE_DIR}/3rd/toml)