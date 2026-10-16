# Windows 部署指南（已有游戏压缩包）

本指南适合手里已经有 **Bloodborne 1.09 游戏本体压缩包（.zip）** 的玩家，例如从自己网盘下载的备份。按顺序做完下面的步骤，一条命令就能完成全部部署：自动安装编译环境、下载并编译 bbport、解压游戏、检查能否启动。之后双击一个文件就能开始游戏。

> 本仓库不提供任何游戏文件。压缩包必须来自你自己的游戏备份，不要公开分享。

文中用 `<部署目录>` 表示**你自己选择的文件夹**。命令里使用的都是相对路径，只要文件按要求放好，命令可以原样复制运行，不需要修改。

---

## 0. 开始前确认

| 项目 | 要求 |
|---|---|
| 系统 | Windows 10（1803 或更新）或 Windows 11，64 位 |
| 显卡 | 支持 Vulkan 1.3 的独立显卡，**驱动已安装**（NVIDIA / AMD 官网驱动即可） |
| 游戏压缩包 | `.zip` 格式，游戏版本 **1.09**（已合并 1.09 补丁），解压后能看到 `eboot.bin`、`sce_module`、`sce_sys`、`dvdroot_ps4` |
| 磁盘空间 | 约 **压缩包大小 × 2 + 10 GB**（压缩包本身 + 解压后的游戏 + 编译环境和程序）；部署完成后压缩包可以删除 |
| 网络 | 能访问 GitHub 和 MSYS2 软件源（repo.msys2.org）；第一次部署要下载约 0.6 GB |
| 时间 | 第一次约 10–20 分钟（主要看网速），之后更新只需几分钟 |

不需要管理员权限，也不需要事先安装 Git、Python 或编译器，脚本会自己准备。

---

## 1. 新建部署目录

在空间足够的磁盘上新建一个**空文件夹**，这就是 `<部署目录>`。之后所有东西都会放在这个文件夹里。

文件夹的完整路径必须满足：

- **只用英文字母、数字、`-`、`_`**：不能有空格，不能有中文，也不能有 `& % ! ' "` 等符号。不符合时脚本会报错并停下：`must be a path without spaces, quotes, special or non-ASCII characters`。
- **不要放在桌面、文档等被 OneDrive 同步的位置**。几十 GB 的文件会被 OneDrive 自动上传。建议直接放在某个盘的根目录下，例如在 D 盘根目录新建一个英文名的文件夹。

> 建议先在资源管理器里打开"查看 → 显示 → 文件扩展名"。否则重命名时容易把 `game.zip` 写成 `game.zip.zip`，或把脚本存成 `deploy_windows.ps1.txt`。

---

## 2. 放入部署脚本

需要的是 `deploy_windows.ps1` 这一个文件，任选一种方式下载：

- **方式 A（推荐）**：先完成第 4 步打开 PowerShell，再在 `<部署目录>` 里运行：

  ```powershell
  curl.exe -L -o deploy_windows.ps1 https://raw.githubusercontent.com/in0brain/bloodborne_pc/windows-port/tools/deploy_windows.ps1
  ```

- **方式 B**：在浏览器打开 <https://github.com/in0brain/bloodborne_pc/blob/windows-port/tools/deploy_windows.ps1>，点击右上方的下载按钮（Download raw file），把文件保存或移动到 `<部署目录>`。

保存后，文件名必须正好是 `deploy_windows.ps1`。

---

## 3. 放入游戏压缩包

把游戏压缩包复制或移动到 `<部署目录>`，然后**改名为 `game.zip`**。

压缩包里的结构，下面两种都可以，脚本会自动识别：

```
game.zip                        game.zip
├─ eboot.bin                    └─ 任意一个文件夹\
├─ sce_module\                     ├─ eboot.bin
├─ sce_sys\                        ├─ sce_module\
├─ dvdroot_ps4\                    ├─ sce_sys\
└─ ...                             └─ dvdroot_ps4\ ...
```

现在 `<部署目录>` 里应该正好是这两个文件：

```
<部署目录>\
├─ deploy_windows.ps1
└─ game.zip
```

