# uballoc用户手册

**中文** | **[English](user_manual.en.md)**

本手册帮助用户全面了解uballoc分布式共享内存分配器的项目背景、安装部署、快速入门及API使用方法。

---

# 项目介绍

## 项目简介

uballoc是专为aarch64架构、Linux操作系统下多进程多节点分布式部署场景设计的C++17共享内存分配器。uballoc使用固定虚拟地址（VA）映射、透明大页、userfaultfd（uffd）与SIGSEGV fault handler等技术，实现了跨进程共享内存的统一分配/释放、基于type_id的发布与发现、动态成员发现、崩溃恢复等能力，可最大限度地降低多节点间数据搬运开销，提升分布式应用性能与可扩展性。

适用场景：分布式共享内存的申请、释放、发布、发现、跨节点访问等。

## 特性介绍

**表1** uballoc特性介绍

| 特性名称 | 特性介绍 |
|---------|---------|
| 三档位尺寸分级堆 | 将对象按大小划分为Small（8B–16KB，37类，4-per-doubling间距，32KB slab）、Large（20KB–4MB，32类，4-per-doubling间距，4MB slab）、Huge（≥4MB，4MB slot）三档位，每次分配匹配最接近的档位，降低内部碎片（≤25%）。 |
| 固定VA跨进程共享 | 预留约28TB虚拟地址空间，所有节点采用相同VA基址（`DEFAULT_VA_BASE=0x200000000000`），使同一指针在所有进程语义等价，无需地址翻译。 |
| 基于type_id的发布/发现 | 调用`malloc(size, type_id)`一次完成分配与发布；任一进程可通过`lookup_by_type(type_id)`阻塞或非阻塞地发现其他进程发布的区域，`owner_process`通过CAS原子登记，保证一种type_id只有一个所有者。 |
| 动态成员发现 | 设置环境变量`UBALLOC_HEAP_ID`后，进程通过自举（bootstrap）共享内存块自动发现rank与总成员数，无需提前知晓`rank`或`total_processes`。 |
| uffd默认开启 | 构造器优先级102自动装配uffd handler线程，按需加载远端数据段；当uffd不可用时，回退至SIGSEGV fault handler。`init()`末尾亦会调用`try_enable_uffd_locked()`。 |
| CRTP零开销后端 | 后端通过CRTP静态分派，分配热路径无虚函数调用；可选ARMv8.1 LSE原子指令，编译期开关。 |
| 崩溃恢复 | 可检测CAS、状态日志、缓存刷写三档可选机制，支持崩溃后扫描重建分配器状态。 |

## 安装部署

