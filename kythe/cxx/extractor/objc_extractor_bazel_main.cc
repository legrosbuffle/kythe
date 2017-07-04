/*
 * Copyright 2016 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// objc_extractor_bazel is a Objective-C extractor meant to be run as a Bazel
// extra_action. It should be used with third_party/bazel/get_devdir.sh and
// third_party/bazel/get_sdkroot.sh.
//
// For example:
//
//  action_listener(
//    name = "extract_kindex",
//    extra_actions = [":extra_action"],
//    mnemonics = ["ObjcCompile"],
//    visibility = ["//visibility:public"],
//  )
//
//  extra_action(
//    name = "extra_action",
//    cmd = "$(location :objc_extractor_binary) \
//             $(EXTRA_ACTION_FILE) \
//             $(output $(ACTION_ID).objc.kindex) \
//             $(location :vnames_config) \
//             $(location :get_devdir) \
//             $(location :get_sdkroot)",
//    data = [
//      ":get_devdir",
//      ":get_sdkroot",
//      ":vnames_config",
//    ],
//    out_templates = ["$(ACTION_ID).objc.kindex"],
//    tools = [":objc_extractor_binary"],
//  )
//
//  # In this example, the extractor binary is pre-built.
//  filegroup(
//    name = "objc_extractor_binary",
//    srcs = ["objc_extractor_bazel"],
//  )
//
//  filegroup(
//    name = "vnames_config",
//    srcs = ["vnames.json"],
//  )
//
//  sh_binary(
//    name = "get_devdir",
//    srcs = ["get_devdir.sh"],
//  )
//
//  sh_binary(
//    name = "get_sdkroot",
//    srcs = ["get_sdkroot.sh"],
//  )

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/stubs/common.h"
#include "kythe/cxx/common/language.h"
#include "third_party/bazel/src/main/protobuf/extra_actions_base.pb.h"

#include "cxx_extractor.h"
#include "objc_bazel_support.h"

struct XAState {
  std::string extra_action_file;
  std::string output_file;
  std::string vname_config;
  std::string devdir_script;
  std::string sdkroot_script;
};

static bool ContainsUnsupportedArg(const std::vector<std::string> &args) {
  for (const auto &arg : args) {
    // We do not support compilations using modules yet.
    if (arg == "-fmodules") {
      return true;
    }
  }
  return false;
}

static bool LoadSpawnInfo(const XAState &xa_state,
                          const blaze::ExtraActionInfo &info,
                          kythe::ExtractorConfiguration &config) {
  blaze::SpawnInfo spawn_info = info.GetExtension(blaze::SpawnInfo::spawn_info);

  auto cmdPrefix = kythe::BuildEnvVarCommandPrefix(spawn_info.variable());
  auto devdir = kythe::RunScript(cmdPrefix + xa_state.devdir_script);
  auto sdkroot = kythe::RunScript(cmdPrefix + xa_state.sdkroot_script);

  std::vector<std::string> args;
  kythe::FillWithFixedArgs(args, spawn_info, devdir, sdkroot);

  if (ContainsUnsupportedArg(args)) {
    LOG(INFO) << "Not extracting " << info.owner()
              << " because it had an unsupported argument.";
    return false;
  }

  config.SetKindexOutputFile(xa_state.output_file);
  config.SetArgs(args);
  config.SetVNameConfig(xa_state.vname_config);
  config.SetTargetName(info.owner());
  if (spawn_info.output_file_size() > 0) {
    config.SetOutputPath(spawn_info.output_file(0));
  }
  return true;
}

static bool LoadCppInfo(const XAState &xa_state,
                        const blaze::ExtraActionInfo &info,
                        kythe::ExtractorConfiguration &config) {
  blaze::CppCompileInfo cpp_info =
      info.GetExtension(blaze::CppCompileInfo::cpp_compile_info);

  auto cmdPrefix = kythe::BuildEnvVarCommandPrefix(cpp_info.variable());
  auto devdir = kythe::RunScript(cmdPrefix + xa_state.devdir_script);
  auto sdkroot = kythe::RunScript(cmdPrefix + xa_state.sdkroot_script);

  std::vector<std::string> args;
  kythe::FillWithFixedArgs(args, cpp_info, devdir, sdkroot);

  if (ContainsUnsupportedArg(args)) {
    LOG(INFO) << "Not extracting " << info.owner()
              << " because it had an unsupported argument.";
    return false;
  }

  config.SetKindexOutputFile(xa_state.output_file);
  config.SetArgs(args);
  config.SetVNameConfig(xa_state.vname_config);
  config.SetTargetName(info.owner());
  config.SetOutputPath(cpp_info.output_file());
  return true;
}

static bool LoadExtraAction(const XAState &xa_state,
                            kythe::ExtractorConfiguration &config) {
  using namespace google::protobuf::io;
  blaze::ExtraActionInfo info;
  int fd =
      open(xa_state.extra_action_file.c_str(), O_RDONLY, S_IREAD | S_IWRITE);
  CHECK_GE(fd, 0) << "Couldn't open input file " << xa_state.extra_action_file;
  FileInputStream file_input_stream(fd);
  CodedInputStream coded_input_stream(&file_input_stream);
  coded_input_stream.SetTotalBytesLimit(INT_MAX, -1);
  CHECK(info.ParseFromCodedStream(&coded_input_stream));
  close(fd);

  if (info.HasExtension(blaze::SpawnInfo::spawn_info)) {
    return LoadSpawnInfo(xa_state, info, config);
  } else if (info.HasExtension(blaze::CppCompileInfo::cpp_compile_info)) {
    return LoadCppInfo(xa_state, info, config);
  }
  LOG(ERROR)
      << "ObjcCompile Extra Action didn't have SpawnInfo or CppCompileInfo.";
  return false;
}

int main(int argc, char *argv[]) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  google::InitGoogleLogging(argv[0]);
  gflags::SetVersionString("0.1");
  if (argc != 6) {
    fprintf(stderr,
            "Invalid number of arguments:\n\tCall as %s extra-action-file "
            "output-file vname-config devdir-script sdkroot-script\n",
            argv[0]);
    return 1;
  }
  XAState xa_state;
  xa_state.extra_action_file = argv[1];
  xa_state.output_file = argv[2];
  xa_state.vname_config = argv[3];
  xa_state.devdir_script = argv[4];
  xa_state.sdkroot_script = argv[5];

  kythe::ExtractorConfiguration config;
  bool success = LoadExtraAction(xa_state, config);
  if (success) {
    config.Extract(kythe::supported_language::Language::kObjectiveC);
  } else {
    // If we couldn't extract, just write an empty output file. This way the
    // extra_action will be a success from bazel's perspective, which should
    // remove some log spam.
    auto F = fopen(xa_state.output_file.c_str(), "w");
    if (F != nullptr) {
      fclose(F);
    }
  }
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
