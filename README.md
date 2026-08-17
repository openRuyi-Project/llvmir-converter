# LLVMIR-Converter

将 LLVM IR bitcode 文件转换为 ELF 可执行文件/共享库的工具。

## 简介

llvmir-converter 是一个命令行工具，用于将 LLVM IR bitcode 文件编译成 ELF 格式的可执行文件或共享库。它通过读取 `_cmd` 脚本文件来确定编译参数，并可选地使用模板 LL 文件来设置目标特性。

**主要功能：**
- 将 LLVM IR bitcode 转换为 ELF 可执行文件/共享库
- 自动处理 clang 版本兼容性
- 支持 version script 文件
- 支持增量编译
- 支持详细输出模式

## 构建方法

### 依赖项

- LLVM 开发库 (LLVM 18+)
- Clang 编译器
- C++17 支持的编译器

### 编译

```bash
# 使用默认 LLVM 版本 (22)
make

# 指定 LLVM 版本
make LLVM_VER=18
```

## 使用方法

### 基本用法

```bash
# 转换单个 _cmd 文件
./llvmir-converter-22 test/llvmir/libxx.so.1_cmd

# 指定输出目录
./llvmir-converter-22 -o output test/llvmir/libxx.so.1_cmd

# 使用模板文件
./llvmir-converter-22 -t test/template.ll test/llvmir/libxx.so.1_cmd

# 同时生成 PGO instrumentation 版本
./llvmir-converter-22 -o=output --pgo-output=pgo-output --pgo-profiles-output=pgo-profraw test/llvmir-bin/test_cmd
```

### 常驻批量转换

```bash
python3 llvmir_batch_runner.py --output-dir=output test/llvmir test/llvmir-bin

# 批量转换时同时生成 PGO instrumentation 版本
python3 llvmir_batch_runner.py \
  --output-dir=output \
  --pgo-output=pgo-output \
  --pgo-profiles-output=pgo-profraw \
  test/llvmir-bin
```

`llvmir_batch_runner.py` 接收一个或多个 `_cmd` 文件或目录作为位置参数；目录会被递归扫描以查找 `*_cmd` 文件。脚本会生成最接近当前 CPU 的 native 模板 LL 文件，并根据 `_cmd` 中的 `clang-XX` 调用对应版本的 `llvmir-converter-XX`。当系统 CPU 或内存使用率超过限制值时，脚本会向当前转换进程组发送 `SIGSTOP`；当 CPU 和内存都低于恢复值时发送 `SIGCONT` 继续转换。默认阈值为：`--cpu-limit=90`、`--cpu-resume=60`、`--memory-limit=85`、`--memory-resume=70`。失败的 `_cmd` 会保存到 `<output-dir>/.llvmir-batch-failures.json`；未修改的失败项在后续扫描和重启后都会被跳过，修改后会自动重试。使用 `--failure-log <file>` 可指定记录文件位置。

### 手动使用 PGO profile 重建 ELF

`llvmir_pgo_rebuild.py` 递归扫描 `.profraw`，按目标合并为 `.profdata`，再读取一个或多个 IR 路径中的 `*_cmd` 文件，使用 `-fprofile-instr-use` 生成临时 `_cmd`，并调用对应版本的 `llvmir-converter-XX` 重新生成 ELF。`--profraw-dir` 和 `--output-dir` 都是选项，最后可以传入多个 `_cmd` 文件或目录：

```bash
./llvmir_pgo_rebuild.py \
  --profraw-dir=/var/lib/llvmir-converter/pgo-profraw \
  --output-dir=/var/lib/llvmir-converter/pgo-rebuilt \
  --converter-dir=/usr/lib/llvmir-convert/bin \
  /usr/lib64/llvmir /usr/lib64/llvmir-bin
```

