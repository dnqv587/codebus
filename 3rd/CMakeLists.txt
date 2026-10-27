# 添加 tomlc17 库
add_library(tomlc17 STATIC
    toml/tomlc17.c
)

target_include_directories(tomlc17 PUBLIC ${PROJECT_SOURCE_DIR}/3rd/toml)

# 添加 http_parser 库（joyent v2.9.4+，源码钉死入树）
add_library(http_parser STATIC
    http_parser/http_parser.c
)

target_include_directories(http_parser PUBLIC ${PROJECT_SOURCE_DIR}/3rd/http_parser)