# BabelVoice Windows 配置指南

BabelVoice 有两个方向：

| 方向 | 界面上的名字 | 声音怎么走 |
|---|---|---|
| 说话 | 「翻译你说的话」 | 你的麦克风 → BabelVoice 翻译 → 译音写入 **CABLE Input** → 会议软件把麦克风设为 **CABLE Output**，对方听到译音 |
| 收听 | 「听懂对方在说什么」 | 采集会议正在播放的声音 → 字幕窗显示译文；可选在本机播放译音 |

系统要求：Windows 10 或 Windows 11。收听「某一个会议应用」而不是全部系统声音，需要 Windows 10 2004 或更新。

关闭主窗口不会退出。应用留在任务栏通知区域，单击图标可以重新打开，右键可以选择退出。

## 1. 安装 VB-Audio Virtual Cable

Windows 没有自带的虚拟麦克风，会议软件也就收不到译音。BabelVoice 使用免费的 [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)。

1. 在配置向导第 1 步，或侧边栏「音频路由」里，点「安装」。浏览器会打开 VB-Cable 的下载页。
2. 下载 ZIP，解压后运行里面的安装程序，并允许管理员权限。
3. 回到 BabelVoice。检测到名为 **CABLE Input** 的播放设备后，这一步会显示已安装。

装好后，系统里会多出两个设备：

| 设备 | 类型 | 谁来用 |
|---|---|---|
| CABLE Input | 播放设备 | BabelVoice 把译音写到这里。不要把它设成 Windows 的默认播放设备。 |
| CABLE Output | 录制设备 | 会议软件把它选作麦克风。不要在 BabelVoice 里把它选成你的麦克风。 |

Voicemeeter 的 Input 设备也可以当作同一条虚拟链路，BabelVoice 会优先匹配名字里带 `CABLE Input` 或 `Voicemeeter Input` 的播放设备。

## 2. 会议软件和 BabelVoice 的设备设置

在 Zoom、腾讯会议、飞书、Teams 的音频设置里：

| 会议软件设置 | 选什么 | 为什么 |
|---|---|---|
| 麦克风 | **CABLE Output** | 对方通过它听到你说话的译音 |
| 扬声器 | 你的耳机或音箱 | 你照常听会议，BabelVoice 也从这里采集对方的声音 |

在 BabelVoice 的「音频路由」页：

- **麦克风**：选你真正说话用的麦克风，不要选 CABLE Output。
- **播放译音的设备**：只影响「听对方」时本机要不要出声，选耳机或音箱，不要选 CABLE Input。

你说话的译音只送进虚拟声卡，不会从这台电脑的扬声器里放出来。

## 3. 麦克风权限

Windows 的「设置 → 隐私和安全性 → 麦克风」里，要允许应用访问麦克风，并允许桌面应用访问。拒绝之后，BabelVoice 采到的是静音。

收听对方不需要「屏幕录制」权限。BabelVoice 用系统的音频环回直接采集正在播放的声音。

## 4. 开会

1. 先打开会议软件，麦克风选 CABLE Output，扬声器选耳机或音箱。
2. 在 BabelVoice 的「会议」页选好「你说」和「对方说」的语言。
3. 收听来源选会议软件本身（这样只翻译这个应用的声音）。如果要在本机播放对方的译音，必须选定一个应用，否则扬声器里的译音会被再次采集，形成回环。
4. 点「开始会议」。翻译你说的话可以用 **Ctrl+Shift+Space**（可在设置里改）。Alt+Space 在 Windows 上会打开窗口菜单，所以不是默认快捷键。
5. 结束会议后，对话保存在本机，可以在「历史」里回看。

## 5. 从源码运行

需要 Node.js、以及 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载（编译 WASAPI 音频模块）。

```bash
npm install
npm run dev
```

`npm run dev` 会先编译 `babelvoice_native.node`，再打开应用。
