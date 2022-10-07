/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FsCrypt.h"

#include "KeyStorage.h"
#include "KeyUtil.h"
#include "Utils.h"
#include "VoldUtil.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <private/android_filesystem_config.h>
#include <private/android_projectid_config.h>

#include "android/os/IVold.h"

#define MANAGE_MISC_DIRS 0

#include <cutils/fs.h>
#include <cutils/properties.h>

#include <fscrypt/fscrypt.h>
#include <keyutils.h>
#include <libdm/dm.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

using android::base::Basename;
using android::base::Realpath;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::fs_mgr::GetEntryForMountPoint;
using android::vold::BuildDataPath;
using android::vold::IsDotOrDotDot;
using android::vold::IsFilesystemSupported;
using android::vold::kEmptyAuthentication;
using android::vold::KeyBuffer;
using android::vold::KeyGeneration;
using android::vold::retrieveKey;
using android::vold::retrieveOrGenerateKey;
using android::vold::SetDefaultAcl;
using android::vold::SetQuotaInherit;
using android::vold::SetQuotaProjectId;
using android::vold::writeStringToFile;
using namespace android::fscrypt;
using namespace android::dm;

namespace {

const std::string device_key_dir = std::string() + DATA_MNT_POINT + fscrypt_unencrypted_folder;
const std::string device_key_path = device_key_dir + "/key";
const std::string device_key_temp = device_key_dir + "/temp";

const std::string user_key_dir = std::string() + DATA_MNT_POINT + "/misc/vold/user_keys";
const std::string user_key_temp = user_key_dir + "/temp";
const std::string prepare_subdirs_path = "/system/bin/vold_prepare_subdirs";

const std::string systemwide_volume_key_dir =
    std::string() + DATA_MNT_POINT + "/misc/vold/volume_keys";

const std::string data_data_dir = std::string() + DATA_MNT_POINT + "/data";
const std::string data_user_0_dir = std::string() + DATA_MNT_POINT + "/user/0";
const std::string media_obb_dir = std::string() + DATA_MNT_POINT + "/media/obb";

// Some users are ephemeral, don't try to wipe their keys from disk
std::set<userid_t> s_ephemeral_users;

// New CE keys that haven't been committed to disk yet
std::map<userid_t, KeyBuffer> s_new_ce_keys;

// The system DE encryption policy
EncryptionPolicy s_device_policy;

// Map user ids to encryption policies
std::map<userid_t, EncryptionPolicy> s_de_policies;
std::map<userid_t, EncryptionPolicy> s_ce_policies;

}  // namespace

// Returns KeyGeneration suitable for key as described in EncryptionOptions
static KeyGeneration makeGen(const EncryptionOptions& options) {
    return KeyGeneration{FSCRYPT_MAX_KEY_SIZE, true, options.use_hw_wrapped_key};
}

static const char* escape_empty(const std::string& value) {
    return value.empty() ? "null" : value.c_str();
}

static std::string get_de_key_path(userid_t user_id) {
    return StringPrintf("%s/de/%d", user_key_dir.c_str(), user_id);
}

static std::string get_ce_key_directory_path(userid_t user_id) {
    return StringPrintf("%s/ce/%d", user_key_dir.c_str(), user_id);
}

// Returns the keys newest first
static std::vector<std::string> get_ce_key_paths(const std::string& directory_path) {
    auto dirp = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(directory_path.c_str()), closedir);
    if (!dirp) {
        PLOG(ERROR) << "Unable to open ce key directory: " + directory_path;
        return std::vector<std::string>();
    }
    std::vector<std::string> result;
    for (;;) {
        errno = 0;
        auto const entry = readdir(dirp.get());
        if (!entry) {
            if (errno) {
                PLOG(ERROR) << "Unable to read ce key directory: " + directory_path;
                return std::vector<std::string>();
            }
            break;
        }
        if (IsDotOrDotDot(*entry)) continue;
        if (entry->d_type != DT_DIR || entry->d_name[0] != 'c') {
            LOG(DEBUG) << "Skipping non-key " << entry->d_name;
            continue;
        }
        result.emplace_back(directory_path + "/" + entry->d_name);
    }
    std::sort(result.begin(), result.end());
    std::reverse(result.begin(), result.end());
    return result;
}

static std::string get_ce_key_current_path(const std::string& directory_path) {
    return directory_path + "/current";
}

static bool get_ce_key_new_path(const std::string& directory_path,
                                const std::vector<std::string>& paths, std::string* ce_key_path) {
    if (paths.empty()) {
        *ce_key_path = get_ce_key_current_path(directory_path);
        return true;
    }
    for (unsigned int i = 0; i < UINT_MAX; i++) {
        auto const candidate = StringPrintf("%s/cx%010u", directory_path.c_str(), i);
        if (paths[0] < candidate) {
            *ce_key_path = candidate;
            return true;
        }
    }
    return false;
}

// Discard all keys but the named one; rename it to canonical name.
static bool fixate_user_ce_key(const std::string& directory_path, const std::string& to_fix,
                               const std::vector<std::string>& paths) {
    for (auto const other_path : paths) {
        if (other_path != to_fix) {
            android::vold::destroyKey(other_path);
        }
    }
    auto const current_path = get_ce_key_current_path(directory_path);
    if (to_fix != current_path) {
        LOG(DEBUG) << "Renaming " << to_fix << " to " << current_path;
        if (!android::vold::RenameKeyDir(to_fix, current_path)) return false;
    }
    if (!android::vold::FsyncDirectory(directory_path)) return false;
    return true;
}

