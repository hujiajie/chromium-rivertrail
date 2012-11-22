// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// MediaFileSystemRegistry registers pictures directories and media devices as
// File API filesystems and keeps track of the path to filesystem ID mappings.

#ifndef CHROME_BROWSER_MEDIA_GALLERY_MEDIA_FILE_SYSTEM_REGISTRY_H_
#define CHROME_BROWSER_MEDIA_GALLERY_MEDIA_FILE_SYSTEM_REGISTRY_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/lazy_instance.h"
#include "base/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/prefs/public/pref_change_registrar.h"
#include "base/system_monitor/system_monitor.h"
#include "content/public/browser/notification_observer.h"
#include "webkit/fileapi/media/mtp_device_file_system_config.h"

class Profile;

namespace content {
class RenderViewHost;
}

namespace extensions {
class Extension;
}

namespace fileapi {
class IsolatedContext;
}

namespace chrome {

class ExtensionGalleriesHost;
class MediaGalleriesPreferences;
class ScopedMtpDeviceMapEntry;

struct MediaFileSystemInfo {
  MediaFileSystemInfo(const std::string& fs_name,
                      const FilePath& fs_path,
                      const std::string& filesystem_id);
  MediaFileSystemInfo();

  std::string name;  // JSON string, must not contain slashes.
  FilePath path;
  std::string fsid;
};

class MediaFileSystemContext {
 public:
  virtual ~MediaFileSystemContext() {}

  // Register a media file system (filtered to media files) for |path| and
  // return the new file system id.
  virtual std::string RegisterFileSystemForMassStorage(
      const std::string& device_id, const FilePath& path) = 0;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  // Registers and returns the file system id for the mtp or ptp device
  // specified by |device_id| and |path|. Updates |entry| with the corresponding
  // ScopedMtpDeviceMapEntry object.
  virtual std::string RegisterFileSystemForMtpDevice(
      const std::string& device_id, const FilePath& path,
      scoped_refptr<ScopedMtpDeviceMapEntry>* entry) = 0;
#endif

  // Revoke the passed |fsid|.
  virtual void RevokeFileSystem(const std::string& fsid) = 0;
};

typedef base::Callback<void(const std::vector<MediaFileSystemInfo>&)>
    MediaFileSystemsCallback;

class MediaFileSystemRegistry
    : public base::SystemMonitor::DevicesChangedObserver,
      public content::NotificationObserver {
 public:
  // The instance is lazily created per browser process.
  static MediaFileSystemRegistry* GetInstance();

  // Passes to |callback| the list of media filesystem IDs and paths for a
  // given RVH. Called on the UI thread.
  void GetMediaFileSystemsForExtension(
      const content::RenderViewHost* rvh,
      const extensions::Extension* extension,
      const MediaFileSystemsCallback& callback);

  // Returns the initialized media galleries preferences for the specified
  // |profile|. This method should be used instead of calling
  // MediaGalleriesPreferences directly because this method also ensures that
  // currently attached removable devices are added to the preferences.
  // Called on the UI thread.
  MediaGalleriesPreferences* GetPreferences(Profile* profile);

  // base::SystemMonitor::DevicesChangedObserver implementation.
  virtual void OnRemovableStorageAttached(
      const std::string& id,
      const string16& name,
      const FilePath::StringType& location) OVERRIDE;
  virtual void OnRemovableStorageDetached(const std::string& id) OVERRIDE;

 private:
  friend struct base::DefaultLazyInstanceTraits<MediaFileSystemRegistry>;
  class MediaFileSystemContextImpl;

  // Map an extension to the ExtensionGalleriesHost.
  typedef std::map<std::string /*extension_id*/,
                   scoped_refptr<ExtensionGalleriesHost> > ExtensionHostMap;
  // Map a profile and extension to the ExtensionGalleriesHost.
  typedef std::map<Profile*, ExtensionHostMap> ExtensionGalleriesHostMap;
  // Map a profile to a PrefChangeRegistrar.
  typedef std::map<Profile*, PrefChangeRegistrar*> PrefChangeRegistrarMap;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  // Map a mtp or ptp device location to the weak pointer of
  // ScopedMtpDeviceMapEntry.
  typedef std::map<const FilePath::StringType,
                   base::WeakPtr<ScopedMtpDeviceMapEntry> >
      MTPDeviceDelegateMap;
#endif

  // Obtain an instance of this class via GetInstance().
  MediaFileSystemRegistry();
  virtual ~MediaFileSystemRegistry();

  // NotificationObserver implementation.
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  // Returns ScopedMtpDeviceMapEntry object for the given |device_location|.
  ScopedMtpDeviceMapEntry* GetOrCreateScopedMtpDeviceMapEntry(
      const FilePath::StringType& device_location);

  // Removes the ScopedMtpDeviceMapEntry associated with the given
  // |device_location|.
  void RemoveScopedMtpDeviceMapEntry(
      const FilePath::StringType& device_location);
#endif

  void OnExtensionGalleriesHostEmpty(Profile* profile,
                                     const std::string& extension_id);

  // Only accessed on the UI thread. This map owns all the
  // ExtensionGalleriesHost objects created.
  ExtensionGalleriesHostMap extension_hosts_map_;

  PrefChangeRegistrarMap pref_change_registrar_map_;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  // Only accessed on the UI thread.
  MTPDeviceDelegateMap mtp_delegate_map_;
#endif

  scoped_ptr<MediaFileSystemContext> file_system_context_;

  DISALLOW_COPY_AND_ASSIGN(MediaFileSystemRegistry);
};

}  // namespace chrome

#endif  // CHROME_BROWSER_MEDIA_GALLERY_MEDIA_FILE_SYSTEM_REGISTRY_H_
