/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "artd.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aidl/com/android/server/art/BnArtd.h"
#include "android-base/errors.h"
#include "android-base/logging.h"
#include "android-base/result.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "android/binder_auto_utils.h"
#include "android/binder_manager.h"
#include "android/binder_process.h"
#include "base/array_ref.h"
#include "base/file_utils.h"
#include "oat_file_assistant.h"
#include "path_utils.h"
#include "runtime.h"
#include "tools/tools.h"

namespace art {
namespace artd {

namespace {

using ::aidl::com::android::server::art::ArtifactsPath;
using ::aidl::com::android::server::art::GetOptimizationStatusResult;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::Split;
using ::android::base::StringPrintf;
using ::ndk::ScopedAStatus;

constexpr const char* kServiceName = "artd";

// Deletes a file. Returns the size of the deleted file, or 0 if the deleted file is empty or an
// error occurs.
int64_t GetSizeAndDeleteFile(const std::string& path) {
  std::error_code ec;
  int64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    // It is okay if the file does not exist. We don't have to log it.
    if (ec.value() != ENOENT) {
      LOG(ERROR) << StringPrintf(
          "Failed to get the file size of '%s': %s", path.c_str(), ec.message().c_str());
    }
    return 0;
  }

  if (!std::filesystem::remove(path, ec)) {
    LOG(ERROR) << StringPrintf("Failed to remove '%s': %s", path.c_str(), ec.message().c_str());
    return 0;
  }

  return size;
}

}  // namespace

ScopedAStatus Artd::isAlive(bool* _aidl_return) {
  *_aidl_return = true;
  return ScopedAStatus::ok();
}

ScopedAStatus Artd::deleteArtifacts(const ArtifactsPath& in_artifactsPath, int64_t* _aidl_return) {
  Result<std::string> oat_path = BuildOatPath(in_artifactsPath);
  if (!oat_path.ok()) {
    return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_STATE,
                                                       oat_path.error().message().c_str());
  }

  *_aidl_return = 0;
  *_aidl_return += GetSizeAndDeleteFile(*oat_path);
  *_aidl_return += GetSizeAndDeleteFile(OatPathToVdexPath(*oat_path));
  *_aidl_return += GetSizeAndDeleteFile(OatPathToArtPath(*oat_path));

  return ScopedAStatus::ok();
}

ScopedAStatus Artd::getOptimizationStatus(const std::string& in_dexFile,
                                          const std::string& in_instructionSet,
                                          const std::string& in_classLoaderContext,
                                          GetOptimizationStatusResult* _aidl_return) {
  Result<OatFileAssistant::RuntimeOptions> runtime_options = GetRuntimeOptions();
  if (!runtime_options.ok()) {
    return ScopedAStatus::fromExceptionCodeWithMessage(
        EX_ILLEGAL_STATE,
        ("Failed to get runtime options: " + runtime_options.error().message()).c_str());
  }

  std::unique_ptr<ClassLoaderContext> context;
  std::string error_msg;
  auto oat_file_assistant = OatFileAssistant::Create(
      in_dexFile.c_str(),
      in_instructionSet.c_str(),
      in_classLoaderContext.c_str(),
      /*load_executable=*/false,
      /*only_load_trusted_executable=*/true,
      std::make_unique<OatFileAssistant::RuntimeOptions>(std::move(*runtime_options)),
      &context,
      &error_msg);
  if (oat_file_assistant == nullptr) {
    return ScopedAStatus::fromExceptionCodeWithMessage(
        EX_ILLEGAL_STATE, ("Failed to create OatFileAssistant: " + error_msg).c_str());
  }

  std::string ignored_odex_status;
  oat_file_assistant->GetOptimizationStatus(&_aidl_return->compilerFilter,
                                            &_aidl_return->compilationReason,
                                            &_aidl_return->locationDebugString,
                                            &ignored_odex_status);

  // We ignore odex_status because it is not meaningful. It can only be either "up-to-date",
  // "apk-more-recent", or "io-error-no-oat", which means it doesn't give us information in addition
  // to what we can learn from compiler_filter because compiler_filter will be the actual compiler
  // filter, "run-from-apk-fallback", and "run-from-apk" in those three cases respectively.
  DCHECK(ignored_odex_status == "up-to-date" || ignored_odex_status == "apk-more-recent" ||
         ignored_odex_status == "io-error-no-oat");

  return ScopedAStatus::ok();
}

Result<void> Artd::Start() {
  ScopedAStatus status = ScopedAStatus::fromStatus(
      AServiceManager_registerLazyService(this->asBinder().get(), kServiceName));
  if (!status.isOk()) {
    return Error() << status.getDescription();
  }

  ABinderProcess_startThreadPool();

  return {};
}

Result<OatFileAssistant::RuntimeOptions> Artd::GetRuntimeOptions() {
  return OatFileAssistant::RuntimeOptions{
      .image_locations = *OR_RETURN(GetBootImageLocations()),
      .boot_class_path = *OR_RETURN(GetBootClassPath()),
      .boot_class_path_locations = *OR_RETURN(GetBootClassPath()),
      .deny_art_apex_data_files = DenyArtApexDataFiles(),
      .apex_versions = *OR_RETURN(GetApexVersions()),
  };
}

Result<const std::vector<std::string>*> Artd::GetBootImageLocations() {
  if (!cached_boot_image_locations_.has_value()) {
    std::string location_str;

    if (UseJitZygote()) {
      location_str = GetJitZygoteBootImageLocation();
    } else if (std::string value = props_->GetOrEmpty("dalvik.vm.boot-image"); !value.empty()) {
      location_str = std::move(value);
    } else {
      std::string error_msg;
      std::string android_root = GetAndroidRootSafe(&error_msg);
      if (!error_msg.empty()) {
        return Errorf("Failed to get ANDROID_ROOT: {}", error_msg);
      }
      location_str = GetDefaultBootImageLocation(android_root, DenyArtApexDataFiles());
    }

    cached_boot_image_locations_ = Split(location_str, ":");
  }

  return &cached_boot_image_locations_.value();
}

Result<const std::vector<std::string>*> Artd::GetBootClassPath() {
  if (!cached_boot_class_path_.has_value()) {
    const char* env_value = getenv("BOOTCLASSPATH");
    if (env_value == nullptr || strlen(env_value) == 0) {
      return Errorf("Failed to get environment variable 'BOOTCLASSPATH'");
    }
    cached_boot_class_path_ = Split(env_value, ":");
  }

  return &cached_boot_class_path_.value();
}

android::base::Result<const std::string*> Artd::GetApexVersions() {
  if (!cached_apex_versions_.has_value()) {
    cached_apex_versions_ =
        Runtime::GetApexVersions(ArrayRef<const std::string>(*OR_RETURN(GetBootClassPath())));
  }

  return &cached_apex_versions_.value();
}

bool Artd::UseJitZygote() {
  if (!cached_use_jit_zygote_.has_value()) {
    cached_use_jit_zygote_ =
        props_->GetBool("dalvik.vm.profilebootclasspath",
                        "persist.device_config.runtime_native_boot.profilebootclasspath",
                        /*default_value=*/false);
  }

  return cached_use_jit_zygote_.value();
}

bool Artd::DenyArtApexDataFiles() {
  if (!cached_deny_art_apex_data_files_.has_value()) {
    cached_deny_art_apex_data_files_ =
        !props_->GetBool("odsign.verification.success", /*default_value=*/false);
  }

  return cached_deny_art_apex_data_files_.value();
}

}  // namespace artd
}  // namespace art
