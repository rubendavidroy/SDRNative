# Contributing

Include the OS, receiver model, and reproduction steps with hardware bug
reports. Run the mock-backed C tests before submitting patches:

```sh
cmake -S . -B build -DSDRNATIVE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For hardware changes, check device enumeration and capture IQ and DVB-T data
on a receiver. The programs are in `examples/c/`. Record the OS, hardware,
frequencies and any failures.