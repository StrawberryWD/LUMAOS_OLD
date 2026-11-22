# LumaOS 5.1 → 9.0 总路线

| 版本      | 定位     | 核心目标                            |
| ------- | ------ | ------------------------------- |
| **5.1** | 中文桌面   | 中文字体、GB2312、字体引擎                |
| **5.2** | 完整 GUI | GUI 修复、窗口系统、重绘优化                |
| **6.0** | 多媒体桌面  | 图片读取与显示、图片查看器                   |
| **7.0** | 多任务 OS | Process、Scheduler、Timer、IPC     |
| **8.0** | PC OS  | PCI、ACPI、USB、网卡、TCP/IP、声音       |
| **9.0** | 应用平台   | Luma API、窗口 API、程序安装、用户/权限、系统配置 |

---

# LumaOS 5.1

## 中文字体支持

**目标：让 LumaOS 从“英文实验 OS”变成真正能使用中文的桌面系统。**

原计划明确把字体加载完成作为 **5.1** 的版本节点。

### 第 1 周：字体文件与字体引擎

| Day | 任务                                       | 验收               |
| --- | ---------------------------------------- | ---------------- |
| 1   | 将 `asc24.bin`、`hzk24.bin` 放入 ISO `/boot` | VFS 能找到字体文件      |
| 2   | 创建 `kernel/font/font.c/h`                | `font_init()` 正常 |
| 3   | 实现 ASCII 24×24 字体读取                      | 能显示 `A B C`      |
| 4   | 实现 GB2312 汉字读取                           | 能显示“中”           |
| 5   | 实现 `draw_char()`                         | 任意位置绘制字符         |
| 6   | 实现 `draw_string()`                       | ASCII 字符串正常      |
| 7   | 周复盘                                      | ASCII + 中文混合显示   |

字体文件通过 VFS 加载、缺失时回退 8×8，是原计划明确设计的机制。

### 第 2 周：GB2312

| Day | 任务            | 验收            |
| --- | ------------- | ------------- |
| 8   | GB2312 双字节解析  | 正确识别汉字        |
| 9   | ASCII/中文自动切换  | `Hello 你好` 正常 |
| 10  | 中文坐标与宽度处理     | 不重叠           |
| 11  | 未知字符 fallback | 不崩溃           |
| 12  | 中文文件名显示       | `ls` 能显示      |
| 13  | 中文终端输出        | `echo 你好`     |
| 14  | 周复盘           | 中文 Shell      |

### 第 3 周：GUI 字体适配

| Day | 任务           | 验收        |
| --- | ------------ | --------- |
| 15  | 菜单栏中文        | “程序/设置”   |
| 16  | 窗口标题中文       | “终端/设置”   |
| 17  | Settings 中文化 | 设置页面完整    |
| 18  | Info 中文化     | About 页面  |
| 19  | Terminal 中文  | 中文命令输出    |
| 20  | 桌面图标文字       | 中文名称      |
| 21  | 集成测试         | 全 GUI 无乱码 |

### 第 4 周：字体性能

| Day | 任务                | 验收              |
| --- | ----------------- | --------------- |
| 22  | 字形缓存              | 重复字符不重复解析       |
| 23  | 字符串缓存优化           | 长文本不卡           |
| 24  | 字体边界测试            | 各类 GB2312 字符    |
| 25  | 8×8 fallback      | 字体加载失败仍能启动      |
| 26  | 中文文件系统测试          | FAT/LMFS 均正常    |
| 27  | 全面回归              | GUI/Terminal 正常 |
| 28  | **发布 LumaOS 5.1** | 中文桌面 ISO        |

---

# LumaOS 5.2

# GUI 稳定与性能版本

这一版专门解决：

> **“能显示” → “真正好用”**

原计划已经设计了脏矩形、裁剪、局部刷新、字形缓存。

### 第 1 周：窗口系统

| Day | 任务                |
| --- | ----------------- |
| 1   | 重构 Window Manager |
| 2   | 窗口 Z-order        |
| 3   | 窗口激活/失活           |
| 4   | 窗口移动              |
| 5   | 窗口缩放              |
| 6   | 最小化/最大化           |
| 7   | 多窗口测试             |

### 第 2 周：重绘

