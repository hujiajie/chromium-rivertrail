// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/metro_pin_tab_helper_win.h"

#include <set>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "base/string_number_conversions.h"
#include "base/utf_string_conversions.h"
#include "base/win/metro.h"
#include "chrome/browser/favicon/favicon_tab_helper.h"
#include "chrome/browser/favicon/favicon_util.h"
#include "chrome/browser/ui/tab_contents/tab_contents.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/icon_messages.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "crypto/sha2.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/color_analysis.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/rect.h"
#include "ui/gfx/size.h"

DEFINE_WEB_CONTENTS_USER_DATA_KEY(MetroPinTabHelper)

namespace {

// Generate an ID for the tile based on |url_str|. The ID is simply a hash of
// the URL.
string16 GenerateTileId(const string16& url_str) {
  uint8 hash[crypto::kSHA256Length];
  crypto::SHA256HashString(UTF16ToUTF8(url_str), hash, sizeof(hash));
  std::string hash_str = base::HexEncode(hash, sizeof(hash));
  return UTF8ToUTF16(hash_str);
}

// Get the path of the directory to store the tile logos in.
FilePath GetTileImagesDir() {
  FilePath tile_images_dir;
  if (!PathService::Get(chrome::DIR_USER_DATA, &tile_images_dir))
    return FilePath();

  tile_images_dir = tile_images_dir.Append(L"TileImages");
  if (!file_util::DirectoryExists(tile_images_dir) &&
      !file_util::CreateDirectory(tile_images_dir))
    return FilePath();

  return tile_images_dir;
}

// For the given |image| and |tile_id|, try to create a site specific logo in
// |logo_dir|. The path of any created logo is returned in |logo_path|. Return
// value indicates whether a site specific logo was created.
bool CreateSiteSpecificLogo(const gfx::ImageSkia& image,
                            const string16& tile_id,
                            const FilePath& logo_dir,
                            FilePath* logo_path) {
  const int kLogoWidth = 120;
  const int kLogoHeight = 120;
  const int kBoxWidth = 40;
  const int kBoxHeight = 40;
  const int kCaptionHeight = 20;
  const double kBoxFade = 0.75;
  const int kColorMeanDarknessLimit = 100;
  const int kColorMeanLightnessLimit = 650;

  if (image.isNull())
    return false;

  // First paint the image onto an opaque background to get rid of transparency.
  // White is used as it will be disregarded in the mean calculation because of
  // lightness limit.
  SkPaint paint;
  paint.setColor(SK_ColorWHITE);
  gfx::Canvas favicon_canvas(gfx::Size(image.width(), image.height()),
                             ui::SCALE_FACTOR_100P, true);
  favicon_canvas.DrawRect(gfx::Rect(0, 0, image.width(), image.height()),
                          paint);
  favicon_canvas.DrawImageInt(image, 0, 0);

  // Fill the tile logo with the average color from bitmap. To do this we need
  // to work out the 'average color' which is calculated using PNG encoded data
  // of the bitmap.
  std::vector<unsigned char> icon_png;
  if (!gfx::PNGCodec::EncodeBGRASkBitmap(
          favicon_canvas.ExtractImageRep().sk_bitmap(), false, &icon_png)) {
    return false;
  }

  scoped_refptr<base::RefCountedStaticMemory> icon_mem(
      new base::RefCountedStaticMemory(&icon_png.front(), icon_png.size()));
  color_utils::GridSampler sampler;
  SkColor mean_color = color_utils::CalculateKMeanColorOfPNG(
      icon_mem, kColorMeanDarknessLimit, kColorMeanLightnessLimit, sampler);
  paint.setColor(mean_color);
  gfx::Canvas canvas(gfx::Size(kLogoWidth, kLogoHeight), ui::SCALE_FACTOR_100P,
                     true);
  canvas.DrawRect(gfx::Rect(0, 0, kLogoWidth, kLogoHeight), paint);

  // Now paint a faded square for the favicon to go in.
  color_utils::HSL shift = {-1, -1, kBoxFade};
  paint.setColor(color_utils::HSLShift(mean_color, shift));
  int box_left = (kLogoWidth - kBoxWidth) / 2;
  int box_top = (kLogoHeight - kCaptionHeight - kBoxHeight) / 2;
  canvas.DrawRect(gfx::Rect(box_left, box_top, kBoxWidth, kBoxHeight), paint);

  // Now paint the favicon into the tile, leaving some room at the bottom for
  // the caption.
  int left = (kLogoWidth - image.width()) / 2;
  int top = (kLogoHeight - kCaptionHeight - image.height()) / 2;
  canvas.DrawImageInt(image, left, top);

  SkBitmap logo_bitmap = canvas.ExtractImageRep().sk_bitmap();
  std::vector<unsigned char> logo_png;
  if (!gfx::PNGCodec::EncodeBGRASkBitmap(logo_bitmap, true, &logo_png))
    return false;

  *logo_path = logo_dir.Append(tile_id).ReplaceExtension(L".png");
  return file_util::WriteFile(*logo_path,
                              reinterpret_cast<char*>(&logo_png[0]),
                              logo_png.size()) > 0;
}

// Get the path to the backup logo. If the backup logo already exists in
// |logo_dir|, it will be used, otherwise it will be copied out of the install
// folder. (The version in the install folder is not used as it may disappear
// after an upgrade, causing tiles to lose their images if Windows rebuilds
// its tile image cache.)
// The path to the logo is returned in |logo_path|, with the return value
// indicating success.
bool GetPathToBackupLogo(const FilePath& logo_dir,
                         FilePath* logo_path) {
  const wchar_t kDefaultLogoFileName[] = L"SecondaryTile.png";
  *logo_path = logo_dir.Append(kDefaultLogoFileName);
  if (file_util::PathExists(*logo_path))
    return true;

  FilePath default_logo_path;
  if (!PathService::Get(base::DIR_MODULE, &default_logo_path))
    return false;

  default_logo_path = default_logo_path.Append(kDefaultLogoFileName);
  return file_util::CopyFile(default_logo_path, *logo_path);
}

// The PinPageTaskRunner class performs the necessary FILE thread actions to
// pin a page, such as generating or copying the tile image file. When it
// has performed these actions it will send the tile creation request to the
// metro driver.
class PinPageTaskRunner : public base::RefCountedThreadSafe<PinPageTaskRunner> {
 public:
  // Creates a task runner for the pinning operation with the given details.
  // |favicon| can be a null image (i.e. favicon.isNull() can be true), in
  // which case the backup tile image will be used.
  PinPageTaskRunner(const string16& title,
                    const string16& url,
                    const gfx::ImageSkia& favicon);

