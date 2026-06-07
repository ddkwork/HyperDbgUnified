cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release . && cmake --build build --config Release > build.log 2>&1
@REM cmake -B build_debug -G "Ninja" -DCMAKE_BUILD_TYPE=Debug . && cmake --build build_debug --config Debug