| Day | 任务              |
| --- | --------------- |
| 8   | `dirty_rects[]` |
| 9   | 窗口脏区域记录         |
| 10  | 裁剪算法            |
| 11  | 局部 `vga_flip()` |
| 12  | 暴露区域重绘          |
| 13  | 光标局部刷新          |
| 14  | FPS/性能测试        |

### 第 3 周：GUI 控件

| Day | 任务        |
| --- | --------- |
| 15  | Button    |
| 16  | Label     |
| 17  | TextBox   |
| 18  | ListBox   |
| 19  | ScrollBar |
| 20  | Menu      |
| 21  | Dialog    |

### 第 4 周：桌面体验

| Day | 任务                |
| --- | ----------------- |
| 22  | 桌面图标              |
| 23  | 任务栏               |
| 24  | 开始菜单              |
| 25  | Settings          |
| 26  | Clipboard         |
| 27  | GUI 压力测试          |
| 28  | **发布 LumaOS 5.2** |

### 5.2 验收

最终应该做到：

```text
桌面
 ├── 开始菜单
 ├── 任务栏
 ├── Terminal
 ├── Settings
 ├── Info
 └── 多个窗口
```

并且窗口移动、重叠、缩放不会出现明显残影或闪烁。原计划也明确把这些作为测试项目。

---

# LumaOS 6.0

# 图片与多媒体基础

**版本目标：让 LumaOS 第一次拥有“真正的桌面应用”。**

核心：

```text
文件
 ↓
图片解码
 ↓
Framebuffer
 ↓
GUI
 ↓
Image Viewer
```

### 第 1 周：图片基础

| Day | 任务              |
| --- | --------------- |
| 1   | 设计 `Image` 数据结构 |
| 2   | 从 VFS 读取图片      |
| 3   | BMP Header      |
| 4   | BMP 24-bit      |
| 5   | BMP 32-bit      |
| 6   | 行对齐处理           |
| 7   | BMP 测试          |

**第一阶段建议只做 BMP。**

不要一上来 PNG/JPEG。

---

### 第 2 周：图片渲染

| Day | 任务                |
| --- | ----------------- |
| 8   | RGB → framebuffer |
| 9   | 图片绘制 API          |
| 10  | 图片缩放              |
| 11  | 图片裁剪              |
| 12  | 居中显示              |
| 13  | 大图片性能优化           |
| 14  | 图片渲染测试            |

---

### 第 3 周：Image Viewer

| Day | 任务              |
| --- | --------------- |
| 15  | 创建 Image Viewer |
| 16  | Open 图片         |
| 17  | 下一张/上一张         |
| 18  | 缩放              |
| 19  | 窗口滚动            |
| 20  | 图片错误处理          |
| 21  | 多图片测试           |

---

### 第 4 周：完善

| Day | 任务                |
| --- | ----------------- |
| 22  | 文件管理器基础           |
| 23  | 双击图片打开            |
| 24  | 文件关联              |
| 25  | 图片缩略图             |
| 26  | BMP 大文件压力测试       |
| 27  | GUI + 图片回归测试      |
| 28  | **发布 LumaOS 6.0** |

### 6.0 的标志性 Demo

启动后：

```text
桌面
  ↓
文件管理器
  ↓
双击 test.bmp
  ↓
Image Viewer
  ↓
显示图片
```

**到这里，我会认为 LumaOS 已经真正进入“桌面 OS”阶段。**

---

# LumaOS 7.0

# 真正的多任务操作系统

这一版是整个路线中非常重要的一次升级。

之前：

```text
程序 A
 ↓
退出
 ↓
程序 B
```

7.0：

```text
Terminal ──────┐
Image Viewer ──┼──→ Scheduler
File Manager ──┤
Settings ──────┘
```

---

## 第 1 周：Process

| Day | 任务           |
| --- | ------------ |
| 1   | `process_t`  |
| 2   | PID          |
| 3   | Process 状态   |
| 4   | Process 创建   |
| 5   | Process 销毁   |
| 6   | Process list |
| 7   | `ps`         |

---

## 第 2 周：Timer + Scheduler

| Day | 任务         |
| --- | ---------- |
| 8   | PIT 初始化    |
| 9   | Timer IRQ  |
| 10  | Tick       |
| 11  | Context 保存 |
| 12  | Context 恢复 |
| 13  | Scheduler  |
| 14  | 第一次任务切换    |

