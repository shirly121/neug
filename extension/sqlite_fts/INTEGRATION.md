# M0 root build integration

The M0 implementation is contained in `extension/sqlite_fts/` and has no
SQLite dependency. Its root-build integration consists of one entry in an
existing build file:

- `extension/CMakeLists.txt` contains
  `add_extension_if_enabled("sqlite_fts")` beside the other extension entries.

This is the minimum required change: the root project already accepts arbitrary
values in `BUILD_EXTENSIONS`, and the extension-local CMake file defines both
`neug_sqlite_fts_extension` and the C++ test target
`sqlite_fts_extension_test`.

After approval, configure and run only the M0 target with:

```sh
cmake -S . -B build -DBUILD_TEST=ON -DBUILD_EXTENSIONS=sqlite_fts
cmake --build build --target sqlite_fts_extension_test -j$(sysctl -n hw.ncpu)
ctest --test-dir build -R '^sqlite_fts_extension_test$' --output-on-failure
```