> 如果你的游戏是 `.7z`、`.rar` 或分卷压缩包，或者已经解压好了，请看后面的[已经解压好的游戏](#已经解压好的游戏)。

---

## 4. 在部署目录里打开 PowerShell

任选一种方式：

- 在资源管理器中打开 `<部署目录>`，点一下上方的**地址栏**，输入 `powershell`，按回车。
- Windows 11：在 `<部署目录>` 的空白处点右键，选择"在终端中打开"。

打开的窗口里，提示符应该以你的 `<部署目录>` 路径结尾，例如 `PS X:\你的文件夹>`。如果不是，说明打开的位置不对，请关掉重来。

---

## 5. 运行部署命令

把下面这条命令**原样**复制到 PowerShell 窗口，按回车：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\deploy_windows.ps1 -InstallDir .\bbport -GameZip .\game.zip -Msys2Dir .\msys64 -SmokeTest
```

各个参数的含义：

| 参数 | 含义 |
|---|---|
| `-ExecutionPolicy Bypass` | 只对这一次运行放行脚本，**不会**修改系统设置 |
| `-InstallDir .\bbport` | bbport 的程序、存档、设置和日志放在 `<部署目录>\bbport` |
| `-GameZip .\game.zip` | 游戏压缩包。它会被解压到 `<部署目录>\game`（文件夹名取自压缩包名） |
| `-Msys2Dir .\msys64` | 编译环境（MSYS2）装在 `<部署目录>\msys64`，不影响电脑上其他东西，删除文件夹就能清理干净 |
| `-SmokeTest` | 部署完成后自动启动一次游戏，确认能正常运行（可以省略） |

**可选：使用作者的键鼠布局**：在命令末尾加上 `-Settings configs\keymap-in0brain.ini`。这套布局是按亚洲版（CUSA03023，○ 确认、× 闪避）设计的；美版、欧版用户请在游戏菜单里把 ○ 和 × 两项的按键对调。不加这个参数就使用默认键位，之后也能在游戏菜单里修改。

---

## 6. 等待部署完成

脚本会依次显示 9 个步骤，每一步以 `== 序号. 名称` 开头：

| 步骤 | 做什么 | 大约耗时 |
|---|---|---|
| `1. System and game folder` | 检查系统、显卡驱动、磁盘空间，解压 `game.zip`，确认游戏是 1.09 版 | 解压 30 GB：SSD 约 1 分钟，机械硬盘更久 |
| `2. MSYS2` | 下载并解压编译环境（约 40 MB）到 `msys64` | 1 分钟内 |
| `3. MSYS2 packages` | 更新 MSYS2，安装编译器、SDL3、Vulkan、FFmpeg 等（下载约 400 MB） | 3–10 分钟，看网速 |
| `4. Sources` | 从 GitHub 下载 bbport 源代码（约 140 MB） | 1–3 分钟 |
| `5. Build` | 编译 bbport，期间几分钟没有新输出是正常的 | 几分钟，CPU 越快越短 |
| `6. Launcher and settings` | 生成启动文件 `Bloodborne.cmd`，写入设置 | 瞬间 |
| `7. Vulkan and DLLs` | 不开窗口地检查程序能否加载、显卡能否工作，看到 `PASS` 即正常 | 几秒 |
| `8. Launcher dry run` | 准备游戏镜像和补丁，但不启动游戏 | 十几秒 |
| `9. Smoke test` | **会弹出游戏窗口**，渲染约 30 秒后自动关闭 | 约 40 秒 |

> **第 9 步弹出游戏窗口后，请不要关闭它，也不用操作。** 脚本会自己关掉。手动关闭会导致测试失败（`The game window was closed during the test`），重新运行一次命令即可。

中间偶尔出现黄色的 `... failed (exit code N); trying again (2/3)` 是网络断了一下，脚本正在自动重试，不用管。另外 `CMake Warning` 和 `warning: ...` 开头的行也都是正常输出。

最后看到绿色的 `Done in N min. Start the game with ...\bbport\Bloodborne.cmd` 就是部署成功。

如果中途出错，最后一行会是红色的 `FAILED: ...`，请对照后面的[常见问题](#常见问题)处理。完整记录保存在 `<部署目录>\bbport\logs\deploy-时间.log`，求助时请附上这个文件。

---

## 7. 开始游戏

部署完成后，`<部署目录>` 的结构是：

```
<部署目录>\
├─ deploy_windows.ps1
├─ game.zip            ← 可以删除或移走（游戏已经解压到 game\）
├─ game\               ← 游戏本体
├─ msys64\             ← 编译环境
└─ bbport\
   ├─ Bloodborne.cmd   ← 双击开始游戏
   ├─ bbport.ini       ← 设置（游戏保存设置时生成；用了 -Settings 时部署就会生成）
   ├─ user\            ← 存档
   └─ logs\            ← 日志
```

**双击 `bbport\Bloodborne.cmd` 就能开始游戏。** 想放到桌面：右键 `Bloodborne.cmd` →（Windows 11 先点"显示更多选项"）→ 发送到 → 桌面快捷方式。

第一次进游戏时，遇到新画面会编译着色器，可能会有短暂卡顿，之后会越来越流畅。

常用按键：

| 按键 | 作用 |
|---|---|
| `Insert`（手柄 L3+R3） | 打开 bbport 设置菜单：分辨率、画质、超分、键鼠按键 |
| `F11` | 切换全屏 / 窗口 |
| `F1` | 释放 / 重新捕获鼠标 |

手柄插上就能用；键盘鼠标的按键可以在设置菜单里修改。

---

## 8. 以后更新

在 `<部署目录>` 打开 PowerShell（第 4 步），再运行一次第 5 步的**同一条命令**。脚本会：

- 拉取最新代码并重新编译；
- 发现 `game\` 里已经有游戏，不会再解压一遍（`game.zip` 已经删掉也没关系）；
- 编译环境齐全时不更新 MSYS2。

存档和设置都会保留。

> 部署完成后，请不要移动或重命名 `<部署目录>`。启动文件里记录的是完整路径；如果确实移动了，在新位置重新运行一次第 5 步的命令即可。

---

## 已经解压好的游戏

如果游戏是 `.7z`、`.rar`、分卷压缩包，先用 7-Zip、Bandizip 等工具把它解压出来，确认解压后的文件夹里直接就有 `eboot.bin`。然后把这个文件夹放进 `<部署目录>` 并改名为 `game`：

```
<部署目录>\
├─ deploy_windows.ps1
└─ game\
   ├─ eboot.bin
   ├─ sce_module\
   ├─ sce_sys\
   └─ dvdroot_ps4\
```

然后把第 5 步的 `-GameZip .\game.zip` 换成 `-GameDir .\game`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\deploy_windows.ps1 -InstallDir .\bbport -GameDir .\game -Msys2Dir .\msys64 -SmokeTest
```

---

## 常见问题

| 红字提示（`FAILED:` 后面） | 原因和解决方法 |
|---|---|
| `... must be a path without spaces, quotes, special or non-ASCII characters` | `<部署目录>` 的路径里有空格、中文或特殊符号。换一个只有英文和数字的文件夹，从第 1 步重来 |
| `No Vulkan loader (System32\vulkan-1.dll)` | 显卡驱动没装好。到 NVIDIA / AMD 官网安装最新驱动 |
| `bbport needs game version 01.09; ... has 01.0x` | 游戏不是 1.09 版。需要已经合并 1.09 补丁的游戏 |
| `... has no eboot.bin at its root or in a single folder` | 压缩包结构不对（见第 3 步），或者里面不是 PS4 版游戏文件 |
| `Extracting the game zip failed` | 压缩包损坏或没下载完整（常见于网盘下载中断）。重新下载 `game.zip` 后再运行；上次解压的残留会被自动清理 |
| `-GameZip: no file ...` | 没找到 `game.zip`：检查文件名（是不是变成了 `game.zip.zip`），以及 PowerShell 是否在 `<部署目录>` 里打开（第 4 步） |
| `... is not empty and holds no eboot.bin` | `<部署目录>\game` 已经存在，但里面不是游戏。删掉或改名这个文件夹后重试 |
| `...: N GB free on X:\, about N GB needed` | 磁盘空间不够（前面的 `Game folder`、`Install folder`、`MSYS2` 说明是哪一部分），清理空间或换一个盘 |
| `Downloading MSYS2 failed`、`Updating MSYS2 failed`、`Installing the MSYS2 packages failed`、`Fetching https://... failed`、`Fetching the submodules failed`、`Updating the sources failed` | 网络问题。脚本已经自动重试过（下载 MSYS2 时最多 5 次并断点续传，其他步骤 3 次），仍然失败。重新运行同一条命令即可：已经完成的部分不会重做，下载到一半的也会接着下。网络需要代理时，先在同一个窗口里运行 `$env:HTTPS_PROXY = 'http://127.0.0.1:<代理端口>'`（`<代理端口>` 换成你代理软件的 HTTP 端口），再运行第 5 步的命令 |
| `The build ... failed` | 编译失败。把 `bbport\logs\deploy-*.log` 和 `bbport\out\gpu-build.log` 发给开发者 |
| `bb-probe.exe --vulkan-only failed` | 程序无法启动或显卡不支持。更新显卡驱动；日志里列出的缺失 DLL 一并附上求助 |
| `The game window was closed during the test` | 测试时游戏窗口被关掉了。重新运行命令，测试时别动窗口 |
| `The game ended after ... frame reports` 或 `Fewer than 6 frame reports ...` | 游戏启动了但没能正常显示画面。附上 `bbport\logs\` 下最新的 `bbport-*.log` 求助 |

其他说明：

- **想卸载**：先把 `bbport\user\`（存档）备份出来，然后删除整个 `<部署目录>`。按本指南部署时，游戏、程序和编译环境都在这个文件夹里，没有安装程序，也不写注册表（显卡驱动自己的着色器缓存除外）。
- **想跳过启动测试**：去掉命令里的 `-SmokeTest`。
- **想单独检查环境**：再运行一次同一条命令即可。所有检查都会重新执行，已经完成的下载、解压和编译会自动跳过。
