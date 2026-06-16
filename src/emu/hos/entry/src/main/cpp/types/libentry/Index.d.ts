// NAPI bridge for the EKA2L1 emulator core. Implemented in napi_init.cpp.
// String arrays returned by getApps/getDevices/getPackages mirror the Android
// JNI surface: getApps returns a flat [uid, name, uid, name, ...] list, etc.

export interface AppIcon {
  width: number;
  height: number;
  data: ArrayBuffer;     // RGBA8888, premultiplied
  maskWidth?: number;
  maskHeight?: number;
  maskData?: ArrayBuffer;
}

// Lifecycle
export const setDirectory: (path: string) => void;
export const startNative: () => boolean;
export const isInitialized: () => boolean;

// App list / icons
export const getApps: () => string[];
export const getAppIcon: (uid: number) => AppIcon | undefined;
export const launchApp: (uid: number) => void;

// Devices
export const getDevices: () => string[];
export const getDeviceFirmwareCodes: () => string[];
export const setCurrentDevice: (id: number, temporary: boolean) => void;
export const setDeviceName: (id: number, newName: string) => void;
export const rescanDevices: () => void;
export const getCurrentDevice: () => number;
export const bootFirstDevice: () => boolean;
export const installDevice: (rpkgPath: string, romPath: string, installRpkg: boolean) => number;
export const doesRomNeedRPKG: (romPath: string) => boolean;

// Packages / installs
export const installApp: (path: string) => number;
export const getPackages: () => string[];
export const uninstallPackage: (uid: number, extIndex: number) => void;
export const mountSdCard: (path: string) => void;
export const installNGageGame: (path: string) => number;

// Config / languages / settings
export const loadConfig: () => void;
export const setLanguage: (languageId: number) => void;
export const setRtosLevel: (level: number) => void;
export const updateAppSetting: (uid: number) => void;
export const getLanguageIds: () => string[];
export const getLanguageNames: () => string[];
export const setScreenParams: (backgroundColor: number, scaleRatio: number, scaleType: number,
  gravity: number, bgImgPath: string, bgImgOpacity: number, bgImgKeepAspect: boolean) => void;

// Runtime stats / dialogs / misc
export const getFps: () => number;
export const isFpsCounterEnabled: () => boolean;
export const submitInput: (text: string) => void;
export const submitQuestionDialogResponse: (value: number) => void;
export const saveScreenshotTo: (filePath: string) => boolean;
export const setCurrentMMCID: (newMmcId: string) => void;

// Input (fallback to XComponent native touch handling)
export const pressKey: (key: number, keyState: number) => void;
export const touchScreen: (x: number, y: number, z: number, action: number, id: number) => void;
