// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/installed_loader.h"

#include "base/files/file_path.h"
#include "base/metrics/histogram.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/extension_action_manager.h"
#include "chrome/browser/extensions/extension_error_reporter.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/api/supervised_user_private/supervised_user_handler.h"
#include "chrome/common/extensions/extension_constants.h"
#include "chrome/common/extensions/manifest_url_handler.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/user_metrics.h"
#include "extensions/browser/api/runtime/runtime_api.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/management_policy.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_l10n_util.h"
#include "extensions/common/extension_set.h"
#include "extensions/common/file_util.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_handlers/background_info.h"

using base::UserMetricsAction;
using content::BrowserThread;

namespace extensions {

namespace errors = manifest_errors;

namespace {

// The following enumeration is used in histograms matching
// Extensions.ManifestReload*.
enum ManifestReloadReason {
  NOT_NEEDED = 0,        // Reload not needed.
  UNPACKED_DIR,          // Unpacked directory.
  NEEDS_RELOCALIZATION,  // The locale has changed since we read this extension.
  CORRUPT_PREFERENCES,   // The manifest in the preferences is corrupt.

  // New enum values must go above here.
  NUM_MANIFEST_RELOAD_REASONS
};

// Used in histogram Extension.BackgroundPageType.
enum BackgroundPageType {
  NO_BACKGROUND_PAGE = 0,
  BACKGROUND_PAGE_PERSISTENT,
  EVENT_PAGE,

  // New enum values must go above here.
  NUM_BACKGROUND_PAGE_TYPES
};

// Used in histogram Extensions.ExternalItemState.
enum ExternalItemState {
  DEPRECATED_EXTERNAL_ITEM_DISABLED = 0,
  DEPRECATED_EXTERNAL_ITEM_ENABLED,
  EXTERNAL_ITEM_WEBSTORE_DISABLED,
  EXTERNAL_ITEM_WEBSTORE_ENABLED,
  EXTERNAL_ITEM_NONWEBSTORE_DISABLED,
  EXTERNAL_ITEM_NONWEBSTORE_ENABLED,
  EXTERNAL_ITEM_WEBSTORE_UNINSTALLED,
  EXTERNAL_ITEM_NONWEBSTORE_UNINSTALLED,