static bool read_and_fixate_user_ce_key(userid_t user_id,
                                        const android::vold::KeyAuthentication& auth,
                                        KeyBuffer* ce_key) {
    auto const directory_path = get_ce_key_directory_path(user_id);
    auto const paths = get_ce_key_paths(directory_path);
    for (auto const ce_key_path : paths) {
        LOG(DEBUG) << "Trying user CE key " << ce_key_path;
        if (retrieveKey(ce_key_path, auth, ce_key)) {
            LOG(DEBUG) << "Successfully retrieved key";
            fixate_user_ce_key(directory_path, ce_key_path, paths);
            return true;
        }
    }
    LOG(ERROR) << "Failed to find working ce key for user " << user_id;
    return false;
}

static bool MightBeEmmcStorage(const std::string& blk_device) {
    // Handle symlinks.
    std::string real_path;
    if (!Realpath(blk_device, &real_path)) {
        real_path = blk_device;
    }

    // Handle logical volumes.
    auto& dm = DeviceMapper::Instance();
    for (;;) {
        auto parent = dm.GetParentBlockDeviceByPath(real_path);
        if (!parent.has_value()) break;
        real_path = *parent;
    }

    // Now we should have the "real" block device.
    LOG(DEBUG) << "MightBeEmmcStorage(): blk_device = " << blk_device
               << ", real_path=" << real_path;
    std::string name = Basename(real_path);
    return StartsWith(name, "mmcblk") ||
           // virtio devices may provide inline encryption support that is
           // backed by eMMC inline encryption on the host, thus inheriting the
           // DUN size limitation.  So virtio devices must be allowed here too.
           // TODO(b/207390665): check the maximum DUN size directly instead.
           StartsWith(name, "vd");
}

// Retrieve the options to use for encryption policies on the /data filesystem.
static bool get_data_file_encryption_options(EncryptionOptions* options) {
    auto entry = GetEntryForMountPoint(&fstab_default, DATA_MNT_POINT);
    if (entry == nullptr) {
        LOG(ERROR) << "No mount point entry for " << DATA_MNT_POINT;
        return false;
    }
    if (!ParseOptions(entry->encryption_options, options)) {
        LOG(ERROR) << "Unable to parse encryption options for " << DATA_MNT_POINT ": "
                   << entry->encryption_options;
        return false;
    }
    if ((options->flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) &&
        !MightBeEmmcStorage(entry->blk_device)) {
        LOG(ERROR) << "The emmc_optimized encryption flag is only allowed on eMMC storage.  Remove "
                      "this flag from the device's fstab";
        return false;
    }
    return true;
}

static bool install_storage_key(const std::string& mountpoint, const EncryptionOptions& options,
                                const KeyBuffer& key, EncryptionPolicy* policy) {
    KeyBuffer ephemeral_wrapped_key;
    if (options.use_hw_wrapped_key) {
        if (!exportWrappedStorageKey(key, &ephemeral_wrapped_key)) {
            LOG(ERROR) << "Failed to get ephemeral wrapped key";
            return false;
        }
    }
    return installKey(mountpoint, options, options.use_hw_wrapped_key ? ephemeral_wrapped_key : key,
                      policy);
}

// Retrieve the options to use for encryption policies on adoptable storage.
static bool get_volume_file_encryption_options(EncryptionOptions* options) {
    // If we give the empty string, libfscrypt will use the default (currently XTS)
    auto contents_mode = android::base::GetProperty("ro.crypto.volume.contents_mode", "");
    // HEH as default was always a mistake. Use the libfscrypt default (CTS)
    // for devices launching on versions above Android 10.
    auto first_api_level = GetFirstApiLevel();
    auto filenames_mode =
            android::base::GetProperty("ro.crypto.volume.filenames_mode",
                                       first_api_level > __ANDROID_API_Q__ ? "" : "aes-256-heh");
    auto options_string = android::base::GetProperty("ro.crypto.volume.options",
                                                     contents_mode + ":" + filenames_mode);
    if (!ParseOptionsForApiLevel(first_api_level, options_string, options)) {
        LOG(ERROR) << "Unable to parse volume encryption options: " << options_string;
        return false;
    }
    if (options->flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) {
        LOG(ERROR) << "The emmc_optimized encryption flag is only allowed on eMMC storage.  Remove "
                      "this flag from ro.crypto.volume.options";
        return false;
    }
    return true;
}

static bool read_and_install_user_ce_key(userid_t user_id,
                                         const android::vold::KeyAuthentication& auth) {
    if (s_ce_policies.count(user_id) != 0) return true;
    EncryptionOptions options;
    if (!get_data_file_encryption_options(&options)) return false;
    KeyBuffer ce_key;
    if (!read_and_fixate_user_ce_key(user_id, auth, &ce_key)) return false;
    EncryptionPolicy ce_policy;
    if (!install_storage_key(DATA_MNT_POINT, options, ce_key, &ce_policy)) return false;
    s_ce_policies[user_id] = ce_policy;
    LOG(DEBUG) << "Installed ce key for user " << user_id;
    return true;
}

// Prepare a directory without assigning it an encryption policy.  The directory
// will inherit the encryption policy of its parent directory, or will be
// unencrypted if the parent directory is unencrypted.
static bool prepare_dir(const std::string& dir, mode_t mode, uid_t uid, gid_t gid) {
    LOG(DEBUG) << "Preparing: " << dir;
    if (android::vold::PrepareDir(dir, mode, uid, gid, 0) != 0) {
        PLOG(ERROR) << "Failed to prepare " << dir;
        return false;
    }
    return true;
}

