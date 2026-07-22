# M0 root build integration

The implementation is contained in `extension/hybrid_search/`. Its root-build
integration consists of one entry in an existing build file:

- `extension/CMakeLists.txt` contains
  `add_extension_if_enabled("hybrid_search")` beside the other extension
  entries.

This is the minimum required change: the root project already accepts arbitrary
values in `BUILD_EXTENSIONS`, and the extension-local CMake file defines both
`neug_hybrid_search_extension` and the C++ test target
`hybrid_search_extension_test`.

After approval, configure and run only the M0 target with:

```sh
cmake -S . -B build -DBUILD_TEST=ON -DBUILD_EXTENSIONS=hybrid_search
cmake --build build --target hybrid_search_extension_test -j$(sysctl -n hw.ncpu)
ctest --test-dir build -R '^hybrid_search_extension_test$' --output-on-failure
```
