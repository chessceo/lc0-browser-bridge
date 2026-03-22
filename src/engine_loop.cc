/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018-2025 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "engine_loop.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unistd.h>

#include "engine.h"
#include "neural/shared_params.h"
#include "utils/configfile.h"

namespace lczero {
namespace {
const OptionId kLogFileId{
    {.long_flag = "logfile",
     .uci_option = "LogFile",
     .help_text = "Write log to that file. Special value <stderr> to "
                  "output the log to the console.",
     .short_flag = 'l',
     .visibility = OptionId::kAlwaysVisible}};

const OptionId kStateDirId{
    {.long_flag = "state-dir",
     .help_text = "Directory used for file-backed UCI transport.",
     .visibility = OptionId::kAlwaysVisible}};

const OptionId kPollIntervalMsId{
    {.long_flag = "poll-interval-ms",
     .help_text = "Polling interval in milliseconds for file-backed UCI transport.",
     .visibility = OptionId::kAlwaysVisible}};

std::string JsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::optional<std::string> ExtractJsonString(const std::string& line,
                                             const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto start = line.find(needle);
  if (start == std::string::npos) return std::nullopt;

  std::string value;
  bool escaped = false;
  for (size_t i = start + needle.size(); i < line.size(); ++i) {
    const char ch = line[i];
    if (escaped) {
      switch (ch) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        default:
          value += ch;
          break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return value;
    value += ch;
  }
  return std::nullopt;
}

std::optional<long long> ExtractJsonInteger(const std::string& line,
                                            const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto start = line.find(needle);
  if (start == std::string::npos) return std::nullopt;
  size_t begin = start + needle.size();
  size_t end = begin;
  while (end < line.size() && (std::isdigit(line[end]) || line[end] == '-')) {
    ++end;
  }
  if (begin == end) return std::nullopt;
  return std::stoll(line.substr(begin, end - begin));
}

std::string CurrentIsoTime() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm = *std::gmtime(&now_time);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
  return std::string(buffer);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

struct FileCommand {
  long long seq = 0;
  std::string type;
  std::string line;
};

class FileUciTransport {
 public:
  FileUciTransport(const std::filesystem::path& state_dir, int poll_interval_ms,
                   const std::string& engine_name,
                   const std::string& weights_path)
      : state_dir_(state_dir),
        poll_interval_ms_(poll_interval_ms),
        engine_name_(engine_name),
        weights_path_(weights_path) {}

  void Initialize() {
    std::filesystem::create_directories(state_dir_);
    std::ofstream(state_dir_ / "browser-to-engine.ndjson", std::ios::app).close();
    std::ofstream(state_dir_ / "engine-to-browser.ndjson", std::ios::app).close();
    WriteStatus("starting", "");
  }

  int poll_interval_ms() const { return poll_interval_ms_; }

  std::vector<FileCommand> ReadCommands() {
    const auto input_path = state_dir_ / "browser-to-engine.ndjson";
    std::ifstream input(input_path);
    if (!input) return {};

    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string content = stream.str();
    if (input_offset_ > content.size()) input_offset_ = 0;

    const std::string unread = content.substr(input_offset_);
    const bool has_trailing_newline = !unread.empty() && unread.back() == '\n';
    std::istringstream lines(unread);
    std::string line;
    std::vector<FileCommand> commands;
    size_t consumed_bytes = 0;

    while (std::getline(lines, line)) {
      consumed_bytes += line.size() + 1;
      if (line.empty()) continue;
      if (!has_trailing_newline && lines.peek() == EOF) {
        consumed_bytes -= line.size() + 1;
        break;
      }

      auto seq = ExtractJsonInteger(line, "seq");
      auto type = ExtractJsonString(line, "type");
      if (!seq || !type) continue;

      FileCommand command;
      command.seq = *seq;
      command.type = *type;
      if (command.type == "uci_line") {
        auto payload_line = ExtractJsonString(line, "line");
        if (!payload_line) continue;
        command.line = *payload_line;
      }
      commands.push_back(std::move(command));
    }

    input_offset_ += consumed_bytes;
    return commands;
  }

  void AppendUciResponses(const std::vector<std::string>& responses) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::ofstream output(state_dir_ / "engine-to-browser.ndjson", std::ios::app);
    for (const auto& response : responses) {
      output << "{\"seq\":" << output_seq_++
             << ",\"ts\":\"" << CurrentIsoTime()
             << "\",\"type\":\"uci_line\",\"payload\":{\"line\":\""
             << JsonEscape(response) << "\"}}\n";
    }
  }

  void SetLastProcessedSeq(long long seq) { last_processed_seq_ = seq; }

  void WriteStatus(const std::string& state, const std::string& last_error) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::ofstream status(state_dir_ / "engine-status.json", std::ios::trunc);
    status << "{\n"
           << "  \"state\": \"" << JsonEscape(state) << "\",\n"
           << "  \"pid\": " << static_cast<long long>(getpid()) << ",\n"
           << "  \"engineName\": \"" << JsonEscape(engine_name_) << "\",\n"
           << "  \"weightsPath\": \"" << JsonEscape(weights_path_) << "\",\n"
           << "  \"lastProcessedSeq\": " << last_processed_seq_ << ",\n"
           << "  \"lastError\": "
           << (last_error.empty() ? "null" : "\"" + JsonEscape(last_error) + "\"") << ",\n"
           << "  \"startedAt\": \"" << started_at_ << "\"\n"
           << "}\n";
  }

