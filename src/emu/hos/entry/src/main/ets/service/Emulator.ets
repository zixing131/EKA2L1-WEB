/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project (HarmonyOS port).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

import emuNative from 'libentry.so';
import { image } from '@kit.ImageKit';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { resourceManager } from '@kit.LocalizationKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AppItem } from '../model/AppItem';
import { DeviceItem, PackageItem } from '../model/DeviceItem';

const DOMAIN = 0x1311;
const TAG = 'EKA2L1';

// Runtime asset folders bundled under entry/src/main/resources/rawfile/assets
// that the emulator expects to find next to its working directory (the same set
// the Android frontend copies into app assets). Copied once into the sandbox.
const ASSET_DIRS: string[] = ['resources', 'patch', 'compat', 'scripts'];

/**
 * ArkTS facade over the native EKA2L1 core (libentry.so). Mirrors the static
 * API of the Android Emulator.java: same method names, same flat string-array
 * encodings, adapted to ArkTS types.
 *
 * This is a singleton; call init(context) once at app start.
 */
export class Emulator {
  private static instance: Emulator | null = null;
  private started: boolean = false;
  private emulatorDir: string = '';

  static get(): Emulator {
    if (!Emulator.instance) {
      Emulator.instance = new Emulator();
    }
    return Emulator.instance;
  }

  isStarted(): boolean {
    return this.started;
  }

  getEmulatorDir(): string {
    return this.emulatorDir;
  }

  /**
   * Prepare the sandbox working directory, copy bundled assets, set the native
   * working directory and boot the emulator core. Idempotent.
   */
  async init(context: common.UIAbilityContext): Promise<void> {
    if (this.started) {
      return;
    }

    this.emulatorDir = context.filesDir;
    await this.copyAssets(context);

    // The native side cds into the directory of the path it is given, so hand it
    // a file path inside the working dir.
    emuNative.setDirectory(this.emulatorDir + '/eka2l1.cfg');

    const ok: boolean = emuNative.startNative();
    if (!ok) {
      hilog.warn(DOMAIN, TAG, 'startNative returned false (no device yet?)');
    }
    this.started = true;
  }

  // Copy rawfile/assets/<dir> trees into filesDir/<dir> the first time only.
  private async copyAssets(context: common.UIAbilityContext): Promise<void> {
    const mgr = context.resourceManager;
    for (const dir of ASSET_DIRS) {
      const dest = `${this.emulatorDir}/${dir}`;
      if (fs.accessSync(dest)) {
        continue; // already copied
      }
      try {
        await this.copyRawfileDir(mgr, `assets/${dir}`, dest);
      } catch (err) {
        hilog.warn(DOMAIN, TAG, 'Asset copy skipped for %{public}s: %{public}s', dir, JSON.stringify(err));
      }
    }
  }

  private async copyRawfileDir(mgr: resourceManager.ResourceManager, rawPath: string, destDir: string): Promise<void> {
    fs.mkdirSync(destDir, true);
    let entries: string[] = [];
    try {
      entries = mgr.getRawFileListSync(rawPath);
    } catch (err) {
      return;
    }
    for (const name of entries) {
      const childRaw = `${rawPath}/${name}`;
      const childDest = `${destDir}/${name}`;
      // A rawfile entry is a directory if listing it yields children.
      let sub: string[] = [];
      try {
        sub = mgr.getRawFileListSync(childRaw);
      } catch (err) {
        sub = [];
      }
      if (sub.length > 0) {
        await this.copyRawfileDir(mgr, childRaw, childDest);
      } else {
        try {
          const content: Uint8Array = mgr.getRawFileContentSync(childRaw);
          const file = fs.openSync(childDest, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE);
          fs.writeSync(file.fd, content.buffer as ArrayBuffer);
          fs.closeSync(file);
        } catch (err) {
          hilog.warn(DOMAIN, TAG, 'copy file fail %{public}s: %{public}s', childRaw, JSON.stringify(err));
        }
      }
    }
  }

  // ---- Devices ----

  hasDevice(): boolean {
    return this.getDevices().length > 0;
  }

  getDevices(): DeviceItem[] {
    const names: string[] = emuNative.getDevices();
    const codes: string[] = emuNative.getDeviceFirmwareCodes();
    const out: DeviceItem[] = [];
    for (let i = 0; i < names.length; i++) {
      out.push(new DeviceItem(i, names[i], i < codes.length ? codes[i] : ''));
    }
    return out;
  }