uballoc分配器的安装指南，包括已验证环境、构建选项、编译产物及使用方法等，具体请参见[安装指南](#安装指南)。

## 快速入门

uballoc分配器的快速入门，包括环境变量配置、双节点示例运行及性能验证等，具体请参见[快速入门](#快速入门)。

## 文档

| 资源名称 | 资源简介 |
|---------|---------|
| [快速入门](#快速入门) | 介绍uballoc的基本概念、安装部署、环境变量、双节点示例及性能验证，帮助用户快速上手。 |
| [安装指南](#安装指南) | 详细介绍uballoc的已验证环境、CMake选项、编译产物、分发包生成（tarball）、下游消费方式及卸载方式。 |
| [API参考](#api参考) | API分为**基础API**（`init`/`malloc`/`malloc_published`/`free`/`realloc`/`lookup_by_type*`/`shm_new`/`shm_delete`，绝大多数应用只需此组）与**可选/高级API**（`memalign`/`publish`/`unpublish`/`lookup_by_address`/`rank`/`total_processes`/`is_owner`/`is_initialized`/`reset`/`enable_*fault_handler`），提供详细说明与使用示例。 |
| [常见问题](#常见问题) | 汇总用户高频关注的行为语义与使用约束，涵盖`realloc`缩容语义、发布/查找的进程生命周期要求、跨进程释放限制等。 |

## 贡献声明

欢迎大家为社区做贡献，如果使用过程中有任何问题或建议，或者需要反馈特性需求和bug报告，可通过issue或pull request提交至代码仓。

## 免责声明

uballoc为底层共享内存原语库，计算流程涉及跨进程内存读写、虚拟地址映射等操作。uballoc不提供和也不发布操作系统，操作系统须用户自行安装，uballoc不承担操作系统的安全责任，用户需要结合自身应用对操作系统安全加固，包括不安装或者剔除不必要的应用等。

## License

本项目采用 [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0) 许可证。

```
Copyright 2021-2026 uballoc contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

# 安装指南

## 已验证环境

为保证您可以顺利安全地使用uballoc，请确保所使用的环境信息在已验证环境范围内。

| 操作系统 | CPU类型 | 编译器 |
|---------|--------|--------|
| openEuler 22.03 LTS SP3 | 鲲鹏920系列处理器（aarch64） | GCC 10.3.1 / 毕昇4.2.0 |
| openEuler 24.03 LTS SP4 | UBSE机器（8节点，每节点1.5TB物理内存） | GCC 12.3.1 |

- uballoc目前仅支持aarch64架构；在其他架构上编译可能失败或运行时异常退出。
- UBShmProvider后端仅在UBSE机器上可用；其他环境请使用`UBALLOC_USE_UBSE=OFF`以采用PosixShmProvider（基于`/dev/shm`）。
- 编译uballoc需要CMake 3.16+与C++17标准库。

## 获取软件包

uballoc当前以源码形式发布，请从代码仓克隆：

```shell
git clone <uballoc-repo-url> uballoc
cd uballoc
```

## 构建选项

**表2** CMake构建选项说明

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `UBALLOC_USE_UBSE` | ON | 使用UBShmProvider作为默认后端（要求安装ubse-client）；OFF时使用PosixShmProvider（标准Linux `/dev/shm`） |
| `UBALLOC_LSE` | ON | 启用ARMv8.1 LSE原子指令 |
| `UBALLOC_LOG` | ON | 启用分布式路径日志（编译期开关，OFF时所有非致命日志编译为no-op） |
| `UBALLOC_BUILD_TESTS` | ON | 构建单元测试 |
| `UBALLOC_BUILD_EXAMPLES` | ON | 构建示例程序 |
| `UBALLOC_RECOVER_CAS` | ON | 启用可检测CAS用于崩溃恢复 |
| `UBALLOC_RECOVER_LOG` | ON | 启用状态日志用于崩溃恢复 |
| `UBALLOC_RECOVER_FLUSH` | OFF | 启用缓存刷写用于崩溃恢复 |
| `UBALLOC_NO_EXTERN_TEMPLATE` | OFF | 用户自定义宏（非CMake选项）；定义后取消`extern template class`声明，恢复header-only模板实例化，用于自定义ShmProvider后端 |

### 构建命令

**默认构建（UBShmProvider，仅UBSE机器可用）**

```shell
cmake -B build && cmake --build build -j
```

**POSIX构建（标准Linux，无ubse依赖）**

```shell
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j
```

## 编译产物

构建成功后，`build/`目录下生成如下产物：

| 产物 | 说明 |
|------|------|
| `libuballoc.so` | uballoc动态库，链接UBShmProvider或PosixShmProvider（取决于`UBALLOC_USE_UBSE`） |
| `ub_example` | 分布式双节点示例（publish/lookup、remote free） |
| `published_example` | 演示`malloc(size, type_id)` + 阻塞lookup |
| `uffd_example` | 演示uffd默认开启与eager attach共存 |
| `fault_handler_example` | 演示SIGSEGV fault handler作为uffd回退 |
| `dynamic_discovery_example` | 演示`UBALLOC_HEAP_ID`驱动的动态成员发现 |
| `distributed_example` | 演示多档位分配、`lookup_by_address`与cross-process remote free |
| `stl_full_shm_example` | 演示`shm_new<T>(pub_tid{})`合并发布STL容器（header+data均在shm） |
| `stl_data_only_example` | 演示STL容器data buffer在shm、header在本地的模式 |
| `uballoc_test` | 单元测试（`ctest`入口） |

## 使用方法

uballoc作为动态库供应用链接使用，可在编译期通过`-rpath`指定链接路径，或运行期通过`LD_LIBRARY_PATH`指定。

### 编译期链接

```shell
g++ -std=c++17 -o my_app my_app.cpp \
    -I<path to uballoc>/include \
    -L<path to uballoc>/build -luballoc \
    -Wl,-rpath=<path to uballoc>/build
```

### 运行期指定路径

```shell
export LD_LIBRARY_PATH=/<path to uballoc>/build:$LD_LIBRARY_PATH
./my_app
```

## 验证uballoc

设置`LD_LIBRARY_PATH`或`rpath`后，可使用`ldd`命令检查是否成功链接uballoc库。正常链接结果如下。

```text
[root@localhost build]# ldd ./ub_example
        linux-vdso.so.1 (0x0000ffff9ee6a000)
        libuballoc.so => /path/to/uballoc/build/libuballoc.so (0x0000ffff9b400000)
        libstdc++.so.6 => /usr/lib64/libstdc++.so.6 (0x0000ffff9b200000)
        libm.so.6 => /usr/lib64/libm.so.6 (0x0000ffff9b150000)
        libgcc_s.so.1 => /usr/lib64/libgcc_s.so.1 (0x0000ffff9b110000)
        libc.so.6 => /usr/lib64/libc.so.6 (0x0000ffff9af60000)
        /lib/ld-linux-aarch64.so.1 (0x0000ffff9ee2d000)
```

## 分发包生成

构建完成后，可通过CPack生成可分发的tarball，供最终用户安装使用。

**生成分发包**

```shell
# POSIX构建（标准Linux）
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j
(cd build && cpack -G TGZ)

# UBSE构建（仅UBSE机器）
cmake -B build && cmake --build build -j
(cd build && cpack -G TGZ)
```

生成产物：`build/uballoc-<version>-linux-<arch>.tar.gz`（如`uballoc-0.6.0-linux-aarch64.tar.gz`，约135KB）。

**分发包内容**

| 路径 | 说明 |
|------|------|
| `lib/libuballoc.so.<version>` | 动态库实际文件（已strip调试符号） |
| `lib/libuballoc.so.<SOVERSION>` | SONAME符号链接（如`libuballoc.so.0`） |
| `lib/libuballoc.so` | 开发符号链接（供`-luballoc`使用） |
| `lib/pkgconfig/uballoc.pc` | pkg-config文件，自动包含正确的Cflags/Libs |
| `lib/cmake/uballoc/` | CMake包配置文件（4个），供`find_package(uballoc)`使用 |
| `include/uballoc.h`、`include/uballoc.hpp`、`include/uballoc/*.hpp` | 全部头文件 |
| `share/uballoc/examples/` | 示例源码（9个`.cpp`）+ 独立`CMakeLists.txt`，供最终用户编译验证 |
| `share/uballoc/scripts/cleanup_shms.sh` | POSIX环境残留shm清理脚本（基于`/dev/shm`文件路径） |
| `share/uballoc/scripts/cleanup_shms_ub.sh` | UBSE环境残留shm清理脚本（基于`ubsectl`自动发现） |

**最终用户安装方式**

```shell
# 解压到任意prefix（推荐 /usr/local 或 /opt/uballoc）
tar xzf uballoc-0.6.0-linux-aarch64.tar.gz --strip-components=1 -C /opt/uballoc
```

**下游应用消费方式（二选一）**

方式一：pkg-config（无需CMake）

```shell
g++ app.cpp $(PKG_CONFIG_PATH=/opt/uballoc/lib/pkgconfig pkg-config --cflags --libs uballoc) -o app
```

方式二：CMake `find_package`

```cmake
# 下游CMakeLists.txt
find_package(uballoc CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE uballoc::uballoc)
```

```shell
cmake -B build -DCMAKE_PREFIX_PATH=/opt/uballoc && cmake --build build
```

> **注**：`uballoc::uballoc`导入目标自动传递C++17标准要求（`cxx_std_17`），下游项目无需显式设置`CMAKE_CXX_STANDARD`。对于POSIX构建，pkg-config/CMake配置不会定义`UBALLOC_USE_UBSE`宏，使`#ifdef UBALLOC_USE_UBSE`为false，正确排除UBSE相关代码。

**编译分发包中的示例**

分发包附带9个示例源码（`share/uballoc/examples/`），可通过以下两种方式编译：

方式一：CMake（推荐）

```shell
cd /opt/uballoc/share/uballoc/examples
cmake -B build -DCMAKE_PREFIX_PATH=/opt/uballoc && cmake --build build -j
```

方式二：pkg-config（无需CMake）

```shell
cd /opt/uballoc/share/uballoc/examples
g++ -std=c++17 distributed_example.cpp \
    $(PKG_CONFIG_PATH=/opt/uballoc/lib/pkgconfig pkg-config --cflags --libs uballoc) \
    -Wl,-rpath,/opt/uballoc/lib -o distributed_example
```

> **注**：示例源码统一使用`#include <uballoc.hpp>`（尖括号），在源码树内构建（`target_link_libraries`传递include路径）与安装后构建（`-I<prefix>/include`）两种场景下均可正确找到头文件。

## 显式模板实例化

V0.6起，库在`src/uballoc_instances.cpp`中显式实例化了后端模板组合，使下游消费方只需include头文件并链接`-luballoc`即可，无需在每个编译单元重新编译模板代码。

**支持的实例化组合**：

| `UBALLOC_USE_UBSE` | 实例化的模板 |
|---------------------|-------------|
| `OFF`（POSIX） | `DistributedShmBackend<PosixShmProvider>` + `GlobalAllocator<DistributedShmBackend<PosixShmProvider>>` |
| `ON`（UBSE） | `DistributedShmBackend<UBShmProvider>` + `GlobalAllocator<DistributedShmBackend<UBShmProvider>>` |

头文件中通过`extern template class`声明抑制下游编译期实例化。实测下游每个编译单元编译时间减少约36%（2.24s→1.44s），目标文件体积减少约85%（1.33MB→199KB）。

**自定义后端**：如果用户使用自定义`ShmProvider`或自定义分配器后端（非上述两种标准组合），需在编译命令中定义`UBALLOC_NO_EXTERN_TEMPLATE`宏，恢复header-only行为：

```shell
g++ -std=c++17 -DUBALLOC_NO_EXTERN_TEMPLATE -I/opt/uballoc/include \
    my_app.cpp -L/opt/uballoc/lib -luballoc -o my_app
```

此宏取消所有`extern template class`声明，使模板在用户代码中按需实例化。

## 卸载方法

直接删除`build/`目录即可卸载uballoc；共享内存对象需通过清理脚本释放：

```shell
# UBShmProvider（ubse）—— 通过 ubsectl 自动发现现存 shm 及其 attacher 节点
./scripts/cleanup_shms_ub.sh                            # 默认 UBALLOC_HEAP_ID=heap
UBALLOC_HEAP_ID=myheap ./scripts/cleanup_shms_ub.sh     # 指定 heap_id

# PosixShmProvider（/dev/shm）
./scripts/cleanup_shms.sh <total_processes> "" <node>
```

---

# 快速入门

本文档帮助用户快速了解uballoc的使用方法及性能验证。

## 安装部署

获取源码并构建的详细步骤请参见[安装指南](#安装指南)，编译产物如下表。

| 库文件 / 可执行文件 | 说明 |
|-------------------|------|
| `libuballoc.so` | uballoc动态库（UBShmProvider或PosixShmProvider） |
| `ub_example` | 双节点publish/lookup与remote free示例 |
| `uffd_example` | uffd默认开启与eager attach共存示例 |
| `dynamic_discovery_example` | `UBALLOC_HEAP_ID`动态成员发现示例 |
| `stl_full_shm_example` | `shm_new<T>(pub_tid{})`合并发布STL容器示例 |
| `uballoc_test` | 单元测试 |

## 环境变量

如[**表3** uballoc环境变量说明](#uballoc环境变量说明)所示，介绍了uballoc支持的环境变量，用户可根据实际需求进行配置。

**表3** uballoc环境变量说明

| 变量名称 | 说明 | 默认值 | 可选值 |
|---------|------|--------|--------|
| `UBALLOC_HEAP_ID` | 自举共享内存的heap标识字符串；未设置时，`init`打印警告并返回（不初始化） | 空（未设置） | 任意非空字符串，如`myheap` |
| `UBALLOC_LOG_LEVEL` | 运行期日志等级（不区分大小写）；默认`error`以避免constructor(102)期间触发未初始化的iostream | `error` | `error`/`warn`/`info`/`debug`/`trace` |
| `UBALLOC_LOG` | 编译期日志总开关；OFF时所有非致命日志编译为no-op | `ON`（CMake） | `ON`/`OFF` |
| `UBALLOC_USE_UBSE` | 编译期后端选择；ON采用UBShmProvider，OFF采用PosixShmProvider | `ON`（CMake） | `ON`/`OFF` |
| `UBALLOC_VALIDATE` | 编译期校验断言开关（始终开启，不可关闭） | `ON`（强制） | — |
| `UBALLOC_RECOVER_CAS` | 编译期可检测CAS开关（崩溃恢复） | `ON`（CMake） | `ON`/`OFF` |
| `UBALLOC_RECOVER_LOG` | 编译期状态日志开关（崩溃恢复） | `ON`（CMake） | `ON`/`OFF` |
| `UBALLOC_RECOVER_FLUSH` | 编译期缓存刷写开关（崩溃恢复） | `OFF`（CMake） | `ON`/`OFF` |
| `UBALLOC_LSE` | 编译期ARMv8.1 LSE原子指令开关 | `ON`（CMake） | `ON`/`OFF` |

### 环境变量配置示例

以配置`UBALLOC_HEAP_ID`与`UBALLOC_LOG_LEVEL`为例。

1. 打开`/etc/profile`文件。

    ```shell
    vi /etc/profile
    ```

2. 按`i`进入编辑模式，在`/etc/profile`文件中增加以下内容。

    ```shell
    export UBALLOC_HEAP_ID=myheap
    export UBALLOC_LOG_LEVEL=info
    ```

3. 执行`source`命令使环境变量生效。

    ```shell
    source /etc/profile
    ```

## 加速效果验证

本文以`ub_example`双节点publish/lookup测试为例，说明如何验证uballoc的跨节点共享能力。

### 测试环境

- 处理器：鲲鹏920（aarch64）
- 节点数：2
- 操作系统：openEuler 22.03 LTS SP3
- 编译选项：`UBALLOC_USE_UBSE=OFF`（POSIX本地验证）或`UBALLOC_USE_UBSE=ON`（UBSE机器）
- uballoc版本：0.1.0

### 测试步骤

在两个节点（或两个终端）上分别运行P0与P1：

**节点0：分配并发布**

```shell
UBALLOC_HEAP_ID=ubex ./build/ub_example 0
```

**节点1：发现并验证**

```shell
UBALLOC_HEAP_ID=ubex ./build/ub_example 1
```

> **注**：自V0.6起，`ub_example`仅需1个参数（`app_pid`），不再需要`total_processes`。uballoc通过`UBALLOC_HEAP_ID`自举自动发现成员数。

### 结果对比

**节点0输出**

```text
=== Published Allocator Example ===
Process 0 of 2
  malloc(96, type_id=100)...
  Allocated+published at 0x200000100000
  lookup_by_type_blocking(type_id=101, 10s)...
  Found peer's block: address=0x280000200000 size=96 owner=1
  Peer config: pid=1 values=[1000, 1001, ... 1007]
  Peer data verified OK
  Non-blocking lookup_by_type also found it: OK
  Process 0 complete.
```

**节点1输出**

```text
=== Published Allocator Example ===
Process 1 of 2
  malloc(96, type_id=101)...
  Allocated+published at 0x280000200000
  lookup_by_type_blocking(type_id=100, 10s)...
  Found peer's block: address=0x200000100000 size=96 owner=0
  Peer config: pid=0 values=[0, 1, ... 7]
  Peer data verified OK
  Non-blocking lookup_by_type also found it: OK
  Process 1 complete.
```

### 性能分析

| 指标 | uballoc（固定VA跨进程共享） | 传统socket搬运 | 提升 |
|------|--------------------------|---------------|------|
| 跨节点数据访问延迟 | 单次指针解引用（远端段已加载） | 单次socket send/recv往返 | 数量级降低 |
| 跨节点数据搬运字节 | 0（指针语义等价） | 等同数据量 | 100% |
| 多节点成员发现时延 | 一次自举shm attach | 需外部协调服务 | 显著降低 |

从测试结果可以看出，uballoc通过固定VA与type_id发布/发现机制，使跨节点数据访问无需字节搬运，性能与可扩展性均显著提升。

---

# API参考

## 函数说明

uballoc是专为aarch64架构、Linux操作系统下多进程多节点分布式部署场景设计的C++17共享内存分配器。uballoc使用固定虚拟地址映射、透明大页、userfaultfd与SIGSEGV fault handler等技术，可最大限度地降低多节点间数据搬运开销。

uballoc的API分为两类：

- **基础API**：`init`、`malloc`、`malloc_published`、`free`、`realloc`、`lookup_by_type`、`lookup_by_type_blocking`、`shm_new`/`shm_delete`（含`pub_tid`合并发布重载）。绝大多数应用只需使用这一组API即可完成跨进程共享内存的分配、重分配、发布、发现与释放。
- **可选/高级API**：`memalign`、`publish`、`unpublish`、`lookup_by_address`、`rank`、`total_processes`、`is_owner`、`is_initialized`、`reset`、`enable_fault_handler`/`disable_fault_handler`、`enable_userfaultfd`/`disable_userfaultfd`。这些API不是必须的——通常用于对齐分配、手动发布/取消发布、查询集群状态、fault handler回退或完全重置分配器等高级场景。

线程ID在首次使用时自动分配，无需显式调用`init_thread`。

下文示例默认包含以下头文件：

```cpp
#include <uballoc.hpp>   // C++ API
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
```

## 基础API

本节API是绝大多数应用所需的全部接口：初始化分配器、分配/重分配/释放内存、发布/查找区域、以及在共享内存中构造/析构C++对象。

### uballoc_init

**函数功能**

初始化本进程的分配器。从`UBALLOC_HEAP_ID`环境变量读取heap标识，通过自举共享内存PID表动态发现进程rank与总成员数。无需提前知晓`rank`或`total_processes`。

**契约**：应用程序在使用任何分配器功能（包括直接访问远端共享内存）之前**必须**调用`uballoc_init()`。`uballoc_malloc`/`uballoc_free`/`uballoc_lookup_by_type*`系列函数在首次使用时会自动调用`uballoc_init()`作为安全网，但对于直接访问远端shm的代码，显式调用`uballoc_init()`是推荐模式。

**函数定义**

```cpp
// C++ API
void uballoc::init();

// C API
void uballoc_init(void);
```

**示例**

```shell
export UBALLOC_HEAP_ID=myheap
./worker   # 内部首次调用malloc时自动触发init
```

```cpp
// 在程序入口显式调用一次亦可：
uballoc::init();
```

### uballoc_malloc

**函数功能**

分配`size`字节内存，返回指向分配内存的指针。

**函数定义**

```cpp
void* uballoc::malloc(size_t size);
void* uballoc_malloc(size_t size);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `size` | 申请内存的字节数 | 非负数 | 输入 |

**返回值**

- 成功：返回指向分配内存的指针。
- 失败：返回`nullptr`。

**示例**

```cpp
void MallocExample() {
    int8_t* res = static_cast<int8_t*>(uballoc::malloc(64));
    if (res == nullptr) {
        std::cerr << "res is null" << std::endl;
    } else {
        std::cout << "malloc pointer address: " << res << std::endl;
        uballoc::free(res);
    }
}
```

### uballoc_malloc_published

**函数功能**

分配`size`字节内存并通过CAS将指针发布到`type_id`槽位，一次完成分配与发布。

**函数定义**

```cpp
void* uballoc::malloc(size_t size, uint32_t type_id);
void* uballoc_malloc_published(size_t size, uint32_t type_id);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `size` | 申请内存的字节数 | 非负数 | 输入 |
| `type_id` | 应用自定义的类型ID | 0..2^32-1 | 输入 |

**返回值**

- 成功：返回已发布的指针。
- 失败：返回`nullptr`（分配或发布失败时，已分配内存会被`free`）。

**示例**

```cpp
constexpr uint32_t TYPE_DATA = 200;

struct alignas(64) DataBlock {
    std::atomic<uint32_t> ready;
    uint32_t rank;
    char message[64];
};

void PublishExample() {
    DataBlock* block = static_cast<DataBlock*>(
        uballoc::malloc(sizeof(DataBlock), TYPE_DATA));
    if (!block) {
        std::cerr << "malloc(size, type_id) failed" << std::endl;
        return;
    }
    block->ready.store(0, std::memory_order_relaxed);
    block->rank = 0;
    std::snprintf(block->message, sizeof(block->message), "Hello from P0");
    block->ready.store(1, std::memory_order_release);
    // ... peer会通过lookup_by_type_blocking(TYPE_DATA, ...)发现此块 ...
    uballoc::free(block);
}
```

> **数据就绪性竞态说明**：指针在调用方初始化内存之前即被发布。调用方应在发布区域内放置原子`ready`标志位，在`malloc(size, type_id)`返回后置位；查找方应在`lookup_by_type_*`返回后自旋等待`ready`。

### uballoc_free

**函数功能**

释放已分配的内存。支持跨线程、跨进程释放（由内部`owner_process`路由至对应后端）。

**函数定义**

```cpp
void uballoc::free(void* ptr);
void uballoc_free(void* ptr);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `ptr` | 指向需释放内存的指针 | 非空 | 输入 |

**示例**

同`uballoc_malloc`函数示例。

### uballoc_realloc

**函数功能**

将`old_pointer`指向的内存重分配为`new_size`字节。当`new_size > old_size`（扩容）时，分配新块、拷贝数据、释放旧块；当`new_size <= old_size`（缩容或等尺寸）时，直接返回原指针，不分配新内存也不释放旧块——此为标准C `realloc`语义（与glibc、jemalloc、mimalloc一致）。

**函数定义**

```cpp
void* uballoc::realloc(void* ptr, size_t size);
void* uballoc_realloc(void* ptr, size_t size);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `ptr` | 指向需重新分配的旧内存指针 | 非空 | 输入 |
| `size` | 重新申请内存的字节数 | 非负数 | 输入 |

**返回值**

- 成功：返回指向分配内存的指针。扩容时为新分配的指针（`!= ptr`），缩容或等尺寸时为原指针（`== ptr`）。
- 失败：返回`nullptr`。

### uballoc_lookup_by_type

**函数功能**

非阻塞查找`type_id`对应的发布信息。

**函数定义**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_type(uint32_t type_id);
int uballoc_lookup_by_type(uint32_t type_id, uballoc_published_info_t* out);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `type_id` | 待查找的类型ID | 0..2^32-1 | 输入 |
| `out`（C API） | 输出参数，写入发布信息结构体 | 非空 | 输出 |

**返回值（C API）**

- 成功：0。
- 未找到：非0。

### uballoc_lookup_by_type_blocking

**函数功能**

阻塞式查找`type_id`对应的发布信息，每100ms轮询一次，直到找到或超时。

**函数定义**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_type_blocking(uint32_t type_id, int timeout_ms);
int uballoc_lookup_by_type_blocking(uint32_t type_id, int timeout_ms, uballoc_published_info_t* out);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `type_id` | 待查找的类型ID | 0..2^32-1 | 输入 |
| `timeout_ms` | 超时时长（毫秒）；负值表示无限等待 | 任意整数 | 输入 |
| `out`（C API） | 输出参数，写入发布信息结构体 | 非空 | 输出 |

**返回值（C API）**

- 成功：0。
- 超时：非0。

**示例**

```cpp
constexpr uint32_t TYPE_PEER = 101;

auto info = uballoc::lookup_by_type_blocking(TYPE_PEER, 10000);
if (info.owner_process < 0) {
    std::cerr << "Timeout: peer did not publish type " << TYPE_PEER << std::endl;
    return;
}
std::cout << "Found peer at " << info.address
          << " size=" << info.size
          << " owner=" << info.owner_process << std::endl;
```

> **进程生命周期要求**：发布方进程在消费方查找期间必须保持存活。发布方退出时，其析构函数会清理自举槽位并释放共享内存，此后消费方将无法发现该发布。若发布方在消费方查找前退出，`lookup_by_type_blocking`将超时返回`owner_process == -1`。调用方应始终检查`owner_process >= 0`并处理超时。推荐的发布/消费模式见[常见问题：发布方何时可以退出？](#发布方何时可以退出)。

**`uballoc_published_info_t`结构体定义**

```c
typedef struct {
    void*    address;        /* 指向发布内存的指针 */
    size_t   size;           /* 发布分配的字节数 */
    uint32_t type_id;        /* 应用自定义的类型ID */
    int      owner_process;  /* 所有者进程ID；未找到时为-1 */
} uballoc_published_info_t;
```

### shm_new / shm_delete

**函数功能**

在共享内存中构造/析构C++对象，等价于`new`/`delete`但基于uballoc分配。`shm_new`提供两个重载：纯分配+构造（无发布），以及分配+构造+发布（通过`pub_tid`参数区分）。

**`pub_tid`包装类型**

`pub_tid`是一个distinct user-defined type，用于在重载决议中无歧义地区分"publish版"与"non-publish版"`shm_new`。它包装一个`uint32_t type_id`，构造函数为`explicit`，避免与构造参数冲突。

```cpp
struct pub_tid {
    uint32_t value;
    explicit constexpr pub_tid(uint32_t v) noexcept : value(v) {}
};
```

**函数定义**

```cpp
// 分配 + 构造（无发布）
template <typename T, typename... Args>
T* shm_new(Args&&... args);

// 分配 + 构造 + 发布（合并API，取代两步式shm_new + publish）
template <typename T, typename... Args>
T* shm_new(pub_tid type_id, Args&&... args);

// 析构 + 释放（不自动取消发布；如需取消发布请先调用unpublish）
template <typename T>
void shm_delete(T* p);
```

**示例：纯分配（无发布）**

```cpp
struct MyConfig {
    uint32_t version;
    uint32_t flags;
    uint64_t values[8];
};

MyConfig* cfg = uballoc::shm_new<MyConfig>();
cfg->version = 1;
// ... 跨进程通过固定VA访问cfg ...
uballoc::shm_delete(cfg);
```

**示例：分配+发布（合并API）**

```cpp
constexpr uint32_t TYPE_CONFIG = 42;

// 一次完成分配+构造+发布，取代两步式：
//   auto* cfg = uballoc::shm_new<MyConfig>();
//   uballoc::publish(cfg, sizeof(MyConfig), TYPE_CONFIG);
auto* cfg = uballoc::shm_new<MyConfig>(uballoc::pub_tid{TYPE_CONFIG});
cfg->version = 1;
// ... peer通过lookup_by_type_blocking(TYPE_CONFIG, ...)发现cfg ...
uballoc::unpublish(cfg);    // 先取消发布
uballoc::shm_delete(cfg);  // 再析构+释放
```

> **数据就绪性竞态说明**：与`malloc(size, type_id)`相同，指针在调用方初始化内存之前即被发布。调用方应在对象内放置原子`ready`标志位，在`shm_new`返回后置位；查找方应在`lookup_by_type_*`返回后自旋等待`ready`。
>
> **限制**：`shm_new`/`shm_delete`不支持数组形式，不支持`nothrow`重载。`shm_delete`不自动取消发布——若对象已通过`pub_tid`发布，需先调用`unpublish(ptr)`再`shm_delete(ptr)`。

## 可选/高级API

本节API不是必须的，绝大多数应用无需使用。仅在以下场景按需调用：

- 需要对齐分配（`memalign`）；
- 需要手动控制发布/取消发布时机（`publish`/`unpublish`）；
- 需要按地址反查发布信息（`lookup_by_address`）；
- 需要查询集群状态（`rank`/`total_processes`/`is_owner`/`is_initialized`）；
- 需要完全重置分配器（`reset`）；
- uffd不可用时手动启用SIGSEGV fault handler作为回退（`enable_fault_handler`）。

### uballoc_memalign

**函数功能**

按`alignment`对齐要求分配`size`字节内存。

**函数定义**

```cpp
void* uballoc::memalign(size_t size, size_t alignment);
void* uballoc_memalign(size_t size, size_t alignment);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `size` | 申请内存的字节数 | 非负数 | 输入 |
| `alignment` | 对齐字节数，需为2的幂 | 2的幂 | 输入 |

### uballoc_publish

**函数功能**

将已分配的指针发布到`type_id`槽位。与`malloc(size, type_id)`相比，本函数用于在调用方需要先初始化内存后再发布的场景。

**函数定义**

```cpp
int uballoc::publish(void* ptr, size_t size, uint32_t type_id);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `ptr` | 已分配的内存指针 | 非空 | 输入 |
| `size` | 已分配内存的字节数 | 非负数 | 输入 |
| `type_id` | 应用自定义的类型ID | 0..2^32-1 | 输入 |

**返回值**

- 成功：返回`slot index`（≥0）。
- 失败：返回负数（如该`type_id`已被其他进程占用）。

### uballoc_unpublish

**函数功能**

将`ptr`从对应的`type_id`槽位中移除。

**函数定义**

```cpp
bool uballoc::unpublish(void* ptr);
bool uballoc::unpublish(void* ptr, int owner_pid);
```

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `ptr` | 待取消发布的内存指针 | 非空 | 输入 |
| `owner_pid`（可选） | 已知的所有者进程ID；省略时自动推导 | -1..MAX_PROCESSES-1 | 输入 |

**返回值**

- 成功：`true`。
- 失败：`false`（指针未发布或owner_pid不匹配）。

### uballoc_lookup_by_address

**函数功能**

按地址反查发布信息：给定一个共享内存指针，返回其所属的`type_id`、`owner_process`与分配大小。常用于调试或验证跨进程指针归属。

**函数定义**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_address(void* ptr);
```

> **注**：仅C++ API。

**参数说明**

| 参数名 | 描述 | 取值范围 | 输入/输出 |
|--------|------|---------|----------|
| `ptr` | 待反查的共享内存指针 | 非空 | 输入 |

**返回值**

返回`PublishedInfo`结构体。若指针不属于任何已发布区域，`owner_process`字段为-1。

### uballoc_rank

**函数功能**

返回本进程在uballoc集群中的逻辑rank（0-based）。rank由自举共享内存PID表在`init()`时动态分配，与OS进程ID（`getpid()`）相互独立。

**函数定义**

```cpp
int uballoc::rank();
```

> **注**：仅C++ API。C API未暴露此函数。

**返回值**

- 当前进程的rank（0..MAX_PROCESSES-1）。

### uballoc_total_processes

**函数功能**

返回集群总进程数。在动态发现路径下，返回`MAX_PROCESSES`（默认8）；在显式配置路径下，返回构造时传入的`total_processes`。

**函数定义**

```cpp
int uballoc::total_processes();
```

> **注**：仅C++ API。

### uballoc_is_owner

**函数功能**

判断本进程是否为heap的owner（rank为0的进程）。

**函数定义**

```cpp
bool uballoc::is_owner();
```

> **注**：仅C++ API。owner负责创建bootstrap shm与全局元数据段。

### uballoc_is_initialized

**函数功能**

判断分配器是否已初始化（`init()`已调用且成功）。

**函数定义**

```cpp
bool uballoc::is_initialized();
```

> **注**：仅C++ API。

### uballoc_reset

**函数功能**

完全清理并重置分配器状态。执行`uffd_uninstall()`→`fh_uninstall()`→`clear_bootstrap_slot()`→`detach_all_regions()`→`wait_for_unreferenced()`→`unlink_own_shms()`→`cleanup_bootstrap_mapping()`→`cleanup_bootstrap()`→`release_reservation()`，然后调用`soft_reset()`清空内存状态。fork子进程应使用`soft_reset()`而非`reset()`。

**函数定义**

```cpp
void uballoc::reset();
```

> **注**：仅C++ API。

### uballoc_enable_fault_handler

**函数功能**

启用SIGSEGV-based lazy-attach handler作为uffd的回退机制。handler在缺页时通过预登记的设备fd加载远端段，缺页指令重放后成功执行。

**函数定义**

```cpp
bool uballoc::enable_fault_handler();
bool uballoc_enable_fault_handler(void);
```

**返回值**

- 成功：`true`。
- 失败：`false`（如备用栈分配失败）。

> **注意**：自V0.4起，uffd为默认开启。仅在uffd不可用（`ENOSYS`或`EPERM`）时，应用需要手动启用fault handler作为回退。

### uballoc_disable_fault_handler

**函数功能**

停用fault handler，恢复前序SIGSEGV handler，清空段注册表。在`reset()`/`soft_reset()`中自动调用，多次调用安全。

**函数定义**

```cpp
void uballoc::disable_fault_handler();
void uballoc_disable_fault_handler(void);
```

### uballoc_enable_userfaultfd

**函数功能**

启用userfaultfd-based lazy-attach handler。优先尝试uffd，失败时回退至SIGSEGV handler。uffd handler在独立线程的普通上下文中运行，可调用`ubs_mem_shm_attach`（socket/malloc/connect）。

**函数定义**

```cpp
bool uballoc::enable_userfaultfd();
bool uballoc_enable_userfaultfd(void);
```

**返回值**

- 成功：`true`。
- 失败：`false`。

> **注意**：自V0.4起，本函数在构造器优先级102处自动调用，应用通常无需显式调用。

### uballoc_disable_userfaultfd

**函数功能**

停用uffd handler：停止handler线程、关闭uffd fd、将预留区域`mprotect`回`PROT_NONE`。在`reset()`/`soft_reset()`与`~DistributedShmBackend()`中自动调用，多次调用安全。

**函数定义**

```cpp
void uballoc::disable_userfaultfd();
void uballoc_disable_userfaultfd(void);
```

---

# 常见问题

本节汇总用户高频关注的行为语义与使用约束，帮助避免常见误区。

## `realloc`缩容时是否分配新内存并释放旧块？

**不会。** 当`new_size <= old_size`（缩容或等尺寸）时，`realloc`直接返回原指针，不分配新内存、不拷贝数据、不释放旧块。此行为与标准C `realloc`一致（glibc、jemalloc、mimalloc均如此实现）。

缩容返回原指针意味着：原slab slot/huge slot的 excess 空间不会被回收，该slot仍按原尺寸占用。如需释放原slot以供其他进程或线程复用，请显式执行`malloc(new_size)` + `memcpy` + `free(old_ptr)`。

扩容（`new_size > old_size`）时，`realloc`分配新块、拷贝数据、释放旧块，返回新指针（`!= old_ptr`）。

**Huge分配的realloc特殊行为：**

Huge分配的`class_size`返回`slot_count * SLAB_SIZE`（SLAB_SIZE=4MB），而非实际请求的分配大小。例如，请求5MB的Huge分配占用2个slot，`class_size`为8MB。

这意味着：

- **`new_size` <= `class_size`（即使`new_size` > 原始请求大小）**：视为缩容，返回原指针。例如，原分配5MB（`class_size`=8MB），`realloc`到6MB → 6MB <= 8MB → 返回原指针，不分配新块。多余的slot空间（8MB - 6MB = 2MB）被浪费。

- **`new_size` > `class_size`**：视为扩容，分配新块、拷贝数据、释放旧块，返回新指针（`!= old_ptr`）。由于旧slot在`allocate`时仍被占用（CAS已claim），新分配必然落在不同slot，因此`new_ptr != old_ptr`。

因此，对Huge分配调用`realloc`时，新旧指针可能相同（缩容时）也可能不同（扩容时），取决于`new_size`与`class_size`（而非原始请求大小）的比较结果。

## 进程间协调与退出时机

### 核心原则

**拥有共享内存的进程在退出（或调用`reset()`）前，必须确保其他进程已完成对该数据的所有访问。** 违反此原则会导致数据不可访问（查找超时）或进程崩溃（SIGBUS）。

### 两个阶段，两种后果

进程间共享数据的典型生命周期分为两个阶段，每个阶段都有退出过早的后果：

**阶段一：查找阶段** — 消费方通过`lookup_by_type_blocking`发现发布方的数据。

发布方进程退出时，其析构函数会清理自举槽位（bootstrap slot）并释放共享内存元数据。此后消费方无法发现该发布方，`lookup_by_type_blocking`超时返回`owner_process == -1`。

**阶段二：访问阶段** — 消费方读取、修改数据，或对容器调用`shm_delete`。

在UBSE多节点环境下，进程退出时调用`shm_detach_by_name`释放共享内存，UBSE设备会**失效（invalidate）所有节点上该shm的映射**。如果其他进程仍在访问该数据（例如STL容器析构遍历树节点），映射被失效，访问触发**SIGBUS**。

```
P0 (节点A)                        P1 (节点B)
──────────                        ──────────
                                  发布数据 → 等待消费方确认
查找数据 ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ 
读取/修改数据 → 信号"完成"         ← 阶段一：发布方不能在此前退出
                                  （否则查找超时）
                                  
验证数据 → shm_delete → ...        退出 → shm_detach_by_name()
  (析构遍历P1数据段中的树节点)        → UBSE使所有节点映射失效 ✗
P0访问失效页面 → SIGBUS ✗          ← 阶段二：P1不能在此前退出
                                  （否则SIGBUS）
```

### 关键机制说明

1. **`shm_detach_by_name`是失效操作**：不仅释放本节点的borrow，还会使所有跨节点映射失效。这是UBSE设备的机制，不是uballoc的行为。POSIX上此函数为no-op，映射不受影响。
2. **懒加载（uffd）导致引用计数不可靠**：消费方可能尚未访问数据（uffd handler尚未attach），引用计数为0。发布方无法通过引用计数判断消费方是否仍在使用，必须等待明确信号。
3. **`shm_delete`访问跨进程数据**：当P0对P1修改过的STL容器调用`shm_delete`时，析构函数遍历容器内部结构（树节点、数组缓冲区），这些数据位于P1的数据段中。P1必须存活到P0的`shm_delete`完成。
4. **进程退出时的自动清理也会使映射失效**：UBSE在进程退出时自动释放所有borrow（auto-cleanup），此操作同样会使跨节点映射失效。因此仅推迟`shm_detach_by_name`的调用无法解决问题。

### 推荐模式

**模式一：双方sleep（简单，推荐用于测试）**

```cpp
// P1: 通知完成后sleep
uballoc::publish(reinterpret_cast<void*>(1), 0, TYPE_P1_DONE);
sleep(2);  // 保持存活，让P0完成shm_delete
return 0;

// P0: 收到通知后执行shm_delete，然后sleep
auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
// ... 验证数据 ...
uballoc::shm_delete(vec);  // 析构遍历P1数据段（P1仍存活）
sleep(2);  // 保持存活，让P1的cleanup在P0退出后运行
return 0;
```

此模式在`stl_full_shm_example.cpp`中验证通过。

**模式二：显式完成握手（更健壮）**

```cpp
// P1: 通知完成 → 等待P0的"shm_delete完成"信号
uballoc::publish(&done_flag, sizeof(done_flag), TYPE_P1_DONE);
// 等待P0确认shm_delete完成
auto p0_done = uballoc::lookup_by_type_blocking(TYPE_P0_DELETE_DONE, 30000);
return 0;

// P0: 收到P1完成 → shm_delete → 通知P1可以退出
auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
// ... 验证数据 ...
uballoc::shm_delete(vec);
uballoc::publish(&done_flag, sizeof(done_flag), TYPE_P0_DELETE_DONE);
return 0;
```

**模式三：发布/消费ready标志（适用于简单数据传递）**

适用于发布方仅传递原始数据（非STL容器）的场景：

```cpp
// 发布方
auto* cfg = static_cast<ShareBlock*>(uballoc::malloc(sizeof(ShareBlock), type_id));
cfg->ready.store(1, std::memory_order_release);
while (cfg->ready.load(std::memory_order_acquire) != 2) { ::usleep(1000); }
uballoc::free(cfg);  // 安全释放

// 消费方
auto info = uballoc::lookup_by_type_blocking(type_id, 10000);
auto* cfg = static_cast<ShareBlock*>(info.address);
while (cfg->ready.load(std::memory_order_acquire) != 1) { ::usleep(1000); }
// ... 读取并处理数据 ...
cfg->ready.store(2, std::memory_order_release);  // 通知发布方
```

注意：此模式不覆盖`shm_delete`场景。如果消费方修改了STL容器内容，发布方调用`shm_delete`时仍需使用模式一或模式二。

### 不应做的事

- 不要在信号"完成"后立即退出 — 对方进程需要时间完成`shm_delete`
- 不要在其他进程访问你的数据时调用`reset()`
- 不要假设库会阻止跨节点映射失效 — 这是UBSE设备行为，库无法拦截

### 库提供的能力

- `publish()` / `lookup_by_type_blocking()` — 数据发现与同步
- 发布方进入cleanup后，消费方仍可发现其数据（bootstrap slot在`wait_for_unreferenced`期间保持`ready=1`）
- `wait_for_unreferenced()` 无限等待其他节点释放borrow——所有进程正常退出时，shms自动清理，无残留
- `check_remote_segments()` — 显式（非懒加载）attach远端段（线程安全，多线程并发调用不会重复attach）

### 库无法提供的能力

- 知道其他进程何时"完成访问"你的数据（需要应用层协调）
- 阻止进程退出时的跨节点映射失效（UBSE设备行为）
- 远端进程崩溃后自动恢复——本地进程会无限等待（挂起），需要手动kill + 清理脚本清理残留shms

### 清理残留共享内存

当进程崩溃或被kill后，其创建的共享内存对象可能残留。安装包附带两个清理脚本（位于`share/uballoc/scripts/`）：

**UBSE环境（`cleanup_shms_ub.sh`）：**

基于`ubsectl`自动发现并清理。脚本查询集群中所有匹配`ub-<heap_id>-*`前缀的shm，自动识别借方（borrower）和贷方（lender）节点，分两阶段清理（先detach再delete）。

```bash
# 清理默认heap_id="heap"的所有残留shm
./cleanup_shms_ub.sh

# 清理指定heap_id
UBALLOC_HEAP_ID=myheap ./cleanup_shms_ub.sh
```

要求：
- 本节点和所有对端节点的`PATH`中有`ubsectl`
- 到所有对端节点有免密SSH（或对端就是本节点）

**POSIX环境（`cleanup_shms.sh`）：**

基于`/dev/shm`文件路径清理。需要指定进程数和节点列表。

```bash
# 单机清理（2进程）
./cleanup_shms.sh 2

# 多节点：P0本地，P1在node1
./cleanup_shms.sh 2 "" node1

# 使用节点配置文件
./cleanup_shms.sh -f nodes.txt
```

`nodes.txt`格式（每行一个主机名，空行或"local"表示本机）：
```
local
10.0.0.2
10.0.0.3
```

可通过`UBALLOC_HEAP_ID`环境变量指定heap_id（默认为"heap"）。

**何时使用：**
- 进程崩溃后（`wait_for_unreferenced`挂起 → kill后残留shm）
- 测试间清理（确保干净环境）
- 定期维护（清理长期运行后积累的残留shm）

---

以上结果仅供参考，以实际运行结果为准。

---

## 已知问题：UBSE VMA 销毁

**严重程度：高** — 影响 UBSE 后端的多进程/多线程场景。

### 问题描述

UBSE 设备驱动或运行时在执行跨进程 `shm_create`/`shm_attach` 操作时，会**销毁**目标进程地址空间中已有的 mmap'd VMA（虚拟内存区域）。VMA 被完全销毁（不是 PROT_NONE，而是从 `/proc/PID/maps` 中消失），导致后续访问该地址时触发 SIGSEGV。

### 影响

- **uffd 无法捕获此故障**：uffd 只能捕获 uffd 注册 VMA 上的"缺页"故障。VMA 被销毁后，没有 uffd 注册，故障变为原始 SIGSEGV。
- **不加处理则进程崩溃**：SIGSEGV 默认行为是终止进程。
- **多线程 STL 容器场景易触发**：`test_multi_thread`（4 生产线程 + 4 消费线程，STL 容器 clear+refill）中观察到此问题。单线程测试（如 `test_stl`）因时序不同通常不触发。

### 当前方案：SIGSEGV+SIGBUS handler 作为安全网

在 uffd 激活时**也安装** SIGSEGV+SIGBUS handler（在 `uballoc_uffd_auto_init` 构造函数和 `try_enable_uffd_locked` 中）。handler 检测到 VA 范围内的故障时，从 `g_fh_entries` 注册表中查找对应的段 fd，重新 `mmap(MAP_FIXED, PROT_RW)` 恢复映射，故障线程继续执行。

**stderr 诊断输出：**
- `fh_sigsegv: resolved remap fault=0x...` — handler 重新映射了被销毁的 VMA（正常运行，无需担心）
- `fh_sigsegv: fault=0x... reason=...` + maps dump — handler 无法恢复，进程即将崩溃

### 根因

UBSE 设备驱动行为，超出 uballoc 控制范围。需要 UBSE 团队调查：
- `ubs_mem_shm_attach`/`ubs_mem_shm_create` 是否会修改调用进程的已有 VMA？
- UBSE 内核模块是否会在其他进程创建 shm 时 munmap 其地址空间中的 VMA？

### 规避建议

- 每次测试前运行 `cleanup_shms_ub.sh` 清理残留 shm
- 多线程场景中，生产端先启动、消费端后启动可减少触发概率
- 关注 stderr 中的 `fh_sigsegv: resolved` 消息——如果频繁出现，说明 VMA 销毁正在发生

---

Licensed under the Apache License, Version 2.0.
See [LICENSE](LICENSE) for the full license text.
