---
title: "A2 环境编译与验证 DataSystem 接入"
description: "在真实 Ascend 910B 环境构建包含 DS 私有静态 SDK 的 xLLM，并分层验证。"
sidebar:
  order: 5
---

<!-- Copyright 2026 The xLLM Authors. All Rights Reserved. -->

本文对应 `codex/yuanrong-private-sdk-validation-20261116` 分支。无卡环境适配提交
`abf6c340`、`9f1c7bb2` 已回退，保留 DS 私有静态 PIC SDK、bridge 符号隔离、
加载和打包实现，以及 CATLASS 依赖检查、GoogleTest 扫描和 ATB customize 导入修复。

截至交接，DS bridge 已实际编译、链接并通过符号检查，独立 Host KV 调用也已通过；
完整 xLLM 主程序、完整 wheel、真实 NPU 推理尚未完成验证。下列步骤是新环境的验收流程。

## 1. 准备真实 A2 环境

按[昇腾 NPU 环境配置](/zh/hardware/ascend_npu/)创建 A2 容器，透传实际使用的 NPU、管理设备和
宿主机驱动。以下命令均在容器内执行，使用 Bash；构建时也需要真实 NPU 可用。

本次 DS 二进制包只支持 **Linux aarch64、libstdc++、DS ABI=1**。
如果 A2 服务器的 CPU 是 x86_64，需要 DS 提供对应架构的 SDK，不能直接复用此包。
此前编译使用 CANN 9.0.0、Torch/torch_npu 2.10.0、Python 3.11、GCC 11.4；
新环境应具备相互兼容的驱动、CANN、NNAL/ATB、Torch 和 torch_npu。
SDK 要求至少提供 GLIBC 2.34、GLIBCXX 3.4.30、CXXABI 1.3.13。

编译工具链可能有更高要求：此前获取的固定 A2 TileLang wheel 需要 GLIBC 2.38。
SDK 的运行库下限不能代表完整工具链的下限；若镜像不满足编译器 wheel 的要求，
应选用匹配的开发镜像或编译器构建，真实 NPU 本身不会解决运行库版本不匹配。

从新的 shell 开始，加载镜像内实际安装的 CANN 与 NNAL/ATB 环境，例如：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh
export ASCEND_RT_VISIBLE_DEVICES=0

uname -m
npu-smi info
python3 - <<'PY'
import acl
import torch
import torch_npu

assert acl.init() == 0
soc = acl.get_soc_name()
assert "910B" in soc.upper(), soc
assert torch.npu.is_available()
value = torch.ones(4, device="npu")
torch.npu.synchronize()
assert value.cpu().sum().item() == 4
print("SoC:", soc)
print("Torch:", torch.__version__, "torch_npu:", torch_npu.__version__)
print("Torch CXX11 ABI:", int(torch.compiled_with_cxx11_abi()))
PY
cmake --version
g++ --version
```

环境脚本路径随安装布局调整。必须先通过上述真实设备测试，再开始编译。
不设置 `XLLM_ASCEND_PLATFORM`；平台恢复为实际设备探测。
不沿用旧无卡容器的 `build-env.sh`、私有 glibc/Python、驱动 stub 搜索路径或修改后的 Ninja 构建图。
Torch ABI 由当前安装决定；ATB customize 必须匹配 Torch ABI，DS bridge 则独立使用 SDK 的 ABI。

## 2. 获取源码和 SDK

使用新目录，避免旧构建缓存或已安装 wheel 混入：

```bash
git clone --branch codex/yuanrong-private-sdk-validation-20261116 \
  https://github.com/GoMarck/xllm.git xllm-a2-ds
