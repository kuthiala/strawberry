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

#include "config.h"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <ntddstor.h>

#include <string>
#include <vector>

#include <QtGlobal>
#include <QTimer>
#include <QMutexLocker>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

#include "core/logging.h"
#include "windowsdevicelister.h"

using namespace Qt::Literals::StringLiterals;

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kLockRetries = 10;
constexpr DWORD kLockRetryDelayMs = 500;
constexpr int kEjectRetries = 3;

std::wstring RootPath(const QChar drive_letter) {
  return std::wstring(1, static_cast<wchar_t>(drive_letter.unicode())) + L":\\";
}

std::wstring VolumeDevicePath(const QChar drive_letter) {
  return std::wstring(L"\\\\.\\") + static_cast<wchar_t>(drive_letter.unicode()) + L':';
}

// Reads a NUL-terminated ASCII field of a STORAGE_DEVICE_DESCRIPTOR, bounded by the bytes the driver returned.
QString DescriptorField(const QByteArray &buffer, const DWORD returned, const DWORD offset) {

  if (offset == 0 || offset >= returned) return QString();
  const char *field = buffer.constData() + offset;
  return QString::fromLatin1(field, static_cast<qsizetype>(qstrnlen(field, returned - offset))).trimmed();

}

// Returns the STORAGE_DEVICE_NUMBER of the disk holding the volume, used to find the disk in the device tree.
bool VolumeDeviceNumber(const QChar drive_letter, STORAGE_DEVICE_NUMBER &device_number) {

  HANDLE handle = CreateFileW(VolumeDevicePath(drive_letter).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD returned = 0;
  const bool success = DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &device_number, sizeof(device_number), &returned, nullptr);
  CloseHandle(handle);
  return success;

}

// Finds the device instance of the disk with the given device number by walking the disk interfaces.
DEVINST DiskDeviceInstance(const STORAGE_DEVICE_NUMBER &volume_number) {

  HDEVINFO dev_info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE) return 0;

  DEVINST ret = 0;
  SP_DEVICE_INTERFACE_DATA interface_data{};
  interface_data.cbSize = sizeof(interface_data);
  for (DWORD i = 0; ret == 0 && SetupDiEnumDeviceInterfaces(dev_info, nullptr, &GUID_DEVINTERFACE_DISK, i, &interface_data); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(dev_info, &interface_data, nullptr, 0, &size, nullptr);
    if (size == 0) continue;
    std::vector<char> detail_buffer(size);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA devinfo_data{};
    devinfo_data.cbSize = sizeof(devinfo_data);
    if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &interface_data, detail, size, &size, &devinfo_data)) continue;

    HANDLE handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) continue;
    STORAGE_DEVICE_NUMBER disk_number{};
    DWORD returned = 0;
    if (DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &disk_number, sizeof(disk_number), &returned, nullptr) && disk_number.DeviceType == volume_number.DeviceType && disk_number.DeviceNumber == volume_number.DeviceNumber) {
      ret = devinfo_data.DevInst;
    }
    CloseHandle(handle);
  }

  SetupDiDestroyDeviceInfoList(dev_info);

  return ret;

}

}  // namespace

WindowsDeviceLister::WindowsDeviceLister(QObject *parent)
    : DeviceLister(parent),
      timer_poll_(nullptr) {}

WindowsDeviceLister::~WindowsDeviceLister() {

  // Stop the thread first, so Poll() can't run while the derived part of the object is being destroyed.
  StopThread();

}

bool WindowsDeviceLister::Init() {

  // Don't let Windows show "There is no disk in the drive" dialogs when an empty card reader slot is probed.
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);

  timer_poll_ = new QTimer(this);
  timer_poll_->setInterval(kPollIntervalMs);
  QObject::connect(timer_poll_, &QTimer::timeout, this, &WindowsDeviceLister::Poll);

  Poll();
  timer_poll_->start();

  return true;

}

void WindowsDeviceLister::ShutDown() {

  if (timer_poll_) timer_poll_->stop();

}

