/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project (HarmonyOS port).
 *
 * Main screen: app grid + device switcher + install actions. Mirrors the
 * Android AppsListFragment / MainActivity behaviour.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { promptAction, router } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { Emulator } from '../service/Emulator';
import { AppItem } from '../model/AppItem';
import { DeviceItem } from '../model/DeviceItem';

const DOMAIN = 0x1311;
const TAG = 'EKA2L1';

@Entry
@Component
struct Index {
  @State apps: AppItem[] = [];
  @State devices: DeviceItem[] = [];
  @State currentDevice: number = 0;
  @State loading: boolean = true;
  @State busyText: string = '';
  @State gridMode: boolean = true;
  private emu: Emulator = Emulator.get();

  aboutToAppear(): void {
    this.boot();
  }

  onPageShow(): void {
    // Returning from the run page - refresh the list (apps/devices may change).
    if (this.emu.isStarted()) {
      this.refresh();
    }
  }

  private async boot(): Promise<void> {
    this.loading = true;
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      await this.emu.init(ctx);
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'init failed: %{public}s', JSON.stringify(err));
    }
    this.refresh();
    this.loading = false;
  }

  private refresh(): void {
    this.devices = this.emu.getDevices();
    this.currentDevice = this.emu.getCurrentDevice();
    this.apps = this.emu.getApps();
    this.loadIcons();
  }

  private async loadIcons(): Promise<void> {
    for (const app of this.apps) {
      if (!app.icon) {
        const pm = await this.emu.getAppIcon(app.uid);
        if (pm) {
          app.icon = pm;
          // Reassign array element to trigger re-render of the observed item.
          this.apps = this.apps.slice();
        }
      }
    }
  }

  private toast(msg: string): void {
    this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
  }

  // Copy a picked file URI into the sandbox cache and return the real path the
  // native installer can read (it uses stdio, not content URIs).
  private async copyPickedToCache(uri: string, suggestedName: string): Promise<string> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const dest = `${ctx.cacheDir}/${Date.now()}_${suggestedName}`;
    const src = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    const dst = fs.openSync(dest, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE);
    try {
      fs.copyFileSync(src.fd, dst.fd);
    } finally {
      fs.closeSync(src);
      fs.closeSync(dst);
    }
    return dest;
  }

  private async pickFile(suffixes: string[]): Promise<string | undefined> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const docPicker = new picker.DocumentViewPicker(ctx);
    try {
      const uris = await docPicker.select({ maxSelectNumber: 1, fileSuffixFilters: suffixes });
      if (uris && uris.length > 0) {
        return uris[0];
      }
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'pick cancelled: %{public}s', JSON.stringify(err));
    }
    return undefined;
  }

  private nameFromUri(uri: string): string {
    const slash = uri.lastIndexOf('/');
    return slash >= 0 ? uri.substring(slash + 1) : uri;
  }

  private async onInstallRom(): Promise<void> {
    // Step 1: pick the ROM file.
    const romUri = await this.pickFile(['ROM file|.rom', 'All|.*']);
    if (!romUri) {
      return;
    }

    this.busyText = '正在安装设备 / Installing device...';
    try {
      const romPath = await this.copyPickedToCache(romUri, this.nameFromUri(romUri));

      // Step 2: most Symbian ROMs ship without their writable ROFS/drive-C
      // content, so an accompanying RPKG is required to complete the device.
      // Ask the core; if needed, prompt for the RPKG file too (mirrors the
      // desktop/Android two-file install flow).
      let rpkgPath = '';
      if (this.emu.doesRomNeedRpkg(romPath)) {
        // Stop blocking the UI while the picker is up.
        this.busyText = '';
        this.toast('该 ROM 需要配套 RPKG，请选择 / Select the matching RPKG');
        const rpkgUri = await this.pickFile(['RPKG file|.rpkg', 'All|.*']);
        if (!rpkgUri) {
          this.toast('已取消：缺少 RPKG / Cancelled: RPKG required');
          return;
        }
        this.busyText = '正在安装设备 / Installing device...';
        rpkgPath = await this.copyPickedToCache(rpkgUri, this.nameFromUri(rpkgUri));
      }

      // installRpkg = true takes the ROM (+ optional RPKG) path; the core picks
      // install_rpkg vs install_rom internally based on the ROM contents.
      const result = this.emu.installDevice(rpkgPath, romPath, true);
      if (result === 0) {
        this.toast('设备安装成功 / Device installed');
        // If the app started with no device, the kernel/OS thread were never
        // brought up. Boot the freshly installed device now so its apps appear;
        // otherwise the system is already running and a refresh suffices.
        if (this.devices.length === 0) {
          this.emu.bootFirstDevice();
        }
        this.refresh();
      } else {
        this.toast(`安装失败 / Install failed (code ${result})`);
      }
    } catch (err) {
      this.toast('安装出错 / Install error');
      hilog.error(DOMAIN, TAG, 'install device error: %{public}s', JSON.stringify(err));
    } finally {
      this.busyText = '';
    }
  }

  private async onInstallApp(): Promise<void> {
    const uri = await this.pickFile(['SIS|.sis', 'SISX|.sisx', 'All|.*']);
    if (!uri) {
      return;
    }
    this.busyText = '正在安装应用 / Installing app...';
    try {
      const path = await this.copyPickedToCache(uri, this.nameFromUri(uri));
      const result = this.emu.installApp(path);
      if (result === 0) {
        this.toast('应用安装成功 / App installed');
        this.refresh();
      } else {
        this.toast(`安装失败 / Install failed (code ${result})`);
      }
    } catch (err) {
      this.toast('安装出错 / Install error');
    } finally {
      this.busyText = '';
    }
  }

  private onRunApp(app: AppItem): void {
    if (this.devices.length === 0) {
      this.toast('请先安装设备 / Install a device first');
      return;
    }
    router.pushUrl({ url: 'pages/RunPage', params: { uid: app.uid, title: app.title } });
  }

  private onSwitchDevice(): void {
    if (this.devices.length === 0) {
      this.toast('没有设备 / No devices');
      return;
    }
    const names: string[] = this.devices.map((d) => d.model);
    this.getUIContext().showActionSheet({
      title: '切换设备 / Switch device',
      message: '',
      confirm: { value: '取消 / Cancel', action: () => {} },
      sheets: this.devices.map((d) => {
        const sheet: SheetInfo = {
          title: `${d.model}  (${d.firmwareCode})`,
          action: () => {
            this.emu.setCurrentDevice(d.id, false);
            this.toast(`已切换到 ${d.model}`);
            this.refresh();
          }
        };
        return sheet;
      })
    });
  }

  @Builder
  TopBar() {
    Row() {
      Text($r('app.string.app_name'))
        .fontSize(20)
        .fontWeight(FontWeight.Bold)
        .layoutWeight(1)
      Button({ type: ButtonType.Circle, stateEffect: true }) {
        SymbolGlyph($r('sys.symbol.arrow_clockwise')).fontSize(20)
      }
      .backgroundColor(Color.Transparent)
      .width(40).height(40)
      .onClick(() => this.refresh())
      Button({ type: ButtonType.Circle, stateEffect: true }) {
        SymbolGlyph($r('sys.symbol.gearshape')).fontSize(20)
      }
      .backgroundColor(Color.Transparent)
      .width(40).height(40)
      .onClick(() => router.pushUrl({ url: 'pages/SettingsPage' }))
    }
    .width('100%')
    .height(56)
    .padding({ left: 16, right: 8 })
  }

  @Builder
  DeviceBar() {
    Row() {
      SymbolGlyph($r('sys.symbol.checkmark_shield')).fontSize(16).fontColor([$r('sys.color.ohos_id_color_text_secondary')])
      Text(this.devices.length > 0 && this.currentDevice < this.devices.length
        ? this.devices[this.currentDevice].model
        : '无设备 / No device')
        .fontSize(14)
        .fontColor($r('sys.color.ohos_id_color_text_secondary'))
        .margin({ left: 8 })
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
      Text('切换 / Switch')
        .fontSize(14)
        .fontColor($r('sys.color.ohos_id_color_text_primary_activated'))
        .onClick(() => this.onSwitchDevice())
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 8, bottom: 8 })
  }

  @Builder
  AppCell(app: AppItem) {
    Column() {
      if (app.icon) {
        Image(app.icon)
          .width(56).height(56)
          .objectFit(ImageFit.Contain)
          .borderRadius(12)
      } else {
        Column() {
          SymbolGlyph($r('sys.symbol.square_grid_2x2')).fontSize(32).fontColor([Color.Gray])
        }
        .width(56).height(56)
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#1F000000')
        .borderRadius(12)
      }
      Text(app.title)
        .fontSize(12)
        .margin({ top: 6 })
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .textAlign(TextAlign.Center)
        .width('100%')
    }
    .width('100%')
    .padding(8)
    .onClick(() => this.onRunApp(app))
  }

  @Builder
  EmptyState() {
    Column({ space: 16 }) {
      SymbolGlyph($r('sys.symbol.exclamationmark_circle')).fontSize(64).fontColor([Color.Gray])
      Text(this.devices.length === 0
        ? '未找到设备\n请先安装一个 ROM / 固件\n\nNo device found.\nInstall a ROM/firmware to begin.'
        : '没有已安装的应用\n点击下方按钮安装 SIS/SISX\n\nNo apps installed.\nInstall a SIS/SISX below.')
        .fontSize(15)
        .fontColor($r('sys.color.ohos_id_color_text_secondary'))
        .textAlign(TextAlign.Center)
    }
    .width('100%')
    .layoutWeight(1)
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  BottomActions() {
    Row({ space: 12 }) {
      Button('安装 ROM / Install ROM')
        .layoutWeight(1)
        .onClick(() => this.onInstallRom())
      Button('安装应用 / Install App')
        .layoutWeight(1)
        .backgroundColor($r('sys.color.ohos_id_color_palette9'))
        .onClick(() => this.onInstallApp())
    }
    .width('100%')
    .padding(16)
  }

  build() {
    Stack() {
      Column() {
        this.TopBar()
        this.DeviceBar()

        if (this.apps.length === 0) {
          this.EmptyState()
        } else {
          Grid() {
            ForEach(this.apps, (app: AppItem) => {
              GridItem() {
                this.AppCell(app)
              }
            }, (app: AppItem) => app.uid.toString())
          }
          .columnsTemplate('1fr 1fr 1fr 1fr')
          .layoutWeight(1)
          .padding({ left: 8, right: 8 })
        }

        this.BottomActions()
      }
      .width('100%')
      .height('100%')

      if (this.loading || this.busyText.length > 0) {
        Column({ space: 12 }) {
          LoadingProgress().width(48).height(48).color($r('sys.color.ohos_id_color_text_primary_activated'))
          Text(this.busyText.length > 0 ? this.busyText : '正在启动 / Starting...')
            .fontSize(14)
            .fontColor(Color.White)
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#99000000')
      }
    }
    .width('100%')
    .height('100%')
  }
}
