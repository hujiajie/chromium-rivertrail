// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// MediaFileSystemRegistry implementation.

#include "chrome/browser/media_gallery/media_file_system_registry.h"

#include <set>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
#include "base/file_path.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/stl_util.h"
#include "base/string_util.h"
#include "base/system_monitor/system_monitor.h"
#include "base/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/extensions/api/media_galleries_private/media_galleries_private_event_router.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_system.h"
#include "chrome/browser/media_gallery/media_galleries_preferences.h"
#include "chrome/browser/media_gallery/media_galleries_preferences_factory.h"
#include "chrome/browser/prefs/pref_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/system_monitor/media_storage_util.h"
#include "chrome/common/chrome_notification_types.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/extensions/extension_constants.h"
#include "chrome/common/extensions/extension_set.h"
#include "chrome/common/extensions/extension.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "webkit/fileapi/file_system_types.h"
#include "webkit/fileapi/isolated_context.h"

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
#include "chrome/browser/media_gallery/mtp_device_delegate_impl.h"
#include "webkit/fileapi/media/mtp_device_map_service.h"
#endif

using base::Bind;
using base::SystemMonitor;
using content::BrowserThread;
using content::NavigationController;
using content::RenderProcessHost;
using content::WebContents;
using fileapi::IsolatedContext;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
using fileapi::MtpDeviceMapService;
#endif

namespace chrome {

static base::LazyInstance<MediaFileSystemRegistry>::Leaky
    g_media_file_system_registry = LAZY_INSTANCE_INITIALIZER;

namespace {

struct InvalidatedGalleriesInfo {
  std::set<ExtensionGalleriesHost*> extension_hosts;
  std::set<MediaGalleryPrefId> pref_ids;
};

// Make a JSON string out of |name|, |pref_id| and |device_id|. The IDs makes
// the combined name unique. The JSON string should not contain any slashes.
std::string MakeJSONFileSystemName(const string16& name,
                                   const MediaGalleryPrefId& pref_id,
                                   const std::string& device_id) {
  string16 sanitized_name;
  string16 separators =
#if defined(FILE_PATH_USES_WIN_SEPARATORS)
      FilePath::kSeparators
#else
      ASCIIToUTF16(FilePath::kSeparators)
#endif
      ;  // NOLINT
  ReplaceChars(name, separators.c_str(), ASCIIToUTF16("_"), &sanitized_name);

  base::DictionaryValue dict_value;
  dict_value.SetWithoutPathExpansion(
      "name", Value::CreateStringValue(sanitized_name));
  dict_value.SetWithoutPathExpansion("galleryId",
                                     Value::CreateIntegerValue(pref_id));
  // |device_id| can be empty, in which case, just omit it.
  if (!device_id.empty()) {
    dict_value.SetWithoutPathExpansion("deviceId",
                                       Value::CreateStringValue(device_id));
  }

  std::string json_string;
  base::JSONWriter::Write(&dict_value, &json_string);
  return json_string;
}

std::string GetTransientIdForRemovableDeviceId(const std::string& device_id) {
  using extensions::MediaGalleriesPrivateEventRouter;

  if (!MediaStorageUtil::IsRemovableDevice(device_id))
    return std::string();
  return MediaGalleriesPrivateEventRouter::GetTransientIdForDeviceId(device_id);
}

}  // namespace

MediaFileSystemInfo::MediaFileSystemInfo(const std::string& fs_name,
                                         const FilePath& fs_path,
                                         const std::string& filesystem_id)
    : name(fs_name),
      path(fs_path),
      fsid(filesystem_id) {
}

MediaFileSystemInfo::MediaFileSystemInfo() {}

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
// Class to manage MtpDeviceDelegateImpl object for the attached mtp device.
// Refcounted to reuse the same mtp device delegate entry across extensions.
// This class supports WeakPtr (extends SupportsWeakPtr) to expose itself as
// a weak pointer to MediaFileSystemRegistry.
class ScopedMtpDeviceMapEntry
    : public base::RefCounted<ScopedMtpDeviceMapEntry>,
      public base::SupportsWeakPtr<ScopedMtpDeviceMapEntry> {
 public:
  // |no_references_callback| is called when the last ScopedMtpDeviceMapEntry
  // reference goes away.
  ScopedMtpDeviceMapEntry(const FilePath::StringType& device_location,
                          const base::Closure& no_references_callback)
      : device_location_(device_location),
        delegate_(new MtpDeviceDelegateImpl(device_location)),
        no_references_callback_(no_references_callback) {
    DCHECK(delegate_);
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        Bind(&MtpDeviceMapService::AddDelegate,
             base::Unretained(MtpDeviceMapService::GetInstance()),
             device_location_, make_scoped_refptr(delegate_)));
  }

