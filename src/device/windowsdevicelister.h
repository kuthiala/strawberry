/*
 * Strawberry Music Player
 * Copyright 2026, Strawberry contributors
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef WINDOWSDEVICELISTER_H
#define WINDOWSDEVICELISTER_H

#include "config.h"

#include <QtGlobal>
#include <QObject>
#include <QMutex>
#include <QMap>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

#include "devicelister.h"

class QTimer;

// Lists removable and USB volumes (iPods in disk mode, USB sticks, card readers) by drive letter.
// GLib's Windows volume monitor reports no GVolumes and no hot-plug events, so GioLister sees nothing on Windows; this lister polls the logical drive mask instead.
// Device IDs are "Win/<USB serial>" so GPodDevice can derive an iPod's FirewireGuid from them, as it does with the macOS lister's IDs.
class WindowsDeviceLister : public DeviceLister {
  Q_OBJECT

 public:
  explicit WindowsDeviceLister(QObject *parent = nullptr);
  ~WindowsDeviceLister() override;

  QStringList DeviceUniqueIDs() override;
  QVariantList DeviceIcons(const QString &id) override;
  QString DeviceManufacturer(const QString &id) override;
  QString DeviceModel(const QString &id) override;
  quint64 DeviceCapacity(const QString &id) override;
  quint64 DeviceFreeSpace(const QString &id) override;
  QVariantMap DeviceHardwareInfo(const QString &id) override;
  QString MakeFriendlyName(const QString &id) override;
  QList<QUrl> MakeDeviceUrls(const QString &id) override;

 public Q_SLOTS:
  void UpdateDeviceFreeSpace(const QString &id) override;
  void UnmountDevice(const QString &id) override;
  void ShutDown() override;

 protected:
  bool Init() override;

 private Q_SLOTS:
  void Poll();

 private:
  struct DeviceInfo {
    DeviceInfo() : volume_serial(0), capacity(0), free_space(0) {}
    QChar drive_letter;
    QString root_path;
    QString label;
    QString filesystem;
    QString vendor;
    QString product;
    QString serial;
    quint32 volume_serial;
    quint64 capacity;
    quint64 free_space;
  };

  static bool ReadDevice(const QChar drive_letter, DeviceInfo &info);
  static QString MakeUniqueId(const DeviceInfo &info);
  static QString MountPath(const DeviceInfo &info);
  static void ReadFreeSpace(DeviceInfo &info);
  static bool DismountAndEjectMedia(const QChar drive_letter);
  // Returns the DEVINST of the device to hand to "Safely Remove Hardware", or 0.
  static unsigned long RemovableAncestor(const QChar drive_letter);
  static bool EjectDevice(const unsigned long dev_inst);

  DeviceInfo LockedDeviceInfo(const QString &id);

  QTimer *timer_poll_;
  QMutex mutex_;
  QMap<QString, DeviceInfo> devices_;
};

#endif  // WINDOWSDEVICELISTER_H