cd xllm-a2-ds
git submodule update --init --recursive
mkdir -p artifacts-a2
git rev-parse HEAD | tee artifacts-a2/xllm-commit.txt
git submodule status --recursive > artifacts-a2/submodules.txt
python3 -m pip freeze > artifacts-a2/pip-freeze.txt
```

SDK 源码版本为 `7fa20a784473a8cffa758759ac81984d0967ba30`，交付包名为
`datasystem-private-sdk-linux-aarch64-7fa20a7.tar.gz`。尚无公共下载地址；可以使用交付附件，
也可按交付说明中的 SSH 连接方式，从已有验证节点复制已落盘的安装包。

将包放到源码根目录后校验并解压。解压目录就是 SDK 安装前缀：

```bash
echo '4b353815fd29fc7ef65ede590f964a8133df4947ad6b8df40c226a366146ccea  datasystem-private-sdk-linux-aarch64-7fa20a7.tar.gz' \
  | sha256sum -c -
mkdir -p ds-sdk
tar -xzf datasystem-private-sdk-linux-aarch64-7fa20a7.tar.gz -C ds-sdk
export YUANRONG_ROOT="$PWD/ds-sdk"
test -f "$YUANRONG_ROOT/lib/cmake/DatasystemPrivate/DatasystemPrivateConfig.cmake"
cat "$YUANRONG_ROOT/share/datasystem-private/build-info.txt"
```

不需要把 DS 源码加入 xLLM，也不需要安装 DS Python wheel 或添加 DS 私有动态库路径。

## 3. 准备构建依赖与 ATB customize

使用 A2 开发镜像提供的匹配工具链；CMake 至少为 3.26，需有 Ninja、binutils、
Git、C/C++ 编译器和 Python 构建工具。仓库的 `pre_build` 会检查并按需安装依赖，
涉及系统软件安装、Git 和第三方源下载，需要相应权限及网络。
Go 版本跟随 Mooncake，Rust 用于 tokenizer/safetensors，不应跳过对应目标。

```bash
python3 -m pip install 'setuptools>=68' wheel pre-commit
pre-commit install
export MAX_JOBS=6
export OMP_NUM_THREADS=6
export TRITON_CACHE_DIR="$PWD/.cache/triton"
mkdir -p "$TRITON_CACHE_DIR"
```

若镜像未提供匹配的 ATB customize，先构建真实库。此脚本还会获取 JSON、MKI 和
ATB 内部头文件依赖，必须确认命令成功并检查产物；不使用占位头文件或库：

```bash
ATB_ABI="$(python3 -c 'import torch; print(int(torch.compiled_with_cxx11_abi()))')"
export ATB_PATH="${ATB_PATH:-/usr/local/Ascend/nnal/atb}"
export ATB_HOME_PATH="$ATB_PATH/latest/atb/cxx_abi_${ATB_ABI}"
export LD_LIBRARY_PATH="$ATB_HOME_PATH/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
(
  set -o pipefail
  cd third_party/xllm_ops/atb_customize
  bash build.sh --use_cxx11_abi="$ATB_ABI" 2>&1 \
    | tee ../../../artifacts-a2/atb-customize-build.log
)
export XLLM_ATB_CUSTOMIZE_ROOT="$PWD/third_party/xllm_ops/atb_customize/output/ops_customize/cxx_abi_${ATB_ABI}"
test -f "$XLLM_ATB_CUSTOMIZE_ROOT/include/customize/customize_op_params.h"
test -f "$XLLM_ATB_CUSTOMIZE_ROOT/lib/libatb_customize.so"
export LD_LIBRARY_PATH="$XLLM_ATB_CUSTOMIZE_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ldd "$XLLM_ATB_CUSTOMIZE_ROOT/lib/libatb_customize.so"
```

`ldd` 不应出现 `not found`。`libatb_customize.so` 还依赖实际安装的 ATB 及
`libcustomize_ops.so`；同目录和 ATB 环境均需可见。如果复用已有安装，直接将
`XLLM_ATB_CUSTOMIZE_ROOT` 指向其前缀，并完成同样的头文件、ABI 和依赖检查。

## 4. 完整构建并检查 wheel

此处走正常构建入口，包括 TileLang AOT、CANN 算子和 Triton 资源生成及其 NPU 测试。
不跳过资源目标、不创建替代 stamp。首次构建可能耗时较长。

```bash
export CMAKE_ARGS="-DUSE_XLITE=OFF -DXLLM_ATB_CUSTOMIZE_ROOT=$XLLM_ATB_CUSTOMIZE_ROOT -DPARALLEL_COMPILE_JOBS=6 -DPARALLEL_LINK_JOBS=1 -DCMAKE_JOB_POOL_LINK=link_pool"
set -o pipefail
bash scripts/build_support/build_xllm_yuanrong_wheel.sh --device npu \
  2>&1 | tee artifacts-a2/full-build.log