 private:
  // Friend declaration for ref counted implementation.
  friend class base::RefCounted<ScopedMtpDeviceMapEntry>;

  // Private because this class is ref-counted.
  ~ScopedMtpDeviceMapEntry() {
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        Bind(&MtpDeviceMapService::RemoveDelegate,
             base::Unretained(MtpDeviceMapService::GetInstance()),
             device_location_));
    no_references_callback_.Run();
  }

  // Store the mtp or ptp device location.
  const FilePath::StringType device_location_;

  // Store a raw pointer of MtpDeviceDelegateImpl object.
  // MtpDeviceDelegateImpl is ref-counted and owned by MtpDeviceMapService.
  // This class tells MtpDeviceMapService to dispose of it when the last
  // reference to |this| goes away.
  MtpDeviceDelegateImpl* delegate_;

  // A callback to call when the last reference of this object goes away.
  base::Closure no_references_callback_;

  DISALLOW_COPY_AND_ASSIGN(ScopedMtpDeviceMapEntry);
};
#endif

// The main owner of this class is
// |MediaFileSystemRegistry::extension_hosts_map_|, but a callback may
// temporarily hold a reference.
class ExtensionGalleriesHost
    : public base::RefCountedThreadSafe<ExtensionGalleriesHost>,
      public content::NotificationObserver {
 public:
  // |no_references_callback| is called when the last RenderViewHost reference
  // goes away. RenderViewHost references are added through ReferenceFromRVH().
  ExtensionGalleriesHost(MediaFileSystemContext* file_system_context,
                         const base::Closure& no_references_callback)
      : file_system_context_(file_system_context),
        no_references_callback_(no_references_callback) {
  }

  // For each gallery in the list of permitted |galleries|, checks if the
  // device is attached and if so looks up or creates a file system id and
  // passes the information needed for the renderer to create those file
  // system objects to the |callback|.
  void GetMediaFileSystems(const MediaGalleryPrefIdSet& galleries,
                           const MediaGalleriesPrefInfoMap& galleries_info,
                           const MediaFileSystemsCallback& callback) {
    // Extract all the device ids so we can make sure they are attached.
    MediaStorageUtil::DeviceIdSet* device_ids =
        new MediaStorageUtil::DeviceIdSet;
    for (std::set<MediaGalleryPrefId>::const_iterator id = galleries.begin();
         id != galleries.end();
         ++id) {
      device_ids->insert(galleries_info.find(*id)->second.device_id);
    }
    MediaStorageUtil::FilterAttachedDevices(device_ids, base::Bind(
        &ExtensionGalleriesHost::GetMediaFileSystemsForAttachedDevices, this,
        base::Owned(device_ids), galleries, galleries_info, callback));
  }

  void RevokeOldGalleries(const MediaGalleryPrefIdSet& new_galleries) {
    if (new_galleries.size() == pref_id_map_.size())
      return;

    MediaGalleryPrefIdSet old_galleries;
    for (PrefIdFsInfoMap::const_iterator it = pref_id_map_.begin();
         it != pref_id_map_.end();
         ++it) {
      old_galleries.insert(it->first);
    }
    MediaGalleryPrefIdSet invalid_galleries;
    std::set_difference(old_galleries.begin(), old_galleries.end(),
                        new_galleries.begin(), new_galleries.end(),
                        std::inserter(invalid_galleries,
                                      invalid_galleries.begin()));
    for (MediaGalleryPrefIdSet::const_iterator it = invalid_galleries.begin();
         it != invalid_galleries.end();
         ++it) {
      RevokeGalleryByPrefId(*it);
    }
  }

  // TODO(kmadhusu): Clean up this code. http://crbug.com/140340.
  // Revoke the file system for |id| if this extension has created one for |id|.
  void RevokeGalleryByPrefId(MediaGalleryPrefId id) {
    PrefIdFsInfoMap::iterator gallery = pref_id_map_.find(id);
    if (gallery == pref_id_map_.end())
      return;

    file_system_context_->RevokeFileSystem(gallery->second.fsid);
    pref_id_map_.erase(gallery);

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
    MediaDeviceEntryReferencesMap::iterator mtp_device_host =
        media_device_map_references_.find(id);
    if (mtp_device_host != media_device_map_references_.end())
      media_device_map_references_.erase(mtp_device_host);
#endif
  }

  // Indicate that the passed |rvh| will reference the file system ids created
  // by this class.  It is safe to call this multiple times with the same RVH.
  void ReferenceFromRVH(const content::RenderViewHost* rvh) {
    WebContents* contents = WebContents::FromRenderViewHost(rvh);
    if (registrar_.IsRegistered(this,
                                content::NOTIFICATION_WEB_CONTENTS_DESTROYED,
                                content::Source<WebContents>(contents))) {
      return;
    }
    registrar_.Add(this, content::NOTIFICATION_WEB_CONTENTS_DESTROYED,
                   content::Source<WebContents>(contents));
    registrar_.Add(
        this, content::NOTIFICATION_NAV_ENTRY_COMMITTED,
        content::Source<NavigationController>(&contents->GetController()));

    RenderProcessHost* rph = contents->GetRenderProcessHost();
    rph_refs_[rph].insert(contents);
    if (rph_refs_[rph].size() == 1) {
      registrar_.Add(this, content::NOTIFICATION_RENDERER_PROCESS_CLOSED,
                     content::Source<RenderProcessHost>(rph));
    }
  }

 private:
  typedef std::map<MediaGalleryPrefId, MediaFileSystemInfo>
      PrefIdFsInfoMap;
#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  typedef std::map<MediaGalleryPrefId,
                   scoped_refptr<ScopedMtpDeviceMapEntry> >
      MediaDeviceEntryReferencesMap;
#endif
  typedef std::map<const RenderProcessHost*, std::set<const WebContents*> >
      RenderProcessHostRefCount;

  // Private destructor and friend declaration for ref counted implementation.
  friend class base::RefCountedThreadSafe<ExtensionGalleriesHost>;

  virtual ~ExtensionGalleriesHost() {
    DCHECK(rph_refs_.empty());
    DCHECK(pref_id_map_.empty());

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
    DCHECK(media_device_map_references_.empty());
#endif
  }

  // NotificationObserver implementation.
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE {
    switch (type) {
      case content::NOTIFICATION_RENDERER_PROCESS_CLOSED: {
        OnRendererProcessClosed(
            content::Source<RenderProcessHost>(source).ptr());
        break;
      }
      case content::NOTIFICATION_WEB_CONTENTS_DESTROYED: {
        OnWebContentsDestroyedOrNavigated(
            content::Source<WebContents>(source).ptr());
        break;
      }
      case content::NOTIFICATION_NAV_ENTRY_COMMITTED: {
        NavigationController* controller =
            content::Source<NavigationController>(source).ptr();
        WebContents* contents = controller->GetWebContents();
        OnWebContentsDestroyedOrNavigated(contents);
        break;
      }
      default: {
        NOTREACHED();
        break;
      }
    }
  }

  void GetMediaFileSystemsForAttachedDevices(
      const MediaStorageUtil::DeviceIdSet* attached_devices,
      const MediaGalleryPrefIdSet& galleries,
      const MediaGalleriesPrefInfoMap& galleries_info,
      const MediaFileSystemsCallback& callback) {
    std::vector<MediaFileSystemInfo> result;
    MediaGalleryPrefIdSet new_galleries;
    for (std::set<MediaGalleryPrefId>::const_iterator pref_id_it =
             galleries.begin();
         pref_id_it != galleries.end();
         ++pref_id_it) {
      const MediaGalleryPrefId& pref_id = *pref_id_it;
      const MediaGalleryPrefInfo& gallery_info =
          galleries_info.find(pref_id)->second;
      const std::string& device_id = gallery_info.device_id;
      if (!ContainsKey(*attached_devices, device_id))
        continue;

      PrefIdFsInfoMap::const_iterator existing_info =
          pref_id_map_.find(pref_id);
      if (existing_info != pref_id_map_.end()) {
        result.push_back(existing_info->second);
        new_galleries.insert(pref_id);
        continue;
      }

      FilePath path = gallery_info.AbsolutePath();
      if (!path.IsAbsolute())
        continue;

      std::string fsid;
      if (MediaStorageUtil::IsMassStorageDevice(device_id)) {
        fsid = file_system_context_->RegisterFileSystemForMassStorage(
            device_id, path);
      } else {
#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
        scoped_refptr<ScopedMtpDeviceMapEntry> mtp_device_host;
        fsid = file_system_context_->RegisterFileSystemForMtpDevice(
            device_id, path, &mtp_device_host);
        DCHECK(mtp_device_host.get());
        media_device_map_references_[pref_id] = mtp_device_host;
#else
        NOTIMPLEMENTED();
        continue;
#endif
      }
      DCHECK(!fsid.empty());

      MediaFileSystemInfo new_entry(
          MakeJSONFileSystemName(gallery_info.display_name,
                                 pref_id,
                                 GetTransientIdForRemovableDeviceId(device_id)),
          path,
          fsid);
      result.push_back(new_entry);
      new_galleries.insert(pref_id);
      pref_id_map_[pref_id] = new_entry;
    }

    RevokeOldGalleries(new_galleries);

    callback.Run(result);
  }

  void OnRendererProcessClosed(const RenderProcessHost* rph) {
    RenderProcessHostRefCount::const_iterator rph_info = rph_refs_.find(rph);
    DCHECK(rph_info != rph_refs_.end());
    // We're going to remove everything from the set, so we make a copy
    // before operating on it.
    std::set<const WebContents*> closed_web_contents = rph_info->second;
    DCHECK(!closed_web_contents.empty());

    for (std::set<const WebContents*>::const_iterator it =
             closed_web_contents.begin();
         it != closed_web_contents.end();
         ++it) {
      OnWebContentsDestroyedOrNavigated(*it);
    }
  }

  void OnWebContentsDestroyedOrNavigated(const WebContents* contents) {
    registrar_.Remove(this, content::NOTIFICATION_WEB_CONTENTS_DESTROYED,
                      content::Source<WebContents>(contents));
    registrar_.Remove(
        this, content::NOTIFICATION_NAV_ENTRY_COMMITTED,
        content::Source<NavigationController>(&contents->GetController()));

    RenderProcessHost* rph = contents->GetRenderProcessHost();
    RenderProcessHostRefCount::iterator process_refs = rph_refs_.find(rph);
    DCHECK(process_refs != rph_refs_.end());
    process_refs->second.erase(contents);
    if (process_refs->second.empty()) {
      registrar_.Remove(this, content::NOTIFICATION_RENDERER_PROCESS_CLOSED,
                        content::Source<RenderProcessHost>(rph));
      rph_refs_.erase(process_refs);
    }

    if (rph_refs_.empty()) {
      for (PrefIdFsInfoMap::const_iterator it = pref_id_map_.begin();
           it != pref_id_map_.end();
           ++it) {
        file_system_context_->RevokeFileSystem(it->second.fsid);
      }
      pref_id_map_.clear();

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
      media_device_map_references_.clear();
#endif

      no_references_callback_.Run();
    }
  }

  // MediaFileSystemRegistry owns |this| and |file_system_context_|, so it's
  // safe to store a raw pointer.
  MediaFileSystemContext* file_system_context_;

  // A callback to call when the last RVH reference goes away.
  base::Closure no_references_callback_;

  // A map from the gallery preferences id to the file system information.
  PrefIdFsInfoMap pref_id_map_;

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  // A map from the gallery preferences id to the corresponding media device
  // host object.
  MediaDeviceEntryReferencesMap media_device_map_references_;
#endif

  // The set of render processes and web contents that may have references to
  // the file system ids this instance manages.
  RenderProcessHostRefCount rph_refs_;

  // A registrar for listening notifications.
  content::NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(ExtensionGalleriesHost);
};

