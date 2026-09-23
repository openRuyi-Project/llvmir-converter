# LLVMIR-Converter PGO 实现总结

## 1. 总体流程

项目将 PGO 实现为一个基于预编译 LLVM IR 的两阶段闭环：

1. 从 `_cmd` 文件读取原始 Clang 链接命令。
2. 将 LLVM IR/bitcode 处理成普通 ELF。
3. 可选地再生成一份带 PGO instrumentation 的 ELF。
4. 运行插桩 ELF，产生 `.profraw` 文件。
5. 使用 `llvm-profdata` 将多个 `.profraw` 合并为 `.profdata`。
6. 根据原始 `_cmd` 生成临时 PGO 使用命令。
7. 使用 `-fprofile-use` 重新链接，生成经过 PGO 优化的 ELF。

主流程由 [llvmir-converter.cpp](llvmir-converter.cpp) 实现，PGO profile 使用阶段由 [llvmir_pgo_rebuild.py](llvmir_pgo_rebuild.py) 实现，批量调度由 [llvmir_batch_runner.py](llvmir_batch_runner.py) 实现。

## 2. PGO instrumentation 生成

### 2.1 独立输出目录

指定以下两个选项时，转换器会在普通 ELF 之外额外生成一份插桩版本：

```bash
--pgo-output=<instrumented-output-dir>
--pgo-profiles-output=<profraw-dir>
```

`--pgo-output` 必须与普通 `-o` 输出目录不同，并且必须同时指定 `--pgo-profiles-output`。这样普通产物不会被插桩版本覆盖，运行环境也可以明确选择是否使用 instrumentation ELF。

实现位置：

- 参数校验：[llvmir-converter.cpp:1198](llvmir-converter.cpp:1198)
- PGO 输出流程：[llvmir-converter.cpp:1050](llvmir-converter.cpp:1050)

### 2.2 面向预编译 IR 的插桩参数

由于输入已经是 LLVM IR/bitcode，而不是 C/C++ 源文件，项目使用 LLVM IR 层 instrumentation：

```bash
-fprofile-generate
-Xclang -fprofile-instrument-path=<profraw-dir>/<target>_%p.profraw
```

其中：

- `-fprofile-generate` 启用 LLVM IR instrumentation。
- `-Xclang -fprofile-instrument-path=...` 将 profile 输出路径传递给 Clang/LLVM instrumentation 逻辑。
- `<target>` 来自原始 `_cmd` 中 `-o` 或 `--output` 指定路径的文件名。
- `%p` 在运行时展开为进程号。

例如输出目标为 `output/myapp` 时，生成的 profile 路径模式为：

```text
<profraw-dir>/myapp_%p.profraw
```

使用 `%p` 可以避免多个进程同时运行时写入同一个 profile 文件。

### 2.3 为什么不使用 `-fprofile-instr-generate`

`-fprofile-instr-generate` 更适合由前端处理源代码的场景。本项目接收的是已经生成好的 LLVM IR；如果继续使用前端 instrumentation 选项，Clang 可能不会按照预期为 IR 中的函数加入有效计数器，最终产生空 profile 或缺少函数计数的 profile。

因此，项目最近的修复将 PGO 生成参数从：

```bash
-fprofile-instr-generate=<path>
```

改为：

```bash
-fprofile-generate -Xclang -fprofile-instrument-path=<path>
```

这属于本项目 PGO 处理的关键实现细节。

实现位置：[llvmir-converter.cpp:1108](llvmir-converter.cpp:1108)

## 3. PGO IR 处理细节

### 3.1 移除 `optnone`

LLVM 的 IR instrumentation pass 会跳过带有 `optnone` 属性的函数。为了让 PGO 版本能够为这些函数采集计数器，转换器只在 PGO 临时副本中移除 `OptimizeNone` 属性：

```cpp
if (ProfileFilename && F.hasOptNone())
  F.removeFnAttr(Attribute::OptimizeNone);
```

普通输出不会修改输入函数的 `optnone` 属性，因此普通构建与 PGO 构建的语义保持隔离。

实现位置：[llvmir-converter.cpp:653](llvmir-converter.cpp:653)

### 3.2 显式声明输入类型为 LLVM IR

转换器会把原始 IR 替换为临时 bitcode 文件。临时文件通常没有 `.bc` 或 `.ll` 扩展名，因此在链接命令中，工具会在该输入前增加：

```bash
-x ir
```

这样可以确保 Clang 将输入解释为 LLVM IR，而不是根据无扩展名路径选择错误的前端处理流程。这对 PGO 尤其重要，因为错误的输入类型可能导致 instrumentation 阶段走错路径。

实现位置：[llvmir-converter.cpp:821](llvmir-converter.cpp:821)

### 3.3 目标特性与 PGO 同时处理

在生成 PGO 版本时，输入 IR 仍会经过目标特性处理：

1. 读取模板 LL 文件中的 `target-features`。
2. 与 LLVM codegen 默认特性合并。
3. 与输入模块每个函数已有的 `target-features` 合并。
4. 将合并后的特性写回函数属性。
5. 对 RISC-V 额外同步 `riscv-isa` module flag。

因此 PGO 插桩 ELF 同时具备目标 CPU 特性和 profile instrumentation，不需要牺牲目标特性优化。

## 4. Profile 文件组织与合并

### 4.1 文件命名约定

运行插桩 ELF 后，profile 文件以如下形式保存：

```text
<target>_<process-id>.profraw
```

PGO 重建脚本通过正则表达式从文件名中提取 `<target>`，并递归扫描 profile 目录，将同一个目标的多个 `.profraw` 聚合到一起。

例如：

```text
app_100.profraw
nested/app_200.profraw
```