```

要求命令退出码为 0，日志包含 `YuanRong bridge passed dependency and symbol checks`。
构建脚本会检查 wheel 同时包含宿主和 bridge。进一步检查实际 wheel 内容：

```bash
export A2_WHEEL="$(find "$PWD/dist" -maxdepth 1 -name '*.whl' -print -quit)"
test -n "$A2_WHEEL"
sha256sum "$A2_WHEEL" | tee artifacts-a2/wheel.sha256
python3 - <<'PY'
import os
import zipfile

with zipfile.ZipFile(os.environ["A2_WHEEL"]) as wheel:
    names = wheel.namelist()
    assert "xllm/xllm" in names
    assert "xllm/libxllm_yuanrong_store_bridge.so" in names
    assert any(n.startswith("xllm/triton_npu/binary/") and n.endswith(".npubin") for n in names)
    assert any(n.startswith("xllm/opp/vendors/custom_xllm_math/") for n in names)
    wheel.extractall("artifacts-a2/wheel-unpacked")
print("Wheel host, bridge and NPU resources found")
PY
export BRIDGE="$PWD/artifacts-a2/wheel-unpacked/xllm/libxllm_yuanrong_store_bridge.so"
cmake "-DBRIDGE_FILE=$BRIDGE" "-DREADELF=$(command -v readelf)" \
  "-DNM=$(command -v nm)" -P cmake/check_yuanrong_bridge.cmake
readelf --version-info "$BRIDGE" > artifacts-a2/bridge-symbol-versions.txt
ldd "$BRIDGE" | tee artifacts-a2/bridge-ldd.txt
```

bridge 应只导出五个 `xllm_yr_store_*` C 接口，动态依赖应仅有审计脚本允许的系统运行库，
没有 DS、brpc、protobuf、gflags 等私有依赖 DSO。部署环境必须满足实际 ELF 符号版本要求。

## 5. 验证真实 DS Host KV Worker

由 DS 部署并启动与 SDK 对应版本的 Host KV Worker，明确其 client 端口和可达地址。
历史容器或历史测试端口不代表 Worker 已运行。以下参数必须替换为真实服务地址：

```bash
export DS_WORKER_HOST=127.0.0.1
export DS_WORKER_PORT=50001
set -o pipefail
python3 -m scripts.build_support.validate_yuanrong_bridge \
  --bridge "$BRIDGE" --host "$DS_WORKER_HOST" --port "$DS_WORKER_PORT" \
  --clients 4 --rounds 10 2>&1 | tee artifacts-a2/bridge-worker.log
