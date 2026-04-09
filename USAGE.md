# LLVMIR-Converter 详细使用指南

本文档提供 llvmir-converter 工具的详细使用说明和示例。

## 目录

1. [快速开始](#快速开始)
2. [命令行选项详解](#命令行选项详解)
3. [_cmd 脚本编写指南](#_cmd-脚本编写指南)
4. [模板文件](#模板文件)
5. [使用场景](#使用场景)
6. [故障排除](#故障排除)
7. [高级用法](#高级用法)

---

## 快速开始

### 安装与构建

```bash
# 克隆仓库
git clone <repository-url>
cd llvmir-converter

# 构建
make LLVM_VER=22

# 验证构建
./llvmir-converter-22 --tool-version
```

### 最简单的使用

```bash
# 处理单个 _cmd 文件
./llvmir-converter-22 test/llvmir/libxx.so.1_cmd

# 查看输出
ls output/lib/
```

---

## 命令行选项详解

### 输入输出选项

#### `-o=<directory>` - 输出目录

指定最终输出文件的存放目录。

```bash
# 输出到 build 目录
./llvmir-converter-22 -o=build test/llvmir/libxx.so.1_cmd

# 输出到当前目录
./llvmir-converter-22 -o=. test/llvmir/libxx.so.1_cmd
```

**默认值：** `output`

**注意：** 工具会根据输入文件所在目录自动创建子目录：
- 来自 `llvmir/` 的文件 → `<output>/lib/`
- 来自 `llvmir-bin/` 的文件 → `<output>/bin/`

#### `--pgo-output=<directory>` - PGO instrumentation 输出目录

额外生成一份带 PGO instrumentation 的转换结果。该目录不能与 `-o` 指定的普通输出目录相同。

使用该选项时，必须同时指定 `--pgo-profiles-output=<directory>`：

```bash
./llvmir-converter-22 \
  -o=output \
  --pgo-output=pgo-output \
  --pgo-profiles-output=pgo-profraw \
  test/llvmir-bin/test_cmd
```

PGO 版本会保持普通输出的子目录规则，例如来自 `llvmir-bin/` 的文件会输出到 `<pgo-output>/bin/`。

#### `--pgo-profiles-output=<directory>` - PGO profile raw 输出目录

指定被 instrumentation 的程序运行时写入 `.profraw` 的目录。生成 PGO 版本时，工具会向链接命令追加：

```bash
-fprofile-instr-generate=<pgo-profiles-output>/<output-name>_%p.profraw
```

其中 `<output-name>` 来自 `_cmd` 中 `-o` / `--output` 指定输出路径的文件名。例如 `_cmd` 中输出为 `output/myapp`，则追加参数为：

```bash
-fprofile-instr-generate=pgo-profraw/myapp_%p.profraw
```

#### `-t=<filename>` - 模板文件

指定包含目标特性的模板 LL 文件。模板文件中的 `target-features` 属性将被合并到输入 IR 的每个函数中。

```bash
# 使用模板文件
./llvmir-converter-22 -t test/template.ll test/llvmir/libxx.so.1_cmd
```

模板文件示例 (`template.ll`)：

```llvm
; Template for x86_64 with SSE2
define void @template() {
entry:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"target-features", !"+64bit,+fxsr,+sse,+sse2"}
```

### 编译控制选项

#### `--incremental` - 增量编译

启用增量编译模式。工具会比较输入文件和输出文件的修改时间，如果输出较新则跳过编译。

```bash
# 增量编译
./llvmir-converter-22 --incremental test/llvmir/libxx.so.1_cmd
```

**使用场景：**
- 大型项目部分重新编译
- 自动化构建系统
- 节省编译时间

#### `--dry-run` - 试运行

仅解析和验证输入文件，不执行实际编译。

```bash
# 验证配置但不编译
./llvmir-converter-22 --dry-run test/llvmir/libxx.so.1_cmd
```

**输出示例：**
```
Dry run: test/llvmir/libxx.so.1_cmd
  Input: /path/to/libxx.so.1
  Output: /path/to/output/lib/libxx.so.1
  Version script: ./libxx.so.1_verscript
Validation successful.
```

#### `--keep-temp` - 保留临时文件

保留编译过程中创建的临时文件，便于调试。

```bash
# 保留临时文件
./llvmir-converter-22 --keep-temp test/llvmir/libxx.so.1_cmd
```

**临时文件位置：** `/tmp/llvmir-XXXXXX/` (XXXXXX 为随机字符串)

### 信息显示选项

#### `-v` - 详细输出

启用详细输出模式，显示编译过程的详细信息。

```bash
# 详细模式
./llvmir-converter-22 -v test/llvmir/libxx.so.1_cmd
```

**输出示例：**
```
[VERBOSE] Processing _cmd file: /path/to/libxx.so.1_cmd
[VERBOSE] Command directory: /path/to/llvmir
[VERBOSE] Input file: ./libxx.so.1
[VERBOSE] Output file: output/libxx.so.1
[VERBOSE] Version script: ./libxx.so.1_verscript
Processing IR: /path/to/libxx.so.1
Using template: test/template.ll
...
```

#### `--tool-version` - 版本信息

显示工具版本和编译使用的 LLVM 版本。

```bash
./llvmir-converter-22 --tool-version
```

**输出：**
```
llvmir-converter version 1.0.0
Built with LLVM 22.1.0

Copyright 2024 Institute of Software, Chinese Academy of Sciences (ISCAS)
Author: YunQiang Su <yunqiang@isrc.iscas.ac.cn>
License: Apache-2.0
```

`--version` 是 `--tool-version` 的兼容别名。

#### `--list-targets` - 常见目标架构

列出工具识别的常见目标架构；实际可用目标取决于链接的 LLVM 构建。

```bash
./llvmir-converter-22 --list-targets
```

---

## _cmd 脚本编写指南

### 基本格式

`_cmd` 脚本是一个包含 clang 编译命令的 shell 脚本：

```bash
#!/bin/bash
mkdir -p output
cmd="/usr/bin/clang-22 -O3 -fuse-ld=lld -shared -lc ./input.bc -o output/output.so"
eval $cmd
```

clang-wrap 生成的 Bash 数组格式同样受支持：

```bash
cmd=(
  clang-21
  -x
  ir
  ./input.bc
  --output=output/output.so
)
"${cmd[@]}"
```

旧式 `cmd="..."` 字符串可以写成多行反斜杠续行格式。

### 必需元素

1. **Shebang 行**：`#!/bin/bash`
2. **输出目录创建**：`mkdir -p output`
3. **Clang 命令**：包含输入文件、输出文件和编译选项

### 支持的编译选项

工具会解析以下选项：

| 选项 | 说明 |
|------|------|
| `./<file>` | 输入文件（以 `./` 开头） |
| `-o <file>` | 输出文件 |
| `--output=<file>` | 输出文件 |
| `--output <file>` | 输出文件 |
| `--version-script <file>` | 版本脚本文件 |
| `--version-script=<file>` | 版本脚本文件 |
| `-Wl,--version-script=<file>` | 链接器版本脚本 |

如果命令中包含多个 `./<file>` 输入，工具会逐个处理这些 IR 文件，并在链接命令中替换为对应的临时处理结果。

### 完整示例

#### 共享库

```bash
#!/bin/bash
mkdir -p output
cmd="/usr/bin/clang-22 -O3 -fuse-ld=lld -shared -lc \
     -Wl,--version-script=./libmylib.so.1_verscript \
     ./libmylib.so.1 \
     -o output/libmylib.so.1"
eval $cmd
```

#### 可执行文件

```bash
#!/bin/bash
mkdir -p output
cmd="/usr/bin/clang-22 -O3 -fuse-ld=lld -lc \
     ./myprogram \
     -o output/myprogram"
eval $cmd
```

#### 带 RISC-V 目标

```bash
#!/bin/bash
mkdir -p output
cmd="/usr/bin/clang-22 -O3 -target riscv64-linux-gnu \
     -fuse-ld=lld -lc \
     ./myprogram \
     -o output/myprogram"
eval $cmd
```

### Version Script 文件

Version script 用于控制共享库的符号可见性：

```
LIBMYLIB_1.0 {
    global:
        my_public_function;
        another_public_function;
    local:
        *;
};
```

---

## 模板文件

### 作用

模板文件用于指定目标特性（target-features），这些特性将被合并到输入 IR 的每个函数中。

### 创建模板文件

#### 方法 1：手动创建

```llvm
; Template for x86_64 with AVX2
define void @template() {
entry:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"target-features", !"+64bit,+fxsr,+sse,+sse2,+avx,+avx2"}
```

#### 方法 2：使用 clang 生成

```bash
# 创建简单的 C 文件
echo 'void template(void) {}' > template.c

# 编译为 LLVM IR
clang -S -emit-llvm -O2 -march=native -o template.ll template.c

# 查看生成的 target-features
grep target-features template.ll
```

### Target Features 示例

#### x86_64

```
"+64bit,+fxsr,+sse,+sse2,+avx,+avx2,+fma"
```

#### ARM64

```
"+v8.5a,+crc,+crypto,+fp-armv8,+neon"
```

#### RISC-V 64

```
"+64bit,+m,+a,+f,+d,+c"
```

---

## 使用场景

### 场景 1：批量编译共享库

```bash
# 编译 llvmir/ 目录下的所有 _cmd 文件
for cmd_file in llvmir/*_cmd; do
    ./llvmir-converter-22 -t template.ll "$cmd_file"
done
```

### 场景 2：增量编译大型项目

```bash
# 使用增量模式，只编译修改过的文件
./llvmir-converter-22 --incremental -t template.ll project/llvmir/lib1.so_cmd
./llvmir-converter-22 --incremental -t template.ll project/llvmir/lib2.so_cmd
```

### 场景 3：CI/CD 验证

```bash
# 在 CI 中验证配置
./llvmir-converter-22 --dry-run test/llvmir/libxx.so.1_cmd
if [ $? -eq 0 ]; then
    echo "Configuration is valid"
else
    echo "Configuration error"
    exit 1
fi
```

### 场景 4：调试编译问题

```bash
# 使用详细模式和保留临时文件进行调试
./llvmir-converter-22 -v --keep-temp test/llvmir/libxx.so.1_cmd

# 检查临时文件
ls /tmp/llvmir-*
```

### 场景 5：常驻批量转换并按资源限速

```bash
python3 llvmir_batch_runner.py --output-dir=output test/llvmir test/llvmir-bin

# 同时生成 PGO instrumentation 版本
python3 llvmir_batch_runner.py \
  --output-dir=output \
  --pgo-output=pgo-output \
  --pgo-profiles-output=pgo-profraw \
  test/llvmir-bin
```

必选参数为一个或多个输入路径，可以是 `_cmd` 文件，也可以是包含 `*_cmd` 文件的目录；目录会被递归扫描。`--output-dir` 默认值为 `output`。脚本会读取 `_cmd` 中的 `clang-XX` 并优先调用同版本 `llvmir-converter-XX`，同时自动生成 `-march=native` 模板 LL 文件用于 `-t` 参数。

性能相关选项都有默认值，可按需覆盖：

- `--cpu-limit=90`：系统 CPU 使用率达到该值时暂停转换进程
- `--cpu-resume=60`：系统 CPU 使用率低于该值时允许恢复转换进程
- `--memory-limit=85`：系统内存使用率达到该值时暂停转换进程
- `--memory-resume=70`：系统内存使用率低于该值时允许恢复转换进程

常用可选项：

- `--scan-interval <seconds>`：常驻扫描间隔，默认 5 秒
- `--monitor-interval <seconds>`：资源监控间隔，默认 1 秒
- `--template <file>`：使用已有模板 LL 文件，不自动生成
- `--converter-dir <dir>`：优先从指定目录查找 `llvmir-converter-XX`
- `--incremental`：传递给 `llvmir-converter`，跳过已是最新的输出
- `--dry-run`：传递给 `llvmir-converter`，只解析验证不执行编译
- `--pgo-output <dir>`：传递给 `llvmir-converter`，指定 PGO instrumentation 输出目录
- `--pgo-profiles-output <dir>`：传递给 `llvmir-converter`，指定运行时 `.profraw` 输出目录；使用 `--pgo-output` 时必须同时指定
- `--keep-going`：单个 `_cmd` 失败后继续处理其他文件

`llvmir_batch_runner.py` 会将 `--pgo-output` 和 `--pgo-profiles-output` 转换为绝对路径后传给底层 converter。`--pgo-output` 不能与 `--output-dir` 解析后的绝对路径相同；只指定 `--pgo-profiles-output` 不会报错，但不会单独生成 PGO 输出。

---

## 故障排除

### 错误：找不到输入文件

```
Error: Could not find input file in _cmd file
```

**解决方法：** 确保 _cmd 脚本中有以 `./` 开头的输入文件路径。

### 错误：输入 IR 文件不存在

```
Error: Input IR file does not exist: /path/to/input.bc
```

**解决方法：** 检查 IR 文件路径是否正确，确保文件存在。

### 错误：编译失败

```
Error: Compilation failed
```

**解决方法：**
1. 使用 `-v` 查看详细信息
2. 使用 `--keep-temp` 保留临时文件
3. 手动运行临时目录中的 _cmd 脚本进行调试

### 错误：clang 版本不匹配

```
/usr/bin/clang-18: command not found
```

**解决方法：** 工具会自动更新 clang 路径到当前 LLVM 版本。如果问题仍存在，请检查系统 clang 安装。

---

## 高级用法

### 集成到构建系统

#### Makefile 集成

```makefile
LLVMIR_CONVERTER = ./llvmir-converter-22
TEMPLATE = template.ll

libs:
	for cmd in llvmir/*_cmd; do \
		$(LLVMIR_CONVERTER) -t $(TEMPLATE) $$cmd; \
	done

bins:
	for cmd in llvmir-bin/*_cmd; do \
		$(LLVMIR_CONVERTER) -t $(TEMPLATE) $$cmd; \
	done
```

#### CMake 集成

```cmake
find_program(LLVMIR_CONVERTER llvmir-converter-22)

add_custom_target(convert_llvmir
    COMMAND ${LLVMIR_CONVERTER} -t ${CMAKE_SOURCE_DIR}/template.ll
            ${CMAKE_SOURCE_DIR}/llvmir/mylib.so_cmd
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
```

---

## 更多帮助

如有问题或建议，请联系：

- Email: yunqiang@isrc.iscas.ac.cn
- 项目主页: https://github.com/iscas-tis/llvmir-converter
- Copyright: 2026 Institute of Software, Chinese Academy of Sciences (ISCAS)
