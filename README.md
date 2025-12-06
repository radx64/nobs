# nobs (No Build System)

A lightweight, header-only C++ library for building projects without external build system dependencies. nobs allows you to compile C++ projects using just your compiler, eliminating the need for complex build systems like Make, Ninja, or CMake.

## Overview

nobs is designed for developers who want to:
- Build small C++ projects without build system overhead
- Maintain simple project structures with minimal dependencies
- Avoid the complexity of traditional build systems
- Use a header-only solution that works with any standard C++ compiler

## Features

- Header-only implementation - no library dependencies
- Zero external build tool requirements
- Works with any standard C++ compiler
- Simple and straightforward project configuration
- Minimal setup required to start building

## Getting Started

Simply include the `nobs.hpp` header in your project and you're ready to go. No installation or configuration required.

```cpp
#include "nobs.hpp"
```

## Examples

Check out the `tests/simple_demo` directory for example usage and project configuration.

## Requirements

- Any standard C++ compiler (GCC, Clang, MSVC, etc.)
- No additional dependencies needed

## TODO
- [x] Simple Linux build support
- [x] Incremental builds (avoid recompiling unchanged files)
- [x] Add self rebuilding capability
- [x] Clean build dir as parameter support
- [ ] Linking parameters support
- [x] Dependency graph support (build ordering of files)
- [x] Parallel translation units compilation support
- [ ] Static and shared libraries support
- [ ] Windows support

## License

This project is licensed under the MIT License - see the LICENSE file for details.
