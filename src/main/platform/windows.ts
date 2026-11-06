/**
 * Windows platform adapter. System-audio loopback does not need a screen-
 * recording consent. Translated speech is played into a virtual cable the
 * user installs (VB-Audio Virtual Cable); meeting apps select that cable as
 * their microphone.
 */
import { app, shell } from 'electron';
import { join } from 'node:path';
import type {
  InstallVirtualOutputResult,
  ScreenPermissionRequestResult,
  ScreenPermissionStatus,
  VirtualOutputStatus
} from '@shared/ipc-types.js';
import { log } from '@core/utils/logger.js';
import { t } from '../i18n.js';
import { platformNativeBridge } from '../native/platform-native-bridge.js';

const VIRTUAL_CABLE_URL = 'https://vb-audio.com/Cable/';

export const getVirtualAudioDeviceHint = (): string =>
  t('audio:virtualDeviceHintWindows');

export const getSetupGuideRelativePath = (): string => 'docs/SETUP-windows.md';

const resolveSetupGuidePath = (): string =>
  join(
    app.isPackaged ? process.resourcesPath : app.getAppPath(),
    getSetupGuideRelativePath()
  );

export const openSetupGuide = async (): Promise<void> => {
  const guide = resolveSetupGuidePath();
  const error = await shell.openPath(guide);
  if (error) {
    log.warn(`open setup guide failed (${guide}): ${error}`);
    shell.showItemInFolder(guide);
  }
};

export const getScreenCapturePermission = (): ScreenPermissionStatus =>
  platformNativeBridge.getPermissions().screen;

export const hasMicrophonePermission = (): boolean =>
  platformNativeBridge.getPermissions().microphone === 'granted';

export const requestScreenCapturePermission =
  async (): Promise<ScreenPermissionRequestResult> => {
    const before = getScreenCapturePermission();
    return { statusBefore: before, statusAfter: before, sourceCount: before === 'granted' ? 1 : 0 };
  };

export const openScreenCaptureSettings = async (): Promise<void> => {
  await shell.openExternal('ms-settings:privacy-microphone');
};

export const getVirtualOutputStatus = async (): Promise<VirtualOutputStatus> => {
  const devices = platformNativeBridge.listAudioDevices();
  const cable = devices.find((device) => device.kind === 'output' && device.isVirtual && device.isAlive);
  return cable
    ? { installed: true, driverPath: cable.name }
    : { installed: false };
};

export const requestVirtualOutputInstall =
  async (): Promise<InstallVirtualOutputResult> => {
    try {
      await shell.openExternal(VIRTUAL_CABLE_URL);
      return { started: true };
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      return { started: false, error: t('errors:windowsCableOpenFailed', { message }) };
    }
  };
