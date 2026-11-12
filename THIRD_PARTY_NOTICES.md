# Third-party notices and license scope

Original integration code, tests, and documentation contributed to this project
are offered under the Apache License 2.0 in LICENSE, with the attribution notices
in NOTICE. Releases through 1.0 were offered under the MIT license. This grant
covers only original project contributions and does not relicense third-party
code or material embedded in a patch or generated test fixture.

| Component | Source | Licensing / distribution status |
|---|---|---|
| DXMT integration patch | [DXMT fork](https://github.com/NerRobDog/dxmt) at `c5dc3a0dfe9108e667da43de871324bd298c9c02` | LGPL-2.1-or-later; original notices and license text retained under licenses/ |
| Wine Mac driver patch | [CodeWeavers Wine 26.3 source](https://media.codeweavers.com/pub/crossover/source/crossover-sources-26.3.0.tar.gz) | Existing Wine LGPL-2.1-or-later terms retained; this is not the proprietary CrossOver application |
| Runtime assembly reference | [Soju](https://github.com/BCD1210/soju/tree/3a350b32bf906dd2a509b18c642a5a2676de022a) | Separate upstream project; GPL-3.0 scripts are referenced, not relicensed or vendored as project code |
| DXMT build dependencies | LLVM, DirectX headers, Metal toolchain, build tools | Separate upstream licenses; not included in this repository |
| NVIDIA NVAPI headers | [NVIDIA/nvapi](https://github.com/NVIDIA/nvapi) at `d08488fcc82eef313b0464db37d2955709691e94`, a DXMT submodule | MIT; used to build DXMT's NVAPI and DLSS stand-ins (MetalFX upscaling), which contain no NVIDIA code; the license text ships in the app under `Contents/Resources/Licenses/NVAPI-MIT` |
| Native runtime dependencies | Soju's referenced library stack, FreeType, SDL2, Wine Mono, and their dependencies | License texts for the bundled libraries ship in the app under `Contents/Resources/Licenses` |
| Design reference for THE FINALS' Rosetta hook (`environments/finals/tests/rosetta_nt/`) | [rosettax87_jit](https://github.com/Lifeisawful/rosettax87_jit) by Lifeisawful | MIT, Copyright (c) 2025 Lifeisawful. Studied for its design of hooking Rosetta's per-instruction translator, its entry-hook technique (a 16-byte absolute jump with a copy-on-write page), and the `MAP_TRANSLATED_ALLOW_EXECUTE` flag for running native arm64 code in a translated process. The hook's code, its NT flag correction and its syscall fast path were written for this project. |
| Apple support components and Rosetta | Apple | Proprietary; absent from this repository, bundled in the app under Apple's accompanying license, not covered by the project's Apache 2.0 license or the Wine/DXMT licenses |
| D3DMetal 4.0 beta 3 (GPTK4 evaluation app) | Apple's Evaluation environment for Windows games | The complete original framework and matching `/redist` components are included unmodified for free, non-commercial evaluation on Apple-branded systems. Apple's license and acknowledgements ship under `Contents/Resources/Licenses/apple-gptk4`; these components are not Apache-licensed. Rosetta is provided by macOS, not included in this package. |
| Battle.net and Overwatch | Blizzard | Proprietary; obtained through official channels and absent from this repository |

Upstream patches preserve their applicable terms. Generated tests incorporating
upstream source must retain those terms when distributed. No blanket Apache 2.0 claim
applies to the assembled runtime. Corresponding source, build recipes, notices,
and any required relinking information must accompany future binary distribution
as required by the individual component licenses.

This repository does not grant rights to any third-party trademarks or game
assets. Source availability is distinct from permission to redistribute Apple
components or to market the complete runtime for a particular intended use.
