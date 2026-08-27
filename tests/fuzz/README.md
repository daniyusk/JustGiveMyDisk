# MFT parser fuzzing

The fuzz target is deliberately excluded from normal builds. Configure it with
Clang explicitly:

```sh
cmake -S . -B build-fuzz \
  -DJUSTGIVEMYDISK_BUILD_APP=OFF \
  -DBUILD_TESTING=OFF \
  -DJUSTGIVEMYDISK_BUILD_FUZZER=ON \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target MftRecordParserFuzz
./build-fuzz/MftRecordParserFuzz -max_len=2048 corpus/
```

Each input is exercised both as raw bytes (covering truncation and arbitrary
corruption) and as deterministic mutations of a valid synthetic FILE record
(reaching USA, attribute, UTF-16, and data-run parsing quickly).
