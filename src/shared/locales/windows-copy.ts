/**
 * Copy that would be wrong on Windows if we kept the macOS strings. Applied
 * once, in place, on the shared locale bundle before either process reads it.
 * macOS builds never call this, so the original JSON stays the macOS wording.
 */
import type { LocaleResources } from './index.js';
import type { Locale } from '../i18n-types.js';

type Bundle = Record<Locale, LocaleResources>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === 'object' && !Array.isArray(value);

const merge = (base: Record<string, unknown>, patch: Record<string, unknown>): void => {
  for (const [key, value] of Object.entries(patch)) {
    if (isRecord(value)) {
      const current = base[key];
      if (!isRecord(current)) base[key] = {};
      merge(base[key] as Record<string, unknown>, value);
    } else {
      base[key] = value;
    }
  }
};

const zhCN = {
  nav: { brandSubtitle: 'v0.2 · Windows' },
  onboarding: {
    step1: {
      title: '安装虚拟声卡',
      desc: 'Windows 不允许应用之间直接传递音频。安装免费的 VB-Audio Virtual Cable 后，把会议软件的麦克风选为 CABLE Output，对方就能听到译音。点击安装会打开官网下载页。',
      driverName: 'VB-Cable 虚拟声卡',
      status: {
        installing: '已打开 VB-Cable 下载页。解压后运行安装程序（需要管理员权限），装好后回到这里会自动检测到。',
        installedAlive: '已检测到虚拟声卡。会议软件的麦克风请选 CABLE Output。',
        installedWaiting: '正在等待系统出现 CABLE Input…',
        notInstalled: '尚未安装。'
      },
      doneBanner: '虚拟声卡已就绪。会议软件的麦克风请选 CABLE Output。'
    },
    step2: {
      desc: '麦克风权限用来翻译你说的话。收听对方不需要屏幕录制权限，Windows 可以直接采集正在播放的声音。',
      screen: {
        title: '系统音频',
        desc: 'Windows 可以直接采集正在播放的声音，这一项无需额外授权。'
      },
      tip: '如果麦克风被拒绝，打开「设置 → 隐私和安全性 → 麦克风」，允许桌面应用访问麦克风，然后重新打开 BabelVoice。'
    }
  },
  audio: {
    speak: {
      flow: {
        driver: { ready: 'CABLE Input' },
        meetingAppDetail: '麦克风选 CABLE Output'
      },
      driver: {
        label: 'VB-Cable 虚拟麦克风',
        description: {
          checking: '会议软件要把 CABLE Output 选作麦克风，才能听到译音。',
          missing: '还没有虚拟声卡。安装 VB-Audio Virtual Cable 后，会议软件把麦克风选为 CABLE Output。',
          installing: '已打开 VB-Cable 下载页。安装完成并重新检测后即可继续。',
          'not-loaded': '声卡已安装，但 Windows 还没有把它列出来。请在系统声音设置里确认 CABLE Input 已启用。',
          ready: '会议软件把麦克风选为 CABLE Output 即可听到译音。'
        }
      },
      micPermission: {
        denied: '麦克风权限被拒绝了。在「设置 → 隐私和安全性 → 麦克风」里允许桌面应用访问，然后重新打开 BabelVoice。'
      },
      meetingApp: {
        label: '在会议软件里选 CABLE Output',
        description: '在 Zoom、腾讯会议、飞书、Teams 的音频设置里，把麦克风设为 CABLE Output。扬声器仍选你的耳机或音箱。'
      },
      verify: '想确认对方听得到：在会议软件里测试麦克风，开始翻译后说一句话，音量条有跳动就对了。译音送进虚拟声卡，这台电脑的扬声器里听不到。'
    },
    listen: {
      permission: {
        label: '系统音频',
        needed: 'Windows 可以直接采集正在播放的声音。',
        granted: 'BabelVoice 会采集会议软件正在播放的声音。'
      },
      output: {
        defaultIsVirtual: '系统默认输出现在是虚拟声卡，译音会被送进会议。请选一个耳机或扬声器。'
      }
    }
  },
  control: {
    listen: {
      permission: {
        needed: '需要允许麦克风后才能翻译你说的话'
      }
    }
  },
  settings: {
    appearance: {
      options: {
        system: { hint: '根据 Windows 浅色 / 深色设置自动切换。' }
      }
    }
  }
} as const;