// Prepare a directory and assign it the given encryption policy.
static bool prepare_dir_with_policy(const std::string& dir, mode_t mode, uid_t uid, gid_t gid,
                                    const EncryptionPolicy& policy) {
    if (!prepare_dir(dir, mode, uid, gid)) return false;
    if (IsFbeEnabled() && !EnsurePolicy(policy, dir)) return false;
    return true;
}

static bool destroy_dir(const std::string& dir) {
    LOG(DEBUG) << "Destroying: " << dir;
    if (rmdir(dir.c_str()) != 0 && errno != ENOENT) {
        PLOG(ERROR) << "Failed to destroy " << dir;
        return false;
    }
    return true;
}

// Checks whether at least one CE key subdirectory exists.
static bool ce_key_exists(const std::string& directory_path) {
    // The common case is that "$dir/current" exists, so check for that first.
    if (android::vold::pathExists(get_ce_key_current_path(directory_path))) return true;

    // Else, there could still be another subdirectory of $dir (if a crash
    // occurred during fixate_user_ce_key()), so check for one.
    return android::vold::pathExists(directory_path) && !get_ce_key_paths(directory_path).empty();
}

// Creates and installs the CE and DE keys for the given user, as needed.
//
// We store the DE key right away.  We don't store the CE key yet, because the
// secret needed to do so securely isn't available yet.  Instead, we cache the
// CE key in memory and store it later in fscrypt_set_user_key_protection().
//
// For user 0, this function is called on every boot, so we need to create the
// keys only if they weren't already stored.  In doing so, we must consider the
// DE and CE keys independently, since the first boot might have been
// interrupted between the DE key being stored and the CE key being stored.
//
// For other users, this is only called at user creation time, and neither key
// directory should exist already.   |create_ephemeral| means that the user is
// ephemeral; in that case the keys are generated and installed, but not stored.
static bool create_and_install_user_keys(userid_t user_id, bool create_ephemeral) {
    EncryptionOptions options;
    if (!get_data_file_encryption_options(&options)) return false;

    auto de_key_path = get_de_key_path(user_id);
    if (create_ephemeral || !android::vold::pathExists(de_key_path)) {
        KeyBuffer de_key;
        if (!generateStorageKey(makeGen(options), &de_key)) return false;
        if (!create_ephemeral && !android::vold::storeKeyAtomically(de_key_path, user_key_temp,
                                                                    kEmptyAuthentication, de_key))
            return false;
        EncryptionPolicy de_policy;
        if (!install_storage_key(DATA_MNT_POINT, options, de_key, &de_policy)) return false;
        s_de_policies[user_id] = de_policy;
        LOG(INFO) << "Created DE key for user " << user_id;
    } else {
        if (user_id != 0) {
            LOG(ERROR) << "DE key already exists on disk";
            return false;
        }
    }

    auto ce_path = get_ce_key_directory_path(user_id);
    if (create_ephemeral || !ce_key_exists(ce_path)) {
        KeyBuffer ce_key;
        if (!generateStorageKey(makeGen(options), &ce_key)) return false;
        if (!create_ephemeral) {
            if (!prepare_dir(ce_path, 0700, AID_ROOT, AID_ROOT)) return false;
            s_new_ce_keys.insert({user_id, ce_key});
        }
        EncryptionPolicy ce_policy;
        if (!install_storage_key(DATA_MNT_POINT, options, ce_key, &ce_policy)) return false;
        s_ce_policies[user_id] = ce_policy;
        LOG(INFO) << "Created CE key for user " << user_id;
    } else {
        if (user_id != 0) {
            LOG(ERROR) << "CE key already exists on disk";
            return false;
        }
    }

    if (create_ephemeral) {
        s_ephemeral_users.insert(user_id);
    }
    return true;
}

static bool lookup_policy(const std::map<userid_t, EncryptionPolicy>& key_map, userid_t user_id,
                          EncryptionPolicy* policy) {
    auto refi = key_map.find(user_id);
    if (refi == key_map.end()) {
        return false;
    }
    *policy = refi->second;
    return true;
}

static bool is_numeric(const char* name) {
    for (const char* p = name; *p != '\0'; p++) {
        if (!isdigit(*p)) return false;
    }
    return true;
}

static bool load_all_de_keys() {
    EncryptionOptions options;
    if (!get_data_file_encryption_options(&options)) return false;
    auto de_dir = user_key_dir + "/de";
    auto dirp = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(de_dir.c_str()), closedir);
    if (!dirp) {
        PLOG(ERROR) << "Unable to read de key directory";
        return false;
    }
    for (;;) {
        errno = 0;
        auto entry = readdir(dirp.get());
        if (!entry) {
            if (errno) {
                PLOG(ERROR) << "Unable to read de key directory";
                return false;
            }
            break;
        }
        if (IsDotOrDotDot(*entry)) continue;
        if (entry->d_type != DT_DIR || !is_numeric(entry->d_name)) {
            LOG(DEBUG) << "Skipping non-de-key " << entry->d_name;
            continue;
        }
        userid_t user_id = std::stoi(entry->d_name);
        auto key_path = de_dir + "/" + entry->d_name;
        KeyBuffer de_key;
        if (!retrieveKey(key_path, kEmptyAuthentication, &de_key)) return false;
        EncryptionPolicy de_policy;
        if (!install_storage_key(DATA_MNT_POINT, options, de_key, &de_policy)) return false;
        auto ret = s_de_policies.insert({user_id, de_policy});
        if (!ret.second && ret.first->second != de_policy) {
            LOG(ERROR) << "DE policy for user" << user_id << " changed";
            return false;
        }
        LOG(DEBUG) << "Installed de key for user " << user_id;
    }
    // fscrypt:TODO: go through all DE directories, ensure that all user dirs have the
    // correct policy set on them, and that no rogue ones exist.
    return true;
}

