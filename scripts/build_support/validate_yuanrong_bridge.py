# Copyright 2026 The xLLM Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://github.com/xLLM-AI/xllm/blob/main/LICENSE
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exercise the real C bridge against a running Host KV Worker.

Run from the checkout root with python -m scripts.build_support.validate_yuanrong_bridge.
Each concurrent client has its own handle, matching the bridge's ownership contract.
"""

import argparse
import ctypes
import os
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from scripts.logger import logger


class _StoreBuffer(ctypes.Structure):
    _fields_ = [
        ("addresses", ctypes.POINTER(ctypes.c_void_p)),
        ("sizes", ctypes.POINTER(ctypes.c_uint64)),
        ("count", ctypes.c_size_t),
    ]


class _BufferBatch:
    def __init__(self, payloads: list[list[bytes]]) -> None:
        self._ranges = [
            [ctypes.create_string_buffer(part, len(part)) for part in payload]
            for payload in payloads
        ]
        self._addresses = [
            (ctypes.c_void_p * len(ranges))(
                *(ctypes.addressof(part) for part in ranges)
            )
            for ranges in self._ranges
        ]
        self._sizes = [
            (ctypes.c_uint64 * len(ranges))(*(len(part) for part in ranges))
            for ranges in self._ranges
        ]
        self.values = (_StoreBuffer * len(payloads))(
            *(
                _StoreBuffer(addresses, sizes, len(ranges))
                for addresses, sizes, ranges in zip(
                    self._addresses, self._sizes, self._ranges
                )
            )
        )

    def _read(self, index: int) -> bytes:
        return b"".join(part.raw for part in self._ranges[index])


def _load_bridge(path: Path) -> ctypes.CDLL:
    library = ctypes.CDLL(
        str(path.resolve()), mode=os.RTLD_NOW | os.RTLD_LOCAL | os.RTLD_NODELETE
    )
    library.xllm_yr_store_create.argtypes = [
        ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32
    ]
    library.xllm_yr_store_create.restype = ctypes.c_void_p
    library.xllm_yr_store_destroy.argtypes = [ctypes.c_void_p]
    library.xllm_yr_store_destroy.restype = None
    library.xllm_yr_store_mset.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(_StoreBuffer), ctypes.c_size_t,
    ]
    library.xllm_yr_store_mset.restype = ctypes.c_bool
    library.xllm_yr_store_mget.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(_StoreBuffer), ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    library.xllm_yr_store_mget.restype = ctypes.c_bool
    library.xllm_yr_store_last_error.argtypes = [ctypes.c_void_p]
    library.xllm_yr_store_last_error.restype = ctypes.c_char_p
    return library


def _exercise_client(
    library: ctypes.CDLL, host: str, port: int, rounds: int
) -> None:
    client = library.xllm_yr_store_create(host.encode(), port, 5000)
    if not client:
        raise RuntimeError(library.xllm_yr_store_last_error(None).decode())
    try:
        prefix = f"xllm-bridge-{uuid.uuid4().hex}"
        for iteration in range(rounds):
            keys = (ctypes.c_char_p * 2)(
                f"{prefix}-{iteration}-a".encode(),
                f"{prefix}-{iteration}-b".encode(),
            )
            payloads = [[b"first\x00", b"payload"], [b"second", b"-value"]]
            writes = _BufferBatch(payloads)
            if not library.xllm_yr_store_mset(client, keys, writes.values, 2):
                raise RuntimeError(library.xllm_yr_store_last_error(client).decode())

            reads = _BufferBatch([
                [bytes(len(part)) for part in payloads[0]],
                [bytes(7)],
                [bytes(len(part)) for part in payloads[1]],
            ])
            read_keys = (ctypes.c_char_p * 3)(
                keys[0], f"{prefix}-{iteration}-missing".encode(), keys[1]
            )
            statuses = (ctypes.c_uint8 * 3)()
            if not library.xllm_yr_store_mget(
                client, read_keys, reads.values, statuses, 3
            ):
                raise RuntimeError(library.xllm_yr_store_last_error(client).decode())
            if list(statuses) != [1, 0, 1]:
                raise RuntimeError(f"Unexpected partial-hit statuses: {list(statuses)}")
            if (
                reads._read(0) != b"".join(payloads[0])
                or reads._read(2) != b"".join(payloads[1])
            ):
                raise RuntimeError("Scatter/gather payload mismatch")

            replacements = _BufferBatch([[b"replacement-a"], [b"replacement-b"]])
            if not library.xllm_yr_store_mset(client, keys, replacements.values, 2):
                raise RuntimeError(library.xllm_yr_store_last_error(client).decode())
            if not library.xllm_yr_store_mget(
                client, read_keys, reads.values, statuses, 3
            ):
                raise RuntimeError(library.xllm_yr_store_last_error(client).decode())
            if (
                reads._read(0) != b"".join(payloads[0])
                or reads._read(2) != b"".join(payloads[1])
            ):
                raise RuntimeError("NX write replaced an existing value")
        logger.info(f"Client passed {rounds} rounds of writes, partial hits, and NX checks")
    finally:
        library.xllm_yr_store_destroy(client)


def _main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--preload", action="append", default=[])
    args = parser.parse_args()
    if not 0 <= args.port <= 65535 or args.clients < 1 or args.rounds < 1:
        parser.error("port must be 0..65535; clients and rounds must be positive")

    preloaded = [
        ctypes.CDLL(path, mode=os.RTLD_NOW | os.RTLD_GLOBAL)
        for path in args.preload
    ]
    logger.info(f"Loaded {len(preloaded)} host libraries with RTLD_GLOBAL")
    library = _load_bridge(args.bridge)
    # Check reinitialization after destroying all client handles. The DSO stays
    # resident because DS process-wide state is not covered by ShutDown().
    _exercise_client(library, args.host, args.port, args.rounds)
    with ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [
            executor.submit(
                _exercise_client, library, args.host, args.port, args.rounds
            )
            for _ in range(args.clients)
        ]
        for future in futures:
            future.result()
    logger.info("Bridge validation passed; SDK remains loaded until process exit")


if __name__ == "__main__":
    _main()