  // New enum values must go above here.
  EXTERNAL_ITEM_MAX_ITEMS
};

bool IsManifestCorrupt(const base::DictionaryValue* manifest) {
  if (!manifest)
    return false;

  // Because of bug #272524 sometimes manifests got mangled in the preferences
  // file, one particularly bad case resulting in having both a background page
  // and background scripts values. In those situations we want to reload the
  // manifest from the extension to fix this.
  const base::Value* background_page;
  const base::Value* background_scripts;
  return manifest->Get(manifest_keys::kBackgroundPage, &background_page) &&
      manifest->Get(manifest_keys::kBackgroundScripts, &background_scripts);
}

ManifestReloadReason ShouldReloadExtensionManifest(const ExtensionInfo& info) {
  // Always reload manifests of unpacked extensions, because they can change
  // on disk independent of the manifest in our prefs.
  if (Manifest::IsUnpackedLocation(info.extension_location))
    return UNPACKED_DIR;

  // Reload the manifest if it needs to be relocalized.
  if (extension_l10n_util::ShouldRelocalizeManifest(
          info.extension_manifest.get()))
    return NEEDS_RELOCALIZATION;

  // Reload if the copy of the manifest in the preferences is corrupt.
  if (IsManifestCorrupt(info.extension_manifest.get()))
    return CORRUPT_PREFERENCES;

  return NOT_NEEDED;
}

BackgroundPageType GetBackgroundPageType(const Extension* extension) {
  if (!BackgroundInfo::HasBackgroundPage(extension))
    return NO_BACKGROUND_PAGE;
  if (BackgroundInfo::HasPersistentBackgroundPage(extension))
    return BACKGROUND_PAGE_PERSISTENT;
  return EVENT_PAGE;
}

// Records the creation flags of an extension grouped by
// Extension::InitFromValueFlags.
void RecordCreationFlags(const Extension* extension) {
  for (int i = 0; i < Extension::kInitFromValueFlagBits; ++i) {
    int flag = 1 << i;
    if (extension->creation_flags() & flag) {
      UMA_HISTOGRAM_ENUMERATION(
          "Extensions.LoadCreationFlags", i, Extension::kInitFromValueFlagBits);
    }
  }
}

}  // namespace

InstalledLoader::InstalledLoader(ExtensionService* extension_service)
    : extension_service_(extension_service),
      extension_registry_(ExtensionRegistry::Get(extension_service->profile())),
      extension_prefs_(ExtensionPrefs::Get(extension_service->profile())) {}

InstalledLoader::~InstalledLoader() {
}

void InstalledLoader::Load(const ExtensionInfo& info, bool write_to_prefs) {
  std::string error;
  scoped_refptr<const Extension> extension(NULL);
  if (info.extension_manifest) {
    extension = Extension::Create(
        info.extension_path,
        info.extension_location,
        *info.extension_manifest,
        GetCreationFlags(&info),
        &error);
  } else {
    error = errors::kManifestUnreadable;
  }

  // Once installed, non-unpacked extensions cannot change their IDs (e.g., by
  // updating the 'key' field in their manifest).
  // TODO(jstritar): migrate preferences when unpacked extensions change IDs.
  if (extension.get() && !Manifest::IsUnpackedLocation(extension->location()) &&
      info.extension_id != extension->id()) {
    error = errors::kCannotChangeExtensionID;
    extension = NULL;
  }

  // Check policy on every load in case an extension was blacklisted while
  // Chrome was not running.
  const ManagementPolicy* policy = extensions::ExtensionSystem::Get(
      extension_service_->profile())->management_policy();
  if (extension.get()) {
    Extension::DisableReason disable_reason = Extension::DISABLE_NONE;
    bool force_disabled = false;
    if (!policy->UserMayLoad(extension.get(), NULL)) {
      // The error message from UserMayInstall() often contains the extension ID
      // and is therefore not well suited to this UI.
      error = errors::kDisabledByPolicy;
      extension = NULL;
    } else if (!extension_prefs_->IsExtensionDisabled(extension->id()) &&
               policy->MustRemainDisabled(extension, &disable_reason, NULL)) {
      extension_prefs_->SetExtensionState(extension->id(), Extension::DISABLED);
      extension_prefs_->AddDisableReason(extension->id(), disable_reason);
      force_disabled = true;
    }
    UMA_HISTOGRAM_BOOLEAN("ExtensionInstalledLoader.ForceDisabled",
                          force_disabled);
  }

  if (!extension.get()) {
    ExtensionErrorReporter::GetInstance()->ReportLoadError(
        info.extension_path,
        error,
        extension_service_->profile(),
        false);  // Be quiet.
    return;
  }

  if (write_to_prefs)
    extension_prefs_->UpdateManifest(extension.get());

  extension_service_->AddExtension(extension.get());
}

void InstalledLoader::LoadAllExtensions() {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  base::TimeTicks start_time = base::TimeTicks::Now();

  Profile* profile = extension_service_->profile();
  scoped_ptr<ExtensionPrefs::ExtensionsInfo> extensions_info(
      extension_prefs_->GetInstalledExtensionsInfo());

  std::vector<int> reload_reason_counts(NUM_MANIFEST_RELOAD_REASONS, 0);
  bool should_write_prefs = false;

  for (size_t i = 0; i < extensions_info->size(); ++i) {
    ExtensionInfo* info = extensions_info->at(i).get();

    // Skip extensions that were loaded from the command-line because we don't
    // want those to persist across browser restart.
    if (info->extension_location == Manifest::COMMAND_LINE)
      continue;

    ManifestReloadReason reload_reason = ShouldReloadExtensionManifest(*info);
    ++reload_reason_counts[reload_reason];

    if (reload_reason != NOT_NEEDED) {
      // Reloading an extension reads files from disk.  We do this on the
      // UI thread because reloads should be very rare, and the complexity
      // added by delaying the time when the extensions service knows about
      // all extensions is significant.  See crbug.com/37548 for details.
      // |allow_io| disables tests that file operations run on the file
      // thread.
      base::ThreadRestrictions::ScopedAllowIO allow_io;

      std::string error;
      scoped_refptr<const Extension> extension(
          file_util::LoadExtension(info->extension_path,
                                   info->extension_location,
                                   GetCreationFlags(info),
                                   &error));

      if (!extension.get()) {
        ExtensionErrorReporter::GetInstance()->ReportLoadError(
            info->extension_path,
            error,
            profile,
            false);  // Be quiet.
        continue;
      }

      extensions_info->at(i)->extension_manifest.reset(
          static_cast<base::DictionaryValue*>(
              extension->manifest()->value()->DeepCopy()));
      should_write_prefs = true;
    }
  }

  for (size_t i = 0; i < extensions_info->size(); ++i) {
    if (extensions_info->at(i)->extension_location != Manifest::COMMAND_LINE)
      Load(*extensions_info->at(i), should_write_prefs);
  }

  extension_service_->OnLoadedInstalledExtensions();

  // The histograms Extensions.ManifestReload* allow us to validate
  // the assumption that reloading manifest is a rare event.
  UMA_HISTOGRAM_COUNTS_100("Extensions.ManifestReloadNotNeeded",
                           reload_reason_counts[NOT_NEEDED]);
  UMA_HISTOGRAM_COUNTS_100("Extensions.ManifestReloadUnpackedDir",
                           reload_reason_counts[UNPACKED_DIR]);
  UMA_HISTOGRAM_COUNTS_100("Extensions.ManifestReloadNeedsRelocalization",
                           reload_reason_counts[NEEDS_RELOCALIZATION]);

  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadAll",
                           extension_registry_->enabled_extensions().size());
  UMA_HISTOGRAM_COUNTS_100("Extensions.Disabled",
                           extension_registry_->disabled_extensions().size());