// Attempt to reinstall CE keys for users that we think are unlocked.
static bool try_reload_ce_keys() {
    for (const auto& it : s_ce_policies) {
        if (!android::vold::reloadKeyFromSessionKeyring(DATA_MNT_POINT, it.second)) {
            LOG(ERROR) << "Failed to load CE key from session keyring for user " << it.first;
            return false;
        }
    }
    return true;
}

bool fscrypt_initialize_systemwide_keys() {
    LOG(INFO) << "fscrypt_initialize_systemwide_keys";

    EncryptionOptions options;
    if (!get_data_file_encryption_options(&options)) return false;

    KeyBuffer device_key;
    if (!retrieveOrGenerateKey(device_key_path, device_key_temp, kEmptyAuthentication,
                               makeGen(options), &device_key))
        return false;

    // This initializes s_device_policy, which is a global variable so that
    // fscrypt_init_user0() can access it later.
    if (!install_storage_key(DATA_MNT_POINT, options, device_key, &s_device_policy)) return false;

    std::string options_string;
    if (!OptionsToString(s_device_policy.options, &options_string)) {
        LOG(ERROR) << "Unable to serialize options";
        return false;
    }
    std::string options_filename = std::string(DATA_MNT_POINT) + fscrypt_key_mode;
    if (!android::vold::writeStringToFile(options_string, options_filename)) return false;

    std::string ref_filename = std::string(DATA_MNT_POINT) + fscrypt_key_ref;
    if (!android::vold::writeStringToFile(s_device_policy.key_raw_ref, ref_filename)) return false;
    LOG(INFO) << "Wrote system DE key reference to:" << ref_filename;

    KeyBuffer per_boot_key;
    if (!generateStorageKey(makeGen(options), &per_boot_key)) return false;
    EncryptionPolicy per_boot_policy;
    if (!install_storage_key(DATA_MNT_POINT, options, per_boot_key, &per_boot_policy)) return false;
    std::string per_boot_ref_filename = std::string("/data") + fscrypt_key_per_boot_ref;
    if (!android::vold::writeStringToFile(per_boot_policy.key_raw_ref, per_boot_ref_filename))
        return false;
    LOG(INFO) << "Wrote per boot key reference to:" << per_boot_ref_filename;

    return true;
}

static bool prepare_special_dirs() {
    // Ensure that /data/data and its "alias" /data/user/0 exist, and create the
    // bind mount of /data/data onto /data/user/0.  This *should* happen in
    // fscrypt_prepare_user_storage().  However, it actually must be done early,
    // before the rest of user 0's CE storage is prepared.  This is because
    // zygote may need to set up app data isolation before then, which requires
    // mounting a tmpfs over /data/data to ensure it remains hidden.  This issue
    // arises due to /data/data being in the top-level directory.

    // /data/user/0 used to be a symlink to /data/data, so we must first delete
    // the old symlink if present.
    if (android::vold::IsSymlink(data_user_0_dir) && android::vold::Unlink(data_user_0_dir) != 0)
        return false;
    // On first boot, we'll be creating /data/data for the first time, and user
    // 0's CE key will be installed already since it was just created.  Take the
    // opportunity to also set the encryption policy of /data/data right away.
    EncryptionPolicy ce_policy;
    if (lookup_policy(s_ce_policies, 0, &ce_policy)) {
        if (!prepare_dir_with_policy(data_data_dir, 0771, AID_SYSTEM, AID_SYSTEM, ce_policy)) {
            // Preparing /data/data failed, yet we had just generated a new CE
            // key because one wasn't stored.  Before erroring out, try deleting
            // the directory and retrying, as it's possible that the directory
            // exists with different CE policy from an interrupted first boot.
            if (rmdir(data_data_dir.c_str()) != 0) {
                PLOG(ERROR) << "rmdir " << data_data_dir << " failed";
            }
            if (!prepare_dir_with_policy(data_data_dir, 0771, AID_SYSTEM, AID_SYSTEM, ce_policy))
                return false;
        }
    } else {
        if (!prepare_dir(data_data_dir, 0771, AID_SYSTEM, AID_SYSTEM)) return false;
        // EnsurePolicy() will have to happen later, in fscrypt_prepare_user_storage().
    }
    if (!prepare_dir(data_user_0_dir, 0700, AID_SYSTEM, AID_SYSTEM)) return false;
    if (android::vold::BindMount(data_data_dir, data_user_0_dir) != 0) return false;

    // If /data/media/obb doesn't exist, create it and encrypt it with the
    // device policy.  Normally, device-policy-encrypted directories are created
    // and encrypted by init; /data/media/obb is special because it is located
    // in /data/media.  Since /data/media also contains per-user encrypted
    // directories, by design only vold can write to it.  As a side effect of
    // that, vold must create /data/media/obb.
    //
    // We must tolerate /data/media/obb being unencrypted if it already exists
    // on-disk, since it used to be unencrypted (b/64566063).
    if (android::vold::pathExists(media_obb_dir)) {
        if (!prepare_dir(media_obb_dir, 0770, AID_MEDIA_RW, AID_MEDIA_RW)) return false;
    } else {
        if (!prepare_dir_with_policy(media_obb_dir, 0770, AID_MEDIA_RW, AID_MEDIA_RW,
                                     s_device_policy))
            return false;
    }
    return true;
}

bool fscrypt_init_user0_done;

