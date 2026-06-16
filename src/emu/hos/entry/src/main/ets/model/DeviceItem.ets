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

// An installed device (firmware/ROM) entry. Mirrors the Android device list.
@Observed
export class DeviceItem {
  id: number;
  model: string;
  firmwareCode: string;

  constructor(id: number, model: string, firmwareCode: string) {
    this.id = id;
    this.model = model;
    this.firmwareCode = firmwareCode;
  }
}

// A removable installed package shown in the package manager.
@Observed
export class PackageItem {
  uid: number;
  index: number;
  name: string;

  constructor(uid: number, index: number, name: string) {
    this.uid = uid;
    this.index = index;
    this.name = name;
  }
}