`--converter-dir` 的查找顺序与 `llvmir_batch_runner.py` 一致：先查该目录，再查脚本目录、当前目录和 `PATH`。版本化名称优先使用 `llvmir-converter-XX`，其次是 `llvmir-convert-XX`，最后回退到无版本名称。未指定 `--template` 时，脚本会在输出目录生成 `.llvmir-native-template.ll`，其生成方式与 batch runner 相同；也可以用 `--template <file>` 指定已有模板。首次运行可以加 `--dry-run` 检查待执行的 `llvm-profdata merge` 和 `llvmir-converter-XX` 命令。合并生成的 `.profdata` 和临时 `_cmd` 会在本次运行结束后自动删除。多个 IR 路径可以是目录或单个 `*_cmd` 文件，目录会递归扫描并自动去重。

批量 runner 会将 `--pgo-output` 和 `--pgo-profiles-output` 以绝对路径转发给底层 converter。使用 `--pgo-output` 时必须同时指定 `--pgo-profiles-output`，且 PGO 输出目录不能与 `--output-dir` 指向同一目录。

### 命令行选项

| 选项 | 说明 |
|------|------|
| `-o=<directory>` | 输出目录 (默认: output) |
| `--pgo-output=<directory>` | 额外输出 PGO instrumentation 版本的目录，不能与 `-o` 相同 |
| `--pgo-profiles-output=<directory>` | PGO 运行时 `.profraw` 输出目录；使用 `--pgo-output` 时必须指定 |
| `-t=<filename>` | 模板 LL 文件路径 |
| `-v` | 启用详细输出模式 |
| `--dry-run` | 仅解析验证，不执行编译 |
| `--incremental` | 增量编译，跳过已是最新输出的文件 |
| `--keep-temp` | 保留临时文件 |
| `--failure-log=<file>` | 失败项记录 JSON 文件，默认位于输出目录 |
| `--version` | 显示工具版本信息 |
| `--tool-version` | 显示工具版本信息 |
| `--list-targets` | 列出常见目标架构 |
| `--help` | 显示帮助信息 |

### _cmd 脚本格式

`_cmd` 脚本是一个 shell 脚本，包含 clang 编译命令：

```bash
#!/bin/bash
mkdir -p output
cmd="/usr/bin/clang-22 -O3 -fuse-ld=lld -shared -lc -Wl,--version-script=./libxx.so.1_verscript ./libxx.so.1 -o output/libxx.so.1"
eval $cmd
```

也支持 clang-wrap 生成的 Bash 数组格式：

```bash
cmd=(
  clang-21
  -x
  ir
  ./program
  --output=output/program
)
"${cmd[@]}"
```

命令中出现多个 `./<file>` 输入时，工具会逐个处理并在链接阶段替换为对应的临时 IR。

启用 `--pgo-output` 时，工具会额外执行一次带 PGO instrumentation 的链接，并追加如下参数：

```bash
-fprofile-instr-generate=<pgo-profiles-output>/<output-name>_%p.profraw
```

其中 `<output-name>` 来自 `_cmd` 中 `-o` / `--output` 指定路径的文件名。

### 目录结构

工具根据输入文件的父目录名自动确定输出子目录：

- `llvmir/` 目录下的文件 → `output/lib/`
- `llvmir-bin/` 目录下的文件 → `output/bin/`
- 其他目录 → 使用父目录名作为输出子目录

## 测试

```bash
make check
```

## 示例

查看 `test/` 目录中的示例文件：

- `test/llvmir/` - 共享库示例
- `test/llvmir-bin/` - 可执行文件示例
- `test/llvmir-bin/test_multi_cmd` - 多输入解析示例
- `test/llvmir-bin/test_multiline_cmd` - 多行字符串命令示例
- `test/template.ll` - 模板 LL 文件示例

## 许可证

Apache-2.0

## 作者

YunQiang Su <yunqiang@isrc.iscas.ac.cn>

Copyright 2026 Institute of Software, Chinese Academy of Sciences (ISCAS)