目标：

```text
A → B → A → B
```

---

## 第 3 周：抢占式多任务

| Day | 任务                    |
| --- | --------------------- |
| 15  | 时间片                   |
| 16  | Round Robin           |
| 17  | 用户态进程切换               |
| 18  | Kernel stack          |
| 19  | Process address space |
| 20  | `sleep()`             |
| 21  | 多任务压力测试               |

最终：

```text
Terminal
Image Viewer
Shell
```

可以同时存在。

---

## 第 4 周：IPC + Process API

| Day | 任务                |
| --- | ----------------- |
| 22  | `fork()` 完善       |
| 23  | `execve()` 完善     |
| 24  | `wait()`          |
| 25  | Pipe              |
| 26  | IPC 基础            |
| 27  | Process Manager   |
| 28  | **发布 LumaOS 7.0** |

### 7.0 验收

```text
$ ps

PID   NAME
1     shell
2     terminal
3     imageview
4     settings
```

这时候 LumaOS 才真正拥有：

> **现代意义上的多任务桌面 OS。**

---

# LumaOS 8.0

# PC 硬件 + 网络 + 多媒体

这是最大的一版。

目标：

> **让 LumaOS 不再只依赖 QEMU 给你的几块虚拟设备。**

---

# 8.0-A：PCI

### 第 1 周

| Day | 任务           |
| --- | ------------ |
| 1   | PCI 配置空间     |
| 2   | PCI Bus Scan |
| 3   | Vendor ID    |
| 4   | Device ID    |
| 5   | Class Code   |
| 6   | BAR          |
| 7   | PCI 设备列表     |

最终：

```text
$ lspci

00:01.0 VGA
00:02.0 IDE
00:03.0 Network
00:04.0 Audio
```

---

# 8.0-B：ACPI / Timer / 硬件基础

| Day | 任务        |
| --- | --------- |
| 8   | ACPI RSDP |
| 9   | RSDT/XSDT |
| 10  | MADT      |
| 11  | CPU 信息    |
| 12  | APIC 基础   |
| 13  | IRQ 重构    |
| 14  | 硬件初始化框架   |

---

# 8.0-C：网络

| Day | 任务             |
| --- | -------------- |
| 15  | 网卡驱动接口         |
| 16  | Ethernet Frame |
| 17  | ARP            |
| 18  | IPv4           |
| 19  | ICMP           |
| 20  | UDP            |
| 21  | `ping`         |

然后：

| Day | 任务                   |
| --- | -------------------- |
| 22  | TCP 基础               |
| 23  | TCP Connect          |
| 24  | TCP Send/Receive     |
| 25  | Socket API           |
| 26  | DNS                  |
| 27  | HTTP 基础              |
| 28  | **LumaOS 8.0 网络版发布** |

最终：

```text
$ ping 1.1.1.1
64 bytes from 1.1.1.1
```

以及：

```text
$ curl http://...
```

---

# 8.1 / 后续补丁可以做 USB + Audio

这里我建议**不要硬塞进 8.0 首发**。

8.0 先完成：

```text
PCI
ACPI
Ethernet
TCP/IP
DNS
HTTP
```

然后 8.1：

```text
USB
USB HID
USB Storage
Audio
HDA/AC97
```

这样风险小很多。

---

# LumaOS 9.0

# 应用平台

这是整个路线最终的“大结”。

前面 5～8 做的是：

> **操作系统**

9.0 开始做：

> **平台。**

---

# 9.0-A：Luma API

### 第 1 周

建立统一 API：

```c
luma_window_create();
luma_window_destroy();

luma_draw_text();
luma_draw_rect();
luma_draw_image();

luma_file_open();
luma_file_read();

luma_process_create();
```

形成：

```text
Applications
      ↓
   Luma API
      ↓
   libc / syscall
      ↓
     Kernel
```

---

# 9.0-B：GUI Application Framework

### 第 2 周

| Day | 任务               |
| --- | ---------------- |
| 8   | Application 生命周期 |
| 9   | Window API       |
| 10  | Event API        |
| 11  | Button API       |
| 12  | TextBox API      |
| 13  | Menu API         |
| 14  | Dialog API       |

让普通程序可以这样写：