  UMA_HISTOGRAM_TIMES("Extensions.LoadAllTime",
                      base::TimeTicks::Now() - start_time);

  int app_user_count = 0;
  int app_external_count = 0;
  int hosted_app_count = 0;
  int legacy_packaged_app_count = 0;
  int platform_app_count = 0;
  int user_script_count = 0;
  int content_pack_count = 0;
  int extension_user_count = 0;
  int extension_external_count = 0;
  int theme_count = 0;
  int page_action_count = 0;
  int browser_action_count = 0;
  int disabled_for_permissions_count = 0;
  int non_webstore_ntp_override_count = 0;
  int incognito_allowed_count = 0;
  int incognito_not_allowed_count = 0;
  int file_access_allowed_count = 0;
  int file_access_not_allowed_count = 0;

  const ExtensionSet& extensions = extension_registry_->enabled_extensions();
  ExtensionActionManager* extension_action_manager =
      ExtensionActionManager::Get(profile);
  for (ExtensionSet::const_iterator iter = extensions.begin();
       iter != extensions.end();
       ++iter) {
    const Extension* extension = *iter;
    Manifest::Location location = extension->location();
    Manifest::Type type = extension->GetType();

    // For the first few metrics, include all extensions and apps (component,
    // unpacked, etc). It's good to know these locations, and it doesn't
    // muck up any of the stats. Later, though, we want to omit component and
    // unpacked, as they are less interesting.
    if (extension->is_app())
      UMA_HISTOGRAM_ENUMERATION(
          "Extensions.AppLocation", location, Manifest::NUM_LOCATIONS);
    else if (extension->is_extension())
      UMA_HISTOGRAM_ENUMERATION(
          "Extensions.ExtensionLocation", location, Manifest::NUM_LOCATIONS);

    if (!ManifestURL::UpdatesFromGallery(extension)) {
      UMA_HISTOGRAM_ENUMERATION(
          "Extensions.NonWebstoreLocation", location, Manifest::NUM_LOCATIONS);

      // Check for inconsistencies if the extension was supposedly installed
      // from the webstore.
      enum {
        BAD_UPDATE_URL = 0,
        // This value was a mistake. Turns out sideloaded extensions can
        // have the from_webstore bit if they update from the webstore.
        DEPRECATED_IS_EXTERNAL = 1,
      };
      if (extension->from_webstore()) {
        UMA_HISTOGRAM_ENUMERATION(
            "Extensions.FromWebstoreInconsistency", BAD_UPDATE_URL, 2);
      }
    }

    if (Manifest::IsExternalLocation(location)) {
      // See loop below for DISABLED.
      if (ManifestURL::UpdatesFromGallery(extension)) {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_WEBSTORE_ENABLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      } else {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_NONWEBSTORE_ENABLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      }
    }

    // From now on, don't count component extensions, since they are only
    // extensions as an implementation detail. Continue to count unpacked
    // extensions for a few metrics.
    if (location == Manifest::COMPONENT)
      continue;

    // Histogram for non-webstore extensions overriding new tab page should
    // include unpacked extensions.
    if (!extension->from_webstore() &&
        URLOverrides::GetChromeURLOverrides(extension).count("newtab")) {
      ++non_webstore_ntp_override_count;
    }

    // Don't count unpacked extensions anymore, either.
    if (Manifest::IsUnpackedLocation(location))
      continue;

    UMA_HISTOGRAM_ENUMERATION("Extensions.ManifestVersion",
                              extension->manifest_version(),
                              10);  // TODO(kalman): Why 10 manifest versions?

    // We might have wanted to count legacy packaged apps here, too, since they
    // are effectively extensions. Unfortunately, it's too late, as we don't
    // want to mess up the existing stats.
    if (type == Manifest::TYPE_EXTENSION) {
      UMA_HISTOGRAM_ENUMERATION("Extensions.BackgroundPageType",
                                GetBackgroundPageType(extension),
                                NUM_BACKGROUND_PAGE_TYPES);
    }

    // Using an enumeration shows us the total installed ratio across all users.
    // Using the totals per user at each startup tells us the distribution of
    // usage for each user (e.g. 40% of users have at least one app installed).
    UMA_HISTOGRAM_ENUMERATION(
        "Extensions.LoadType", type, Manifest::NUM_LOAD_TYPES);
    switch (type) {
      case Manifest::TYPE_THEME:
        ++theme_count;
        break;
      case Manifest::TYPE_USER_SCRIPT:
        ++user_script_count;
        break;
      case Manifest::TYPE_HOSTED_APP:
        ++hosted_app_count;
        if (Manifest::IsExternalLocation(location)) {
          ++app_external_count;
        } else {
          ++app_user_count;
        }
        break;
      case Manifest::TYPE_LEGACY_PACKAGED_APP:
        ++legacy_packaged_app_count;
        if (Manifest::IsExternalLocation(location)) {
          ++app_external_count;
        } else {
          ++app_user_count;
        }
        break;
      case Manifest::TYPE_PLATFORM_APP:
        ++platform_app_count;
        if (Manifest::IsExternalLocation(location)) {
          ++app_external_count;
        } else {
          ++app_user_count;
        }
        break;
      case Manifest::TYPE_EXTENSION:
      default:
        if (Manifest::IsExternalLocation(location)) {
          ++extension_external_count;
        } else {
          ++extension_user_count;
        }
        break;
    }

    if (extension_action_manager->GetPageAction(*extension))
      ++page_action_count;

    if (extension_action_manager->GetBrowserAction(*extension))
      ++browser_action_count;

    if (SupervisedUserInfo::IsContentPack(extension))
      ++content_pack_count;

    RecordCreationFlags(extension);

    ExtensionService::RecordPermissionMessagesHistogram(
        extension, "Extensions.Permissions_Load2");

    // For incognito and file access, skip anything that doesn't appear in
    // settings. Also, policy-installed (and unpacked of course, checked above)
    // extensions are boring.
    if (extension->ShouldDisplayInExtensionSettings() &&
        !Manifest::IsPolicyLocation(extension->location())) {
      if (extension->can_be_incognito_enabled()) {
        if (util::IsIncognitoEnabled(extension->id(), profile))
          ++incognito_allowed_count;
        else
          ++incognito_not_allowed_count;
      }
      if (extension->wants_file_access()) {
        if (util::AllowFileAccess(extension->id(), profile))
          ++file_access_allowed_count;
        else
          ++file_access_not_allowed_count;
      }
    }
  }