/******************
 * Public methods
 ******************/

// static
MediaFileSystemRegistry* MediaFileSystemRegistry::GetInstance() {
  return g_media_file_system_registry.Pointer();
}

void MediaFileSystemRegistry::GetMediaFileSystemsForExtension(
    const content::RenderViewHost* rvh,
    const extensions::Extension* extension,
    const MediaFileSystemsCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  Profile* profile =
      Profile::FromBrowserContext(rvh->GetProcess()->GetBrowserContext());
  MediaGalleriesPreferences* preferences = GetPreferences(profile);
  MediaGalleryPrefIdSet galleries =
      preferences->GalleriesForExtension(*extension);

  if (galleries.empty()) {
    callback.Run(std::vector<MediaFileSystemInfo>());
    return;
  }

  if (!ContainsKey(pref_change_registrar_map_, profile)) {
    PrefChangeRegistrar* pref_registrar = new PrefChangeRegistrar;
    pref_registrar->Init(profile->GetPrefs());
    pref_registrar->Add(prefs::kMediaGalleriesRememberedGalleries, this);
    pref_change_registrar_map_[profile] = pref_registrar;
  }

  ExtensionGalleriesHost* extension_host =
      extension_hosts_map_[profile][extension->id()].get();
  if (!extension_host) {
    extension_host = new ExtensionGalleriesHost(
        file_system_context_.get(),
        base::Bind(&MediaFileSystemRegistry::OnExtensionGalleriesHostEmpty,
                   base::Unretained(this), profile, extension->id()));
    extension_hosts_map_[profile][extension->id()] = extension_host;
  }
  extension_host->ReferenceFromRVH(rvh);

  extension_host->GetMediaFileSystems(galleries, preferences->known_galleries(),
                                      callback);
}

