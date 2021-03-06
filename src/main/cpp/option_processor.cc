// Copyright 2014 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/main/cpp/option_processor.h"
#include "src/main/cpp/option_processor-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cassert>
#include <set>
#include <sstream>
#include <utility>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/logging.h"
#include "src/main/cpp/util/path.h"
#include "src/main/cpp/util/path_platform.h"
#include "src/main/cpp/util/strings.h"
#include "src/main/cpp/workspace_layout.h"

// On OSX, there apparently is no header that defines this.
#ifndef environ
extern char **environ;
#endif

namespace blaze {

using std::map;
using std::set;
using std::string;
using std::vector;

constexpr char WorkspaceLayout::WorkspacePrefix[];
static constexpr const char* kRcBasename = ".bazelrc";
static std::vector<std::string> GetProcessedEnv();

// Path to the system-wide bazelrc configuration file.
// This is a mutable global for testing purposes only.
const char* system_bazelrc_path = BAZEL_SYSTEM_BAZELRC_PATH;

OptionProcessor::OptionProcessor(
    const WorkspaceLayout* workspace_layout,
    std::unique_ptr<StartupOptions> default_startup_options)
    : workspace_layout_(workspace_layout),
      parsed_startup_options_(std::move(default_startup_options)) {
}

std::unique_ptr<CommandLine> OptionProcessor::SplitCommandLine(
    const vector<string>& args, string* error) const {
  const string lowercase_product_name =
      parsed_startup_options_->GetLowercaseProductName();

  if (args.empty()) {
    blaze_util::StringPrintf(error,
                             "Unable to split command line, args is empty");
    return nullptr;
  }

  const string path_to_binary(args[0]);

  // Process the startup options.
  vector<string> startup_args;
  vector<string>::size_type i = 1;
  while (i < args.size() && IsArg(args[i])) {
    const string current_arg(args[i]);
    // If the current argument is a valid nullary startup option such as
    // --master_bazelrc or --nomaster_bazelrc proceed to examine the next
    // argument.
    if (parsed_startup_options_->IsNullary(current_arg)) {
      startup_args.push_back(current_arg);
      i++;
    } else if (parsed_startup_options_->IsUnary(current_arg)) {
      // If the current argument is a valid unary startup option such as
      // --bazelrc there are two cases to consider.

      // The option is of the form '--bazelrc=value', hence proceed to
      // examine the next argument.
      if (current_arg.find("=") != string::npos) {
        startup_args.push_back(current_arg);
        i++;
      } else {
        // Otherwise, the option is of the form '--bazelrc value', hence
        // skip the next argument and proceed to examine the argument located
        // two places after.
        if (i + 1 >= args.size()) {
          blaze_util::StringPrintf(
              error,
              "Startup option '%s' expects a value.\n"
              "Usage: '%s=somevalue' or '%s somevalue'.\n"
              "  For more info, run '%s help startup_options'.",
              current_arg.c_str(), current_arg.c_str(), current_arg.c_str(),
              lowercase_product_name.c_str());
          return nullptr;
        }
        // In this case we transform it to the form '--bazelrc=value'.
        startup_args.push_back(current_arg + string("=") + args[i + 1]);
        i += 2;
      }
    } else {
      // If the current argument is not a valid unary or nullary startup option
      // then fail.
      blaze_util::StringPrintf(
          error,
          "Unknown startup option: '%s'.\n"
          "  For more info, run '%s help startup_options'.",
          current_arg.c_str(), lowercase_product_name.c_str());
      return nullptr;
    }
  }

  // The command is the arg right after the startup options.
  if (i == args.size()) {
    return std::unique_ptr<CommandLine>(
        new CommandLine(path_to_binary, startup_args, "", {}));
  }
  const string command(args[i]);

  // The rest are the command arguments.
  const vector<string> command_args(args.begin() + i + 1, args.end());

  return std::unique_ptr<CommandLine>(
      new CommandLine(path_to_binary, startup_args, command, command_args));
}

namespace internal {

std::string FindLegacyUserBazelrc(const char* cmd_line_rc_file,
                                  const std::string& workspace) {
  if (cmd_line_rc_file != nullptr) {
    string rcFile = blaze::AbsolutePathFromFlag(cmd_line_rc_file);
    if (!blaze_util::CanReadFile(rcFile)) {
      // The actual rc file reading will catch this - we ignore this here in the
      // legacy version since this is just a warning. Exit eagerly though.
      return "";
    }
    return rcFile;
  }

  string workspaceRcFile = blaze_util::JoinPath(workspace, kRcBasename);
  if (blaze_util::CanReadFile(workspaceRcFile)) {
    return workspaceRcFile;
  }

  string home = blaze::GetHomeDir();
  if (!home.empty()) {
    string userRcFile = blaze_util::JoinPath(home, kRcBasename);
    if (blaze_util::CanReadFile(userRcFile)) {
      return userRcFile;
    }
  }
  return "";
}

std::set<std::string> GetOldRcPaths(
    const WorkspaceLayout* workspace_layout, const std::string& workspace,
    const std::string& cwd, const std::string& path_to_binary,
    const std::vector<std::string>& startup_args) {
  // Find the old list of rc files that would have been loaded here, so we can
  // provide a useful warning about old rc files that might no longer be read.
  std::vector<std::string> candidate_bazelrc_paths;
  if (SearchNullaryOption(startup_args, "master_bazelrc", true)) {
    const std::string workspace_rc =
        workspace_layout->GetWorkspaceRcPath(workspace, startup_args);
    const std::string binary_rc =
        internal::FindRcAlongsideBinary(cwd, path_to_binary);
    const std::string system_rc = internal::FindSystemWideRc();
    candidate_bazelrc_paths = {workspace_rc, binary_rc, system_rc};
  }
  const std::vector<std::string> deduped_blazerc_paths =
      internal::DedupeBlazercPaths(candidate_bazelrc_paths);
  std::set<std::string> old_rc_paths(deduped_blazerc_paths.begin(),
                                     deduped_blazerc_paths.end());
  string user_bazelrc_path = internal::FindLegacyUserBazelrc(
      SearchUnaryOption(startup_args, "--bazelrc"), workspace);
  if (!user_bazelrc_path.empty()) {
    old_rc_paths.insert(user_bazelrc_path);
  }
  return old_rc_paths;
}

std::vector<std::string> DedupeBlazercPaths(
    const std::vector<std::string>& paths) {
  std::set<std::string> canonical_paths;
  std::vector<std::string> result;
  for (const std::string& path : paths) {
    const std::string canonical_path = blaze_util::MakeCanonical(path.c_str());
    if (canonical_path.empty()) {
      // MakeCanonical returns an empty string when it fails. We ignore this
      // failure since blazerc paths may point to invalid locations.
    } else if (canonical_paths.find(canonical_path) == canonical_paths.end()) {
      result.push_back(path);
      canonical_paths.insert(canonical_path);
    }
  }
  return result;
}

std::string FindSystemWideRc() {
  const std::string path =
      blaze_util::MakeAbsoluteAndResolveWindowsEnvvars(system_bazelrc_path);
  if (blaze_util::CanReadFile(path)) {
    return path;
  }
  return "";
}

std::string FindRcAlongsideBinary(const std::string& cwd,
                                  const std::string& path_to_binary) {
  const std::string path = blaze_util::IsAbsolute(path_to_binary)
                               ? path_to_binary
                               : blaze_util::JoinPath(cwd, path_to_binary);
  const std::string base = blaze_util::Basename(path_to_binary);
  const std::string binary_blazerc_path = path + "." + base + "rc";
  if (blaze_util::CanReadFile(binary_blazerc_path)) {
    return binary_blazerc_path;
  }
  return "";
}

blaze_exit_code::ExitCode ParseErrorToExitCode(RcFile::ParseError parse_error) {
  switch (parse_error) {
    case RcFile::ParseError::NONE:
      return blaze_exit_code::SUCCESS;
    case RcFile::ParseError::UNREADABLE_FILE:
      // We check readability before parsing, so this is unexpected.
      return blaze_exit_code::INTERNAL_ERROR;
    case RcFile::ParseError::INVALID_FORMAT:
    case RcFile::ParseError::IMPORT_LOOP:
      return blaze_exit_code::BAD_ARGV;
    default:
      return blaze_exit_code::INTERNAL_ERROR;
  }
}

}  // namespace internal

// TODO(#4502) Consider simplifying result_rc_files to a vector of RcFiles, no
// unique_ptrs.
blaze_exit_code::ExitCode OptionProcessor::GetRcFiles(
    const WorkspaceLayout* workspace_layout, const std::string& workspace,
    const std::string& cwd, const CommandLine* cmd_line,
    std::vector<std::unique_ptr<RcFile>>* result_rc_files,
    std::string* error) const {
  assert(cmd_line != nullptr);
  assert(result_rc_files != nullptr);
  assert(!workspace.empty());

  std::vector<std::string> rc_files;

  // Get the system rc (unless --nosystem_rc).
  if (SearchNullaryOption(cmd_line->startup_args, "system_rc", true)) {
    // MakeAbsoluteAndResolveWindowsEnvvars will standardize the form of the
    // provided path. This also means we accept relative paths, which is
    // is convenient for testing.
    const std::string system_rc =
        blaze_util::MakeAbsoluteAndResolveWindowsEnvvars(system_bazelrc_path);
    rc_files.push_back(system_rc);
  }

  // Get the workspace rc: %workspace%/.bazelrc (unless --noworkspace_rc)
  if (SearchNullaryOption(cmd_line->startup_args, "workspace_rc", true)) {
    const std::string workspaceRcFile =
        blaze_util::JoinPath(workspace, kRcBasename);
    rc_files.push_back(workspaceRcFile);
  }

  // Get the user rc: $HOME/.bazelrc (unless --nohome_rc)
  if (SearchNullaryOption(cmd_line->startup_args, "home_rc", true)) {
    const std::string home = blaze::GetHomeDir();
    if (home.empty()) {
      BAZEL_LOG(WARNING) << "The home directory is not defined, no home_rc "
                            "will be looked for.";
    } else {
      rc_files.push_back(blaze_util::JoinPath(home, kRcBasename));
    }
  }

  // Get the command-line provided rc, passed as --bazelrc or nothing if the
  // flag is absent.
  const char* cmd_line_rc_file =
      SearchUnaryOption(cmd_line->startup_args, "--bazelrc");
  if (cmd_line_rc_file != nullptr) {
    string absolute_cmd_line_rc = blaze::AbsolutePathFromFlag(cmd_line_rc_file);
    // Unlike the previous 3 paths, where we ignore it if the file does not
    // exist or is unreadable, since this path is explicitly passed, this is an
    // error. Check this condition here.
    if (!blaze_util::CanReadFile(absolute_cmd_line_rc)) {
      BAZEL_LOG(ERROR) << "Error: Unable to read .bazelrc file '"
                       << absolute_cmd_line_rc << "'.";
      return blaze_exit_code::BAD_ARGV;
    }
    rc_files.push_back(absolute_cmd_line_rc);
  }

  // Log which files we're looking for before removing duplicates and
  // non-existent files, so that this can serve to debug why a certain file is
  // not being read. The final files which are read will be logged as they are
  // parsed, and can be found using --announce_rc.
  std::string joined_rcs;
  blaze_util::JoinStrings(rc_files, ',', &joined_rcs);
  BAZEL_LOG(INFO) << "Looking for the following rc files: " << joined_rcs;

  // It's possible that workspace == home, that files are symlinks for each
  // other, or that the --bazelrc flag is a duplicate. Dedupe them to minimize
  // the likelihood of repeated options. Since bazelrcs can include one another,
  // this isn't sufficient to prevent duplicate options, but it's good enough.
  // This also has the effect of removing paths that don't point to real files.
  rc_files = internal::DedupeBlazercPaths(rc_files);

  // Parse these potential files, in priority order;
  for (const auto& bazelrc_path : rc_files) {
    std::unique_ptr<RcFile> parsed_rc;
    blaze_exit_code::ExitCode parse_rcfile_exit_code = ParseRcFile(
        workspace_layout, workspace, bazelrc_path, &parsed_rc, error);
    if (parse_rcfile_exit_code != blaze_exit_code::SUCCESS) {
      return parse_rcfile_exit_code;
    }
    result_rc_files->push_back(std::move(parsed_rc));
  }

  // Provide a warning for any old file that might have been missed with the new
  // expectations. This compares "canonical" paths to one another, so should not
  // require additional transformation.
  // TODO(b/36168162): Remove this warning along with
  // internal::GetOldRcPaths and internal::FindLegacyUserBazelrc after
  // the transition period has passed.
  std::set<std::string> read_files;
  for (auto& result_rc : *result_rc_files) {
    const std::deque<std::string>& sources = result_rc->sources();
    read_files.insert(sources.begin(), sources.end());
  }

  const std::set<std::string> old_files =
      internal::GetOldRcPaths(workspace_layout, workspace, cwd,
                              cmd_line->path_to_binary, cmd_line->startup_args);

  //   std::vector<std::string> old_files = internal::GetOldRcPathsInOrder(
  //       workspace_layout, workspace, cwd, cmd_line->path_to_binary,
  //       cmd_line->startup_args);
  //
  //   std::sort(old_files.begin(), old_files.end());
  std::vector<std::string> lost_files(old_files.size());
  std::vector<std::string>::iterator end_iter = std::set_difference(
      old_files.begin(), old_files.end(), read_files.begin(), read_files.end(),
      lost_files.begin());
  lost_files.resize(end_iter - lost_files.begin());
  if (!lost_files.empty()) {
    std::string joined_lost_rcs;
    blaze_util::JoinStrings(lost_files, '\n', &joined_lost_rcs);
    BAZEL_LOG(WARNING)
        << "The following rc files are no longer being read, please transfer "
           "their contents or import their path into one of the standard rc "
           "files:\n"
        << joined_lost_rcs;
  }

  return blaze_exit_code::SUCCESS;
}

blaze_exit_code::ExitCode ParseRcFile(const WorkspaceLayout* workspace_layout,
                                      const std::string& workspace,
                                      const std::string& rc_file_path,
                                      std::unique_ptr<RcFile>* result_rc_file,
                                      std::string* error) {
  assert(!rc_file_path.empty());
  assert(result_rc_file != nullptr);

  RcFile::ParseError parse_error;
  std::unique_ptr<RcFile> parsed_file = RcFile::Parse(
      rc_file_path, workspace_layout, workspace, &parse_error, error);
  if (parsed_file == nullptr) {
    return internal::ParseErrorToExitCode(parse_error);
  }
  *result_rc_file = std::move(parsed_file);
  return blaze_exit_code::SUCCESS;
}

blaze_exit_code::ExitCode OptionProcessor::ParseOptions(
    const vector<string>& args, const string& workspace, const string& cwd,
    string* error) {
  cmd_line_ = SplitCommandLine(args, error);
  if (cmd_line_ == nullptr) {
    return blaze_exit_code::BAD_ARGV;
  }

  // Read the rc files, unless --ignore_all_rc_files was provided on the command
  // line. This depends on the startup options in argv since these may contain
  // other rc-modifying options. For all other options, the precedence of
  // options will be rc first, then command line options, though, despite this
  // exception.
  std::vector<std::unique_ptr<RcFile>> rc_files;
  if (!SearchNullaryOption(cmd_line_->startup_args, "ignore_all_rc_files",
                           false)) {
    const blaze_exit_code::ExitCode rc_parsing_exit_code = GetRcFiles(
        workspace_layout_, workspace, cwd, cmd_line_.get(), &rc_files, error);
    if (rc_parsing_exit_code != blaze_exit_code::SUCCESS) {
      return rc_parsing_exit_code;
    }
  }

  // Parse the startup options in the correct priority order.
  const blaze_exit_code::ExitCode parse_startup_options_exit_code =
      ParseStartupOptions(rc_files, error);
  if (parse_startup_options_exit_code != blaze_exit_code::SUCCESS) {
    return parse_startup_options_exit_code;
  }

  blazerc_and_env_command_args_ =
      GetBlazercAndEnvCommandArgs(cwd, rc_files, GetProcessedEnv());
  return blaze_exit_code::SUCCESS;
}

static void PrintStartupOptions(const std::string& source,
                                const std::vector<std::string>& options) {
  if (!source.empty()) {
    std::string startup_args;
    blaze_util::JoinStrings(options, ' ', &startup_args);
    fprintf(stderr, "INFO: Reading 'startup' options from %s: %s\n",
            source.c_str(), startup_args.c_str());
  }
}

void OptionProcessor::PrintStartupOptionsProvenanceMessage() const {
  StartupOptions* parsed_startup_options = GetParsedStartupOptions();

  // Print the startup flags in the order they are parsed, to keep the
  // precendence clear. In order to minimize the number of lines of output in
  // the terminal, group sequential flags by origin. Note that an rc file may
  // turn up multiple times in this list, if, for example, it imports another
  // rc file and contains startup options on either side of the import
  // statement. This is done intentionally to make option priority clear.
  std::string command_line_source;
  std::string& most_recent_blazerc = command_line_source;
  std::vector<std::string> accumulated_options;
  for (const auto& flag : parsed_startup_options->original_startup_options_) {
    if (flag.source == most_recent_blazerc) {
      accumulated_options.push_back(flag.value);
    } else {
      PrintStartupOptions(most_recent_blazerc, accumulated_options);
      // Start accumulating again.
      accumulated_options.clear();
      accumulated_options.push_back(flag.value);
      most_recent_blazerc = flag.source;
    }
  }
  // Don't forget to print out the last ones.
  PrintStartupOptions(most_recent_blazerc, accumulated_options);
}

blaze_exit_code::ExitCode OptionProcessor::ParseStartupOptions(
    const std::vector<std::unique_ptr<RcFile>> &rc_files,
    std::string *error) {
  // Rc files can import other files at any point, and these imported rcs are
  // expanded in place. Here, we isolate just the startup options but keep the
  // file they came from attached for the option_sources tracking and for
  // sending to the server.
  std::vector<RcStartupFlag> rcstartup_flags;

  for (const auto& blazerc : rc_files) {
    const auto iter = blazerc->options().find("startup");
    if (iter == blazerc->options().end()) continue;

    for (const RcOption& option : iter->second) {
      rcstartup_flags.push_back({*option.source_path, option.option});
    }
  }

  for (const std::string& arg : cmd_line_->startup_args) {
    if (!IsArg(arg)) {
      break;
    }
    rcstartup_flags.push_back(RcStartupFlag("", arg));
  }

  return parsed_startup_options_->ProcessArgs(rcstartup_flags, error);
}

static bool IsValidEnvName(const char* p) {
#if defined(_WIN32) || defined(__CYGWIN__)
  for (; *p && *p != '='; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_')) {
      return false;
    }
  }
#endif
  return true;
}

