// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/file_system_access/electron_file_system_access_permission_context.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/values_util.h"
#include "base/metrics/histogram_functions.h"
#include "base/notreached.h"
#include "base/path_service.h"
#include "base/ranges/algorithm.h"
#include "base/strings/strcat.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/default_clock.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/file_system_access/file_system_access_permission_request_manager.h"
#include "chrome/browser/permissions/permission_decision_auto_blocker_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/safe_browsing/download_protection/download_protection_util.h"
#include "chrome/browser/ui/file_system_access_dialogs.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pdf_util.h"
#include "chrome/grit/generated_resources.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/permissions/features.h"
#include "components/permissions/object_permission_context_base.h"
#include "components/permissions/permission_decision_auto_blocker.h"
#include "components/permissions/permission_uma_util.h"
#include "components/permissions/permission_util.h"
#include "components/safe_browsing/buildflags.h"
#include "components/safe_browsing/content/common/file_type_policies.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/disallow_activation_reason.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/origin.h"

namespace {

using FileRequestData =
    FileSystemAccessPermissionRequestManager::FileRequestData;
using RequestAccess = FileSystemAccessPermissionRequestManager::Access;
using HandleType = content::FileSystemAccessPermissionContext::HandleType;
using GrantType = ElectronFileSystemAccessPermissionContext::GrantType;
using blink::mojom::PermissionStatus;
using permissions::PermissionAction;

// This long after the last top-level tab or window for an origin is closed (or
// is navigated to another origin), all the permissions for that origin will be
// revoked.
constexpr base::TimeDelta kPermissionRevocationTimeout = base::Seconds(5);

// Dictionary keys for the FILE_SYSTEM_ACCESS_CHOOSER_DATA setting.
// `kPermissionPathKey[] = "path"` is defined in the header file.
const char kPermissionIsDirectoryKey[] = "is-directory";
const char kPermissionWritableKey[] = "writable";
const char kPermissionReadableKey[] = "readable";
const char kDeprecatedPermissionLastUsedTimeKey[] = "time";

// Dictionary keys for the FILE_SYSTEM_LAST_PICKED_DIRECTORY website setting.
// Schema (per origin):
// {
//  ...
//   {
//     "default-id" : { "path" : <path> , "path-type" : <type>}
//     "custom-id-fruit" : { "path" : <path> , "path-type" : <type> }
//     "custom-id-flower" : { "path" : <path> , "path-type" : <type> }
//     ...
//   }
//  ...
// }
const char kDefaultLastPickedDirectoryKey[] = "default-id";
const char kCustomLastPickedDirectoryKey[] = "custom-id";
const char kPathKey[] = "path";
const char kPathTypeKey[] = "path-type";
const char kTimestampKey[] = "timestamp";

void ShowFileSystemAccessRestrictedDirectoryDialogOnUIThread(
    content::GlobalRenderFrameHostId frame_id,
    const url::Origin& origin,
    HandleType handle_type,
    base::OnceCallback<
        void(ElectronFileSystemAccessPermissionContext::SensitiveEntryResult)>
        callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(frame_id);
  if (!rfh || !rfh->IsActive()) {
    // Requested from a no longer valid RenderFrameHost.
    std::move(callback).Run(ElectronFileSystemAccessPermissionContext::
                                SensitiveEntryResult::kAbort);
    return;
  }

  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(rfh);
  if (!web_contents) {
    // Requested from a worker, or a no longer existing tab.
    std::move(callback).Run(ElectronFileSystemAccessPermissionContext::
                                SensitiveEntryResult::kAbort);
    return;
  }

  ShowFileSystemAccessRestrictedDirectoryDialog(
      origin, handle_type, std::move(callback), web_contents);
}

void ShowFileSystemAccessDangerousFileDialogOnUIThread(
    content::GlobalRenderFrameHostId frame_id,
    const url::Origin& origin,
    const base::FilePath& path,
    base::OnceCallback<
        void(ElectronFileSystemAccessPermissionContext::SensitiveEntryResult)>
        callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(frame_id);
  if (!rfh || !rfh->IsActive()) {
    // Requested from a no longer valid RenderFrameHost.
    std::move(callback).Run(ElectronFileSystemAccessPermissionContext::
                                SensitiveEntryResult::kAbort);
    return;
  }

  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(rfh);
  if (!web_contents) {
    // Requested from a worker, or a no longer existing tab.
    std::move(callback).Run(ElectronFileSystemAccessPermissionContext::
                                SensitiveEntryResult::kAbort);
    return;
  }

  ShowFileSystemAccessDangerousFileDialog(origin, path, std::move(callback),
                                          web_contents);
}

#if BUILDFLAG(IS_WIN)
bool ContainsInvalidDNSCharacter(base::FilePath::StringType hostname) {
  for (base::FilePath::CharType c : hostname) {
    if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
          (c >= L'0' && c <= L'9') || (c == L'.') || (c == L'-'))) {
      return true;
    }
  }
  return false;
}

bool MaybeIsLocalUNCPath(const base::FilePath& path) {
  if (!path.IsNetwork()) {
    return false;
  }

  const std::vector<base::FilePath::StringType> components =
      path.GetComponents();

  // Check for server name that could represent a local system. We only
  // check for a very short list, as it is impossible to cover all different
  // variants on Windows.
  if (components.size() >= 2 &&
      (base::FilePath::CompareEqualIgnoreCase(components[1],
                                              FILE_PATH_LITERAL("localhost")) ||
       components[1] == FILE_PATH_LITERAL("127.0.0.1") ||
       components[1] == FILE_PATH_LITERAL(".") ||
       components[1] == FILE_PATH_LITERAL("?") ||
       ContainsInvalidDNSCharacter(components[1]))) {
    return true;
  }

  // In case we missed the server name check above, we also check for shares
  // ending with '$' as they represent pre-defined shares, including the local
  // drives.
  for (size_t i = 2; i < components.size(); ++i) {
    if (components[i].back() == L'$') {
      return true;
    }
  }

  return false;
}
#endif