会被归入 `app` 目标组。

实现位置：[llvmir_pgo_rebuild.py:60](llvmir_pgo_rebuild.py:60)

### 4.2 合并为 `.profdata`

对每个目标，脚本调用与 Clang 主版本匹配的 `llvm-profdata-XX`：

```bash
llvm-profdata-XX merge \
  -output <temporary>/<target>.profdata \
  <target>_*.profraw
```

合并产生的 `.profdata` 放在临时目录中，整个运行结束后自动清理，避免在输出目录中积累中间文件。

## 5. PGO 使用阶段重建

### 5.1 基于原始 `_cmd` 重写命令

`llvmir_pgo_rebuild.py` 不重新构造完整的链接命令，而是读取原始 `_cmd` 并做最小修改：

- 替换 `-o`、`--output` 或 `--output=` 的输出路径。
- 删除原有 instrumentation 参数。
- 保留原始输入、链接器、库、LTO 以及 version script 参数。
- 追加 `-fprofile-use=<profdata>`。

最终命令类似：

```bash
clang-22 ... <input-ir> \
  -o <pgo-output>/<target> \
  -fprofile-use=<target>.profdata
```

实现位置：[llvmir_pgo_rebuild.py:160](llvmir_pgo_rebuild.py:160)

### 5.2 清理旧 instrumentation 参数

重建脚本会删除以下参数：

```text
-fprofile-generate
-fprofile-instr-generate
-fprofile-instr-generate=...
-fprofile-instrument-path=...
```

对于以下参数对也会整体删除：

```text
-Xclang
-fprofile-instrument-path=...
```

最后只追加：

```text
-fprofile-use=<profdata>
```

这样可以避免重建过程同时处于 profile generate 和 profile use 模式，也避免最终 ELF 继续携带 instrumentation 逻辑。

### 5.3 临时 `_cmd` 与相对路径保持

PGO 重建会创建临时 `_cmd`，而不是直接修改原始文件。临时工作区会：

- 保留原 `_cmd` 的父目录名，以复用 `llvmir`/`llvmir-bin` 的输出布局规则。
- 为 `./` 和 `../` 输入建立符号链接。
- 为相对路径的 version script 保留可访问路径。
- 使用 Bash 数组格式写入临时命令，减少转义和空格路径问题。

临时目录在每个目标处理结束后清理，原始 `_cmd` 不会被修改。

实现位置：[llvmir_pgo_rebuild.py:238](llvmir_pgo_rebuild.py:238)

## 6. LLVM/Clang 版本匹配

PGO profile 与 LLVM instrumentation 格式具有版本相关性，因此项目会从 `_cmd` 的 Clang 路径或注释中提取主版本号：

```text
clang-22 -> 22
clang version 22.1.8 -> 22
```

如果命令路径和版本注释同时存在但不一致，脚本会直接报错，避免使用错误版本的 `llvm-profdata` 或 converter。

查找顺序为：

1. 指定的 `--converter-dir`。
2. 脚本所在目录。
3. 当前工作目录。
4. `PATH`。

版本化名称优先：

```text
llvmir-converter-22
llvmir-convert-22
llvmir-converter
llvmir-convert
```

## 7. 批量运行与资源控制

`llvmir_batch_runner.py` 可以持续扫描多个目录中的 `*_cmd`，并自动把 PGO 参数传递给底层 converter。

在资源压力较高时，runner 会暂停当前转换进程组：

- CPU 达到 `--cpu-limit` 或内存达到 `--memory-limit`：发送 `SIGSTOP`。
- CPU 低于 `--cpu-resume` 且内存低于 `--memory-resume`：发送 `SIGCONT`。
- 使用 `start_new_session=True` 创建独立进程组，确保 Clang 及其子进程一起暂停和恢复。

CPU 统计使用 `/proc/stat` 两次采样的时间差，内存统计使用 `/proc/meminfo` 中的 `MemAvailable`。暂停阈值和恢复阈值分离形成滞回，减少临界点附近的反复切换。

## 8. 失败恢复机制

批量 runner 将失败任务记录到：

```text
<output-dir>/.llvmir-batch-failures.json
```

每条记录包含：

- `_cmd` 绝对路径。
- 文件大小。
- 纳秒级修改时间。
- 失败时间。
- 失败原因或退出码。

如果同一个 `_cmd` 文件内容未变化，后续扫描和重启会跳过它；如果 mtime 或大小变化，则自动重试。失败记录使用临时文件和 `os.replace` 原子更新。

## 9. 可能具有特色的地方

项目的特色不在于重新实现 LLVM PGO 算法，而在于针对“只有预编译 LLVM IR、没有源码构建环境”的场景，把以下能力串联起来：

- 从 `_cmd` 恢复原始链接语义。
- 在 IR 层重新设置目标 CPU 特性。
- 使用正确的 LLVM IR instrumentation 生成 PGO ELF。
- 通过 `%p` 管理多进程 profile 文件。
- 自动合并 profile 并按目标重建 ELF。
- 以临时命令和符号链接保留原始相对路径语义。
- 通过批量 runner 进行资源感知的长期运行。

因此，该工具可以被视为一个面向 LLVM IR 分发/重建场景的 PGO 基础设施，而不只是一个简单的 `clang` 命令包装器。

## 10. 已验证内容

当前测试覆盖了：

- `.profraw` 递归扫描和按目标分组。
- `llvmir`/`llvmir-bin` 输出目录映射。
- 输出参数替换。
- 旧 instrumentation 参数删除。
- `-fprofile-use` 参数生成。
- Clang 版本提取和版本冲突检查。
- 版本化 converter 查找。
- 临时 `_cmd` 生成。

已运行 Python 测试：13 个测试全部通过。