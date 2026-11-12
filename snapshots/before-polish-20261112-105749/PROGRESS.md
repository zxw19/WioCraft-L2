# WioCraft on Seeed Wio Tracker L2 Pro — 交接说明（2026-11-12）

## 1. 工程与固件
- 源码：`D:\Mesh_product\L2\WioCraft\src\`（config.h / panel.h / board.cpp / tex.* / world.* / render.* / game.cpp / audio.* / save.* / main.cpp）
- 合并固件：`D:\Mesh_product\L2\WioCraft\WioCraft-merged.bin`
- 设备串口：**COM83**（USB CDC，115200）
- 面板：NV3031B 320x240 QSPI（SPI3_HOST, mode3, 75MHz, CS46, SCLK42, IO0-3=41/40/39/38）
- 触摸：GT911 I2C 0x5D（SDA47/SCL48, 400k），`offset_rotation=2`
- 背光：LP5814 I2C 0x2C；扩展 IO：TCA9535 0x21（LCD_RST/TP_RST/PA_EN 等）
- 编解码：ES8311 + I2S（MCLK10/BCK11/WS12/DOUT16）

## 2. 构建 / 烧录（PowerShell 必须用 cmd /c 包一层）
```
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe -m platformio run -d D:\Mesh_product\L2\WioCraft
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe C:\Users\seeed\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 merge_bin -o WioCraft-merged.bin --flash_mode qio --flash_freq 80m --flash_size 16MB 0x0 .pio\build\wio-l2\bootloader.bin 0x8000 .pio\build\wio-l2\partitions.bin 0x10000 .pio\build\wio-l2\firmware.bin
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe C:\Users\seeed\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port COM83 -b 460800 write_flash 0x0 WioCraft-merged.bin
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe D:\Mesh_product\L2\WioCraft\_read2.py
```
**红线：读串口只能用 `_read2.py` / `_wait.py`（纯读，不设 DTR/RTS）。设 DTR/RTS 会让芯片进下载模式、端口消失。**

## 3. 当前已完成
- 渲染：160x120 内部 → 2x 整数放大到 320x240；天顶/地平线渐变、太阳/月亮、块状云、面朝向明暗、逐格雾化、远处补 1px 消竖缝。
- 颜色问题已解决：`present()` 逐像素字节交换后 `pushImageDMA`（16bpp→16bpp 时 LovyanGFX 不转换，直接按字节发）。
- 世界：112x112x64，y 连续存储 `idx=(x*WORLD_Z+z)*WORLD_H+y`，`world::column()` 整列拷到内部 RAM；地形 fbm + 树；y 上限 40。
- 玩法：走动/跳跃/碰撞、射线拾取、按住挖、点按放、9 格快捷栏、摔落伤害、缓慢回血、死亡保背包回出生点、8 分钟昼夜。
- HUD：9 格快捷栏 + 等距方块图标、红心、右下手持方块（走路摆动）、准星、挖掘进度条、浮动摇杆、Jump 圆钮。
- 音效：过程化合成（脚步/挖掘/破坏/放置/受伤/UI），无 BGM。
- 存档：SD `/wicraft/save.bin`（临时文件 + rename），仅记录编辑增量。
- 性能实测：**27.6 fps | sky 1.2 / world 21.4 / present 12.9 ms | cells 2036 blk 11930 px 15651**
- RAM 22.5% / Flash 16.8%，堆约 142KB。

## 4. 遗留问题（按优先级）
### P0 — 挖树导致重启（最新反馈）
- 现象：靠近树、按住挖掘，设备自动重启。
- 尚未抓到串口崩溃日志。复现方法：先跑 `_read2.py`（或常驻串口记录），再挖树，看重启前最后几行。
- 待验证假设：
  1. 功放瞬时电流导致 brownout（`SFX_BREAK` gain 0.9 是全程最响的事件）。看 `rst:` 原因是否 `BROWN_OUT`。
  2. 音频 mixer 任务（core1, prio3）里调 `board::setAmp()` 走 I2C，与主循环 GT911 触摸读**并发访问同一条 I2C 总线**（Wire 非线程安全），可能触发驱动 assert/panic。
  3. 破坏后 `s_dirty=true`，30s 后 `save::store()` 写 SD 卡时卡死/异常（若插了卡）。
- 对应改法（已规划，未落地）：把功放开关移到主循环（每 80ms 按 `audio::busy()` 设置，带状态缓存）；mixer 任务钉到 core0；`i2s_write` 用 50ms 超时替代 `portMAX_DELAY`；存档只在无触摸时做。

### P1 — 交互手感（用户明确说"交互完全没做好"）
当前实现的问题：
- 转视角过于灵敏：yaw 0.0062 rad/px（全屏 320px ≈ 115°），pitch 0.85 视口像素/px 且限幅 ±78，短拖就顶到极限。
- 触摸按帧判定：任意一帧 `getTouch()==0` 就当作松手 → 长按挖矿被打断，并可能误触发"点按放置"，表现为"功能很奇怪"。
- 快捷栏触摸带 y≥208 与摇杆底部圆（默认圆心 60,176 r36 → 到 212）重叠，摇杆下半区按不动。
- 摇杆按下即把基点瞬移到手指处，且需拖动 >10px 才动，静止按下没有反馈。
- 点按放置无目标面预览，容易放错位置。
- 已写好的改法：按下时锁定区域（BAR/JUMP/JOY/LOOK），区域判定改为 `y<204` 才进摇杆；松手做 2 帧去抖；按下即在快捷栏内滑动选择；摇杆基部半固定、死区 6px、按幅度给速度；yaw 0.0034、pitch 0.26 限 ±34/-30；按住 0.16s 且位移 <12px 才开始挖；挖掘/放置加碎片粒子 + 操作提示文字。

### P2 — 渲染性能
- `world` 21.4ms 是最大头：可预生成 3 档明暗贴图、fog=0 时短路 `mixc`、纹理取样改整型查表。
- `present` 12.9ms 里含 HUD：每帧 10 个立方体走 `texPara`（逐像素浮点除法）≈ 8ms。改法：开机把 9 个快捷栏图标（26x26）和 9 个手持方块（40x46）预渲染成字节交换好的位图，运行时直接 `memcpy` 行。

### P3 — 完成度
- 破坏无碎屑/裂纹（只有进度条）。
- 无物品名提示、无中英文提示层（现无任何文字，5x7 字库和 `drawText` 已规划）。
- 世界边缘可直接走出去掉进虚空（应在 `collides()` 里把世界外当实心）。
- 跳跃无 coyote time（0.09s）。

## 5. 注意事项
- 面板 `invert=true / rgb_order=true` 与 `offset_rotation=1` 已验证正确，**不要再改**。
- 16bpp 下 `pushImage` 不会做字节序转换，必须自己交换。
- `panel->setRotation(0)` 即 320x240（`offset_rotation=1` 已在面板配置里）。
- 参考驱动（已在 L2 验证可用）：`D:\Mesh_product\L2\Meshcore_firmware\MeshCore-L2-LVGL\variants\wio-tracker-l2\WioTrackerL2Display.h`、`src\helpers\ui\LGFXDisplay.cpp`。Meshtastic 没有 NV3031B 驱动，可忽略。
- 版权：不得使用 Mojang 贴图/音效/名称/logo；只做音效不做 BGM；独立固件不集成 Launcher。

## 6. 源码现状
- `game.cpp` 与 `game.cpp.bak` 完全一致（23702 字节，2026/11/12 10:47），即**上次讨论的输入层改造尚未写入**。
- 已烧录固件 = 当前源码 = 用户照片里的这版（9 格快捷栏、红心、摇杆、Jump）。