// Sentinel used to indicate that no PathService key is specified for a path in
// the struct below.
constexpr const int kNoBasePathKey = -1;

enum BlockType {
  kBlockAllChildren,
  kBlockNestedDirectories,
  kDontBlockChildren
};

const struct {
  // base::BasePathKey value (or one of the platform specific extensions to it)
  // for a path that should be blocked. Specify kNoBasePathKey if |path| should
  // be used instead.
  int base_path_key;

  // Explicit path to block instead of using |base_path_key|. Set to nullptr to
  // use |base_path_key| on its own. If both |base_path_key| and |path| are set,
  // |path| is treated relative to the path |base_path_key| resolves to.
  const base::FilePath::CharType* path;

  // If this is set to kDontBlockChildren, only the given path and its parents
  // are blocked. If this is set to kBlockAllChildren, all children of the given
  // path are blocked as well. Finally if this is set to kBlockNestedDirectories
  // access is allowed to individual files in the directory, but nested
  // directories are still blocked.
  // The BlockType of the nearest ancestor of a path to check is what ultimately
  // determines if a path is blocked or not. If a blocked path is a descendent
  // of another blocked path, then it may override the child-blocking policy of
  // its ancestor. For example, if /home blocks all children, but
  // /home/downloads does not, then /home/downloads/file.ext will *not* be
  // blocked.
  BlockType type;
} kBlockedPaths[] = {
    // Don't allow users to share their entire home directory, entire desktop or
    // entire documents folder, but do allow sharing anything inside those
    // directories not otherwise blocked.
    {base::DIR_HOME, nullptr, kDontBlockChildren},
    {base::DIR_USER_DESKTOP, nullptr, kDontBlockChildren},
    {chrome::DIR_USER_DOCUMENTS, nullptr, kDontBlockChildren},
    // Similar restrictions for the downloads directory.
    {chrome::DIR_DEFAULT_DOWNLOADS, nullptr, kDontBlockChildren},
    {chrome::DIR_DEFAULT_DOWNLOADS_SAFE, nullptr, kDontBlockChildren},
    // The Chrome installation itself should not be modified by the web.
    {base::DIR_EXE, nullptr, kBlockAllChildren},
#if !BUILDFLAG(IS_FUCHSIA)
    {base::DIR_MODULE, nullptr, kBlockAllChildren},
#endif
    {base::DIR_ASSETS, nullptr, kBlockAllChildren},
    // And neither should the configuration of at least the currently running
    // Chrome instance (note that this does not take --user-data-dir command
    // line overrides into account).
    {chrome::DIR_USER_DATA, nullptr, kBlockAllChildren},
    // ~/.ssh is pretty sensitive on all platforms, so block access to that.
    {base::DIR_HOME, FILE_PATH_LITERAL(".ssh"), kBlockAllChildren},
    // And limit access to ~/.gnupg as well.
    {base::DIR_HOME, FILE_PATH_LITERAL(".gnupg"), kBlockAllChildren},
#if BUILDFLAG(IS_WIN)
    // Some Windows specific directories to block, basically all apps, the
    // operating system itself, as well as configuration data for apps.
    {base::DIR_PROGRAM_FILES, nullptr, kBlockAllChildren},
    {base::DIR_PROGRAM_FILESX86, nullptr, kBlockAllChildren},
    {base::DIR_PROGRAM_FILES6432, nullptr, kBlockAllChildren},
    {base::DIR_WINDOWS, nullptr, kBlockAllChildren},
    {base::DIR_ROAMING_APP_DATA, nullptr, kBlockAllChildren},
    {base::DIR_LOCAL_APP_DATA, nullptr, kBlockAllChildren},
    {base::DIR_COMMON_APP_DATA, nullptr, kBlockAllChildren},
    // Opening a file from an MTP device, such as a smartphone or a camera, is
    // implemented by Windows as opening a file in the temporary internet files
    // directory. To support that, allow opening files in that directory, but
    // not whole directories.
    {base::DIR_IE_INTERNET_CACHE, nullptr, kBlockNestedDirectories},
#endif
#if BUILDFLAG(IS_MAC)
    // Similar Mac specific blocks.
    {base::DIR_APP_DATA, nullptr, kBlockAllChildren},
    {base::DIR_HOME, FILE_PATH_LITERAL("Library"), kBlockAllChildren},
    // Allow access to other cloud files, such as Google Drive.
    {base::DIR_HOME, FILE_PATH_LITERAL("Library/CloudStorage"),
     kDontBlockChildren},
    // Allow the site to interact with data from its corresponding natively
    // installed (sandboxed) application. It would be nice to limit a site to
    // access only _its_ corresponding natively installed application,
    // but unfortunately there's no straightforward way to do that. See
    // https://crbug.com/984641#c22.
    {base::DIR_HOME, FILE_PATH_LITERAL("Library/Containers"),
     kDontBlockChildren},
    // Allow access to iCloud files...
    {base::DIR_HOME, FILE_PATH_LITERAL("Library/Mobile Documents"),
     kDontBlockChildren},
    // ... which may also appear at this directory.
    {base::DIR_HOME,
     FILE_PATH_LITERAL("Library/Mobile Documents/com~apple~CloudDocs"),
     kDontBlockChildren},
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    // On Linux also block access to devices via /dev.
    {kNoBasePathKey, FILE_PATH_LITERAL("/dev"), kBlockAllChildren},
    // And security sensitive data in /proc and /sys.
    {kNoBasePathKey, FILE_PATH_LITERAL("/proc"), kBlockAllChildren},
    {kNoBasePathKey, FILE_PATH_LITERAL("/sys"), kBlockAllChildren},
    // And system files in /boot and /etc.
    {kNoBasePathKey, FILE_PATH_LITERAL("/boot"), kBlockAllChildren},
    {kNoBasePathKey, FILE_PATH_LITERAL("/etc"), kBlockAllChildren},
    // And block all of ~/.config, matching the similar restrictions on mac
    // and windows.
    {base::DIR_HOME, FILE_PATH_LITERAL(".config"), kBlockAllChildren},
    // Block ~/.dbus as well, just in case, although there probably isn't much a
    // website can do with access to that directory and its contents.
    {base::DIR_HOME, FILE_PATH_LITERAL(".dbus"), kBlockAllChildren},
#endif
    // TODO(https://crbug.com/984641): Refine this list, for example add
    // XDG_CONFIG_HOME when it is not set ~/.config?
};