```c
int main() {
    Window *w = window_create("Hello");

    button_create(w, "OK");

    window_show(w);

    event_loop();

    return 0;
}
```

**这一步非常重要。**

因为以后你不需要修改 Kernel 才能做一个新 GUI 程序。

---

# 9.0-C：系统应用

### 第 3 周

正式做：

```text
/bin
 ├── terminal
 ├── filemgr
 ├── imageview
 ├── settings
 ├── calculator
 ├── editor
 └── about
```

重点：

### File Manager

实现：

```text
双击
打开
复制
移动
删除
新建文件夹
重命名
属性
```

### Text Editor

至少：

```text
新建
打开
保存
另存为
中文
剪切
复制
粘贴
```

---

# 9.0-D：系统级体验

### 第 4 周

| Day | 任务                  |
| --- | ------------------- |
| 22  | 用户账户                |
| 23  | 文件权限                |
| 24  | 系统配置                |
| 25  | Registry-like 配置数据库 |
| 26  | 程序安装机制              |
| 27  | 系统恢复/安全启动基础         |
| 28  | **LumaOS 9.0 发布**   |

最终：

```text
LumaOS
│
├── Desktop
│
├── Start Menu
│
├── Taskbar
│
├── File Manager
│
├── Terminal
│
├── Image Viewer
│
├── Text Editor
│
├── Settings
│
├── Calculator
│
└── Application API
```

---

# 最终完整版本树

```text
LumaOS 5.0
│
├── Kernel
├── VFS
├── FAT12/16/32
├── LMFS
├── ELF
├── syscall
├── paging
├── Ring 3
├── libc
└── Basic GUI
        │
        ▼
LumaOS 5.1
│
└── 中文字体
    ├── GB2312
    ├── 24×24
    ├── 字体缓存
    └── 中文 GUI
        │
        ▼
LumaOS 5.2
│
└── GUI 完善
    ├── Window Manager
    ├── Menu
    ├── Button
    ├── Taskbar
    ├── Clipboard
    ├── Dirty Rectangle
    └── GUI 优化
        │
        ▼
LumaOS 6.0
│
└── 图片
    ├── BMP
    ├── Image API
    ├── Image Viewer
    ├── 缩放
    └── 文件关联
        │
        ▼
LumaOS 7.0
│
└── 多任务
    ├── Process
    ├── PID
    ├── Timer
    ├── Scheduler
    ├── Context Switch
    ├── Preemptive
    ├── fork
    ├── exec
    └── IPC
        │
        ▼
LumaOS 8.0
│
└── PC OS
    ├── PCI
    ├── ACPI
    ├── APIC
    ├── Network
    ├── Ethernet
    ├── ARP
    ├── IPv4
    ├── ICMP
    ├── UDP
    ├── TCP
    ├── DNS
    └── HTTP
        │
        ▼
LumaOS 9.0
│
└── Application Platform
    ├── Luma API
    ├── GUI API
    ├── Application Framework
    ├── File Manager
    ├── Text Editor
    ├── Image Viewer
    ├── Settings
    ├── Users
    ├── Permissions
    ├── Configuration
    └── Application Installer
```

---

# “版本定位”最终就这样定！

| 版本      | 一句话定义  | 达成后感觉              |
| ------- | ------ | ------------------ |
| **5.0** | 基础 OS  | “我做出了一个 OS”        |
| **5.1** | 中文 OS  | “中国人终于能用了”         |
| **5.2** | 桌面 OS  | “像一个真正的桌面”         |
| **6.0** | 多媒体桌面  | “开始像 Windows 了”    |
| **7.0** | 多任务 OS | **“这真的是操作系统”**     |
| **8.0** | PC OS  | **“它能用真实 PC 硬件了”** |
| **9.0** | 应用平台   | **“别人可以给它开发软件了”**  |

而且这个安排和你原来的 5.0 基础路线是连得上的：原计划最后阶段本来就已经有 `README/BUILD`、根文件系统、ISO 发布和用户态程序整理，所以不需要推倒重来。

**把 7.0 定成一个大版本节点。**
5.x/6.x 是“把现有 OS 做完整”，**7.0 的 Scheduler + Timer + Process + 抢占式多任务**是架构上的第二次飞跃；8.0 才是从“实验 OS”进入“PC OS”；9.0 则是从“PC OS”进入“平台”。