  void Run();
  void RunOnFileThread();

 private:
  ~PinPageTaskRunner() {}

  // Details of the page being pinned.
  const string16 title_;
  const string16 url_;
  gfx::ImageSkia favicon_;

  friend class base::RefCountedThreadSafe<PinPageTaskRunner>;
  DISALLOW_COPY_AND_ASSIGN(PinPageTaskRunner);
};

PinPageTaskRunner::PinPageTaskRunner(const string16& title,
                                     const string16& url,
                                     const gfx::ImageSkia& favicon)
    : title_(title),
      url_(url),
      favicon_(favicon) {}

void PinPageTaskRunner::Run() {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  content::BrowserThread::PostTask(
      content::BrowserThread::FILE,
      FROM_HERE,
      base::Bind(&PinPageTaskRunner::RunOnFileThread, this));
}

void PinPageTaskRunner::RunOnFileThread() {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::FILE));

  string16 tile_id = GenerateTileId(url_);
  FilePath logo_dir = GetTileImagesDir();
  if (logo_dir.empty()) {
    LOG(ERROR) << "Could not create directory to store tile image.";
    return;
  }

  FilePath logo_path;
  if (!CreateSiteSpecificLogo(favicon_, tile_id, logo_dir, &logo_path) &&
      !GetPathToBackupLogo(logo_dir, &logo_path)) {
    LOG(ERROR) << "Count not get path to logo tile.";
    return;
  }

  HMODULE metro_module = base::win::GetMetroModule();
  if (!metro_module)
    return;

  typedef void (*MetroPinToStartScreen)(const string16&, const string16&,
      const string16&, const FilePath&);
  MetroPinToStartScreen metro_pin_to_start_screen =
      reinterpret_cast<MetroPinToStartScreen>(
          ::GetProcAddress(metro_module, "MetroPinToStartScreen"));
  if (!metro_pin_to_start_screen) {
    NOTREACHED();
    return;
  }

  metro_pin_to_start_screen(tile_id, title_, url_, logo_path);
}

}  // namespace

