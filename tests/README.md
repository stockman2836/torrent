# Unit Tests

This directory contains comprehensive unit tests for the BitTorrent client.

## Test Framework

We use **Google Test** (gtest) as the testing framework. It's automatically downloaded and configured by CMake via FetchContent.

## Test Coverage

### Implemented Tests

1. **test_bencode.cpp** - Bencode Parser Tests
   - Integer parsing (positive, negative, zero, edge cases)
   - String parsing (empty, long, binary data)
   - List parsing (empty, nested)
   - Dictionary parsing (empty, nested)
   - Encoding tests
   - Round-trip tests
   - Error handling

2. **test_torrent_file.cpp** - Torrent File Parsing Tests
   - Single file torrent parsing
   - Multi-file torrent parsing
   - Announce list parsing
   - Info hash calculation
   - Required field validation
   - Error handling for invalid torrents

3. **test_piece_manager.cpp** - Piece Manager Logic Tests
   - Piece tracking and completion
   - Block management
   - Piece selection strategies (sequential, rarest-first, random-first)
   - Progress calculation
   - Bitfield management
   - Thread safety
   - Edge cases

4. **test_file_manager.cpp** - File Manager I/O Tests
   - File initialization (single and multi-file)
   - Piece writing and reading
   - Multi-file spanning
   - Round-trip verification
   - Concurrent access
   - Edge cases

## Building and Running Tests

### Prerequisites

- CMake 3.15 or higher
- C++17 compatible compiler
- OpenSSL
- libcurl
- Internet connection (first build only, to download Google Test)

### Build Commands

#### Windows (Visual Studio)

```bash
# Configure
cmake -B build -DBUILD_TESTS=ON

# Build
cmake --build build --config Release

# Run all tests
cd build\tests\Release
test_bencode.exe
test_torrent_file.exe
test_piece_manager.exe
test_file_manager.exe
```

#### Linux/macOS

```bash
# Configure
cmake -B build -DBUILD_TESTS=ON

# Build
cmake --build build

# Run all tests
cd build/tests
./test_bencode
./test_torrent_file
./test_piece_manager
./test_file_manager
```

#### Using CTest

```bash
# Build
cmake -B build -DBUILD_TESTS=ON
cmake --build build

# Run all tests at once
cd build
ctest --output-on-failure

# Run tests in verbose mode
ctest -V

# Run specific test
ctest -R test_bencode
```

## Test Organization

Each test file follows this structure:

1. **Test Fixture Class** - Sets up common test data and helper methods
2. **Test Categories** - Tests grouped by functionality
3. **Test Cases** - Individual test functions using EXPECT/ASSERT macros

### Example Test Structure

```cpp
class BencodeParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code
    }

    void TearDown() override {
        // Cleanup code
    }
};

TEST_F(BencodeParserTest, TestName) {
    // Test implementation
    EXPECT_EQ(actual, expected);
}
```

## Code Coverage

To generate code coverage reports (Linux/macOS with GCC/Clang):

```bash
# Configure with coverage flags
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Coverage

# Build and run tests
cmake --build build
cd build && ctest

# Generate coverage report (requires lcov)
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' --output-file coverage.info
lcov --list coverage.info
```

## Continuous Integration

Tests are designed to run in CI/CD environments. Example GitHub Actions workflow:

```yaml
- name: Build and Test
  run: |
    cmake -B build -DBUILD_TESTS=ON
    cmake --build build
    cd build && ctest --output-on-failure
```

## Adding New Tests

1. Create new test file in `tests/` directory (e.g., `test_new_component.cpp`)
2. Add test executable to `tests/CMakeLists.txt`:
   ```cmake
   add_unit_test(test_new_component test_new_component.cpp)
   ```
3. Write tests using Google Test macros
4. Build and run

## Test Guidelines

- **Test Independence**: Each test should be independent and not rely on other tests
- **Clear Names**: Use descriptive test names (e.g., `ParsePositiveInteger`, not `Test1`)
- **Arrange-Act-Assert**: Follow AAA pattern in test structure
- **Edge Cases**: Include tests for boundary conditions and error cases
- **Thread Safety**: Test concurrent access where applicable
- **Clean Up**: Use SetUp/TearDown for resource management

## Current Test Statistics

- **Total Test Files**: 4
- **Coverage Target**: > 80%
- **All Tests**: Should pass ✓

## Troubleshooting

### CMake can't find Google Test
Google Test is downloaded automatically via FetchContent. Ensure you have internet access on first build.

### Linker errors
Make sure all dependencies (OpenSSL, libcurl) are installed and findable by CMake.

### Tests fail on file operations
Ensure the test has write permissions to create temporary directories (uses system temp directory).

### Thread sanitizer warnings
Some thread safety tests may trigger warnings - these are expected when testing concurrent access patterns.

## Next Steps

- [ ] Add integration tests for full download workflow
- [ ] Add mock tests for network components (tracker, peer connections)
- [ ] Implement code coverage CI reporting
- [ ] Add performance benchmarks
