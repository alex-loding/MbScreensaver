# 🖥️ MbScreensaver

基于 [miniblink](https://github.com/weolar/miniblink49) 内核的 Windows 网页屏保程序，支持将任意网页（本地 HTML 或远程 URL）作为屏幕保护程序运行。

---

## ✨ 功能特性

- 支持标准 Windows 屏保命令行参数：`/s`（运行）、`/c`（设置）、`/p`（预览）
- 支持加载 **http/https 远程 URL** 或 **本地 HTML 文件**
- 全屏无边框展示，高 DPI 支持
- 系统启动时：任意键盘/鼠标操作退出屏保
- 手动双击 `.scr` 启动时：仅 `ESC` 键退出，鼠标不触发退出
- URL 配置持久化保存至注册表
- 内置设置界面，支持浏览选择本地 HTML 文件
- 支持同目录 `screensaver.html` 作为默认页面自动加载

---

## 📦 依赖

| 文件 | 说明 |
|------|------|
| `screensaver.scr` | 编译产出的屏保主程序 |
| `mb132_x64.dll` | miniblink 浏览器引擎（64位） |
| `mb.h` | miniblink 头文件（编译时需要） |

---

## 🔨 编译

使用 MinGW（g++）编译：

```bash
g++ screensaver.cpp -I. -o screensaver.scr -luser32 -lcomdlg32 -mwindows
```

> 需要确保 `mb.h` 在当前目录或 include 路径中。

---

## 🚀 部署

1. 将 `screensaver.scr` 和 `mb132_x64.dll` 复制到 `C:\Windows\System32`
2. 右键 `screensaver.scr` → **安装**，或在"屏幕保护程序设置"中选择 `MbScreensaver`

---

## ⚙️ 使用方式

### 运行屏保

```
screensaver.scr /s
```

全屏显示已配置的网页。由系统启动时，任意输入（键盘/鼠标移动/点击）退出；手动启动时，仅 `ESC` 键退出。

### 打开设置

```
screensaver.scr /c
```

弹出设置窗口，可输入 URL 或浏览选择本地 HTML 文件。

支持的 URL 格式：

- `https://example.com`
- `http://example.com`
- `file:///C:/path/to/file.html`
- 本地路径（自动转换为 `file:///` 格式）

### 预览模式

```
screensaver.scr /p <HWND>
```

嵌入到 Windows 屏保预览窗口中显示（由系统调用）。

### 直接运行（无参数）

```
screensaver.scr
```

全屏展示，仅 `ESC` 键退出，鼠标不触发退出。

---

## 🗂️ 注册表

URL 保存于：

```
HKEY_CURRENT_USER\Software\MbScreensaver
值名: Url
```

---

## 📄 URL 加载优先级

1. 注册表中保存的 URL
2. 编译时指定的 `DEFAULT_URL`（默认为空）
3. 与 `.scr` 同目录的 `screensaver.html`
4. 若均不存在，加载 `about:blank`

---

## 📁 项目结构

```
.
├── screensaver.cpp     # 主程序源码
├── mb.h                # miniblink 头文件
├── mb132_x64.dll       # miniblink 运行时（需自行获取）
└── screensaver.html    # 可选：默认屏保页面
```

---

## 📝 许可

本项目依赖 [miniblink](https://github.com/weolar/miniblink49)，请遵守其相应许可协议。