class MetroPinTabHelper::FaviconDownloader {
 public:
  FaviconDownloader(MetroPinTabHelper* helper,
                    const string16& title,
                    const string16& url,
                    const gfx::ImageSkia& history_image);
  ~FaviconDownloader() {}

  void Start(content::RenderViewHost* host,
             const std::vector<FaviconURL>& candidates);

  // Callback for when a favicon has been downloaded. The best bitmap so far
  // will be stored in |best_candidate_|. If this is the last URL that was being
  // downloaded, the page is pinned by calling PinPageToStartScreen on the FILE
  // thread.
  void OnDidDownloadFavicon(int id,
                            const GURL& image_url,
                            bool errored,
                            int requested_size,
                            const std::vector<SkBitmap>& bitmaps);

 private:
  // The tab helper that this downloader is operating for.
  MetroPinTabHelper* helper_;

  // Title and URL of the page being pinned.
  const string16 title_;
  const string16 url_;

  // The best candidate we have so far for the current pin operation.
  gfx::ImageSkia best_candidate_;

  // Outstanding favicon download requests.
  std::set<int> in_progress_requests_;

  DISALLOW_COPY_AND_ASSIGN(FaviconDownloader);
};

MetroPinTabHelper::FaviconDownloader::FaviconDownloader(
    MetroPinTabHelper* helper,
    const string16& title,
    const string16& url,
    const gfx::ImageSkia& history_image)
        : helper_(helper),
          title_(title),
          url_(url),
          best_candidate_(history_image) {}

void MetroPinTabHelper::FaviconDownloader::Start(
    content::RenderViewHost* host,
    const std::vector<FaviconURL>& candidates) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  // If there are no candidate URLs, progress straight to pinning.
  if (candidates.empty()) {
    scoped_refptr<PinPageTaskRunner> runner(
        new PinPageTaskRunner(title_, url_, best_candidate_));
    runner->Run();
    helper_->FaviconDownloaderFinished();
    return;
  }

  // Request all the candidates.
  int image_size = 0; // Request the full sized image.
  for (std::vector<FaviconURL>::const_iterator iter = candidates.begin();
       iter != candidates.end();
       ++iter) {
    in_progress_requests_.insert(
        FaviconUtil::DownloadFavicon(host, iter->icon_url, image_size));
  }
}

void MetroPinTabHelper::FaviconDownloader::OnDidDownloadFavicon(
    int id,
    const GURL& image_url,
    bool errored,
    int requested_size,
    const std::vector<SkBitmap>& bitmaps) {
  const int kMaxIconSize = 32;

  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  std::set<int>::iterator iter = in_progress_requests_.find(id);
  // Check that this request is one of ours.
  if (iter == in_progress_requests_.end())
    return;

  in_progress_requests_.erase(iter);

  // Process the bitmaps, keeping the one that is best so far.
   if (!errored) {
    for (std::vector<SkBitmap>::const_iterator iter = bitmaps.begin();
         iter != bitmaps.end();
         ++iter) {

      // If the new bitmap is too big, ignore it.
      if (iter->height() > kMaxIconSize || iter->width() > kMaxIconSize)
        continue;

      // If we don't have a best candidate yet, this is better so just grab it.
      if (best_candidate_.isNull()) {
        best_candidate_ = gfx::ImageSkia(*iter).DeepCopy();
        continue;
      }

      // If it is smaller than our best one so far, ignore it.
      if (iter->height() <= best_candidate_.height() ||
          iter->width() <= best_candidate_.width()) {
        continue;
      }

      // Othewise it is our new best candidate.
      best_candidate_ = gfx::ImageSkia(*iter).DeepCopy();
    }
   }

  // If there are no more outstanding requests, pin the page on the FILE thread.
  // Once this happens this downloader has done its job, so delete it.
  if (in_progress_requests_.empty()) {
    scoped_refptr<PinPageTaskRunner> runner(
        new PinPageTaskRunner(title_, url_, best_candidate_));
    runner->Run();
    helper_->FaviconDownloaderFinished();
  }
}