// Describes a rule for blocking a directory, which can be constructed
// dynamically (based on state) or statically (from kBlockedPaths).
struct BlockPathRule {
  base::FilePath path;
  BlockType type;
};

bool ShouldBlockAccessToPath(const base::FilePath& path,
                             HandleType handle_type,
                             std::vector<BlockPathRule> rules) {
  DCHECK(!path.empty());
  DCHECK(path.IsAbsolute());

  base::FilePath check_path = path;
  if (base::FeatureList::IsEnabled(
          permissions::features::kFileSystemAccessSymbolicLinkCheck)) {
    // `path` is expected to be absolute, but call `MakeAbsoluteFilePath()`
    // in order to perform normalization, such as resolving any symbolic link.
    check_path = base::MakeAbsoluteFilePath(path);
    if (check_path.empty()) {
      check_path = path;
    }
  } else {
    check_path = path;
  }

#if BUILDFLAG(IS_WIN)
  // On Windows, local UNC paths are rejected, as UNC path can be written in a
  // way that can bypass the blocklist.
  if (base::FeatureList::IsEnabled(
          features::kFileSystemAccessLocalUNCPathBlock) &&
      MaybeIsLocalUNCPath(check_path)) {
    return true;
  }
#endif

  // Add the hard-coded rules to the dynamic rules.
  for (const auto& block : kBlockedPaths) {
    base::FilePath blocked_path;
    if (block.base_path_key != kNoBasePathKey) {
      if (!base::PathService::Get(block.base_path_key, &blocked_path)) {
        continue;
      }
      if (block.path) {
        blocked_path = blocked_path.Append(block.path);
      }
    } else {
      DCHECK(block.path);
      blocked_path = base::FilePath(block.path);
    }
    rules.emplace_back(blocked_path, block.type);
  }

  base::FilePath nearest_ancestor;
  BlockType nearest_ancestor_block_type = kDontBlockChildren;
  for (const auto& block : rules) {
    if (check_path == block.path || check_path.IsParent(block.path)) {
      VLOG(1) << "Blocking access to " << check_path
              << " because it is a parent of " << block.path;
      return true;
    }

    if (block.path.IsParent(check_path) &&
        (nearest_ancestor.empty() || nearest_ancestor.IsParent(block.path))) {
      nearest_ancestor = block.path;
      nearest_ancestor_block_type = block.type;
    }
  }

  // The path we're checking is not in a potentially blocked directory, or the
  // nearest ancestor does not block access to its children. Grant access.
  if (nearest_ancestor.empty() ||
      nearest_ancestor_block_type == kDontBlockChildren) {
    return false;
  }

  // The path we're checking is a file, and the nearest ancestor only blocks
  // access to directories. Grant access.
  if (handle_type == HandleType::kFile &&
      nearest_ancestor_block_type == kBlockNestedDirectories) {
    return false;
  }

  // The nearest ancestor blocks access to its children, so block access.
  VLOG(1) << "Blocking access to " << check_path << " because it is inside "
          << nearest_ancestor;
  return true;
}

std::string GenerateLastPickedDirectoryKey(const std::string& id) {
  return id.empty() ? kDefaultLastPickedDirectoryKey
                    : base::StrCat({kCustomLastPickedDirectoryKey, "-", id});
}

std::string_view PathAsPermissionKey(const base::FilePath& path) {
  return std::string_view(
      reinterpret_cast<const char*>(path.value().data()),
      path.value().size() * sizeof(base::FilePath::CharType));
}

std::string_view GetGrantKeyFromGrantType(GrantType type) {
  return type == GrantType::kWrite ? kPermissionWritableKey
                                   : kPermissionReadableKey;
}

bool FileHasDangerousExtension(const url::Origin& origin,
                               const base::FilePath& path,
                               Profile* profile) {
  safe_browsing::DownloadFileType::DangerLevel danger_level =
      safe_browsing::FileTypePolicies::GetInstance()->GetFileDangerLevel(
          path, origin.GetURL(), profile->GetPrefs());
  // See https://crbug.com/1320877#c4 for justification for why we show the
  // prompt if `danger_level` is ALLOW_ON_USER_GESTURE as well as DANGEROUS.
  return danger_level == safe_browsing::DownloadFileType::DANGEROUS ||
         danger_level == safe_browsing::DownloadFileType::ALLOW_ON_USER_GESTURE;
}

}  // namespace

ElectronFileSystemAccessPermissionContext::Grants::Grants() = default;
ElectronFileSystemAccessPermissionContext::Grants::~Grants() = default;
ElectronFileSystemAccessPermissionContext::Grants::Grants(Grants&&) = default;
ElectronFileSystemAccessPermissionContext::Grants&
ElectronFileSystemAccessPermissionContext::Grants::operator=(Grants&&) =
    default;

