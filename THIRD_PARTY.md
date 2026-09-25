# Third-party components

- **Wasmtime 49.0.0**, Bytecode Alliance: Apache-2.0 with LLVM exception. The build downloads the official [C API release](https://github.com/bytecodealliance/wasmtime/releases/tag/v49.0.0), verifies the archive SHA-256 in `tools/fetch-deps.py`, and includes its license under `dist/licenses/wasmtime/`.
- **MkDocs** and **Material for MkDocs**: BSD-2-Clause and MIT respectively, used for the documentation site. Versions are pinned in `requirements-docs.txt`.

- **MinHook 1.3.4**, Tsuda Kageyu and contributors: BSD-2-Clause, with the included HDE license notices. Experimental gameplay builds download the [tagged source](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), check its pinned SHA-256, link it statically into the trusted runtime, and include `dist/licenses/minhook/LICENSE.txt`.

No game binaries, game assets, or Windows system DLLs are distributed. The proxy loads the user's Windows XInput library by its absolute system path.