 private:
  std::filesystem::path state_dir_;
  int poll_interval_ms_ = 50;
  std::string engine_name_;
  std::string weights_path_;
  std::string started_at_ = CurrentIsoTime();
  size_t input_offset_ = 0;
  long long output_seq_ = 1;
  long long last_processed_seq_ = 0;
  std::mutex output_mutex_;
};

class FileUciResponder : public StringUciResponder {
 public:
  explicit FileUciResponder(FileUciTransport* transport)
      : transport_(transport) {}

  void SendRawResponses(const std::vector<std::string>& responses) override {
    for (const auto& response : responses) {
      LOGFILE << "<< " << response;
    }
    transport_->AppendUciResponses(responses);
  }

 private:
  FileUciTransport* transport_;
};
}  // namespace

void RunEngine(SearchFactory* factory) {
  CERR << "Search algorithm: " << factory->GetName();
  StdoutUciResponder uci_responder;

  // Populate options from various sources.
  OptionsParser options_parser;
  options_parser.Add<StringOption>(kLogFileId);
  ConfigFile::PopulateOptions(&options_parser);
  Engine::PopulateOptions(&options_parser);
  if (factory) factory->PopulateParams(&options_parser);  // Search params.
  uci_responder.PopulateParams(&options_parser);          // UCI params.
  SharedBackendParams::Populate(&options_parser);

  // Parse flags, show help, initialize logging, read config etc.
  if (!ConfigFile::Init() || !options_parser.ProcessAllFlags()) return;
  const auto options = options_parser.GetOptionsDict();
  Logging::Get().SetFilename(options.Get<std::string>(kLogFileId));

  // Create engine.
  Engine engine(*factory, options);
  UciLoop loop(&uci_responder, &options_parser, &engine);

  // Run the stdin loop.
  std::cout.setf(std::ios::unitbuf);
  std::string line;
  while (std::getline(std::cin, line)) {
    LOGFILE << ">> " << line;
    try {
      if (!loop.ProcessLine(line)) break;
      // Set the log filename for the case it was set in UCI option.
      Logging::Get().SetFilename(options.Get<std::string>(kLogFileId));
    } catch (Exception& ex) {
      uci_responder.SendRawResponse(std::string("error ") + ex.what());
    }
  }
}

void RunFileEngine(SearchFactory* factory) {
  CERR << "Search algorithm: " << factory->GetName() << " (fileuci)";

  OptionsParser options_parser;
  options_parser.Add<StringOption>(kLogFileId);
  options_parser.Add<StringOption>(kStateDirId) = "/home/lucas/chess/lc0";
  options_parser.Add<IntOption>(kPollIntervalMsId, 10, 5000) = 50;
  ConfigFile::PopulateOptions(&options_parser);
  Engine::PopulateOptions(&options_parser);
  if (factory) factory->PopulateParams(&options_parser);
  SharedBackendParams::Populate(&options_parser);

  if (!ConfigFile::Init() || !options_parser.ProcessAllFlags()) return;
  const auto options = options_parser.GetOptionsDict();
  Logging::Get().SetFilename(options.Get<std::string>(kLogFileId));

  FileUciTransport transport(options.Get<std::string>(kStateDirId),
                             options.Get<int>(kPollIntervalMsId), "Lc0",
                             options.Get<std::string>(SharedBackendParams::kWeightsId));
  FileUciResponder uci_responder(&transport);
  uci_responder.PopulateParams(&options_parser);
  transport.Initialize();

  Engine engine(*factory, options);
  UciLoop loop(&uci_responder, &options_parser, &engine);
  transport.WriteStatus("ready", "");

  for (;;) {
    const auto commands = transport.ReadCommands();
    if (commands.empty()) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(transport.poll_interval_ms()));
      continue;
    }

    for (const auto& command : commands) {
      transport.SetLastProcessedSeq(command.seq);
      try {
        if (command.type == "sync") {
          transport.WriteStatus("ready", "");
          continue;
        }
        if (command.type != "uci_line") {
          continue;
        }

        LOGFILE << ">> " << command.line;
        const bool keep_running = loop.ProcessLine(command.line);
        if (StartsWith(command.line, "go")) {
          transport.WriteStatus("busy", "");
        } else if (command.line == "quit") {
          transport.WriteStatus("stopped", "");
        } else if (!StartsWith(command.line, "stop")) {
          transport.WriteStatus("ready", "");
        }

        Logging::Get().SetFilename(options.Get<std::string>(kLogFileId));
        if (!keep_running) {
          return;
        }
      } catch (Exception& ex) {
        uci_responder.SendRawResponse(std::string("error ") + ex.what());
        transport.WriteStatus("error", ex.what());
      }
    }
  }
}

}  // namespace lczero