bool fscrypt_init_user0() {
    LOG(DEBUG) << "fscrypt_init_user0";

    if (IsFbeEnabled()) {
        if (!prepare_dir(user_key_dir, 0700, AID_ROOT, AID_ROOT)) return false;
        if (!prepare_dir(user_key_dir + "/ce", 0700, AID_ROOT, AID_ROOT)) return false;
        if (!prepare_dir(user_key_dir + "/de", 0700, AID_ROOT, AID_ROOT)) return false;
        if (!create_and_install_user_keys(0, false)) return false;
        // TODO: switch to loading only DE_0 here once framework makes
        // explicit calls to install DE keys for secondary users
        if (!load_all_de_keys()) return false;
    }

    // Now that user 0's CE key has been created, we can prepare /data/data.
    if (!prepare_special_dirs()) return false;

    // With the exception of what is done by prepare_special_dirs() above, we
    // only prepare DE storage here, since user 0's CE key won't be installed
    // yet unless it was just created.  The framework will prepare the user's CE
    // storage later, once their CE key is installed.
    if (!fscrypt_prepare_user_storage("", 0, 0, android::os::IVold::STORAGE_FLAG_DE)) {
        LOG(ERROR) << "Failed to prepare user 0 storage";
        return false;
    }

    // In some scenarios (e.g. userspace reboot) we might unmount userdata
    // without doing a hard reboot. If CE keys were stored in fs keyring then
    // they will be lost after unmount. Attempt to re-install them.
    if (IsFbeEnabled() && android::vold::isFsKeyringSupported()) {
        if (!try_reload_ce_keys()) return false;
    }

    fscrypt_init_user0_done = true;
    return true;
}

bool fscrypt_vold_create_user_key(userid_t user_id, int serial, bool ephemeral) {
    LOG(DEBUG) << "fscrypt_vold_create_user_key for " << user_id << " serial " << serial;
    if (!IsFbeEnabled()) {
        return true;
    }
    // FIXME test for existence of key that is not loaded yet
    if (s_ce_policies.count(user_id) != 0) {
        LOG(ERROR) << "Already exists, can't fscrypt_vold_create_user_key for " << user_id
                   << " serial " << serial;
        // FIXME should we fail the command?
        return true;
    }
    if (!create_and_install_user_keys(user_id, ephemeral)) {
        return false;
    }
    return true;
}

// "Lock" all encrypted directories whose key has been removed.  This is needed
// in the case where the keys are being put in the session keyring (rather in
// the newer filesystem-level keyrings), because removing a key from the session
// keyring doesn't affect inodes in the kernel's inode cache whose per-file key
// was already set up.  So to remove the per-file keys and make the files
// "appear encrypted", these inodes must be evicted.
//
// To do this, sync() to clean all dirty inodes, then drop all reclaimable slab
// objects systemwide.  This is overkill, but it's the best available method
// currently.  Don't use drop_caches mode "3" because that also evicts pagecache
// for in-use files; all files relevant here are already closed and sync'ed.
static void drop_caches_if_needed() {
    if (android::vold::isFsKeyringSupported()) {
        return;
    }
    sync();
    if (!writeStringToFile("2", "/proc/sys/vm/drop_caches")) {
        PLOG(ERROR) << "Failed to drop caches during key eviction";
    }
}

static bool evict_ce_key(userid_t user_id) {
    bool success = true;
    EncryptionPolicy policy;
    // If we haven't loaded the CE key, no need to evict it.
    if (lookup_policy(s_ce_policies, user_id, &policy)) {
        success &= android::vold::evictKey(DATA_MNT_POINT, policy);
        drop_caches_if_needed();
    }
    s_ce_policies.erase(user_id);
    s_new_ce_keys.erase(user_id);
    return success;
}

bool fscrypt_destroy_user_key(userid_t user_id) {
    LOG(DEBUG) << "fscrypt_destroy_user_key(" << user_id << ")";
    if (!IsFbeEnabled()) {
        return true;
    }
    bool success = true;
    success &= evict_ce_key(user_id);
    EncryptionPolicy de_policy;
    success &= lookup_policy(s_de_policies, user_id, &de_policy) &&
               android::vold::evictKey(DATA_MNT_POINT, de_policy);
    s_de_policies.erase(user_id);
    if (!s_ephemeral_users.erase(user_id)) {
        auto ce_path = get_ce_key_directory_path(user_id);
        if (!s_new_ce_keys.erase(user_id)) {
            for (auto const path : get_ce_key_paths(ce_path)) {
                success &= android::vold::destroyKey(path);
            }
        }
        success &= destroy_dir(ce_path);

        auto de_key_path = get_de_key_path(user_id);
        if (android::vold::pathExists(de_key_path)) {
            success &= android::vold::destroyKey(de_key_path);
        } else {
            LOG(INFO) << "Not present so not erasing: " << de_key_path;
        }
    }
    return success;
}

static bool parse_hex(const std::string& hex, std::string* result) {
    if (hex == "!") {
        *result = "";
        return true;
    }
    if (android::vold::HexToStr(hex, *result) != 0) {
        LOG(ERROR) << "Invalid FBE hex string";  // Don't log the string for security reasons
        return false;
    }
    return true;
}

static std::optional<android::vold::KeyAuthentication> authentication_from_hex(
        const std::string& secret_hex) {
    std::string secret;
    if (!parse_hex(secret_hex, &secret)) return std::optional<android::vold::KeyAuthentication>();
    if (secret.empty()) {
        return kEmptyAuthentication;
    } else {
        return android::vold::KeyAuthentication(secret);
    }
}

static std::string volkey_path(const std::string& misc_path, const std::string& volume_uuid) {
    return misc_path + "/vold/volume_keys/" + volume_uuid + "/default";
}

