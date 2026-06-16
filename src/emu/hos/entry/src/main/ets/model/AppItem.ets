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

import { image } from '@kit.ImageKit';

// A Symbian app entry shown in the app list. Mirrors AppItem.java.
@Observed
export class AppItem {
  uid: number;
  title: string;
  icon?: image.PixelMap;

  constructor(uid: number, title: string, icon?: image.PixelMap) {
    this.uid = uid;
    this.title = title;
    this.icon = icon;
  }
}
