/**
 * Playback-device names that count as the meeting virtual microphone on
 * Windows. "CABLE Input" is the render side of VB-Audio Virtual Cable;
 * meeting apps take its capture side, "CABLE Output", as their microphone.
 * Kept identical to `kVirtualOutputPattern` in native/win/wasapi.h.
 */
export const WINDOWS_VIRTUAL_OUTPUT_PATTERN =
  'cable input|voicemeeter input|vb-audio virtual cable';