MetroPinTabHelper::MetroPinTabHelper(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      is_pinned_(false) {}

MetroPinTabHelper::~MetroPinTabHelper() {}

void MetroPinTabHelper::TogglePinnedToStartScreen() {
  UpdatePinnedStateForCurrentURL();
  bool was_pinned = is_pinned_;

  // TODO(benwells): This will update the state incorrectly if the user
  // cancels. To fix this some sort of callback needs to be introduced as
  // the pinning happens on another thread.
  is_pinned_ = !is_pinned_;

  if (was_pinned) {
    UnPinPageFromStartScreen();
    return;
  }

  GURL url = web_contents()->GetURL();
  string16 url_str = UTF8ToUTF16(url.spec());
  string16 title = web_contents()->GetTitle();
  gfx::ImageSkia favicon;
  FaviconTabHelper* favicon_tab_helper = FaviconTabHelper::FromWebContents(
      web_contents());
  if (favicon_tab_helper->FaviconIsValid())
    favicon = favicon_tab_helper->GetFavicon().AsImageSkia().DeepCopy();

  favicon_downloader_.reset(new FaviconDownloader(this, title, url_str,
                                                  favicon));
  favicon_downloader_->Start(web_contents()->GetRenderViewHost(),
                             favicon_url_candidates_);
}

void MetroPinTabHelper::DidNavigateMainFrame(
    const content::LoadCommittedDetails& /*details*/,
    const content::FrameNavigateParams& /*params*/) {
  UpdatePinnedStateForCurrentURL();
  // Cancel any outstanding pin operations once the user navigates away from
  // the page.
  if (favicon_downloader_.get())
    favicon_downloader_.reset();
  // Any candidate favicons we have are now out of date so clear them.
  favicon_url_candidates_.clear();
}

bool MetroPinTabHelper::OnMessageReceived(const IPC::Message& message) {
  bool message_handled = false;   // Allow other handlers to receive these.
  IPC_BEGIN_MESSAGE_MAP(MetroPinTabHelper, message)
    IPC_MESSAGE_HANDLER(IconHostMsg_UpdateFaviconURL, OnUpdateFaviconURL)
    IPC_MESSAGE_HANDLER(IconHostMsg_DidDownloadFavicon, OnDidDownloadFavicon)
    IPC_MESSAGE_UNHANDLED(message_handled = false)
  IPC_END_MESSAGE_MAP()
  return message_handled;
}

void MetroPinTabHelper::OnUpdateFaviconURL(
    int32 page_id,
    const std::vector<FaviconURL>& candidates) {
  favicon_url_candidates_ = candidates;
}

void MetroPinTabHelper::OnDidDownloadFavicon(
    int id,
    const GURL& image_url,
    bool errored,
    int requested_size,
    const std::vector<SkBitmap>& bitmaps) {
  if (favicon_downloader_.get())
    favicon_downloader_->OnDidDownloadFavicon(id, image_url, errored,
                                              requested_size, bitmaps);
}

void MetroPinTabHelper::UpdatePinnedStateForCurrentURL() {
  HMODULE metro_module = base::win::GetMetroModule();
  if (!metro_module)
    return;

  typedef BOOL (*MetroIsPinnedToStartScreen)(const string16&);
  MetroIsPinnedToStartScreen metro_is_pinned_to_start_screen =
      reinterpret_cast<MetroIsPinnedToStartScreen>(
          ::GetProcAddress(metro_module, "MetroIsPinnedToStartScreen"));
  if (!metro_is_pinned_to_start_screen) {
    NOTREACHED();
    return;
  }

  GURL url = web_contents()->GetURL();
  string16 tile_id = GenerateTileId(UTF8ToUTF16(url.spec()));
  is_pinned_ = metro_is_pinned_to_start_screen(tile_id) != 0;
}

void MetroPinTabHelper::UnPinPageFromStartScreen() {
  HMODULE metro_module = base::win::GetMetroModule();
  if (!metro_module)
    return;

  typedef void (*MetroUnPinFromStartScreen)(const string16&);
  MetroUnPinFromStartScreen metro_un_pin_from_start_screen =
      reinterpret_cast<MetroUnPinFromStartScreen>(
          ::GetProcAddress(metro_module, "MetroUnPinFromStartScreen"));
  if (!metro_un_pin_from_start_screen) {
    NOTREACHED();
    return;
  }

  GURL url = web_contents()->GetURL();
  string16 tile_id = GenerateTileId(UTF8ToUTF16(url.spec()));
  metro_un_pin_from_start_screen(tile_id);
}

void MetroPinTabHelper::FaviconDownloaderFinished() {
  favicon_downloader_.reset();
}