class ElectronFileSystemAccessPermissionContext::PermissionGrantImpl
    : public content::FileSystemAccessPermissionGrant {
 public:
  PermissionGrantImpl(
      base::WeakPtr<ElectronFileSystemAccessPermissionContext> context,
      const url::Origin& origin,
      const base::FilePath& path,
      HandleType handle_type,
      GrantType type,
      UserAction user_action)
      : context_(std::move(context)),
        origin_(origin),
        handle_type_(handle_type),
        type_(type),
        path_(path),
        user_action_(user_action) {}

  // FileSystemAccessPermissionGrant:
  PermissionStatus GetStatus() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return status_;
  }

  base::FilePath GetPath() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return path_;
  }

  void RequestPermission(
      content::GlobalRenderFrameHostId frame_id,
      UserActivationState user_activation_state,
      base::OnceCallback<void(PermissionRequestOutcome)> callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    // Check if a permission request has already been processed previously. This
    // check is done first because we don't want to reset the status of a
    // permission if it has already been granted.
    if (GetStatus() != PermissionStatus::ASK || !context_) {
      if (GetStatus() == PermissionStatus::GRANTED) {
        SetStatus(PermissionStatus::GRANTED);
      }
      std::move(callback).Run(PermissionRequestOutcome::kRequestAborted);
      return;
    }

    content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(frame_id);
    if (!rfh) {
      // Requested from a no longer valid RenderFrameHost.
      std::move(callback).Run(PermissionRequestOutcome::kInvalidFrame);
      return;
    }

    // Don't show request permission UI for an inactive RenderFrameHost as the
    // page might not distinguish properly between user denying the permission
    // and automatic rejection, leading to an inconsistent UX once the page
    // becomes active again.
    // - If this is called when RenderFrameHost is in BackForwardCache, evict
    //   the document from the cache.
    // - If this is called when RenderFrameHost is in prerendering, cancel
    //   prerendering.
    if (rfh->IsInactiveAndDisallowActivation(
            content::DisallowActivationReasonId::
                kFileSystemAccessPermissionRequest)) {
      std::move(callback).Run(PermissionRequestOutcome::kInvalidFrame);
      return;
    }
    // We don't allow file system access from fenced frames.
    if (rfh->IsNestedWithinFencedFrame()) {
      std::move(callback).Run(PermissionRequestOutcome::kInvalidFrame);
      return;
    }

    if (user_activation_state == UserActivationState::kRequired &&
        !rfh->HasTransientUserActivation()) {
      // No permission prompts without user activation.
      std::move(callback).Run(PermissionRequestOutcome::kNoUserActivation);
      return;
    }

    content::WebContents* web_contents =
        content::WebContents::FromRenderFrameHost(rfh);
    if (!web_contents) {
      // Requested from a worker, or a no longer existing tab.
      std::move(callback).Run(PermissionRequestOutcome::kInvalidFrame);
      return;
    }

    url::Origin embedding_origin = url::Origin::Create(
        permissions::PermissionUtil::GetLastCommittedOriginAsURL(
            rfh->GetMainFrame()));
    if (embedding_origin != origin_) {
      // Third party iframes are not allowed to request more permissions.
      std::move(callback).Run(PermissionRequestOutcome::kThirdPartyContext);
      return;
    }

    auto* request_manager =
        FileSystemAccessPermissionRequestManager::FromWebContents(web_contents);
    if (!request_manager) {
      std::move(callback).Run(PermissionRequestOutcome::kRequestAborted);
      return;
    }

    // If a website wants both read and write access, code in content will
    // request those as two separate requests. The |request_manager| will then
    // detect this and combine the two requests into one prompt. As such this
    // code does not have to have any way to request Access::kReadWrite.
    FileRequestData file_request_data = {path_, handle_type_,
                                         type_ == GrantType::kRead
                                             ? RequestAccess::kRead
                                             : RequestAccess::kWrite};
    request_manager->AddRequest(
        {FileSystemAccessPermissionRequestManager::RequestType::kNewPermission,
         origin_,
         {file_request_data}},
        base::BindOnce(&PermissionGrantImpl::OnPermissionRequestResult, this,
                       std::move(callback)),
        std::move(fullscreen_block));
  }

  const url::Origin& origin() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return origin_;
  }

  HandleType handle_type() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return handle_type_;
  }

  GrantType type() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return type_;
  }

  void SetStatus(PermissionStatus new_status) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    auto permission_changed = status_ != new_status;
    status_ = new_status;

    if (permission_changed) {
      NotifyPermissionStatusChanged();
    }
  }

  base::Value::Dict AsValue() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    base::Value::Dict value;
    value.Set(kPermissionPathKey, base::FilePathToValue(path_));
    value.Set(kPermissionIsDirectoryKey,
              handle_type_ == HandleType::kDirectory);
    value.Set(GetGrantKeyFromGrantType(type_), true);
    return value;
  }

  static void UpdateGrantPath(
      std::map<base::FilePath, PermissionGrantImpl*>& grants,
      const base::FilePath& old_path,
      const base::FilePath& new_path) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto entry_it = base::ranges::find_if(
        grants,
        [&old_path](const auto& entry) { return entry.first == old_path; });

    if (entry_it == grants.end()) {
      // There must be an entry for an ancestor of this entry. Nothing to do
      // here.
      //
      // TODO(https://crbug.com/1381302): Consolidate superfluous child grants
      // to support directory moves.
      return;
    }

    DCHECK_EQ(entry_it->second->GetActivePermissionStatus(),
              PermissionStatus::GRANTED);

    auto* const grant_impl = entry_it->second;
    grant_impl->SetPath(new_path);

    // Update the permission grant's key in the map of active permissions.
    grants.erase(entry_it);
    grants.emplace(new_path, grant_impl);
  }

 protected:
  ~PermissionGrantImpl() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (context_) {
      context_->PermissionGrantDestroyed(this);
    }
  }

 private:
  void OnPermissionRequestResult(
      base::OnceCallback<void(PermissionRequestOutcome)> callback,
      PermissionAction result) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    if (context_) {
      context_->UpdateGrantsOnPermissionRequestResult(origin_);
    }

    switch (result) {
      case PermissionAction::GRANTED:
        SetStatus(PermissionStatus::GRANTED);
        std::move(callback).Run(PermissionRequestOutcome::kUserGranted);
        break;
      case PermissionAction::DENIED:
        SetStatus(PermissionStatus::DENIED);
        std::move(callback).Run(PermissionRequestOutcome::kUserDenied);
        break;
      case PermissionAction::DISMISSED:
      case PermissionAction::IGNORED:
        std::move(callback).Run(PermissionRequestOutcome::kUserDismissed);
        break;
      case PermissionAction::REVOKED:
      case PermissionAction::GRANTED_ONCE:
      case PermissionAction::NUM:
        NOTREACHED();
        break;
    }
  }

  std::string_view GetKey() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return PathAsPermissionKey(path_);
  }

  void SetPath(const base::FilePath& new_path) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    if (path_ == new_path)
      return;

    path_ = new_path;
    NotifyPermissionStatusChanged();
  }

  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtr<ElectronFileSystemAccessPermissionContext> const context_;
  const url::Origin origin_;
  const HandleType handle_type_;
  const GrantType type_;
  // `path_` can be updated if the entry is moved.
  base::FilePath path_;
  const UserAction user_action_;

  // This member should only be updated via SetStatus().
  PermissionStatus status_ = PermissionStatus::ASK;
};

