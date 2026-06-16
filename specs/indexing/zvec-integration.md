# 将 ZVec 依赖方式修改为 add_subdirectory

目前 ZVec 依赖方式是直接通过预编译的 `.a` 静态库 + 头文件，但该方案会导致跨平台后 `.a` 静态库存在兼容问题。

目前需要切换成 add_subdirectory 直接源码编译的方式，具体步骤有：
- 启动 BUILD_EXTENSIONS=zvec 之后，git clone zvec 源码到 third_party/zvec
- cmake 编写 zvec 相关配置
- 通过 add_subdirectory 集成进 neug zvec extension

帮我按照这个方案实施，之前调研显示有一些冲突问题：
- ZVec 使用自定义 bazel.cmake，会导致和 NeuG cmake 冲突
- NeuG 和 ZVec 使用的 C++ 版本不一致

解决这些冲突问题，直到编译成功，并可以通过 zvec_extension 和 test_hnsw_index.py 测试