MediaGalleriesPreferences* MediaFileSystemRegistry::GetPreferences(
    Profile* profile) {
  MediaGalleriesPreferences* preferences =
      MediaGalleriesPreferencesFactory::GetForProfile(profile);
  if (ContainsKey(extension_hosts_map_, profile))
    return preferences;

  // SystemMonitor may be NULL in unit tests.
  SystemMonitor* system_monitor = SystemMonitor::Get();
  if (!system_monitor)
    return preferences;
  std::vector<SystemMonitor::RemovableStorageInfo> existing_devices =
      system_monitor->GetAttachedRemovableStorage();
  for (size_t i = 0; i < existing_devices.size(); i++) {
    if (!MediaStorageUtil::IsMediaDevice(existing_devices[i].device_id))
      continue;
    preferences->AddGallery(existing_devices[i].device_id,
                            existing_devices[i].name, FilePath(),
                            false /*not user added*/);
  }
  return preferences;
}

void MediaFileSystemRegistry::OnRemovableStorageAttached(
    const std::string& id, const string16& name,
    const FilePath::StringType& location) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (!MediaStorageUtil::IsMediaDevice(id))
    return;

  for (ExtensionGalleriesHostMap::iterator profile_it =
           extension_hosts_map_.begin();
       profile_it != extension_hosts_map_.end();
       ++profile_it) {
    MediaGalleriesPreferences* preferences = GetPreferences(profile_it->first);
    preferences->AddGallery(id, name, FilePath(), false /*not user added*/);
  }
}