  const ExtensionSet& disabled_extensions =
      extension_registry_->disabled_extensions();

  for (ExtensionSet::const_iterator ex = disabled_extensions.begin();
       ex != disabled_extensions.end();
       ++ex) {
    if (extension_prefs_->DidExtensionEscalatePermissions((*ex)->id())) {
      ++disabled_for_permissions_count;
    }
    if (Manifest::IsExternalLocation((*ex)->location())) {
      // See loop above for ENABLED.
      if (ManifestURL::UpdatesFromGallery(*ex)) {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_WEBSTORE_DISABLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      } else {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_NONWEBSTORE_DISABLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      }
    }
  }

  scoped_ptr<ExtensionPrefs::ExtensionsInfo> uninstalled_extensions_info(
      extension_prefs_->GetUninstalledExtensionsInfo());
  for (size_t i = 0; i < uninstalled_extensions_info->size(); ++i) {
    ExtensionInfo* info = uninstalled_extensions_info->at(i).get();
    if (Manifest::IsExternalLocation(info->extension_location)) {
      std::string update_url;
      if (info->extension_manifest->GetString("update_url", &update_url) &&
          extension_urls::IsWebstoreUpdateUrl(GURL(update_url))) {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_WEBSTORE_UNINSTALLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      } else {
        UMA_HISTOGRAM_ENUMERATION("Extensions.ExternalItemState",
                                  EXTERNAL_ITEM_NONWEBSTORE_UNINSTALLED,
                                  EXTERNAL_ITEM_MAX_ITEMS);
      }
    }
  }

  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadApp",
                           app_user_count + app_external_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadAppUser", app_user_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadAppExternal", app_external_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadHostedApp", hosted_app_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadPackagedApp",
                           legacy_packaged_app_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadPlatformApp", platform_app_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadExtension",
                           extension_user_count + extension_external_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadExtensionUser",
                           extension_user_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadExtensionExternal",
                           extension_external_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadUserScript", user_script_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadTheme", theme_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadPageAction", page_action_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadBrowserAction",
                           browser_action_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.LoadContentPack", content_pack_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.DisabledForPermissions",
                           disabled_for_permissions_count);
  UMA_HISTOGRAM_COUNTS_100("Extensions.NonWebStoreNewTabPageOverrides",
                           non_webstore_ntp_override_count);
  if (incognito_allowed_count + incognito_not_allowed_count > 0) {
    UMA_HISTOGRAM_COUNTS_100("Extensions.IncognitoAllowed",
                             incognito_allowed_count);
    UMA_HISTOGRAM_COUNTS_100("Extensions.IncognitoNotAllowed",
                             incognito_not_allowed_count);
  }
  if (file_access_allowed_count + file_access_not_allowed_count > 0) {
    UMA_HISTOGRAM_COUNTS_100("Extensions.FileAccessAllowed",
                             file_access_allowed_count);
    UMA_HISTOGRAM_COUNTS_100("Extensions.FileAccessNotAllowed",
                             file_access_not_allowed_count);
  }
}

int InstalledLoader::GetCreationFlags(const ExtensionInfo* info) {
  int flags = extension_prefs_->GetCreationFlags(info->extension_id);
  if (!Manifest::IsUnpackedLocation(info->extension_location))
    flags |= Extension::REQUIRE_KEY;
  if (extension_prefs_->AllowFileAccess(info->extension_id))
    flags |= Extension::ALLOW_FILE_ACCESS;
  return flags;
}

}  // namespace extensions