struct ElectronFileSystemAccessPermissionContext::OriginState {
  // Raw pointers, owned collectively by all the handles that reference this
  // grant. When last reference goes away this state is cleared as well by
  // PermissionGrantDestroyed().
  std::map<base::FilePath, PermissionGrantImpl*> read_grants;
  std::map<base::FilePath, PermissionGrantImpl*> write_grants;
};

ElectronFileSystemAccessPermissionContext::
    ElectronFileSystemAccessPermissionContext(content::BrowserContext* context)
    : context_(context) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

ElectronFileSystemAccessPermissionContext::
    ~ElectronFileSystemAccessPermissionContext() = default;

bool ElectronFileSystemAccessPermissionContext::RevokeActiveGrants(
    const url::Origin& origin,
    base::FilePath file_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool grant_revoked = false;

  auto origin_it = active_permissions_map_.find(origin);
  if (origin_it != active_permissions_map_.end()) {
    OriginState& origin_state = origin_it->second;
    for (auto& grant : origin_state.read_grants) {
      if (file_path.empty() || grant.first == file_path) {
        grant.second->SetStatus(PermissionStatus::ASK);
        grant_revoked = true;
      }
    }
    for (auto& grant : origin_state.write_grants) {
      if (file_path.empty() || grant.first == file_path) {
        grant.second->SetStatus(PermissionStatus::ASK);
        grant_revoked = true;
      }
    }
  }
  return grant_revoked;
}

scoped_refptr<content::FileSystemAccessPermissionGrant>
ElectronFileSystemAccessPermissionContext::GetReadPermissionGrant(
    const url::Origin& origin,
    const base::FilePath& path,
    HandleType handle_type,
    UserAction user_action) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // operator[] might insert a new OriginState in |active_permissions_map_|,
  // but that is exactly what we want.
  auto& origin_state = active_permissions_map_[origin];
  auto*& existing_grant = origin_state.read_grants[path];
  scoped_refptr<PermissionGrantImpl> new_grant;

  if (existing_grant && existing_grant->handle_type() != handle_type) {
    // |path| changed from being a directory to being a file or vice versa,
    // don't just re-use the existing grant but revoke the old grant before
    // creating a new grant.
    existing_grant->SetStatus(PermissionStatus::DENIED);
    existing_grant = nullptr;
  }

  if (!existing_grant) {
    new_grant = base::MakeRefCounted<PermissionGrantImpl>(
        weak_factory_.GetWeakPtr(), origin, path, handle_type, GrantType::kRead,
        user_action);
    existing_grant = new_grant.get();
  }

  return existing_grant;
}

scoped_refptr<content::FileSystemAccessPermissionGrant>
ElectronFileSystemAccessPermissionContext::GetWritePermissionGrant(
    const url::Origin& origin,
    const base::FilePath& path,
    HandleType handle_type,
    UserAction user_action) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // operator[] might insert a new OriginState in |active_permissions_map_|,
  // but that is exactly what we want.
  auto& origin_state = active_permissions_map_[origin];
  auto*& existing_grant = origin_state.write_grants[path];
  scoped_refptr<PermissionGrantImpl> new_grant;

  if (existing_grant && existing_grant->handle_type() != handle_type) {
    // |path| changed from being a directory to being a file or vice versa,
    // don't just re-use the existing grant but revoke the old grant before
    // creating a new grant.
    existing_grant->SetStatus(PermissionStatus::DENIED);
    existing_grant = nullptr;
  }

  if (!existing_grant) {
    new_grant = base::MakeRefCounted<PermissionGrantImpl>(
        weak_factory_.GetWeakPtr(), origin, path, handle_type,
        GrantType::kWrite, user_action);
    existing_grant = new_grant.get();
  }

  return existing_grant;
}
// Returns origins that have either extended grants or active grants.
std::set<url::Origin>
ElectronFileSystemAccessPermissionContext::GetOriginsWithGrants() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Add origins that have active grants.
  std::set<url::Origin> origins;
  for (const auto& it : active_permissions_map_) {
    origins.insert(it.first);
  }

  return origins;
}

std::string ElectronFileSystemAccessPermissionContext::GetKeyForObject(
    const base::Value::Dict& object) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto optional_path =
      base::ValueToFilePath(object.Find(kPermissionPathKey));
  DCHECK(optional_path);
  return std::string(PathAsPermissionKey(optional_path.value()));
}

bool ElectronFileSystemAccessPermissionContext::IsValidObject(
    const base::Value::Dict& dict) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (dict.size() != 3 && dict.size() != 4) {
    return false;
  }

  // At least one of the readable/writable keys needs to be set.
  if (!dict.FindBool(kPermissionWritableKey) &&
      !dict.FindBool(kPermissionReadableKey)) {
    return false;
  }

  if (!dict.contains(kPermissionPathKey) ||
      !dict.FindBool(kPermissionIsDirectoryKey) ||
      dict.contains(kDeprecatedPermissionLastUsedTimeKey)) {
    return false;
  }

  return true;
}