bool WindowsDeviceLister::ReadDevice(const QChar drive_letter, DeviceInfo &info) {

  const std::wstring root = RootPath(drive_letter);

  const UINT drive_type = GetDriveTypeW(root.c_str());
  if (drive_type != DRIVE_REMOVABLE && drive_type != DRIVE_FIXED) return false;

  // Never list the drive Windows runs from.
  wchar_t windows_dir[MAX_PATH] = {};
  if (GetWindowsDirectoryW(windows_dir, MAX_PATH) > 0 && QChar(windows_dir[0]).toUpper() == drive_letter) return false;

  // Fails when a removable drive (a card reader slot) has no media.
  wchar_t label[MAX_PATH + 1] = {};
  wchar_t filesystem[MAX_PATH + 1] = {};
  DWORD volume_serial = 0;
  if (!GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, &volume_serial, nullptr, nullptr, filesystem, MAX_PATH + 1)) return false;

  STORAGE_BUS_TYPE bus_type = BusTypeUnknown;
  HANDLE handle = CreateFileW(VolumeDevicePath(drive_letter).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    QByteArray buffer(4096, '\0');
    DWORD returned = 0;
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) && returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
      const STORAGE_DEVICE_DESCRIPTOR *descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.constData());
      bus_type = descriptor->BusType;
      info.vendor = DescriptorField(buffer, returned, descriptor->VendorIdOffset);
      info.product = DescriptorField(buffer, returned, descriptor->ProductIdOffset);
      info.serial = DescriptorField(buffer, returned, descriptor->SerialNumberOffset);
    }
    CloseHandle(handle);
  }

  // Fixed drives are only listed when they hang off a hot-pluggable bus (USB hard disks, SD cards).
  if (drive_type == DRIVE_FIXED && bus_type != BusTypeUsb && bus_type != BusTypeSd && bus_type != BusTypeMmc && bus_type != BusType1394) return false;

  info.drive_letter = drive_letter;
  info.root_path = QString(drive_letter) + u":/"_s;
  info.label = QString::fromWCharArray(label);
  info.filesystem = QString::fromWCharArray(filesystem);
  info.volume_serial = static_cast<quint32>(volume_serial);
  ReadFreeSpace(info);

  return true;

}

QString WindowsDeviceLister::MakeUniqueId(const DeviceInfo &info) {

  // An iPod reports its FirewireGuid as the USB serial number, which GPodDevice needs to write a valid iTunesDB on iPod Classics.
  if (!info.serial.isEmpty()) return u"Win/"_s + info.serial;

  return u"Win/volume-"_s + QString::number(info.volume_serial, 16).toUpper();

}

QString WindowsDeviceLister::MountPath(const DeviceInfo &info) {

  // The iPod code appends "/iPod_Control/..." to the mount point (see Song::InitFromItdb()), so an iPod is mounted at "E:" like "/Volumes/IPOD" on macOS, without a trailing slash.
  // Other drives keep "E:/", since a bare "E:" means the current directory on that drive.
  const QString drive = QString(info.drive_letter) + u':';
  return IsIpod(drive) ? drive : info.root_path;

}

void WindowsDeviceLister::ReadFreeSpace(DeviceInfo &info) {

  ULARGE_INTEGER free_bytes{};
  ULARGE_INTEGER total_bytes{};
  if (GetDiskFreeSpaceExW(RootPath(info.drive_letter).c_str(), &free_bytes, &total_bytes, nullptr)) {
    info.capacity = total_bytes.QuadPart;
    info.free_space = free_bytes.QuadPart;
  }

}