void MediaFileSystemRegistry::OnRemovableStorageDetached(
    const std::string& id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Since revoking a gallery in the ExtensionGalleriesHost may cause it
  // to be removed from the map and therefore invalidate any iterator pointing
  // to it, this code first copies all the invalid gallery ids and the
  // extension hosts in which they may appear (per profile) and revoked it in
  // a second step.
  std::vector<InvalidatedGalleriesInfo> invalid_galleries_info;

  for (ExtensionGalleriesHostMap::iterator profile_it =
           extension_hosts_map_.begin();
       profile_it != extension_hosts_map_.end();
       ++profile_it) {
    MediaGalleriesPreferences* preferences = GetPreferences(profile_it->first);
    InvalidatedGalleriesInfo invalid_galleries_in_profile;
    invalid_galleries_in_profile.pref_ids =
        preferences->LookUpGalleriesByDeviceId(id);

    for (ExtensionHostMap::const_iterator extension_host_it =
             profile_it->second.begin();
         extension_host_it != profile_it->second.end();
         ++extension_host_it) {
      invalid_galleries_in_profile.extension_hosts.insert(
          extension_host_it->second.get());
    }

    invalid_galleries_info.push_back(invalid_galleries_in_profile);
  }

  for (size_t i = 0; i < invalid_galleries_info.size(); i++) {
    for (std::set<ExtensionGalleriesHost*>::const_iterator extension_host_it =
             invalid_galleries_info[i].extension_hosts.begin();
         extension_host_it != invalid_galleries_info[i].extension_hosts.end();
         ++extension_host_it) {
      for (std::set<MediaGalleryPrefId>::const_iterator pref_id_it =
               invalid_galleries_info[i].pref_ids.begin();
           pref_id_it != invalid_galleries_info[i].pref_ids.end();
           ++pref_id_it) {
        (*extension_host_it)->RevokeGalleryByPrefId(*pref_id_it);
      }
    }
  }
}

