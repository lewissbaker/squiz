# Squiz

An experimental playground for improvements to the sender/receiver design for P2300.

This implementation explores the following ideas:
- Optimization of operation-state storage by omitting need to store receiver in cases where the operation-state is
  stored inline as a sub-object of a parent operation-state.
- Alternative cancellation mechanism that uses an optional `request_stop()` member-function on the
  `operation_state` object rather than using stop-tokens.
- Additional `is_always_nothrow_connectable()` query on senders that allows senders to
  declare that the `connect()` overload will be noexcept regardless of what receiver
  type is passed to it. This is needed to avoid algorithms like `let_value()`, which call
  `connect()` during their execution, from unconditionally including `set_error_t(std::exception_ptr)`
  in their completion-signatures, just in case the call to `connect()` on the body-sender is not
  `noexcept`, which in general can't be determined until we have a receiver.
- The ability to query whether or not an operation is stoppable using the `stoppable_operation_state`
  concept as well as querying whether or not the caller is potentially going to send a stop-request
  or whether it will never send a stop-request.
  This can allow for more efficient algorithm implementations if we can determine if there
  is no point doing extra work to try to stop a child operation or doing extra work to
  handle a stop-request that will never arrive.

Note that this library makes use of C++26 features such as pack-indexing and as such
requires a very recent compiler. e.g. Clang-19 or later.

## Usage

### CMake Structure

To cleanly separate the library and subproject code, the outer `CMakeList.txt` only defines the library itself while the tests and other subprojects are self-contained in their own directories. 
During development it is usually convenient to [build all subprojects at once](#build-everything-at-once).

### Build and run test suite

Use the following commands from the project's root directory to run the test suite.

```bash
cmake -S test -B build/test
cmake --build build/test
CTEST_OUTPUT_ON_FAILURE=1 cmake --build build/test --target test

# or simply call the executable: 
./build/test/squiz_tests
```

To collect code coverage information, run CMake with the `-DENABLE_TEST_COVERAGE=1` option.

### Run clang-format

Use the following commands from the project's root directory to check and fix C++ and CMake source style.
This requires _clang-format_, _cmake-format_ and _pyyaml_ to be installed on the current system.

```bash
cmake -S test -B build/test

# view changes
cmake --build build/test --target format

# apply changes
cmake --build build/test --target fix-format
```

See [Format.cmake](https://github.com/TheLartians/Format.cmake) for details.
These dependencies can be easily installed using pip.

```bash
pip install clang-format==14.0.6 cmake_format==0.6.11 pyyaml
```

### Build the documentation

The documentation is automatically built and [published](https://thelartians.github.io/ModernCppStarter) whenever a [GitHub Release](https://help.github.com/en/github/administering-a-repository/managing-releases-in-a-repository) is created.
To manually build documentation, call the following command.

```bash
cmake -S documentation -B build/doc
cmake --build build/doc --target GenerateDocs
# view the docs
open build/doc/doxygen/html/index.html
```

To build the documentation locally, you will need Doxygen, jinja2 and Pygments installed on your system.

### Build everything at once

The project also includes an `all` directory that allows building all targets at the same time.
This is useful during development, as it exposes all subprojects to your IDE and avoids redundant builds of the library.

```bash
cmake -S all -B build
cmake --build build

# run tests
./build/test/squiz_tests
# format code
cmake --build build --target fix-format
# build docs
cmake --build build --target GenerateDocs
```

### Additional tools

The test and standalone subprojects include the [tools.cmake](cmake/tools.cmake) file which is used to import additional tools on-demand through CMake configuration arguments.
The following are currently supported.

#### Sanitizers

Sanitizers can be enabled by configuring CMake with `-DUSE_SANITIZER=<Address | Memory | MemoryWithOrigins | Undefined | Thread | Leak | 'Address;Undefined'>`.

#### Static Analyzers

Static Analyzers can be enabled by setting `-DUSE_STATIC_ANALYZER=<clang-tidy | iwyu | cppcheck>`, or a combination of those in quotation marks, separated by semicolons.
By default, analyzers will automatically find configuration files such as `.clang-format`.
Additional arguments can be passed to the analyzers by setting the `CLANG_TIDY_ARGS`, `IWYU_ARGS` or `CPPCHECK_ARGS` variables.

#### Ccache

Ccache can be enabled by configuring with `-DUSE_CCACHE=<ON | OFF>`.