  getCurrentDevice(): number {
    return emuNative.getCurrentDevice();
  }

  setCurrentDevice(id: number, temporary: boolean): void {
    emuNative.setCurrentDevice(id, temporary);
  }

  setDeviceName(id: number, name: string): void {
    emuNative.setDeviceName(id, name);
  }

  rescanDevices(): void {
    emuNative.rescanDevices();
  }

  doesRomNeedRpkg(romPath: string): boolean {
    return emuNative.doesRomNeedRPKG(romPath);
  }

  // Returns 0 on success (device_installation_none); non-zero is an error code.
  installDevice(rpkgPath: string, romPath: string, installRpkg: boolean): number {
    return emuNative.installDevice(rpkgPath, romPath, installRpkg);
  }

  // ---- Apps ----

  getApps(): AppItem[] {
    // Flat [uid, name, uid, name, ...]
    const flat: string[] = emuNative.getApps();
    const out: AppItem[] = [];
    for (let i = 0; i + 1 < flat.length; i += 2) {
      const uid = Number.parseInt(flat[i], 10);
      out.push(new AppItem(uid, flat[i + 1]));
    }
    return out;
  }

  // Decode the native RGBA icon into a PixelMap, or undefined if none.
  async getAppIcon(uid: number): Promise<image.PixelMap | undefined> {
    const icon = emuNative.getAppIcon(uid);
    if (!icon || !icon.data || icon.width <= 0 || icon.height <= 0) {
      return undefined;
    }
    const opts: image.InitializationOptions = {
      editable: false,
      pixelFormat: image.PixelMapFormat.RGBA_8888,
      size: { width: icon.width, height: icon.height }
    };
    try {
      return await image.createPixelMap(icon.data, opts);
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'createPixelMap failed for uid %{public}d: %{public}s', uid, JSON.stringify(err));
      return undefined;
    }
  }

  launchApp(uid: number): void {
    emuNative.launchApp(uid);
  }

  // Returns installation_result; 0 == success.
  installApp(path: string): number {
    return emuNative.installApp(path);
  }

  installNGageGame(path: string): number {
    return emuNative.installNGageGame(path);
  }

  // ---- Packages ----

  getPackages(): PackageItem[] {
    // Flat [uid, index, name, ...]
    const flat: string[] = emuNative.getPackages();
    const out: PackageItem[] = [];
    for (let i = 0; i + 2 < flat.length; i += 3) {
      out.push(new PackageItem(Number.parseInt(flat[i], 10), Number.parseInt(flat[i + 1], 10), flat[i + 2]));
    }
    return out;
  }

  uninstallPackage(uid: number, extIndex: number): void {
    emuNative.uninstallPackage(uid, extIndex);
  }

  mountSdCard(path: string): void {
    emuNative.mountSdCard(path);
  }

  // ---- Config / languages ----

  loadConfig(): void {
    emuNative.loadConfig();
  }

  setLanguage(languageId: number): void {
    emuNative.setLanguage(languageId);
  }

  getLanguageIds(): string[] {
    return emuNative.getLanguageIds();
  }

  getLanguageNames(): string[] {
    return emuNative.getLanguageNames();
  }

  setRtosLevel(level: number): void {
    emuNative.setRtosLevel(level);
  }

  updateAppSetting(uid: number): void {
    emuNative.updateAppSetting(uid);
  }

  setScreenParams(backgroundColor: number, scaleRatio: number, scaleType: number, gravity: number,
    bgImgPath: string, bgImgOpacity: number, bgImgKeepAspect: boolean): void {
    emuNative.setScreenParams(backgroundColor, scaleRatio, scaleType, gravity, bgImgPath, bgImgOpacity, bgImgKeepAspect);
  }

  // ---- Runtime ----

  getFps(): number {
    return emuNative.getFps();
  }

  isFpsCounterEnabled(): boolean {
    return emuNative.isFpsCounterEnabled();
  }

  pressKey(key: number, keyState: number): void {
    emuNative.pressKey(key, keyState);
  }

  touchScreen(x: number, y: number, z: number, action: number, id: number): void {
    emuNative.touchScreen(x, y, z, action, id);
  }

  submitInput(text: string): void {
    emuNative.submitInput(text);
  }

  submitQuestionDialogResponse(value: number): void {
    emuNative.submitQuestionDialogResponse(value);
  }

  saveScreenshotTo(filePath: string): boolean {
    return emuNative.saveScreenshotTo(filePath);
  }
}