#if defined(_WIN32)
static void PreprocessEnvString(string* env_str) {
  static constexpr const char* vars_to_uppercase[] = {"PATH", "SYSTEMROOT",
                                                      "TEMP", "TEMPDIR", "TMP"};

  int pos = env_str->find_first_of('=');
  if (pos == string::npos) return;

  string name = env_str->substr(0, pos);
  // We do not care about locale. All variable names are ASCII.
  std::transform(name.begin(), name.end(), name.begin(), ::toupper);
  if (std::find(std::begin(vars_to_uppercase), std::end(vars_to_uppercase),
                name) != std::end(vars_to_uppercase)) {
    env_str->assign(name + "=" + env_str->substr(pos + 1));
  }
}

#elif defined(__CYGWIN__)  // not defined(_WIN32)

static void PreprocessEnvString(string* env_str) {
  int pos = env_str->find_first_of('=');
  if (pos == string::npos) return;
  string name = env_str->substr(0, pos);
  if (name == "PATH") {
    env_str->assign("PATH=" + env_str->substr(pos + 1));
  } else if (name == "TMP") {
    // A valid Windows path "c:/foo" is also a valid Unix path list of
    // ["c", "/foo"] so must use ConvertPath here. See GitHub issue #1684.
    env_str->assign("TMP=" + blaze_util::ConvertPath(env_str->substr(pos + 1)));
  }
}