```

要求退出码为 0，并出现 `Bridge validation passed`。该步骤覆盖真实写入、批量读取、
部分命中、scatter/gather 内容校验、NX 语义、独立客户端并发及销毁后重建。
测试使用随机键写入真实 Worker，建议使用测试实例。它不等于 xLLM 宿主依赖共存或推理验收。

## 6. 安装和 xLLM 端到端联调

```bash
python3 -m pip install --force-reinstall --no-deps "$A2_WHEEL"
xllm serve --help
```

使用已在该 A2 环境验证过的模型和启动配方，先关闭 Store 跑通基线，再开启 DS。
单卡示例须按实际模型容量调整；`host_blocks_factor` 必须大于 1，且需为 Host cache
预留内存。运行时保留前面配置的 CANN、ATB customize 动态库环境。

```bash
export MODEL_PATH=/path/to/model/Qwen3-8B
set -o pipefail
xllm serve \
  --model="$MODEL_PATH" --model_id=Qwen3-8B \
  --port=8010 --nnodes=1 --master_node_addr=127.0.0.1:9748 \
  --communication_backend=hccl --block_size=128 \
  --max_memory_utilization=0.80 \
  --enable_prefix_cache=true --host_blocks_factor=2 \
  --enable_kvcache_store=true --store_backend=yuanrong \
  --store_yuanrong_host="$DS_WORKER_HOST" \
  --store_yuanrong_port="$DS_WORKER_PORT" \
  --store_yuanrong_timeout_ms=500 --v=2 \
  2>&1 | tee artifacts-a2/xllm-yuanrong.log
```

默认从已安装包查找 bridge；排查加载路径时可增加
`--store_yuanrong_bridge_path="$BRIDGE"`，但最终需去掉此覆盖以确认 wheel 自带路径可用。
在另一个具有同样环境的 shell 中检查服务：

```bash
curl --fail http://127.0.0.1:8010/health
curl --fail http://127.0.0.1:8010/v1/models
curl --fail http://127.0.0.1:8010/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"Qwen3-8B","messages":[{"role":"user","content":"请介绍一下北京。"}],"max_tokens":32,"temperature":0}'
```

验收时检查各 rank 的 `log/node_*.log`：应出现 `YuanRong store backend ready`，
且无重复 flags 注册、符号解析错误、崩溃或关闭异常。
仅服务健康、backend ready 或重复短请求不能证明 DS 缓存命中：需要用足够长的共同前缀，
结合其他请求或重启清空本地缓存，观察真实 `MSet`、`MGet` 和返回命中状态；
同时核对 Worker 日志、响应正确性以及并发请求。仅见 `MGet success=1` 也不能证明命中，
因为全部未命中仍可能是一次成功调用。

## 7. 保存验收证据与问题定位

保留 `artifacts-a2`、wheel、SDK SHA256、模型版本、完整启动参数、各 rank 日志、
Worker 日志和退出码。分开记录“完整编译”“wheel 内容”“独立 bridge KV 调用”
和“xLLM 内实际 DS 命中/并发/关闭”，逐层确认。

| 失败位置 | 优先检查 |
| --- | --- |
| `npu-smi`、设备张量或 `acl.init` | 设备透传、宿主驱动及 CANN 兼容性 |
| TileLang 加载报 `GLIBC_* not found` | 预编译编译器 wheel 与镜像运行库是否匹配；真实 NPU 不会消除此问题 |
| Folly 编译缺少 `io_uring_*` API | 检查头文件和库是否混用了旧版 liburing；此前构建使用匹配的 liburing 2.5 |
| 缺少 `customize/customize_op_params.h` 或 `atb_customize` | 第 3 步的真实安装前缀、Torch ABI 及依赖库 |
| SDK 架构或 ABI 错误 | SDK 包架构和 bridge 编译选项，不将宿主 Torch ABI 强加给 DS |
| Triton 生成或 NPU pytest 失败 | 实际设备、编译器和对应 kernel；不跳过资源生成来宣称完整构建成功 |
| bridge ELF 审计失败 | 保留完整链接日志；检查是否误用了 DS 动态 SDK 或丢失静态依赖闭包 |
| DS 初始化超时 | Worker 进程、client 端口、网络及服务日志 |
| 启动成功但没有 DS 命中 | Host cache/Store 配置、淘汰/回读是否触发、逐键命中状态 |

DS bridge 使用 `RTLD_NODELETE` 保持驻留至进程退出；客户端销毁不代表 SDK 可以安全卸载。
本流程不默认覆盖 TLS/认证、故障恢复、长期压力和性能验收。
