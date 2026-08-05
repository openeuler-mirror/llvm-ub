# llvm-ub

**中文** | **[English](README.en.md)**

llvm-ub 是 openEuler 社区下基于 UB（Unified Bus）的分布式编程项目集合，为多进程、多节点共享内存场景提供基础设施。

## 子项目

| 项目 | 说明 | 状态 |
|------|------|------|
| [uballoc](uballoc/) | 面向 ARMv8 多进程多节点部署的 C++17 共享内存分配器 | 活跃 |

## 快速上手

以 uballoc 为例：

```bash
# POSIX 构建（标准 Linux，无 UBSE 依赖）
cd uballoc
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j

# 运行测试
cd build && ctest --output-on-failure
```

详细文档见各子项目目录。

## 遗留项目

以下项目不再维护，保留仅供参考：

- [matrixcpp](legacy/matrixcpp/) — 基于 Ray 的分布式并发编程库

## 许可证

Apache-2.0。完整文本见 [LICENSE](LICENSE)。