#else  // Non-Windows platforms.

static void PreprocessEnvString(const string* env_str) {
  // do nothing.
}
#endif  // defined(_WIN32)

static std::vector<std::string> GetProcessedEnv() {
  std::vector<std::string> processed_env;
  for (char** env = environ; *env != NULL; env++) {
    string env_str(*env);
    if (IsValidEnvName(*env)) {
      PreprocessEnvString(&env_str);
      processed_env.push_back(std::move(env_str));
    }
  }
  return processed_env;
}

// IMPORTANT: The options added here do not come from the user. In order for
// their source to be correctly tracked, the options must either be passed
// as --default_override=0, 0 being "client", or must be listed in
// BlazeOptionHandler.INTERNAL_COMMAND_OPTIONS!
std::vector<std::string> OptionProcessor::GetBlazercAndEnvCommandArgs(
    const std::string& cwd,
    const std::vector<std::unique_ptr<RcFile>>& blazercs,
    const std::vector<std::string>& env) {
  // Provide terminal options as coming from the least important rc file.
  std::vector<std::string> result = {
      "--rc_source=client",
      "--default_override=0:common=--isatty=" + ToString(IsStandardTerminal()),
      "--default_override=0:common=--terminal_columns=" +
          ToString(GetTerminalColumns())};
  if (IsEmacsTerminal()) {
    result.push_back("--default_override=0:common=--emacs");
  }

  EnsurePythonPathOption(&result);

  // Map .blazerc numbers to filenames. The indexes here start at 1 because #0
  // is reserved the "client" options created by this function.
  int cur_index = 1;
  std::map<std::string, int> rcfile_indexes;
  for (const auto& blazerc : blazercs) {
    for (const std::string& source_path : blazerc->sources()) {
      // Deduplicate the rc_source list because the same file might be included
      // from multiple places.
      if (rcfile_indexes.find(source_path) != rcfile_indexes.end()) continue;

      result.push_back("--rc_source=" + blaze_util::ConvertPath(source_path));
      rcfile_indexes[source_path] = cur_index;
      cur_index++;
    }
  }

  // Add RcOptions as default_overrides.
  for (const auto& blazerc : blazercs) {
    for (const auto& command_options : blazerc->options()) {
      const string& command = command_options.first;
      // Skip startup flags, which are already parsed by the client.
      if (command == "startup") continue;

      for (const RcOption& rcoption : command_options.second) {
        std::ostringstream oss;
        oss << "--default_override=" << rcfile_indexes[*rcoption.source_path]
            << ':' << command << '=' << rcoption.option;
        result.push_back(oss.str());
      }
    }
  }

  // Pass the client environment to the server.
  for (const string& env_var : env) {
    result.push_back("--client_env=" + env_var);
  }
  result.push_back("--client_cwd=" + blaze_util::ConvertPath(cwd));
  return result;
}

std::vector<std::string> OptionProcessor::GetCommandArguments() const {
  assert(cmd_line_ != nullptr);
  // When the user didn't specify a command, the server expects the command
  // arguments to be empty in order to display the help message.
  if (cmd_line_->command.empty()) {
    return {};
  }

  std::vector<std::string> command_args = blazerc_and_env_command_args_;
  command_args.insert(command_args.end(),
                      cmd_line_->command_args.begin(),
                      cmd_line_->command_args.end());
  return command_args;
}

std::vector<std::string> OptionProcessor::GetExplicitCommandArguments() const {
  assert(cmd_line_ != nullptr);
  return cmd_line_->command_args;
}

std::string OptionProcessor::GetCommand() const {
  assert(cmd_line_ != nullptr);
  return cmd_line_->command;
}

StartupOptions* OptionProcessor::GetParsedStartupOptions() const {
  assert(parsed_startup_options_ != NULL);
  return parsed_startup_options_.get();
}

}  // namespace blaze