std::u16string ElectronFileSystemAccessPermissionContext::GetObjectDisplayName(
    const base::Value::Dict& object) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto optional_path =
      base::ValueToFilePath(object.Find(kPermissionPathKey));
  DCHECK(optional_path);
  return optional_path->LossyDisplayName();
}

std::vector<base::FilePath>
ElectronFileSystemAccessPermissionContext::GetGrantedPaths(
    const url::Origin& origin) {
  std::vector<base::FilePath> granted_paths;
  auto granted_objects = GetGrantedObjects(origin);
  for (auto& granted_object : granted_objects) {
    auto* const optional_path = granted_object->value.Find(kPermissionPathKey);
    DCHECK(optional_path);
    granted_paths.push_back(base::ValueToFilePath(optional_path).value());
  }
  return granted_paths;
}

bool ElectronFileSystemAccessPermissionContext::CanObtainReadPermission(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return true;
}

bool ElectronFileSystemAccessPermissionContext::CanObtainWritePermission(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return true;
}

void ElectronFileSystemAccessPermissionContext::ConfirmSensitiveEntryAccess(
    const url::Origin& origin,
    PathType path_type,
    const base::FilePath& path,
    HandleType handle_type,
    UserAction user_action,
    content::GlobalRenderFrameHostId frame_id,
    base::OnceCallback<void(SensitiveEntryResult)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto after_blocklist_check_callback = base::BindOnce(
      &ElectronFileSystemAccessPermissionContext::DidCheckPathAgainstBlocklist,
      GetWeakPtr(), origin, path, handle_type, user_action, frame_id,
      std::move(callback));
  CheckPathAgainstBlocklist(path_type, path, handle_type,
                            std::move(after_blocklist_check_callback));
}

void ElectronFileSystemAccessPermissionContext::CheckPathAgainstBlocklist(
    PathType path_type,
    const base::FilePath& path,
    HandleType handle_type,
    base::OnceCallback<void(bool)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // TODO(https://crbug.com/1009970): Figure out what external paths should be
  // blocked. We could resolve the external path to a local path, and check for
  // blocked directories based on that, but that doesn't work well. Instead we
  // should have a separate Chrome OS only code path to block for example the
  // root of certain external file systems.
  if (path_type == PathType::kExternal) {
    std::move(callback).Run(/*should_block=*/false);
    return;
  }

  // Unlike the DIR_USER_DATA check, this handles the --user-data-dir override.
  // We check for the user data dir in two different ways: directly, via the
  // profile manager, where it exists (it does not in unit tests), and via the
  // profile's directory, assuming the profile dir is a child of the user data
  // dir.
  std::vector<BlockPathRule> extra_rules;
  extra_rules.emplace_back(profile_->GetPath().DirName(), kBlockAllChildren);
  if (g_browser_process->profile_manager()) {
    extra_rules.emplace_back(
        g_browser_process->profile_manager()->user_data_dir(),
        kBlockAllChildren);
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ShouldBlockAccessToPath, path, handle_type, extra_rules),
      std::move(callback));
}

void ElectronFileSystemAccessPermissionContext::DidCheckPathAgainstBlocklist(
    const url::Origin& origin,
    const base::FilePath& path,
    HandleType handle_type,
    UserAction user_action,
    content::GlobalRenderFrameHostId frame_id,
    base::OnceCallback<void(SensitiveEntryResult)> callback,
    bool should_block) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (user_action == UserAction::kNone) {
    std::move(callback).Run(should_block ? SensitiveEntryResult::kAbort
                                         : SensitiveEntryResult::kAllowed);
    return;
  }

  if (should_block) {
    auto result_callback =
        base::BindPostTaskToCurrentDefault(std::move(callback));
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&ShowFileSystemAccessRestrictedDirectoryDialogOnUIThread,
                       frame_id, origin, handle_type,
                       std::move(result_callback)));
    return;
  }

  // If attempting to save a file with a dangerous extension, prompt the user
  // to make them confirm they actually want to save the file.
  if (handle_type == HandleType::kFile && user_action == UserAction::kSave &&
      FileHasDangerousExtension(origin, path,
                                Profile::FromBrowserContext(profile_))) {
    auto result_callback =
        base::BindPostTaskToCurrentDefault(std::move(callback));
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&ShowFileSystemAccessDangerousFileDialogOnUIThread,
                       frame_id, origin, path, std::move(result_callback)));
    return;
  }

  std::move(callback).Run(SensitiveEntryResult::kAllowed);
}

void ElectronFileSystemAccessPermissionContext::MaybeEvictEntries(
    base::Value::Dict& dict) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::vector<std::pair<base::Time, std::string>> entries;
  entries.reserve(dict.size());
  for (auto entry : dict) {
    // Don't evict the default ID.
    if (entry.first == kDefaultLastPickedDirectoryKey) {
      continue;
    }
    // If the data is corrupted and `entry.second` is for some reason not a
    // dict, it should be first in line for eviction.
    auto timestamp = base::Time::Min();
    if (entry.second.is_dict()) {
      timestamp = base::ValueToTime(entry.second.GetDict().Find(kTimestampKey))
                      .value_or(base::Time::Min());
    }
    entries.emplace_back(timestamp, entry.first);
  }

  if (entries.size() <= max_ids_per_origin_) {
    return;
  }

  base::ranges::sort(entries);
  size_t entries_to_remove = entries.size() - max_ids_per_origin_;
  for (size_t i = 0; i < entries_to_remove; ++i) {
    bool did_remove_entry = dict.Remove(entries[i].second);
    DCHECK(did_remove_entry);
  }
}

void ElectronFileSystemAccessPermissionContext::SetLastPickedDirectory(
    const url::Origin& origin,
    const std::string& id,
    const base::FilePath& path,
    const PathType type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "NOTIMPLEMENTED SetLastPickedDirectory: " << path.value();
}