/******************
 * Private methods
 ******************/

class MediaFileSystemRegistry::MediaFileSystemContextImpl
    : public MediaFileSystemContext {
 public:
  explicit MediaFileSystemContextImpl(MediaFileSystemRegistry* registry)
      : registry_(registry) {
    DCHECK(registry_);  // Suppresses unused warning on Android.
  }
  virtual ~MediaFileSystemContextImpl() {}

  // Registers and returns the file system id for the mass storage device
  // specified by |device_id| and |path|.
  virtual std::string RegisterFileSystemForMassStorage(
      const std::string& device_id, const FilePath& path) {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    DCHECK(MediaStorageUtil::IsMassStorageDevice(device_id));

    // Sanity checks for |path|.
    CHECK(path.IsAbsolute());
    CHECK(!path.ReferencesParent());
    std::string fs_name(extension_misc::kMediaFileSystemPathPart);
    const std::string fsid =
        IsolatedContext::GetInstance()->RegisterFileSystemForPath(
            fileapi::kFileSystemTypeNativeMedia, path, &fs_name);
    CHECK(!fsid.empty());
    return fsid;
  }

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
  virtual std::string RegisterFileSystemForMtpDevice(
      const std::string& device_id, const FilePath& path,
      scoped_refptr<ScopedMtpDeviceMapEntry>* entry) {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    DCHECK(!MediaStorageUtil::IsMassStorageDevice(device_id));

    // Sanity checks for |path|.
    CHECK(path.IsAbsolute());
    CHECK(!path.ReferencesParent());
    std::string fs_name(extension_misc::kMediaFileSystemPathPart);
    const std::string fsid =
        IsolatedContext::GetInstance()->RegisterFileSystemForPath(
            fileapi::kFileSystemTypeDeviceMedia, path, &fs_name);
    CHECK(!fsid.empty());
    DCHECK(entry);
    *entry = registry_->GetOrCreateScopedMtpDeviceMapEntry(path.value());
    return fsid;
  }
#endif

  virtual void RevokeFileSystem(const std::string& fsid) {
    IsolatedContext::GetInstance()->RevokeFileSystem(fsid);
  }

 private:
  MediaFileSystemRegistry* registry_;

  DISALLOW_COPY_AND_ASSIGN(MediaFileSystemContextImpl);
};

MediaFileSystemRegistry::MediaFileSystemRegistry()
    : file_system_context_(new MediaFileSystemContextImpl(this)) {
  // SystemMonitor may be NULL in unit tests.
  SystemMonitor* system_monitor = SystemMonitor::Get();
  if (system_monitor)
    system_monitor->AddDevicesChangedObserver(this);
}

MediaFileSystemRegistry::~MediaFileSystemRegistry() {
  // SystemMonitor may be NULL in unit tests.
  SystemMonitor* system_monitor = SystemMonitor::Get();
  if (system_monitor)
    system_monitor->RemoveDevicesChangedObserver(this);
}