void WindowsDeviceLister::Poll() {

  QMap<QString, DeviceInfo> found;
  const DWORD drives = GetLogicalDrives();
  // A: and B: are floppy drive letters, probing them is slow and pointless.
  for (int i = 2; i < 26; ++i) {
    if ((drives & (1UL << i)) == 0) continue;
    DeviceInfo info;
    if (!ReadDevice(QChar(u'A' + i), info)) continue;
    QString id = MakeUniqueId(info);
    // Several partitions of one disk share its serial number.
    if (found.contains(id)) id += u'-' + QString::number(info.volume_serial, 16).toUpper();
    found.insert(id, info);
  }

  QStringList added;
  QStringList removed;
  {
    QMutexLocker l(&mutex_);
    for (auto it = devices_.cbegin(); it != devices_.cend(); ++it) {
      const auto found_it = found.constFind(it.key());
      if (found_it == found.cend()) {
        removed << it.key();
      }
      else if (found_it->root_path != it->root_path) {
        // Same device, new drive letter.
        removed << it.key();
        added << it.key();
      }
    }
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
      if (!devices_.contains(it.key())) added << it.key();
    }
    devices_ = found;
  }

  for (const QString &id : std::as_const(removed)) {
    qLog(Debug) << "Device removed" << id;
    Q_EMIT DeviceRemoved(id);
  }
  for (const QString &id : std::as_const(added)) {
    qLog(Debug) << "Device added" << id << found.value(id).root_path;
    Q_EMIT DeviceAdded(id);
  }

}

WindowsDeviceLister::DeviceInfo WindowsDeviceLister::LockedDeviceInfo(const QString &id) {

  QMutexLocker l(&mutex_);
  return devices_.value(id);

}

QStringList WindowsDeviceLister::DeviceUniqueIDs() {

  QMutexLocker l(&mutex_);
  return devices_.keys();

}

QVariantList WindowsDeviceLister::DeviceIcons(const QString &id) {

  const DeviceInfo info = LockedDeviceInfo(id);
  if (info.root_path.isEmpty()) return QVariantList();

  QVariantList ret;
  ret << GuessIconForPath(MountPath(info));
  ret << GuessIconForModel(info.vendor, info.product);
  ret << u"drive-removable-media-usb"_s;

  return ret;

}

QString WindowsDeviceLister::DeviceManufacturer(const QString &id) {
  return LockedDeviceInfo(id).vendor;
}

QString WindowsDeviceLister::DeviceModel(const QString &id) {
  return LockedDeviceInfo(id).product;
}

quint64 WindowsDeviceLister::DeviceCapacity(const QString &id) {
  return LockedDeviceInfo(id).capacity;
}

quint64 WindowsDeviceLister::DeviceFreeSpace(const QString &id) {
  return LockedDeviceInfo(id).free_space;
}

QVariantMap WindowsDeviceLister::DeviceHardwareInfo(const QString &id) {

  const DeviceInfo info = LockedDeviceInfo(id);
  if (info.root_path.isEmpty()) return QVariantMap();

  QVariantMap ret;
  ret[QStringLiteral(QT_TR_NOOP("Drive"))] = QString(QString(info.drive_letter) + u':');
  ret[QStringLiteral(QT_TR_NOOP("File system"))] = info.filesystem;
  if (!info.serial.isEmpty()) {
    ret[QStringLiteral(QT_TR_NOOP("Serial number"))] = info.serial;
  }

  return ret;

}

QString WindowsDeviceLister::MakeFriendlyName(const QString &id) {

  const DeviceInfo info = LockedDeviceInfo(id);
  if (!info.label.isEmpty()) return info.label;
  if (!info.product.isEmpty()) return info.vendor.isEmpty() ? info.product : info.vendor + u' ' + info.product;

  return QString(info.drive_letter) + u':';

}

QList<QUrl> WindowsDeviceLister::MakeDeviceUrls(const QString &id) {

  const DeviceInfo info = LockedDeviceInfo(id);
  if (info.root_path.isEmpty()) return QList<QUrl>();

  return QList<QUrl>() << MakeUrlFromLocalPath(MountPath(info));

}

void WindowsDeviceLister::UpdateDeviceFreeSpace(const QString &id) {

  {
    QMutexLocker l(&mutex_);
    auto it = devices_.find(id);
    if (it == devices_.end()) return;
    ReadFreeSpace(*it);
  }

  Q_EMIT DeviceChanged(id);

}