ElectronFileSystemAccessPermissionContext::PathInfo
ElectronFileSystemAccessPermissionContext::GetLastPickedDirectory(
    const url::Origin& origin,
    const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "NOTIMPLEMENTED GetLastPickedDirectory";
  return PathInfo();
}

base::FilePath
ElectronFileSystemAccessPermissionContext::GetWellKnownDirectoryPath(
    blink::mojom::WellKnownDirectory directory,
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // PDF viewer uses the default Download directory set in browser, if possible.
  if (directory == blink::mojom::WellKnownDirectory::kDirDownloads &&
      IsPdfExtensionOrigin(origin)) {
    base::FilePath profile_download_path =
        DownloadPrefs::FromBrowserContext(profile())->DownloadPath();
    if (!profile_download_path.empty()) {
      return profile_download_path;
    }
  }

  int key = base::PATH_START;
  switch (directory) {
    case blink::mojom::WellKnownDirectory::kDirDesktop:
      key = base::DIR_USER_DESKTOP;
      break;
    case blink::mojom::WellKnownDirectory::kDirDocuments:
      key = chrome::DIR_USER_DOCUMENTS;
      break;
    case blink::mojom::WellKnownDirectory::kDirDownloads:
      key = chrome::DIR_DEFAULT_DOWNLOADS;
      break;
    case blink::mojom::WellKnownDirectory::kDirMusic:
      key = chrome::DIR_USER_MUSIC;
      break;
    case blink::mojom::WellKnownDirectory::kDirPictures:
      key = chrome::DIR_USER_PICTURES;
      break;
    case blink::mojom::WellKnownDirectory::kDirVideos:
      key = chrome::DIR_USER_VIDEOS;
      break;
  }
  base::FilePath directory_path;
  base::PathService::Get(key, &directory_path);
  return directory_path;
}

std::u16string ElectronFileSystemAccessPermissionContext::GetPickerTitle(
    const blink::mojom::FilePickerOptionsPtr& options) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // TODO(asully): Consider adding custom strings for invocations of the file
  // picker, as well. Returning the empty string will fall back to the platform
  // default for the given picker type.
  std::u16string title;
  switch (options->type_specific_options->which()) {
    case blink::mojom::TypeSpecificFilePickerOptionsUnion::Tag::
        kDirectoryPickerOptions:
      title = l10n_util::GetStringUTF16(
          options->type_specific_options->get_directory_picker_options()
                  ->request_writable
              ? IDS_FILE_SYSTEM_ACCESS_CHOOSER_OPEN_WRITABLE_DIRECTORY_TITLE
              : IDS_FILE_SYSTEM_ACCESS_CHOOSER_OPEN_READABLE_DIRECTORY_TITLE);
      break;
    case blink::mojom::TypeSpecificFilePickerOptionsUnion::Tag::
        kSaveFilePickerOptions:
      title = l10n_util::GetStringUTF16(
          IDS_FILE_SYSTEM_ACCESS_CHOOSER_OPEN_SAVE_FILE_TITLE);
      break;
    case blink::mojom::TypeSpecificFilePickerOptionsUnion::Tag::
        kOpenFilePickerOptions:
      break;
  }
  return title;
}

void ElectronFileSystemAccessPermissionContext::NotifyEntryMoved(
    const url::Origin& origin,
    const base::FilePath& old_path,
    const base::FilePath& new_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (old_path == new_path) {
    return;
  }

  auto it = active_permissions_map_.find(origin);
  if (it != active_permissions_map_.end()) {
    // TODO(https://crbug.com/1381302): Consolidate superfluous child grants.
    PermissionGrantImpl::UpdateGrantPath(it->second.write_grants, old_path,
                                         new_path);
    PermissionGrantImpl::UpdateGrantPath(it->second.read_grants, old_path,
                                         new_path);
  }
}

// void ElectronFileSystemAccessPermissionContext::
//     OnFileCreatedFromShowSaveFilePicker(const GURL& file_picker_binding_context,
//                                         const storage::FileSystemURL& url) {
//   file_created_from_show_save_file_picker_callback_list_.Notify(
//       file_picker_binding_context, url);
// }

// base::CallbackListSubscription ElectronFileSystemAccessPermissionContext::
//     AddFileCreatedFromShowSaveFilePickerCallback(
//         FileCreatedFromShowSaveFilePickerCallbackList::CallbackType callback) {
//   return file_created_from_show_save_file_picker_callback_list_.Add(
//       std::move(callback));
// }

ElectronFileSystemAccessPermissionContext::Grants
ElectronFileSystemAccessPermissionContext::ConvertObjectsToGrants(
    const std::vector<std::unique_ptr<Object>> objects) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ElectronFileSystemAccessPermissionContext::Grants grants;

  for (const auto& object : objects) {
    if (!IsValidObject(object->value)) {
      continue;
    }

    const base::Value::Dict& object_dict = object->value;
    const base::FilePath path =
        base::ValueToFilePath(object_dict.Find(kPermissionPathKey)).value();
    HandleType handle_type =
        object_dict.FindBool(kPermissionIsDirectoryKey).value()
            ? HandleType::kDirectory
            : HandleType::kFile;
    bool is_write_grant =
        object_dict.FindBool(kPermissionWritableKey).value_or(false);
    bool is_read_grant =
        object_dict.FindBool(kPermissionReadableKey).value_or(false);

    if (handle_type == HandleType::kDirectory) {
      if (is_write_grant &&
          !base::Contains(grants.directory_write_grants, path)) {
        grants.directory_write_grants.push_back(path);
      }
      if (is_read_grant &&
          !base::Contains(grants.directory_read_grants, path)) {
        grants.directory_read_grants.push_back(path);
      }
    }
    if (handle_type == HandleType::kFile) {
      if (is_write_grant && !base::Contains(grants.file_write_grants, path)) {
        grants.file_write_grants.push_back(path);
      }
      if (is_read_grant && !base::Contains(grants.file_read_grants, path)) {
        grants.file_read_grants.push_back(path);
      }
    }
  }

  return grants;
}