const en = {
  nav: { brandSubtitle: 'v0.2 · Windows' },
  onboarding: {
    step1: {
      title: 'Install a virtual cable',
      desc: 'Windows does not let apps pipe audio to each other directly. Install the free VB-Audio Virtual Cable, then set the meeting app microphone to CABLE Output so the other side hears the translation. Install opens the download page.',
      driverName: 'VB-Cable virtual cable',
      status: {
        installing: 'The VB-Cable download page is open. Unzip it, run the installer (admin approval), then come back — this step detects it automatically.',
        installedAlive: 'Virtual cable detected. In the meeting app, set the microphone to CABLE Output.',
        installedWaiting: 'Waiting for Windows to show CABLE Input…',
        notInstalled: 'Not installed yet.'
      },
      doneBanner: 'Virtual cable is ready. Set the meeting microphone to CABLE Output.'
    },
    step2: {
      desc: 'Microphone permission is used to translate your speech. Listening does not need a screen-recording permission; Windows can capture audio that is already playing.',
      screen: {
        title: 'System audio',
        desc: 'Windows can capture audio that is already playing. No extra permission is required.'
      },
      tip: 'If the microphone was denied, open Settings → Privacy & security → Microphone, allow desktop apps, then reopen BabelVoice.'
    }
  },
  audio: {
    speak: {
      flow: {
        driver: { ready: 'CABLE Input' },
        meetingAppDetail: 'Mic: CABLE Output'
      },
      driver: {
        label: 'VB-Cable virtual microphone',
        description: {
          checking: 'The meeting app has to use CABLE Output as its microphone to hear your translation.',
          missing: 'No virtual cable yet. Install VB-Audio Virtual Cable, then set the meeting microphone to CABLE Output.',
          installing: 'The VB-Cable download page is open. Re-check after the installer finishes.',
          'not-loaded': 'The cable is installed but Windows has not listed it yet. Enable CABLE Input in the sound settings.',
          ready: 'Set the meeting microphone to CABLE Output to hear the translation.'
        }
      },
      micPermission: {
        denied: 'Microphone access was denied. Allow desktop apps under Settings → Privacy & security → Microphone, then reopen BabelVoice.'
      },
      meetingApp: {
        label: 'Select CABLE Output in the meeting app',
        description: 'In Zoom, VooV, Feishu, or Teams, set the microphone to CABLE Output. Leave the speaker as your headphones or speakers.'
      },
      verify: 'To confirm the other side can hear you: use the meeting app’s mic test, start translation, and say a sentence. The meter should move. The translation goes to the virtual cable, not this PC’s speakers.'
    },
    listen: {
      permission: {
        label: 'System audio',
        needed: 'Windows can capture audio that is already playing.',
        granted: 'BabelVoice captures what the meeting app is playing.'
      },
      output: {
        defaultIsVirtual: 'The system output is the virtual cable, so the translation would go into the meeting. Pick headphones or speakers.'
      }
    }
  },
  control: {
    listen: {
      permission: {
        needed: 'Allow the microphone before your speech can be translated'
      }
    }
  },
  settings: {
    appearance: {
      options: {
        system: { hint: 'Follow the Windows light / dark setting.' }
      }
    }
  }
} as const;

let applied = false;

export const applyWindowsCopy = (resources: Bundle): void => {
  if (applied) return;
  applied = true;
  merge(resources['zh-CN'] as unknown as Record<string, unknown>, zhCN as unknown as Record<string, unknown>);
  merge(resources.en as unknown as Record<string, unknown>, en as unknown as Record<string, unknown>);
};
