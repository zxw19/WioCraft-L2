# WioCraft / Wio Tracker L2 Pro — 当前交接（2026-11-12）

## 当前真实状态
- 最新 **v0.3** 已构建、合并并烧录 COM83，esptool Flash hash验证通过。
- 源码 `D:\Mesh_product\L2\WioCraft\src`；固件 `WioCraft-merged.bin`（606144 bytes）。工程不是Git仓库。
- SHA256: `C8BD1A809D4A82D03583CA9E520A79286B3DA99F1327BD688499F090FF242B2E`。
- RAM 30.5%（99968 bytes），Flash 17.2%（540245 bytes）。
- 真实设备出生点密林 **21.7–22 fps**：sky .8 / world 32 / present 11.7–12.2 ms。
- 抓帧 `logs\release-v03-frame.png`；当前日志 `logs\release-v03-serial.log`。
- 当前设备 **SD off**；真实SD卡持久化仍需插卡验证。
- 用户提出“一格坑跳不出来”，代码已修复、回归通过并刷入；仍待用户验证最新固件实操。

## 本轮实际完成
- `input.cpp/h`：最多5指ID追踪、按下锁区、2次漏报且至少55ms才松手。
- 左下固定摇杆56,168 R32、死区6px；空白拖视角yaw .0034 rad/px、pitch .26viewpx/px。
- 独立DIG(290,77 R22)、PLACE(290,127 R22)、JUMP(280,179 R24)、顶栏CRAFT与帮助。
- 空白长按可挖，空白短点不再放置；快捷栏滑动选择。
- 9格物品数量、挖掘入包、放置消耗、1原木→4木板、拾取/空包提示、目标名、粒子和挥手；HUD图标启动缓存。
- 新世界初始16草/32土/16石；旧WC10档迁移库存0，需要挖掘收集。
- 世界边界碰撞、防穿地、跳跃缓冲/coyote time。
- **一格坑修复**：按帧扣重力在100ms帧下跳高约.84格；现在移动+垂直按1/120秒小步，16/33/100ms回归跳高1.17格，能出一格坑。长按跳跃落地后继续跳，2格以上需要垫块。
- 俯仰+68/-76，低增益不变；之前+34/-30会使5格射程看不到脚边平地，已放宽。
- 原列渲染漏树下地面/底面/放置遮挡：已替换为准确六面表面跨度，所有暴露顶面/底面与入口侧面按最近深度遮挡，含玩家当前所在列。
- 与完整3D DDA参考100视角192万像素比较，仅5个纹理量化边缘像素差异。
- occupancy位图、光照缓存、遮挡裁剪、快速floor/ceil优化；保留160x120→2x显示。
- waitDMA后才修改发送缓冲；显示参数和RGB565换字节未改变。
- 音频mixer core0 queue独占voice、不再访问Wire；主循环service管理功放；50ms I2S超时；修复半填缓冲/错误音效时间单位，降低峰值加起音包络。
- save::store返回bool、成功才清dirty；仅无触摸且音频空闲时保存。WC11库存，兼容WC10。
- 完整校验后才应用；截断/NaN/非法方块拒绝，备份恢复和写/rename失败保旧档；重复同坐标编辑合并，8192上限不再默默丢失。
- 注意 `-ffast-math`：finiteFloat必须const float&按位检查，按值参数的NaN校验会被优化掉。已修复并回归。
- RTC diag记录reset原始原因、上次阶段、运行时长、帧数、栈余量。

## 复位调查与串口红线（已更新）
- 本轮抓到 `esp_reset_reason=0`，**raw=21/21 = USB UART reset**；没有panic/backtrace/brownout证据。
- pySerial打开端口会调用SetCommState，可能触碰控制线，即使未显式setDTR/setRTS。旧_read2.py/_wait.py不能再当绝对安全工具。
- 以后读/截图只用 **usb_port.py**：Win32 CreateFile + ReadFile/WriteFile + 超时，不调用SetCommState或任何控制线API。
- 新工具多次打开读取，uptime连续、boot不变；不要使用_boot.py。
- esptool刷机需要reset，刷机后的raw21和boot增加属预期。不能把它当游戏崩溃。
- 其他游戏重启不能声称全部排除；新日志若有raw15/panic/watchdog再按实际证据定位。

## 操作与构建
在项目目录执行；PIO只能独占运行，禁止多个代理并发构建同一.pio。
```powershell
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe -m platformio run -d D:\Mesh_product\L2\WioCraft -j 4
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe C:\Users\seeed\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 merge_bin -o WioCraft-merged.bin --flash_mode qio --flash_freq 80m --flash_size 16MB 0x0 .pio\build\wio-l2\bootloader.bin 0x8000 .pio\build\wio-l2\partitions.bin 0x10000 .pio\build\wio-l2\firmware.bin
cmd /c C:\Users\seeed\.platformio\penv\Scripts\python.exe C:\Users\seeed\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port COM83 -b 460800 write_flash 0x0 WioCraft-merged.bin
python serial_capture.py --seconds 900 --log logs\session.log
python capture_frame.py --out logs\device.png
```
串口读取与截图不能同时占COM83。系统Python有Pillow，PIO Python没有。USB命令I=状态/健康、P=RLE帧；不改变控制线。

## 测试
- `node tests\run-wasm-tests.cjs` 输入锁区/多指/去抖、放置遮挡、10000次同位置编辑。
- `node tests\compare-render.cjs` 准确跨度对3D DDA参考100视角192万像素。
- `python tests\run-physics-tests.py` 从实际game.cpp提取物理，16/33/100ms一格坑跳出、两格阻挡、低顶碰撞。
- `python tests\run-save-tests.py` 真实save.cpp FS mock，9项库存/迁移/损坏/备份/失败回滚/8192上限通过。
- 主机WASM编译器 `D:\android-sdk\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe`；不影响ESP构建。

## 不要改变的面板参数
- NV3031B SPI3 mode3 75MHz CS46 SCLK42 IO41/40/39/38。
- invert=true/rgb_order=true/offset_rotation=1；setRotation(0)320x240。
- GT911 offset_rotation=2、0x5D、SDA47/SCL48。
- 16bpp pushImageDMA不自动转换字节序；g_out始终交换后像素。

## 边界与待验证
- 这是挖掘/收集/木板合成/搭建沙盒；未实现Minecraft完整工具/工作台/熔炉/生物/洞穴/无限世界/透明混合。玻璃当前仍实体遮挡。
- 插真实SD卡后写入/重启恢复验证未做；电池/长时间压力测试、最新用户一格坑反馈待确认。
- 英文像素HUD，暂未中文字体。原版源码/固件及旧进度已保存在snapshots。
