# Contributing to Tester

Thank you for your interest in contributing to Tester! This document provides guidelines and instructions for contributing.

## Development Setup

### Prerequisites

- **Clang 19+** with C++23 modules support
- **Make** build system
- **Git** for version control

### Getting Started

1. **Clone the repository:**
   ```bash
   git clone https://github.com/ruoka/tester.git
   cd tester
   ```

2. **Initialize submodules:**
   ```bash
   git submodule init
   git submodule update
   ```

3. **Build the project:**
   ```bash
   make clean
   make module
   ```

4. **Run examples:**
   ```bash
   make run_examples
   ```

5. **Run tests:**
   ```bash
   make tests
   build/bin/test_runner
   ```

### Using Dev Containers

The project includes a VS Code devcontainer configuration. Simply open the project in VS Code and select "Reopen in Container" when prompted.

## Code Style

The project follows the [C++ Core Guidelines](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md) for code style and best practices, with the following exception:

**Naming Convention:**
- **All identifiers use `snake_case`** (including class names, functions, variables, etc.)
- This differs from the Core Guidelines' typical PascalCase for classes and camelCase for functions
- Examples:
  - Classes: `test_case`, `test_runner` (not `TestCase`, `TestRunner`)
  - Functions: `require_eq()`, `check_neq()` (not `requireEq()`, `checkNeq()`)
  - Variables: `test_name`, `assertion_count` (not `testName`, `assertionCount`)

**Other Style Rules:**
- Use 4 spaces for indentation in C++ files
- Follow P1204R0 module organization guidelines
- Keep modules focused and well-documented
- Refer to the C++ Core Guidelines for:
  - [Functions](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#s-functions)
  - [Classes and class hierarchies](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#s-class)
  - [Resource management](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#s-resource)
  - [Error handling](https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#s-errors)
  - And other sections as applicable

## Module Organization

- Module files use `.c++m` extension
- Implementation files use `.impl.c++` extension
- Test files use `.test.c++` extension (co-located with source)
- Module names use the `tester` namespace with submodules (`tester:assertions`, `tester:basic`, etc.)

## Testing

- Unit tests are co-located with source files using `.test.c++` extension
- Examples are in the `examples/` directory
- Run tests with `build/bin/test_runner`
- Use tag-based filtering for selective test execution:
  ```bash
  build/bin/test_runner --tags="scenario.*Happy"
  build/bin/test_runner --tags=[acceptor]
  ```
- Ensure all tests pass before submitting a pull request

## Assertions

Tester provides both fatal (`require_*`) and non-fatal (`check_*`) assertions:

- **Fatal assertions** (`require_*`): Stop test execution on failure by throwing `assertion_failure`
- **Non-fatal assertions** (`check_*`): Log failures but continue test execution

Examples:
- `require_eq(a, b)` - Fatal: throws if `a != b`
- `check_eq(a, b)` - Non-fatal: logs if `a != b`, continues
- `require_throws([] { throw std::exception{}; })` - Fatal: throws if no exception
- `check_throws([] { throw std::exception{}; })` - Non-fatal: logs if no exception

## Submitting Changes

1. **Create a branch:**
   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes** and ensure tests pass

3. **Commit your changes:**
   ```bash
   git add .
   git commit -m "Description of your changes"
   ```

4. **Push and create a pull request**

## Build System

- Main Makefile is in the project root
- Compiler configuration is in `config/compiler.mk` (if present) or inherited from parent
- Build artifacts go to `build/` directory
- Submodules are built with `PREFIX=../../build` when embedded in a parent project

## Questions?

If you have questions or need help, please open an issue on GitHub.