static std::string volume_secdiscardable_path(const std::string& volume_uuid) {
    return systemwide_volume_key_dir + "/" + volume_uuid + "/secdiscardable";
}

static bool read_or_create_volkey(const std::string& misc_path, const std::string& volume_uuid,
                                  EncryptionPolicy* policy) {
    auto secdiscardable_path = volume_secdiscardable_path(volume_uuid);
    std::string secdiscardable_hash;
    if (android::vold::pathExists(secdiscardable_path)) {
        if (!android::vold::readSecdiscardable(secdiscardable_path, &secdiscardable_hash))
            return false;
    } else {
        if (!android::vold::MkdirsSync(secdiscardable_path, 0700)) return false;
        if (!android::vold::createSecdiscardable(secdiscardable_path, &secdiscardable_hash))
            return false;
    }
    auto key_path = volkey_path(misc_path, volume_uuid);
    if (!android::vold::MkdirsSync(key_path, 0700)) return false;
    android::vold::KeyAuthentication auth(secdiscardable_hash);

    EncryptionOptions options;
    if (!get_volume_file_encryption_options(&options)) return false;
    KeyBuffer key;
    if (!retrieveOrGenerateKey(key_path, key_path + "_tmp", auth, makeGen(options), &key))
        return false;
    if (!install_storage_key(BuildDataPath(volume_uuid), options, key, policy)) return false;
    return true;
}

static bool destroy_volkey(const std::string& misc_path, const std::string& volume_uuid) {
    auto path = volkey_path(misc_path, volume_uuid);
    if (!android::vold::pathExists(path)) return true;
    return android::vold::destroyKey(path);
}

// (Re-)encrypts the user's CE key with the given secret.  The CE key must
// either be (a) new (not yet committed), (b) protected by kEmptyAuthentication,
// or (c) already protected by the given secret.  Cases (b) and (c) are needed
// to support upgrades from Android versions where CE keys were stored with
// kEmptyAuthentication when the user didn't have an LSKF.  Case (b) is the
// normal upgrade case, while case (c) can theoretically happen if an upgrade is
// requested for a user more than once due to a power-off or other interruption.
bool fscrypt_set_user_key_protection(userid_t user_id, const std::string& secret_hex) {
    LOG(DEBUG) << "fscrypt_set_user_key_protection " << user_id;
    if (!IsFbeEnabled()) return true;
    auto auth = authentication_from_hex(secret_hex);
    if (!auth) return false;
    if (auth->secret.empty()) {
        LOG(ERROR) << "fscrypt_set_user_key_protection: secret must be nonempty";
        return false;
    }
    if (s_ephemeral_users.count(user_id) != 0) {
        LOG(DEBUG) << "Not storing key because user is ephemeral";
        return true;
    }
    auto const directory_path = get_ce_key_directory_path(user_id);
    KeyBuffer ce_key;
    auto it = s_new_ce_keys.find(user_id);
    if (it != s_new_ce_keys.end()) {
        // Committing the key for a new user.  This happens when the user's
        // synthetic password is created.
        ce_key = it->second;
    } else {
        // Setting the protection on an existing key.  This happens at upgrade
        // time, when CE keys that were previously protected by
        // kEmptyAuthentication are encrypted by the user's synthetic password.
        LOG(DEBUG) << "Key already exists; re-protecting it with the given secret";
        if (!read_and_fixate_user_ce_key(user_id, kEmptyAuthentication, &ce_key)) {
            LOG(ERROR) << "Failed to retrieve key for user " << user_id << " using empty auth";
            // Before failing, also check whether the key is already protected
            // with the given secret.  This isn't expected, but in theory it
            // could happen if an upgrade is requested for a user more than once
            // due to a power-off or other interruption.
            if (read_and_fixate_user_ce_key(user_id, *auth, &ce_key)) {
                LOG(WARNING) << "Key is already protected by given secret";
                return true;
            }
            return false;
        }
    }
    auto const paths = get_ce_key_paths(directory_path);
    std::string ce_key_path;
    if (!get_ce_key_new_path(directory_path, paths, &ce_key_path)) return false;
    if (!android::vold::storeKeyAtomically(ce_key_path, user_key_temp, *auth, ce_key)) return false;
    if (!fixate_user_ce_key(directory_path, ce_key_path, paths)) return false;
    if (s_new_ce_keys.erase(user_id)) {
        LOG(INFO) << "Stored CE key for new user " << user_id;
    }
    return true;
}

std::vector<int> fscrypt_get_unlocked_users() {
    std::vector<int> user_ids;
    for (const auto& it : s_ce_policies) {
        user_ids.push_back(it.first);
    }
    return user_ids;
}

// TODO: rename to 'install' for consistency, and take flags to know which keys to install
bool fscrypt_unlock_user_key(userid_t user_id, int serial, const std::string& secret_hex) {
    LOG(DEBUG) << "fscrypt_unlock_user_key " << user_id << " serial=" << serial;
    if (IsFbeEnabled()) {
        if (s_ce_policies.count(user_id) != 0) {
            LOG(WARNING) << "Tried to unlock already-unlocked key for user " << user_id;
            return true;
        }
        auto auth = authentication_from_hex(secret_hex);
        if (!auth) return false;
        if (!read_and_install_user_ce_key(user_id, *auth)) {
            LOG(ERROR) << "Couldn't read key for " << user_id;
            return false;
        }
    }
    return true;
}

// TODO: rename to 'evict' for consistency
bool fscrypt_lock_user_key(userid_t user_id) {
    LOG(DEBUG) << "fscrypt_lock_user_key " << user_id;
    if (IsFbeEnabled()) {
        return evict_ce_key(user_id);
    }
    return true;
}

