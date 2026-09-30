# uballoc

**中文** | **[English](README.en.md)**

面向 ARMv8 多进程、多节点部署的 C++17 共享内存分配器。

## 特性

- **三档大小分类**（每倍增 4 类的粒度）：
  - Small：8 B – 16 KB，32 KB slab（37 类，4-per-doubling）
  - Large：16 KB – 2 MB，4 MB slab（28 类，4-per-doubling）
  - Huge：> 2 MB，2 MB 槽位（按 slot 数量分配）
- **段递进式分配**：Small/Large 段按 4MB→16MB→64MB→128MB 阶梯增长，首次分配仅占 4MB（非满段 128MB）
- **分布式共享内存**：固定虚址 mmap，跨进程分配/释放，跨节点发布/查找
- **节点内多进程**：按节点引用计数，`claim_rank` 死槽位检测，`node_index` CAS 分配
- **空闲内存回收**：空闲段经历 LIVE→DETACHED→RETURNED 状态流转，归还至 UBSE 池供跨节点复用
- **内省与碎片整理**：`return_stats` 提供 per-slab 占用率、Huge slot 统计、空段检测；`defrag_hints` 返回低占用 slab 中的活跃分配列表，供应用协作式碎片整理
- **延迟挂接**：基于 userfaultfd 的远端数据按需挂接
- **CRTP 后端**：分配路径中无虚函数调用
- **ARMv8.1 LSE 原子指令**：编译期开关
- **崩溃恢复**：可检测 CAS、状态日志、缓存刷新
- **多线程**：每线程分配器，跨线程释放

## 构建

### 环境要求

- aarch64（最低 ARMv8.0；ARMv8.1 LSE 可选，默认开启）
- GCC 12+（或 Clang 14+）
- CMake 3.16+
- C++17

### 配置

```bash
# POSIX 共享内存（标准 Linux /dev/shm，无 UBSE 依赖）
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build

# UBSE 后端（需要 ubse-client SDK；默认）
cmake -B build && cmake --build build
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `UBALLOC_USE_UBSE` | ON | 使用 `UBShmProvider` 作为默认后端（需要 ubse-client） |
| `UBALLOC_LSE` | ON | ARMv8.1 LSE 原子指令 |
| `UBALLOC_LOG` | ON | 分布式路径日志 |
| `UBALLOC_BUILD_TESTS` | ON | 构建测试 |
| `UBALLOC_BUILD_EXAMPLES` | ON | 构建示例 |
| `UBALLOC_RECOVER_CAS` | ON | 可检测 CAS（用于恢复） |
| `UBALLOC_RECOVER_LOG` | ON | 状态日志（用于恢复） |
| `UBALLOC_RECOVER_FLUSH` | OFF | 缓存刷新（用于恢复） |

## 快速上手

```bash
# 每个节点上设置相同的 UBALLOC_HEAP_ID —— 用于选取引导 PID 表
export UBALLOC_HEAP_ID=myheap
```

```cpp
#include "uballoc.hpp"

// init() 从环境变量读取 UBALLOC_HEAP_ID，运行时通过引导 PID 表
// 发现自身 rank 与集群成员关系。
uballoc::init();

void* p = uballoc::malloc(64);
// ... 使用 p ...
uballoc::free(p);
```

在两个节点上运行分布式示例：

```bash
UBALLOC_HEAP_ID=ubex ./build/ub_example 0 &   # 节点 0
UBALLOC_HEAP_ID=ubex ./build/ub_example 1     # 节点 1
```

## 跨节点发布/查找

```cpp
// 节点 0：发布一个块
void* p = alloc.malloc(64);
alloc.publish(p, 64, /*type_id=*/1);

// 节点 1：查找并使用
auto info = alloc.lookup_by_type(1);
// info.address 在节点 1 上同样有效（相同固定虚址）
```

## 共享内存后端

| 后端 | 头文件 | 适用场景 |
|------|--------|----------|
| `UBShmProvider` | `ubshm_provider.hpp` | 配备 ubse 的定制机型（默认） |
| `PosixShmProvider` | `shm_provider.hpp` | 标准 Linux `/dev/shm` |

## 崩溃清理

正常进程退出时，共享内存会自动清理（detach 并通过 `shm_unlink` 删除）。
清理脚本仅用于崩溃进程（未执行析构函数）的残留资源：

```bash
# UBShmProvider（ubse）—— 通过 ubsectl 发现已有 shm 及挂接节点
./scripts/cleanup_shms_ub.sh                            # 默认 UBALLOC_HEAP_ID=heap
UBALLOC_HEAP_ID=myheap ./scripts/cleanup_shms_ub.sh   # 自定义 heap_id

# PosixShmProvider
./scripts/cleanup_shms.sh 2 "" node1
```

## 测试

```bash
cd build && ctest --output-on-failure
```

## 文档

用户手册位于 [`doc/user_manual.zh.md`](doc/user_manual.zh.md)（[`英文版`](doc/user_manual.en.md)）。

## 目录结构

```
include/uballoc/    头文件
src/                源文件
tests/              测试
example/            示例
doc/                用户手册
scripts/            清理脚本
```

## 许可证

Apache-2.0。完整文本见 [`LICENSE`](LICENSE)。
