# v35 — iOS 26 桌面第一次真正上屏（2026-11-07）

## 一句话

Redmi K90 至尊版（`warsaw`, SM8750-AB）上，XNU 12377 / iOS 26.6.2 的 **SpringBoard 进程（pid 211）
跑起来了，并且真的把界面合成到了屏幕上**：状态栏时间、电池百分比、底部 home 指示条、WebKit 报错页
都是 GPU 合成后的真实像素，不是内核控制台。

![v35 screen](evidence/v35/screen-300.png)

## 证据

抓屏（loader `oem screenshot`，1280×2772 BGRA，300 s）：

- 状态栏 `14:50`、右侧电量 `99%`
- 前景 `无法连接 / 无法载入页面。请重试。 / 重试` —— WebKit 的网页加载失败页（没有网络栈，符合预期）
- 底部 home 指示条

日志（`evidence/v35/v35-log-tail.txt`，取自 loader 实时 `oem log` 的环形缓冲）：

```
SpringBoard[211:...] ...                      # 5662 行
backboardd[209:2267] [TouchEvents] contact 1 presence: touching
backboardd[209:2267] [TouchEvents] Touch entered (insideExclusive) <BKSHitTestRegion: {375, 812}>
lsd[50:353] [registration] Registering extension point: ...
systrace: input26-service[212] open("/dev/redwood-sensors") = 2
```

即：SpringBoard 起了、backboardd 在分发触摸、lsd 在扫扩展点、input26-service 在跑。

## 启动参数（v35，实机验证）

```
debug=0x10e rootdev=md0 launchd_unsecure_cache=1 panic_on_crash_disabled=1
cpus=7 bootcpu=7 small_cores_after=0 epoch=...
adreno8=23 adreno8_pause=0 adreno8_fine=0 adreno8_watch=0 adreno8_keep=1 adreno8_return=0
adreno8_speed=14 adreno8_secure_preflight=4
touchscreen=2 buttons=1 battery=1 gpu_coherent=1 metal=1
fb_threads=2 fb_flip=2
applog=2 systrace=1
loader_return_after=320
```

- `bootcpu=7 cpus=7`：loader 留在核 0 服务 USB，内核跑在核 7，**内核运行期间 `oem log` 一直可读**。
- `adreno8=23` + `metal=1`：23 个上电阶段全过，MDSS/DPU/DSI 帧缓冲在翻转。
- `loader_return_after=320`：**这是自设的取证参数**，到点 loader 收回控制权并停机 ——
  用户看到的"卡死"就是它，不是崩溃。
- 无 `cpufreq=1`：CPU 停在引导器留下的低频，所以滑动卡。

## 三个现象对应的根因

### 1. 卡死 = `loader_return_after=320`

内核日志里直接可见：

```
--- loader_return_after expired ---
psci: cpu 2 ends the system: the return to the boot loader is the boot processor's to make
```

到 320 s loader 接管，内核停止运行。删掉该参数即可长期在线（v36 已改）。

### 2. 滑动卡 = 没开 `cpufreq=1` + `sleepd` 缺失导致 SpringBoard 死循环重连

- `redwood_cpufreq.c`：`cpufreq_wanted()` 只认 boot-arg `cpufreq`，没有它就不调 `raise_domain()`，
  核心停在低频。生产参数里本来就有 `cpufreq=1`。
- SpringBoard 的 `HKSPSleepStore` 找不到 `com.apple.sleepd.sleepserver`，在 1 秒内重试了 1416 行日志：

```
[client] [HKSPSleepStore.SpringBoard.0x105707380] failed to connect: ... 4099 ...
"com.apple.sleepd.sleepserver was invalidated: Connection init failed at lookup with error 3
 - No such process."
```

  `sleepd` 的程序和 plist 都在（`/System/Library/PrivateFrameworks/SleepDaemon.framework/sleepd`、
  `/System/Library/LaunchDaemons/com.apple.sleepd.plist`），但**手写的 launchd job 缓存里没有它**
  （`userland/mkcache.py` 只收 27 个 label）。

### 3. WiFi / 蓝牙没驱动 = 驱动编进去了，但启动参数没开、固件不在小镜像里

- 内核里两个驱动都在（payload 里能找到字符串）：

| 驱动 | 文件 | 启动参数 |
|---|---|---|
| Wi-Fi（ath12k 移植） | `bsd/dev/redwood_wifi.c` | `wifi=1`（或 `wifi_step=N`） |
| 蓝牙 | `bsd/dev/redwood_bt_hci.c` / `mhi_pcie.c` | `pcie=N` + `bt=4` |

- v35 的 cmdline **一个都没带**，所以两家都没上电。
- Wi-Fi 还缺固件：驱动要 `fw/wpss.mdt` + `wpss.b02..09`（TrustZone 那种 4 KB MDT 头 + 分段文件）。
  本机 modemfirmware 分区（`/vendor/firmware_mnt/image`）里是 `kiwi/`、`peach/` 两套**扁平**
  `amss.bin`/`amss20.bin` + `bdwlan.e*` 板数据，**没有 MDT**；`/vendor/firmware_mnt/image` 下
  也没有 `wpss*`。所以 `wifi=1` 目前只会打印
  `wifi: the radio's firmware is not in the small image (fw-wifi/, see the README): no Wi-Fi`。
- 蓝牙需要先跑 PCIe（`pcie=10` 系列，含 `pcie_bdf=`、`pcie_power=` 等），是独立一轮的活。

## 产物（留在本地，未上传）

| 文件 | 大小 | sha256 |
|---|---|---|
| `payload-v35.bin` | 24 MiB | `57c6bb559d2154d272367a866b39a88d5256bd96e1122ce70635d1230453c55d` |
| `Mu-warsaw-xnu-k10.img` | 11 MiB | `68d662fafab4473a50ac32500b687f5469c3b8a19a9238fd242abd0951ce5176` |
| `system26.apfs` | 5.5 GiB | 本地构建，含 Apple 系统内容，**不入库** |

内核 `20375224 B`，Darwin 25.6.0（XNU 12377.121.6），GitHub Actions `macos-15` 编译。

## 下一步（v36）

1. 去掉 `loader_return_after` → 不再自动停机。
2. 加 `cpufreq=1`、`fb_beat=0 fb_hz=120 fb_ca_hz=120`、`touch_lat=1 touch_poll_us=4000` → 提帧率与触控跟手。
3. 去 `systrace=1`（每个系统调用都往 console 打，本身拖慢）。
4. 后续：把 `com.apple.sleepd` 加进 job 缓存（重建系统盘）止住重连风暴；
   再单独一轮 `pcie=10 bt=4` 试蓝牙；Wi-Fi 需要给 `redwood_wifi.c` 加扁平 `amss20.bin` 的加载路径。