static bool prepare_subdirs(const std::string& action, const std::string& volume_uuid,
                            userid_t user_id, int flags) {
    if (0 != android::vold::ForkExecvp(
                 std::vector<std::string>{prepare_subdirs_path, action, volume_uuid,
                                          std::to_string(user_id), std::to_string(flags)})) {
        LOG(ERROR) << "vold_prepare_subdirs failed";
        return false;
    }
    return true;
}

bool fscrypt_prepare_user_storage(const std::string& volume_uuid, userid_t user_id, int serial,
                                  int flags) {
    LOG(DEBUG) << "fscrypt_prepare_user_storage for volume " << escape_empty(volume_uuid)
               << ", user " << user_id << ", serial " << serial << ", flags " << flags;

    // Internal storage must be prepared before adoptable storage, since the
    // user's volume keys are stored in their internal storage.
    if (!volume_uuid.empty()) {
        if ((flags & android::os::IVold::STORAGE_FLAG_DE) &&
            !android::vold::pathExists(android::vold::BuildDataMiscDePath("", user_id))) {
            LOG(ERROR) << "Cannot prepare DE storage for user " << user_id << " on volume "
                       << volume_uuid << " before internal storage";
            return false;
        }
        if ((flags & android::os::IVold::STORAGE_FLAG_CE) &&
            !android::vold::pathExists(android::vold::BuildDataMiscCePath("", user_id))) {
            LOG(ERROR) << "Cannot prepare CE storage for user " << user_id << " on volume "
                       << volume_uuid << " before internal storage";
            return false;
        }
    }

    if (flags & android::os::IVold::STORAGE_FLAG_DE) {
        // DE_sys key
        auto system_legacy_path = android::vold::BuildDataSystemLegacyPath(user_id);
        auto misc_legacy_path = android::vold::BuildDataMiscLegacyPath(user_id);
        auto profiles_de_path = android::vold::BuildDataProfilesDePath(user_id);

        // DE_n key
        EncryptionPolicy de_policy;
        auto system_de_path = android::vold::BuildDataSystemDePath(user_id);
        auto misc_de_path = android::vold::BuildDataMiscDePath(volume_uuid, user_id);
        auto vendor_de_path = android::vold::BuildDataVendorDePath(user_id);
        auto user_de_path = android::vold::BuildDataUserDePath(volume_uuid, user_id);

        if (IsFbeEnabled()) {
            if (volume_uuid.empty()) {
                if (!lookup_policy(s_de_policies, user_id, &de_policy)) {
                    LOG(ERROR) << "Cannot find DE policy for user " << user_id;
                    return false;
                }
            } else {
                auto misc_de_empty_volume_path = android::vold::BuildDataMiscDePath("", user_id);
                if (!read_or_create_volkey(misc_de_empty_volume_path, volume_uuid, &de_policy)) {
                    return false;
                }
            }
        }

        if (volume_uuid.empty()) {
            if (!prepare_dir(system_legacy_path, 0700, AID_SYSTEM, AID_SYSTEM)) return false;
#if MANAGE_MISC_DIRS
            if (!prepare_dir(misc_legacy_path, 0750, multiuser_get_uid(user_id, AID_SYSTEM),
                             multiuser_get_uid(user_id, AID_EVERYBODY)))
                return false;
#endif
            if (!prepare_dir(profiles_de_path, 0771, AID_SYSTEM, AID_SYSTEM)) return false;

            if (!prepare_dir_with_policy(system_de_path, 0770, AID_SYSTEM, AID_SYSTEM, de_policy))
                return false;
            if (!prepare_dir_with_policy(vendor_de_path, 0771, AID_ROOT, AID_ROOT, de_policy))
                return false;
        }

        if (!prepare_dir_with_policy(misc_de_path, 01771, AID_SYSTEM, AID_MISC, de_policy))
            return false;
        if (!prepare_dir_with_policy(user_de_path, 0771, AID_SYSTEM, AID_SYSTEM, de_policy))
            return false;
    }

    if (flags & android::os::IVold::STORAGE_FLAG_CE) {
        // CE_n key
        EncryptionPolicy ce_policy;
        auto system_ce_path = android::vold::BuildDataSystemCePath(user_id);
        auto misc_ce_path = android::vold::BuildDataMiscCePath(volume_uuid, user_id);
        auto vendor_ce_path = android::vold::BuildDataVendorCePath(user_id);
        auto media_ce_path = android::vold::BuildDataMediaCePath(volume_uuid, user_id);
        auto user_ce_path = android::vold::BuildDataUserCePath(volume_uuid, user_id);

        if (IsFbeEnabled()) {
            if (volume_uuid.empty()) {
                if (!lookup_policy(s_ce_policies, user_id, &ce_policy)) {
                    LOG(ERROR) << "Cannot find CE policy for user " << user_id;
                    return false;
                }
            } else {
                auto misc_ce_empty_volume_path = android::vold::BuildDataMiscCePath("", user_id);
                if (!read_or_create_volkey(misc_ce_empty_volume_path, volume_uuid, &ce_policy)) {
                    return false;
                }
            }
        }

        if (volume_uuid.empty()) {
            if (!prepare_dir_with_policy(system_ce_path, 0770, AID_SYSTEM, AID_SYSTEM, ce_policy))
                return false;
            if (!prepare_dir_with_policy(vendor_ce_path, 0771, AID_ROOT, AID_ROOT, ce_policy))
                return false;
        }
        if (!prepare_dir_with_policy(media_ce_path, 02770, AID_MEDIA_RW, AID_MEDIA_RW, ce_policy))
            return false;
        // On devices without sdcardfs (kernel 5.4+), the path permissions aren't fixed
        // up automatically; therefore, use a default ACL, to ensure apps with MEDIA_RW
        // can keep reading external storage; in particular, this allows app cloning
        // scenarios to work correctly on such devices.
        int ret = SetDefaultAcl(media_ce_path, 02770, AID_MEDIA_RW, AID_MEDIA_RW, {AID_MEDIA_RW});
        if (ret != android::OK) {
            return false;
        }
        if (!prepare_dir_with_policy(misc_ce_path, 01771, AID_SYSTEM, AID_MISC, ce_policy))
            return false;
        if (!prepare_dir_with_policy(user_ce_path, 0771, AID_SYSTEM, AID_SYSTEM, ce_policy))
            return false;

        if (volume_uuid.empty()) {
            // Now that credentials have been installed, we can run restorecon
            // over these paths
            // NOTE: these paths need to be kept in sync with libselinux
            android::vold::RestoreconRecursive(system_ce_path);
            android::vold::RestoreconRecursive(vendor_ce_path);
            android::vold::RestoreconRecursive(misc_ce_path);
        }
    }
    if (!prepare_subdirs("prepare", volume_uuid, user_id, flags)) return false;

    return true;
}