void ElectronFileSystemAccessPermissionContext::RevokeGrant(
    const url::Origin& origin,
    const base::FilePath& file_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  RevokeActiveGrants(origin, file_path);
}

void ElectronFileSystemAccessPermissionContext::RevokeGrants(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  bool grant_revoked = false;
  if (RevokeActiveGrants(origin)) {
    grant_revoked = true;
  }

  if (grant_revoked) {
    ScheduleUsageIconUpdate();
  }
}

bool ElectronFileSystemAccessPermissionContext::OriginHasReadAccess(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // First, check if an origin has read access granted via active permissions.
  auto it = active_permissions_map_.find(origin);
  if (it != active_permissions_map_.end()) {
    return base::ranges::any_of(it->second.read_grants, [&](const auto& grant) {
      return grant.second->GetActivePermissionStatus() ==
             PermissionStatus::GRANTED;
    });
  }
}

bool ElectronFileSystemAccessPermissionContext::OriginHasWriteAccess(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // First, check if an origin has write access granted via active permissions.
  auto it = active_permissions_map_.find(origin);
  if (it != active_permissions_map_.end()) {
    return base::ranges::any_of(
        it->second.write_grants, [&](const auto& grant) {
          return grant.second->GetActivePermissionStatus() ==
                 PermissionStatus::GRANTED;
        });
  }
}

void ElectronFileSystemAccessPermissionContext::MaybeCleanupPermissions(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
#if !BUILDFLAG(IS_ANDROID)
  // Iterate over all top-level frames by iterating over all browsers, and all
  // tabs within those browsers. This also counts PWAs in windows without
  // tab strips, as those are still implemented as a Browser with a single
  // tab.
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (browser->profile() != profile()) {
      continue;
    }
    TabStripModel* tabs = browser->tab_strip_model();
    for (int i = 0; i < tabs->count(); ++i) {
      content::WebContents* web_contents = tabs->GetWebContentsAt(i);
      url::Origin tab_origin = url::Origin::Create(
          permissions::PermissionUtil::GetLastCommittedOriginAsURL(
              web_contents->GetPrimaryMainFrame()));
      // Found a tab for this origin, so early exit and don't revoke grants.
      if (tab_origin == origin) {
        return;
      }
    }
  }
  CleanupPermissions(origin);
#endif
}

void ElectronFileSystemAccessPermissionContext::CleanupPermissions(
    const url::Origin& origin) {
  RevokeActiveGrants(origin);
}

bool ElectronFileSystemAccessPermissionContext::AncestorHasActivePermission(
    const url::Origin& origin,
    const base::FilePath& path,
    GrantType grant_type) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = active_permissions_map_.find(origin);
  if (it == active_permissions_map_.end()) {
    return false;
  }
  const auto& relevant_grants = grant_type == GrantType::kWrite
                                    ? it->second.write_grants
                                    : it->second.read_grants;
  if (relevant_grants.empty()) {
    return false;
  }

  // Permissions are inherited from the closest ancestor.
  for (base::FilePath parent = path.DirName(); parent != parent.DirName();
       parent = parent.DirName()) {
    auto i = relevant_grants.find(parent);
    if (i != relevant_grants.end() && i->second &&
        i->second->GetActivePermissionStatus() == PermissionStatus::GRANTED) {
      return true;
    }
  }
  return false;
}

std::vector<FileRequestData> ElectronFileSystemAccessPermissionContext::
    GetFileRequestDataForRestorePermissionPrompt(const url::Origin& origin) {
  std::vector<FileRequestData> file_request_data_list;
  auto dormant_grants = ObjectPermissionContextBase::GetGrantedObjects(origin);
  for (auto& dormant_grant : dormant_grants) {
    if (!IsValidObject(dormant_grant->value)) {
      continue;
    }
    const base::Value::Dict& object_dict = dormant_grant->value;
    FileRequestData file_request_data = {
        base::ValueToFilePath(object_dict.Find(kPermissionPathKey)).value(),
        object_dict.FindBool(kPermissionIsDirectoryKey).value_or(false)
            ? HandleType::kDirectory
            : HandleType::kFile,
        object_dict.FindBool(kPermissionWritableKey).value_or(false)
            ? RequestAccess::kWrite
            : RequestAccess::kRead};
    file_request_data_list.push_back(file_request_data);
  }
  return file_request_data_list;
}

bool ElectronFileSystemAccessPermissionContext::HasMatchingValue(
    const base::Value::Dict& value,
    const base::FilePath& file_path,
    HandleType handle_type,
    GrantType grant_type) {
  return ValueToFilePath(value.Find(kPermissionPathKey)).value() == file_path &&
         value.FindBool(kPermissionIsDirectoryKey).value_or(false) ==
             (handle_type == HandleType::kDirectory) &&
         value.FindBool(GetGrantKeyFromGrantType(grant_type)).value_or(false);
}

void ElectronFileSystemAccessPermissionContext::PermissionGrantDestroyed(
    PermissionGrantImpl* grant) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = active_permissions_map_.find(grant->origin());
  if (it == active_permissions_map_.end()) {
    return;
  }

  auto& grants = grant->type() == GrantType::kRead ? it->second.read_grants
                                                   : it->second.write_grants;
  auto grant_it = grants.find(grant->GetPath());
  // Any non-denied permission grants should have still been in our grants
  // list. If this invariant is violated we would have permissions that might
  // be granted but won't be visible in any UI because the permission context
  // isn't tracking them anymore.
  if (grant_it == grants.end()) {
    DCHECK_EQ(PermissionStatus::DENIED, grant->GetActivePermissionStatus());
    return;
  }

  // The grant in |grants| for this path might have been replaced with a
  // different grant. Only erase if it actually matches the grant that was
  // destroyed.
  if (grant_it->second == grant) {
    grants.erase(grant_it);
  }
}

base::WeakPtr<ElectronFileSystemAccessPermissionContext>
ElectronFileSystemAccessPermissionContext::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}