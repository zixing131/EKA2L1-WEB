/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project (HarmonyOS port).
 *
 * Settings: device management, installed packages, language. Mirrors the
 * Android settings / device-list / package-list fragments.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

import { router } from '@kit.ArkUI';
import { Emulator } from '../service/Emulator';
import { DeviceItem, PackageItem } from '../model/DeviceItem';

@Entry
@Component
struct SettingsPage {
  @State devices: DeviceItem[] = [];
  @State packages: PackageItem[] = [];
  @State currentDevice: number = 0;
  @State languageNames: string[] = [];
  @State languageIds: string[] = [];
  private emu: Emulator = Emulator.get();

  aboutToAppear(): void {
    this.refresh();
  }

  private refresh(): void {
    this.devices = this.emu.getDevices();
    this.currentDevice = this.emu.getCurrentDevice();
    this.packages = this.emu.getPackages();
    this.languageNames = this.emu.getLanguageNames();
    this.languageIds = this.emu.getLanguageIds();
  }

  private toast(msg: string): void {
    this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
  }

  private renameDevice(dev: DeviceItem): void {
    // Minimal inline rename via the system text picker is not available; use a
    // simple confirm with the firmware code shown. A full rename dialog can be
    // added with a custom dialog component.
    this.toast(`${dev.model} (${dev.firmwareCode})`);
  }

  private uninstallPackage(pkg: PackageItem): void {
    this.getUIContext().showAlertDialog({
      title: '卸载 / Uninstall',
      message: `卸载 "${pkg.name}" ?`,
      primaryButton: { value: '取消 / Cancel', action: () => {} },
      secondaryButton: {
        value: '卸载 / Uninstall',
        fontColor: Color.Red,
        action: () => {
          this.emu.uninstallPackage(pkg.uid, pkg.index);
          this.toast('已卸载 / Uninstalled');
          this.refresh();
        }
      }
    });
  }

  private chooseLanguage(): void {
    if (this.languageNames.length === 0) {
      this.toast('无可用语言 / No languages');
      return;
    }
    this.getUIContext().showActionSheet({
      title: '语言 / Language',
      message: '',
      confirm: { value: '取消 / Cancel', action: () => {} },
      sheets: this.languageNames.map((name, i) => {
        const sheet: SheetInfo = {
          title: name,
          action: () => {
            const id = Number.parseInt(this.languageIds[i], 10);
            this.emu.setLanguage(id);
            this.toast(`语言: ${name}`);
          }
        };
        return sheet;
      })
    });
  }

  @Builder
  SectionHeader(title: string) {
    Text(title)
      .fontSize(13)
      .fontColor($r('sys.color.ohos_id_color_text_secondary'))
      .width('100%')
      .padding({ left: 16, top: 16, bottom: 8 })
  }

  @Builder
  Row2(label: string, value: string, onTap: () => void) {
    Row() {
      Text(label).fontSize(16).layoutWeight(1)
      Text(value)
        .fontSize(14)
        .fontColor($r('sys.color.ohos_id_color_text_secondary'))
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
      SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(16).fontColor([$r('sys.color.ohos_id_color_text_tertiary')])
    }
    .width('100%')
    .padding(16)
    .onClick(onTap)
  }

  build() {
    Column() {
      // Title bar
      Row() {
        Button({ type: ButtonType.Circle, stateEffect: true }) {
          SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(22)
        }
        .backgroundColor(Color.Transparent)
        .width(40).height(40)
        .onClick(() => router.back())
        Text($r('app.string.settings_title'))
          .fontSize(20).fontWeight(FontWeight.Bold)
          .margin({ left: 8 })
      }
      .width('100%').height(56).padding({ left: 8 })

      Scroll() {
        Column() {
          this.SectionHeader('设备 / Devices')
          ForEach(this.devices, (dev: DeviceItem) => {
            Row() {
              SymbolGlyph(dev.id === this.currentDevice
                ? $r('sys.symbol.checkmark_circle_fill')
                : $r('sys.symbol.circle'))
                .fontSize(20)
                .fontColor([dev.id === this.currentDevice
                  ? $r('sys.color.ohos_id_color_text_primary_activated')
                  : $r('sys.color.ohos_id_color_text_tertiary')])
              Column() {
                Text(dev.model).fontSize(16)
                Text(dev.firmwareCode).fontSize(12).fontColor($r('sys.color.ohos_id_color_text_secondary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
              .margin({ left: 12 })
            }
            .width('100%')
            .padding(16)
            .onClick(() => {
              this.emu.setCurrentDevice(dev.id, false);
              this.currentDevice = dev.id;
              this.toast(`已选择 ${dev.model}`);
            })
          }, (dev: DeviceItem) => dev.id.toString())

          if (this.devices.length === 0) {
            Text('无设备 / No devices')
              .fontSize(14)
              .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
              .padding(16)
          }

          this.SectionHeader('通用 / General')
          this.Row2('语言 / Language', `${this.languageNames.length} 项`, () => this.chooseLanguage())

          this.SectionHeader('已安装应用包 / Installed packages')
          ForEach(this.packages, (pkg: PackageItem) => {
            Row() {
              Text(pkg.name).fontSize(15).layoutWeight(1).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
              Button('卸载')
                .fontSize(13)
                .backgroundColor('#1F000000')
                .fontColor(Color.Red)
                .height(32)
                .onClick(() => this.uninstallPackage(pkg))
            }
            .width('100%')
            .padding({ left: 16, right: 16, top: 10, bottom: 10 })
          }, (pkg: PackageItem) => `${pkg.uid}_${pkg.index}`)

          if (this.packages.length === 0) {
            Text('无可卸载的应用包 / No removable packages')
              .fontSize(14)
              .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
              .padding(16)
          }

          Blank().height(24)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
  }
}
