# warsawPkg —— Mu-Silicium 平台包脚手架（REDMI K90 Ultra / warsaw，SM8750）

状态：**脚手架，未构建、未上机**（2026-11-13 落仓）。本目录是把
`iola1999/hackios-mu-silicium@k90-port`（钉版 `90721af9041bcf8683d12583cc12ba4e4658a48b`）
的 `Platforms/Xiaomi/annibalePkg` 逐文件适配到 warsaw 的起点，不是能直接产出
`SILICIUM_UEFI.fd` 的成品。适配的逐项出处与分类见 fork 目标文档 PHASE4-ALIGN.md。

## 来源类别标记（各文件内沿用）

- **[S1 自有材料推导]**：值来自 warsaw 自有材料——`common/devices/warsaw.conf`、
  `common/devices/warsaw.dtb`（真机 `/sys/firmware/fdt`，SHA-256 `6296d0d6…576a`）。
- **[S2 暂借 annibale]**：同 SoC（SM8750）平台惯例值，从 annibalePkg 暂借，
  逻辑上应同，但**未在 warsaw 真机上核过**。
- **[S3 无从得知]**：现有材料推不出，须真机/原厂材料补齐；脚手架中以占位
  形式出现并显式标注，**不得当真值用**。

## 用法（将来接 FD 构建时的形态，本批未接 job）

把本目录整体覆盖到 Mu 树钉版点的 `Platforms/Xiaomi/warsawPkg/`，另：

1. `warsaw.toml` 放到 Mu 树的 `Resources/Configs/warsaw.toml`；
2. `common/devices/warsaw.dtb` 复制为 `warsawPkg/FdtBlob/warsaw.dtb`
   （见 `FdtBlob/Source.txt`）；
3. `Binaries/warsaw/` 预置二进制（QcomPkg 驱动、SecParti.cfg 等）目前在
   公开 Device-Binaries 仓中**不存在**——`Include/*.inc` 已按该路径引用，
   这是 FD 构建的硬前置，未解决前构建必然在 DXE/RAW 段失败；
4. 然后以 Mu 树的 `build_uefi.py -d warsaw`（或 stuart + DeviceBuild.py）构建，
   FD_BASE/FD_SIZE 由 warsaw.toml 给出（0xA7100000 / 0x300000，[S1]），
   产物应恰为 3,145,728 B——与 build-bootimg 的投放校验一致。

## 红线

- 本目录**不含任何现成 FD 或 annibale 的二进制**；`Binaries/warsaw/` 只许
  以 warsaw 自有来源补齐，不许把 annibale 的 FD 改名充数。
- [S3] 项（SMBIOS 板型/SKU、面板 XML 等）补齐前，本包产出
  的任何 FD 都不得声称可上机。（2026-11-13 第六批：PIL_Reserved 与 RSRV0
  已由 [S3] 改注 [S2]、XNU_Payload 长度疑点已裁决消解，见 fork 的
  PHASE6-OPEN-ITEMS.md C1/C2；面板型号已由 warsaw 真机 DT 钉定为
  qcom,mdss_dsi_p12x_42_02_0a_dsc_cmd，缺的只是 XML 文件本身。第七批：
  DBI_Dump 经 SM8750 三机型横向采样改注 [S2]，XBL_Ramdump 雕刻长对齐
  0x1C0000，见 fork 的 PHASE7-CROSSMODEL.md 任务 B。）