bool WindowsDeviceLister::DismountAndEjectMedia(const QChar drive_letter) {

  HANDLE handle = CreateFileW(VolumeDevicePath(drive_letter).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    qLog(Warning) << "Could not open volume" << drive_letter << "for ejecting, error" << GetLastError();
    return false;
  }

  // The lock only succeeds once no other process has a file open on the volume.
  DWORD returned = 0;
  bool locked = false;
  for (int i = 0; i < kLockRetries && !locked; ++i) {
    locked = DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
    if (!locked) Sleep(kLockRetryDelayMs);
  }
  if (!locked) {
    qLog(Warning) << "Volume" << drive_letter << "is in use by another program, not ejecting it";
    CloseHandle(handle);
    return false;
  }

  // Dismounting flushes the file system, after this the device can be pulled without losing data.
  if (!DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr)) {
    qLog(Warning) << "Could not dismount volume" << drive_letter << "error" << GetLastError();
    CloseHandle(handle);
    return false;
  }

  PREVENT_MEDIA_REMOVAL prevent_media_removal{};
  prevent_media_removal.PreventMediaRemoval = FALSE;
  DeviceIoControl(handle, IOCTL_STORAGE_MEDIA_REMOVAL, &prevent_media_removal, sizeof(prevent_media_removal), nullptr, 0, &returned, nullptr);

  // Fails for fixed USB disks, which have no ejectable media; EjectDevice() handles those.
  const bool ejected = DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &returned, nullptr);

  CloseHandle(handle);

  return ejected;

}

unsigned long WindowsDeviceLister::RemovableAncestor(const QChar drive_letter) {

  STORAGE_DEVICE_NUMBER device_number{};
  if (!VolumeDeviceNumber(drive_letter, device_number)) return 0;

  DEVINST dev_inst = DiskDeviceInstance(device_number);

  // Walk up from the disk to the first removable ancestor, normally the USB device itself, which is what "Safely Remove Hardware" ejects.
  while (dev_inst != 0) {
    ULONG capabilities = 0;
    ULONG size = sizeof(capabilities);
    if (CM_Get_DevNode_Registry_PropertyW(dev_inst, CM_DRP_CAPABILITIES, nullptr, &capabilities, &size, 0) == CR_SUCCESS && (capabilities & CM_DEVCAP_REMOVABLE) != 0) {
      return dev_inst;
    }
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, dev_inst, 0) != CR_SUCCESS) return 0;
    dev_inst = parent;
  }

  return 0;

}

bool WindowsDeviceLister::EjectDevice(const unsigned long dev_inst) {

  if (dev_inst == 0) return false;

  for (int i = 0; i < kEjectRetries; ++i) {
    PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
    wchar_t veto_name[MAX_PATH] = {};
    if (CM_Request_Device_EjectW(static_cast<DEVINST>(dev_inst), &veto_type, veto_name, MAX_PATH, 0) == CR_SUCCESS && veto_type == PNP_VetoTypeUnknown) {
      return true;
    }
    qLog(Warning) << "Eject vetoed by" << QString::fromWCharArray(veto_name) << "veto type" << static_cast<int>(veto_type);
    Sleep(kLockRetryDelayMs);
  }

  return false;

}

void WindowsDeviceLister::UnmountDevice(const QString &id) {

  const DeviceInfo info = LockedDeviceInfo(id);
  if (info.root_path.isEmpty()) return;

  qLog(Info) << "Ejecting" << id << "at" << info.root_path;

  // Look up the device tree before dismounting, the volume can't be opened afterwards.
  const unsigned long removable_ancestor = RemovableAncestor(info.drive_letter);
  const bool media_ejected = DismountAndEjectMedia(info.drive_letter);
  const bool device_ejected = EjectDevice(removable_ancestor);

  if (!media_ejected && !device_ejected) {
    qLog(Warning) << "Could not eject" << id << "at" << info.root_path;
  }

  // Report the removal right away instead of on the next poll.
  Poll();

}
