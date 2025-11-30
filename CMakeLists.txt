# 設定 CMake 最低版本
cmake_minimum_required(VERSION 3.20.0)

# 載入 Zephyr 環境
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# 定義專案名稱
project(tracker_sniffer)

# 告訴編譯器，原始碼只有 src/main.c
target_sources(app PRIVATE src/main.c)
