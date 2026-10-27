# AGENTS.md —— 给 AI 助手的项目约定

> 新会话开工前先读这个文件 + `docs/00-完全手册.md`，不要重新摸索一遍。

## 一句话

2.9 寸黑白墨水屏（296×128，SSD1680）× ESP32-S3：电脑渲染一页位图，串口推给设备显示。
**已在实物上跑通**，不是纸上方案。

## 当前状态（2026-10-27 实测）

| 项 | 值 |
|---|---|
| 固件环境 | `esp32-s3-29`（2.9 寸 + S3），另 4 个环境只编译不验证 |
| 全刷 / 局刷 | 2262 ms（会闪）/ 948 ms（不闪） |
| 显存 | 4736 字节，屏内 RAM 是**竖存 16 字节 × 296 行** |
| 阅读版式 | 15px / **每页 5 行** / 页眉 13px / 无页脚 / pad 4 / 边框贴边 |
| 样书页数 | `tools/sample_book.txt` = 8 页 |
| 已验功能 | 中文文字、图片抖动、整本书分页、键盘翻页、局刷 |

## 硬性规则

1. **2.9 寸是主线。** 4.2 / 4.26 只保证编译通过，无实物；**不许写“已验证”**。
2. **改排版先出预览图**（`-Preview ... -NoPush`），确认后再烧屏，别拿屏做迭代工具。
3. **没在实物上跑过的，不许写“验证通过”。** 说“编译通过”可以，说“跑通”必须有日志或截图。
4. 改 `tools/push.py` 的**渲染或分页**后，必须把 `CACHE_VER` 加 1，否则旧分页缓存会错位。
5. **PowerShell 脚本一律 ASCII-only**（PS 5.1 按 GBK 读 `.ps1`，中文会解析失败）。
6. 仓库里不许出现：`.venv/`、`.pio/`、`*.pages.json`、`*.log`、大图（`.gitignore` 已覆盖）。
7. 文档和实现不一致时，先改代码再改文档，或者明确说“文档没同步”。

## 常用命令（照抄，别自己拼路径）

```powershell
cd D:\04-Code\08-deepseek-harness\epaper29

# 编译 + 烧写（-Monitor 会顺带开串口）
.\build.ps1 -Target 29 -Board s3 -Upload

# 推一页文字（| 分行）
.\push.ps1 "第一行|第二行|第三行"

# 只看渲染效果，不接硬件
.\push.ps1 -Book .\tools\sample_book.txt -Page 3 -Preview tmp.png -NoPush

# 键盘翻页阅读（n/p/f/s/q）
.\.venv\Scripts\python.exe tools\read.py --book .\tools\sample_book.txt --port COM4

# 抓开机日志（板子一插就复位，用这个抓）
.\.venv\Scripts\python.exe tools\cap_serial.py COM4 40

# 刷厂商官方例程做对照
cd vendor_demo
..\.venv\Scripts\python.exe -m platformio run -e s3-29-vendor -t upload

# 刷回自己的固件
cd ..
.\build.ps1 -Target 29 -Board s3 -Upload
```

## 改什么看哪里

| 要改的东西 | 文件 | 关键函数 / 位置 |
|---|---|---|
| 引脚、屏型、SPI 频率 | `firmware/src/board_config.h` | `PIN_EPD_*`、`EPD_MODEL` |
| 屏驱动（初始化、刷屏、转置） | `firmware/src/epd.cpp` | `e29_init()`、`e29_show()`、`e29_build_mem()`、`e29_partial()` |
| 开机流程、串口协议 | `firmware/src/main.cpp` | `setup()`、`read_full_frame()` |
| 版面（字号、行数、边距） | `tools/push.py` | `layout()`、`SMALL_H/SMALL_PAD/SMALL_HDR_SZ` |
| 页眉、下划线、正文绘制 | `tools/push.py` | `render_page()` |
| 分页、空行处理 | `tools/push.py` | `paginate()`、`wrap_text()` |
| 字体选择（小字号用黑体）、单色 hinting | `tools/push.py` | `_font_cands()`、`paste_text()` |
| 串口打包 / CRC / 握手 | `tools/push.py` | `send_frame()`、`wait_ready()` |
| 键盘翻页 | `tools/read.py` | `render()`、`get_key()` |
| 固件内 5×7 字库 | `firmware/include/font_ascii.h` | 由 `tools/genfont.py` 生成 |

## 已经踩过的坑（别重犯）

| 现象 | 真正原因 | 处理 |
|---|---|---|
| 全刷几毫秒、屏不动 | 初始化缺 `0x12/0x11/0x44/0x45/0x21/0x4E/0x4F`，窗口是 0×0 | 见 `e29_init()`，一条不能少 |
| 耗时正常但屏不变 | 显存几何反了（屏是竖存 16×296，不是 37×128） | `e29_build_mem()` |
| 局刷后画面乱 | 全刷没同时写 `0x26`（旧图），局刷拿垃圾做差分 | `e29_show()` 里 `0x24` + `0x26` 都写 |
| 推完图又变回首页 | 串口 DTR/RTS 把板子复位了 | `push.py` 已处理：放线 + 等 `READY.` |
| 只有 `[ep29] refresh` 没 `OK frame` | 读到的是开机日志，不是帧回复 | 读到 `OK`/`NAK` 才算成功 |
| 中文笔画断裂 | 抗锯齿灰度被阈值吃掉细笔画 | 单色 hinting（`paste_text`）+ 15px + 小字号用黑体 |
| 首行偏下、少显示几行 | 空行也占了一行版面 | `paginate()` 跳过空行 |
| `ERR len=15000 expect=4736` | `--model` 和固件不一致 | `--model 29`（现在默认就是） |

## 工作方式（省 token）

1. **需求带验收标准**：“每页 6 行，字号不变”比“排版不好看”有用得多。
2. **先方案后动手**：用户说“先给方案”时，只列改动点，不写代码。
3. **先预览再烧屏**：排版类改动一律先 `-Preview`。
4. **一次只改一件事**，改完立刻验证，别攒着。
5. 会话很长时，建议用户**新开会话**，开头只说：“读 AGENTS.md，我要做 X”。

## 相关文档

- `docs/00-完全手册.md` —— 唯一详版（采购、接线、原理、扩展、踩坑记录、附录 A/B）
- `docs/archive/90-4.2寸方案.md` —— 早期 4.2 寸方案，归档，非主线
- `vendor_demo/` —— 微雪官方例程（MIT），只作对照
