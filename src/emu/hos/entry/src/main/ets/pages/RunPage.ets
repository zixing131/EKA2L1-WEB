/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project (HarmonyOS port).
 *
 * The running screen: a full-bleed XComponent surface drives the emulator's
 * GL output, and launchApp() boots the chosen Symbian app. Mirrors the Android
 * EmulatorActivity.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

import { router, window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { Emulator } from '../service/Emulator';

const DOMAIN = 0x1311;
const TAG = 'EKA2L1';

interface RunParams {
  uid: number;
  title: string;
}

@Entry
@Component
struct RunPage {
  @State fps: number = 0;
  @State showFps: boolean = false;
  @State title: string = '';
  private uid: number = 0;
  private emu: Emulator = Emulator.get();
  private fpsTimer: number = -1;
  private launched: boolean = false;
  // The XComponent surface created callback (in native) starts the threads; we
  // launch the app shortly after so the graphics driver is ready.
  private xcomponentController: XComponentController = new XComponentController();

  aboutToAppear(): void {
    const params = router.getParams() as RunParams;
    if (params) {
      this.uid = params.uid;
      this.title = params.title ?? '';
    }
    this.keepScreenOn(true);
    this.showFps = this.emu.isFpsCounterEnabled();
    if (this.showFps) {
      this.startFpsPolling();
    }
  }

  aboutToDisappear(): void {
    this.keepScreenOn(false);
    if (this.fpsTimer >= 0) {
      clearInterval(this.fpsTimer);
      this.fpsTimer = -1;
    }
  }

  onPageHide(): void {
    // Surface destruction (native) pauses the emulator threads automatically.
  }

  private async keepScreenOn(on: boolean): Promise<void> {
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const win = await window.getLastWindow(ctx);
      await win.setWindowKeepScreenOn(on);
    } catch (err) {
      hilog.warn(DOMAIN, TAG, 'keepScreenOn failed: %{public}s', JSON.stringify(err));
    }
  }

  private startFpsPolling(): void {
    this.fpsTimer = setInterval(() => {
      this.fps = Math.round(this.emu.getFps() * 10) / 10;
    }, 500);
  }

  private launchSoon(): void {
    if (this.launched) {
      return;
    }
    this.launched = true;

    // Mirror Android's updateScreenSize(): push screen params BEFORE launch so
    // the draw callback (which runs on the OS thread the instant the app paints)
    // never reads uninitialised scale/gravity/colour. Values match the Android
    // default profile: bg 0xD0D0D0, scaleType 1 (fit width), gravity 1 (top),
    // ratio 100, no background image.
    this.emu.setScreenParams(0xD0D0D0, 100, 1, 1, '', 0.5, true);

    // Give the graphics thread a moment to finish EGL setup before launching.
    setTimeout(() => {
      hilog.info(DOMAIN, TAG, 'launchApp uid=%{public}d', this.uid);
      this.emu.launchApp(this.uid);
    }, 200);
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      XComponent({
        id: 'eka2l1_surface',
        type: XComponentType.SURFACE,
        libraryname: 'entry',
        controller: this.xcomponentController
      })
        .onLoad(() => {
          // Native OnSurfaceCreated has fired by now (it registers the surface
          // and starts the threads); launch the guest app.
          hilog.info(DOMAIN, TAG, 'XComponent loaded, launching uid=%{public}d', this.uid);
          this.launchSoon();
        })
        .width('100%')
        .height('100%')

      // Top overlay: back button + title + fps.
      Row() {
        Button({ type: ButtonType.Circle, stateEffect: true }) {
          SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(22).fontColor([Color.White])
        }
        .backgroundColor('#66000000')
        .width(40).height(40)
        .onClick(() => router.back())

        Text(this.title)
          .fontSize(14)
          .fontColor(Color.White)
          .margin({ left: 12 })
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })

        if (this.showFps) {
          Text(`${this.fps} FPS`)
            .fontSize(13)
            .fontColor(Color.Yellow)
            .backgroundColor('#66000000')
            .borderRadius(6)
            .padding({ left: 8, right: 8, top: 2, bottom: 2 })
        }
      }
      .width('100%')
      .padding({ left: 8, right: 12, top: 8 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
  }
}
