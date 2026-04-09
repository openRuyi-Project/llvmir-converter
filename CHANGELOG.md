# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2024-01-01

### Added

#### New Features

- **`--tool-version` option** - Display tool version and LLVM version information
  ```bash
  ./llvmir-converter-22 --tool-version
  ```
  Shows tool version, LLVM version, copyright and license information.

- **`--version` alias** - Compatibility alias for `--tool-version`.

- **`--list-targets` option** - List common target architectures supported by typical LLVM builds.

- **`-v` / verbose mode** - Enable detailed output for debugging and monitoring
  ```bash
  ./llvmir-converter-22 -v test/llvmir/libxx.so.1_cmd
  ```
  Displays detailed information about each processing step.

- **`--incremental` option** - Skip compilation if output is newer than input
  ```bash
  ./llvmir-converter-22 --incremental test/llvmir/libxx.so.1_cmd
  ```
  Compares file modification times and skips unnecessary recompilation.

- **`--dry-run` option** - Parse and validate without executing
  ```bash
  ./llvmir-converter-22 --dry-run test/llvmir/libxx.so.1_cmd
  ```
  Validates _cmd file configuration without performing actual compilation.

#### Documentation

- **README.md** - Project overview and quick start guide
- **USAGE.md** - Comprehensive usage documentation with examples
- **CHANGELOG.md** - Version history and change tracking
- **Inline comments** - Enhanced code documentation in llvmir-converter.cpp

### Changed

- Improved `processCmdFile()` function with additional parameters for dry-run and incremental modes
- Enhanced help message to include all new options
- Better error messages with validation for output file specification

### Fixed

- Added validation for missing output file in _cmd scripts
- Improved error handling for non-existent input IR files

---

## [0.1.0] - 2024-12-01 (Initial Release)

### Added

- Core functionality: Convert LLVM IR bitcode to ELF executables/shared libraries
- `_cmd` script parsing for compilation parameters
- Template LL file support for target features
- Automatic clang version path replacement
- Version script file handling for shared libraries
- Basic Makefile for building with configurable LLVM version
- Test examples for shared libraries and executables

### Features

- **IR Processing**: Parse and modify LLVM IR with target feature merging
- **RISC-V Support**: Handle RISC-V module flags and ISA info
- **Temporary Directory Management**: Safe temporary file handling with cleanup
- **Output Organization**: Automatic subdirectory creation based on input location

---

## Upcoming Features (Planned)

### [1.1.0] - Planned

- **`--jobs N` option** - Parallel compilation of multiple files
- **`--log-file` option** - Save compilation logs to file
- **Progress display** - Show progress bar for batch processing
- **CMake support** - Alternative build system for cross-platform compatibility

### [1.2.0] - Planned

- **Configuration file** - Support for `~/.llvmir-converter.conf`
- **Environment variables** - `LLVMIR_TEMPLATE_PATH`, `LLVMIR_OUTPUT_DIR`
- **Cross-compilation improvements** - Better multi-architecture support
- **Compilation cache** - Cache processed IR files for faster rebuilds

### [2.0.0] - Planned

- **Unit testing framework** - GoogleTest integration
- **CI/CD integration** - GitHub Actions workflow
- **Plugin system** - Support for custom IR transformations
- **GUI wrapper** - Optional graphical interface

---

## Version History Summary

| Version | Date | Description |
|---------|------|-------------|
| 1.0.0 | 2024-01-01 | Major features: tool-version, verbose, incremental, dry-run |
| 0.1.0 | 2024-12-01 | Initial release with core functionality |

---

## Migration Guide

### Upgrading from 0.1.0 to 1.0.0

No breaking changes. All existing functionality remains compatible.

**New recommended usage:**

```bash
# Before (still works)
./llvmir-converter-22 -t template.ll input_cmd

# After (recommended for debugging)
./llvmir-converter-22 -v --incremental -t template.ll input_cmd
```

---

## Contributing

Contributions are welcome! Please read the contributing guidelines before submitting pull requests.

### How to Contribute

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

---

## License

Apache-2.0 - See LICENSE file for details.

---

## Contact

- Author: YunQiang Su <yunqiang@isrc.iscas.ac.cn>
- Organization: Institute of Software, Chinese Academy of Sciences (ISCAS)
- Copyright: 2026