bool fscrypt_destroy_user_storage(const std::string& volume_uuid, userid_t user_id, int flags) {
    LOG(DEBUG) << "fscrypt_destroy_user_storage for volume " << escape_empty(volume_uuid)
               << ", user " << user_id << ", flags " << flags;
    bool res = true;

    res &= prepare_subdirs("destroy", volume_uuid, user_id, flags);

    if (flags & android::os::IVold::STORAGE_FLAG_CE) {
        // CE_n key
        auto system_ce_path = android::vold::BuildDataSystemCePath(user_id);
        auto misc_ce_path = android::vold::BuildDataMiscCePath(volume_uuid, user_id);
        auto vendor_ce_path = android::vold::BuildDataVendorCePath(user_id);
        auto media_ce_path = android::vold::BuildDataMediaCePath(volume_uuid, user_id);
        auto user_ce_path = android::vold::BuildDataUserCePath(volume_uuid, user_id);

        res &= destroy_dir(media_ce_path);
        res &= destroy_dir(misc_ce_path);
        res &= destroy_dir(user_ce_path);
        if (volume_uuid.empty()) {
            res &= destroy_dir(system_ce_path);
            res &= destroy_dir(vendor_ce_path);
        } else {
            if (IsFbeEnabled()) {
                auto misc_ce_empty_volume_path = android::vold::BuildDataMiscCePath("", user_id);
                res &= destroy_volkey(misc_ce_empty_volume_path, volume_uuid);
            }
        }
    }

    if (flags & android::os::IVold::STORAGE_FLAG_DE) {
        // DE_sys key
        auto system_legacy_path = android::vold::BuildDataSystemLegacyPath(user_id);
        auto misc_legacy_path = android::vold::BuildDataMiscLegacyPath(user_id);
        auto profiles_de_path = android::vold::BuildDataProfilesDePath(user_id);

        // DE_n key
        auto system_de_path = android::vold::BuildDataSystemDePath(user_id);
        auto misc_de_path = android::vold::BuildDataMiscDePath(volume_uuid, user_id);
        auto vendor_de_path = android::vold::BuildDataVendorDePath(user_id);
        auto user_de_path = android::vold::BuildDataUserDePath(volume_uuid, user_id);

        res &= destroy_dir(user_de_path);
        res &= destroy_dir(misc_de_path);
        if (volume_uuid.empty()) {
            res &= destroy_dir(system_legacy_path);
#if MANAGE_MISC_DIRS
            res &= destroy_dir(misc_legacy_path);
#endif
            res &= destroy_dir(profiles_de_path);
            res &= destroy_dir(system_de_path);
            res &= destroy_dir(vendor_de_path);
        } else {
            if (IsFbeEnabled()) {
                auto misc_de_empty_volume_path = android::vold::BuildDataMiscDePath("", user_id);
                res &= destroy_volkey(misc_de_empty_volume_path, volume_uuid);
            }
        }
    }

    return res;
}

static bool destroy_volume_keys(const std::string& directory_path, const std::string& volume_uuid) {
    auto dirp = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(directory_path.c_str()), closedir);
    if (!dirp) {
        PLOG(ERROR) << "Unable to open directory: " + directory_path;
        return false;
    }
    bool res = true;
    for (;;) {
        errno = 0;
        auto const entry = readdir(dirp.get());
        if (!entry) {
            if (errno) {
                PLOG(ERROR) << "Unable to read directory: " + directory_path;
                return false;
            }
            break;
        }
        if (IsDotOrDotDot(*entry)) continue;
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') {
            LOG(DEBUG) << "Skipping non-user " << entry->d_name;
            continue;
        }
        res &= destroy_volkey(directory_path + "/" + entry->d_name, volume_uuid);
    }
    return res;
}

bool fscrypt_destroy_volume_keys(const std::string& volume_uuid) {
    bool res = true;
    LOG(DEBUG) << "fscrypt_destroy_volume_keys for volume " << escape_empty(volume_uuid);
    auto secdiscardable_path = volume_secdiscardable_path(volume_uuid);
    res &= android::vold::runSecdiscardSingle(secdiscardable_path);
    res &= destroy_volume_keys("/data/misc_ce", volume_uuid);
    res &= destroy_volume_keys("/data/misc_de", volume_uuid);
    return res;
}