void MediaFileSystemRegistry::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  DCHECK_EQ(chrome::NOTIFICATION_PREF_CHANGED, type);
  const std::string& pref_name =
      *content::Details<std::string>(details).ptr();
  DCHECK_EQ(std::string(prefs::kMediaGalleriesRememberedGalleries), pref_name);

  // Find the Profile that contains the source PrefService.
  PrefService* prefs = content::Source<PrefService>(source).ptr();
  PrefChangeRegistrarMap::iterator pref_change_it =
      pref_change_registrar_map_.begin();
  for (; pref_change_it != pref_change_registrar_map_.end(); ++pref_change_it) {
    if (pref_change_it->first->GetPrefs() == prefs)
      break;
  }
  DCHECK(pref_change_it != pref_change_registrar_map_.end());
  Profile* profile = pref_change_it->first;

  // Get the Extensions, MediaGalleriesPreferences and ExtensionHostMap for
  // |profile|.
  const ExtensionService* extension_service =
      extensions::ExtensionSystem::Get(profile)->extension_service();
  const ExtensionSet* extensions_set = extension_service->extensions();
  const MediaGalleriesPreferences* preferences = GetPreferences(profile);
  ExtensionGalleriesHostMap::const_iterator host_map_it =
      extension_hosts_map_.find(profile);
  DCHECK(host_map_it != extension_hosts_map_.end());
  const ExtensionHostMap& extension_host_map = host_map_it->second;

  // Go through ExtensionsHosts, get the updated galleries list and use it to
  // revoke the old galleries.
  for (ExtensionHostMap::const_iterator gallery_host_it =
           extension_host_map.begin();
       gallery_host_it != extension_host_map.end();
       ++gallery_host_it) {
    const extensions::Extension* extension =
        extensions_set->GetByID(gallery_host_it->first);
    gallery_host_it->second->RevokeOldGalleries(
        preferences->GalleriesForExtension(*extension));
  }
}

#if defined(SUPPORT_MTP_DEVICE_FILESYSTEM)
ScopedMtpDeviceMapEntry*
MediaFileSystemRegistry::GetOrCreateScopedMtpDeviceMapEntry(
    const FilePath::StringType& device_location) {
  MTPDeviceDelegateMap::iterator delegate_it =
      mtp_delegate_map_.find(device_location);
  if (delegate_it != mtp_delegate_map_.end() && delegate_it->second.get())
    return delegate_it->second;
  ScopedMtpDeviceMapEntry* mtp_device_host = new ScopedMtpDeviceMapEntry(
      device_location, base::Bind(
          &MediaFileSystemRegistry::RemoveScopedMtpDeviceMapEntry,
          base::Unretained(this), device_location));
  mtp_delegate_map_[device_location] = mtp_device_host->AsWeakPtr();
  return mtp_device_host;
}

void MediaFileSystemRegistry::RemoveScopedMtpDeviceMapEntry(
    const FilePath::StringType& device_location) {
  MTPDeviceDelegateMap::iterator delegate_it =
      mtp_delegate_map_.find(device_location);
  DCHECK(delegate_it != mtp_delegate_map_.end());
  mtp_delegate_map_.erase(delegate_it);
}
#endif

void MediaFileSystemRegistry::OnExtensionGalleriesHostEmpty(
    Profile* profile, const std::string& extension_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  ExtensionGalleriesHostMap::iterator extension_hosts =
      extension_hosts_map_.find(profile);
  DCHECK(extension_hosts != extension_hosts_map_.end());
  ExtensionHostMap::size_type erase_count =
      extension_hosts->second.erase(extension_id);
  DCHECK_EQ(1U, erase_count);
  if (extension_hosts->second.empty()) {
    extension_hosts_map_.erase(extension_hosts);

    PrefChangeRegistrarMap::iterator pref_it =
        pref_change_registrar_map_.find(profile);
    DCHECK(pref_it != pref_change_registrar_map_.end());
    delete pref_it->second;
    pref_change_registrar_map_.erase(pref_it);
  }
}

}  // namespace chrome
