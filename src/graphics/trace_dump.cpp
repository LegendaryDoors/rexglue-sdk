/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <future>
#include <vector>

#include <fmt/format.h>

#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_dump.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/png_write.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_STRING(target_trace_file, "", "GPU", "Specifies the trace file to load.");
REXCVAR_DEFINE_STRING(trace_dump_path, "", "GPU", "Output path for dumped files.");
REXCVAR_DEFINE_INT32(trace_dump_frame, 0, "GPU", "Frame index to replay before dumping.");
REXCVAR_DEFINE_INT32(trace_dump_command, -1, "GPU",
                     "Command index within the frame to stop at; -1 means the end of the frame. "
                     "Use this to bisect a frame draw by draw.");
REXCVAR_DEFINE_STRING(
    trace_dump_commands, "", "GPU",
    "Several stop points within the last listed frame of trace_dump_frames, as a list like "
    "trace_dump_frames (e.g. 0-4000/250), with a PNG written at each. Playback continues "
    "forward between stops, so one replay brackets a defect to a slice of the frame instead "
    "of one replay per probe. Overrides trace_dump_command.");
REXCVAR_DEFINE_STRING(
    trace_dump_packets, "", "GPU",
    "Disassemble the packets of these commands of the last listed trace_dump_frames frame to "
    "the log, as a list like trace_dump_frames, and exit without replaying. Register writes are "
    "named; shader constant ranges are summarised. Shows what the title actually sent, "
    "independent of how the replayer applied it.");
REXCVAR_DEFINE_STRING(
    trace_shader_storage, "", "GPU",
    "Load the persistent shader and pipeline storage from this cache root (the game's "
    "--cache_root, e.g. ~/.local/share/<app>/cache) before replaying, exactly as the live app "
    "does. A replay without it translates every shader fresh, so a defect that only exists "
    "live may be in what the storage brings back.");
REXCVAR_DEFINE_STRING(
    trace_cut, "", "GPU",
    "Write the first N frames of the trace to a new file, as <N>,<path>, and exit. A 200 GB "
    "from-boot stream takes twenty minutes to parse per replay; a cut of the frames under "
    "investigation takes seconds.");
REXCVAR_DEFINE_STRING(
    trace_find_memory, "", "GPU",
    "Scan the whole trace for recorded memory reads and writes covering this guest address "
    "(hex) and log each one with its frame and command, then exit. Finds where a buffer the "
    "replayer used was last recorded; the tracer records a fetch range once and never again.");
REXCVAR_DEFINE_STRING(
    trace_dump_frames, "", "GPU",
    "Sequential dump for stream traces: plays every frame from 0 so state carries across "
    "frames, and writes a PNG after each listed frame. Comma-separated indices and ranges "
    "with an optional step, e.g. 100,500,1000-2000/100. When set, trace_dump_frame is "
    "ignored; trace_dump_command applies to the last listed frame, which is then replayed "
    "from its start up to that command.");

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

// The PNG encoder lives in rex/png_write.h (rexcore), shared with the runtime
// screenshot capture (--screenshot_at / F7).

// Logs the trace commands and PM4 packets of one recorded command, with
// register writes by name. Shader constants are summarised per packet.
void DumpCommandPackets(TraceReader* reader, const TraceReader::Frame* frame, int frame_index,
                        int command_index) {
  const auto& command = frame->commands[command_index];
  REXGPU_INFO("PACKETS frame {} command {} ({})", frame_index, command_index,
              command.type == TraceReader::Frame::Command::Type::kDraw ? "draw" : "swap");
  const PacketStartCommand* pending_packet = nullptr;
  const uint8_t* trace_ptr = command.start_ptr;
  while (trace_ptr < command.end_ptr) {
    auto type = static_cast<TraceCommandType>(memory::load<uint32_t>(trace_ptr));
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        auto cmd = reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        REXGPU_INFO("  PrimaryBufferStart base={:08X} count={}", cmd->base_ptr, cmd->count);
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd:
        trace_ptr += sizeof(PrimaryBufferEndCommand);
        REXGPU_INFO("  PrimaryBufferEnd");
        break;
      case TraceCommandType::kIndirectBufferStart: {
        auto cmd = reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        REXGPU_INFO("  IndirectBufferStart base={:08X} count={}", cmd->base_ptr, cmd->count);
        break;
      }
      case TraceCommandType::kIndirectBufferEnd:
        trace_ptr += sizeof(IndirectBufferEndCommand);
        REXGPU_INFO("  IndirectBufferEnd");
        break;
      case TraceCommandType::kPacketStart: {
        auto cmd = reinterpret_cast<const PacketStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        pending_packet = cmd;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        trace_ptr += sizeof(PacketEndCommand);
        if (!pending_packet) {
          break;
        }
        PacketInfo packet_info = {0};
        const uint8_t* packet_ptr =
            reinterpret_cast<const uint8_t*>(pending_packet) + sizeof(PacketStartCommand);
        if (!PacketDisassembler::DisasmPacket(packet_ptr, &packet_info)) {
          REXGPU_INFO("  PKT <invalid> at {:08X} count={}", pending_packet->base_ptr,
                      pending_packet->count);
          pending_packet = nullptr;
          break;
        }
        REXGPU_INFO("  PKT {}{} count={}", packet_info.type_info->name,
                    packet_info.predicated ? " (predicated)" : "", pending_packet->count);
        // Packets the disassembler reports no actions for still carry their
        // parameters, so show the raw dwords of the short ones.
        if (packet_info.actions.empty() && pending_packet->count <= 8) {
          std::string words;
          for (uint32_t i = 1; i < pending_packet->count; ++i) {
            words += fmt::format(" {:08X}", memory::load_and_swap<uint32_t>(packet_ptr + i * 4));
          }
          REXGPU_INFO("    raw{}", words);
        }
        uint32_t constant_first = 0, constant_count = 0;
        std::vector<uint32_t> constant_values;
        auto flush_constants = [&]() {
          if (constant_count) {
            REXGPU_INFO("    REG {:04X}..{:04X} ({} shader constants)", constant_first,
                        constant_first + constant_count - 1, constant_count);
            // Float constants, four per register, as the shader sees them.
            // Vertex shader constants start at 0x4000, pixel at 0x4400.
            for (uint32_t i = 0; i + 4 <= constant_count && constant_first < 0x4800; i += 4) {
              uint32_t reg = constant_first + i;
              REXGPU_INFO("      {}c{} = ({:g}, {:g}, {:g}, {:g})", reg < 0x4400 ? "vs " : "ps ",
                          (reg - (reg < 0x4400 ? 0x4000 : 0x4400)) / 4,
                          memory::Reinterpret<float>(constant_values[i]),
                          memory::Reinterpret<float>(constant_values[i + 1]),
                          memory::Reinterpret<float>(constant_values[i + 2]),
                          memory::Reinterpret<float>(constant_values[i + 3]));
            }
            constant_count = 0;
            constant_values.clear();
          }
        };
        for (const auto& action : packet_info.actions) {
          if (action.type == PacketAction::Type::kSetBinMask) {
            flush_constants();
            REXGPU_INFO("    BIN_MASK = {:016X}", action.set_bin_mask.value);
            continue;
          }
          if (action.type == PacketAction::Type::kSetBinSelect) {
            flush_constants();
            REXGPU_INFO("    BIN_SELECT = {:016X}", action.set_bin_select.value);
            continue;
          }
          if (action.type != PacketAction::Type::kRegisterWrite) {
            continue;
          }
          uint32_t index = action.register_write.index;
          if (index >= 0x4000 && (index < 0x4800 || index >= 0x4900)) {
            if (constant_count && constant_first + constant_count == index) {
              ++constant_count;
            } else {
              flush_constants();
              constant_first = index;
              constant_count = 1;
            }
            constant_values.push_back(action.register_write.value);
            continue;
          }
          flush_constants();
          auto register_info = RegisterFile::GetRegisterInfo(index);
          REXGPU_INFO("    REG {:04X} {} = {:08X}", index,
                      register_info ? register_info->name : "???", action.register_write.value);
        }
        flush_constants();
        pending_packet = nullptr;
        break;
      }
      case TraceCommandType::kMemoryRead:
      case TraceCommandType::kMemoryWrite: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        REXGPU_INFO("  Memory{} base={:08X} length={}",
                    type == TraceCommandType::kMemoryRead ? "Read" : "Write", cmd->base_ptr,
                    cmd->decoded_length);
        // Small payloads are shader constants, index data or a fetch block,
        // worth seeing inline; guest memory is big-endian, shown as dwords.
        std::vector<uint8_t> payload;
        if (cmd->decoded_length <= 4096 && reader->DecodeMemory(cmd, payload)) {
          std::string line;
          for (size_t i = 0; i + 4 <= payload.size(); i += 4) {
            line += fmt::format(" {:08X}", memory::load_and_swap<uint32_t>(payload.data() + i));
            if ((i / 4) % 8 == 7 || i + 8 > payload.size()) {
              REXGPU_INFO("    {:04X}:{}", uint32_t(i - (i % 32)), line);
              line.clear();
            }
          }
        }
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        REXGPU_INFO("  EdramSnapshot");
        break;
      }
      case TraceCommandType::kEvent: {
        auto cmd = reinterpret_cast<const EventCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        REXGPU_INFO("  Event {}", cmd->event_type == EventCommand::Type::kSwap ? "swap" : "?");
        break;
      }
      case TraceCommandType::kRegisters: {
        auto cmd = reinterpret_cast<const RegistersCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        REXGPU_INFO("  Registers first={:04X} count={} callbacks={}", cmd->first_register,
                    cmd->register_count, cmd->execute_callbacks);
        break;
      }
      case TraceCommandType::kGammaRamp: {
        auto cmd = reinterpret_cast<const GammaRampCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        REXGPU_INFO("  GammaRamp");
        break;
      }
      default:
        REXGPU_ERROR("  unknown trace command type {} at offset {}", uint32_t(type),
                     trace_ptr - command.start_ptr);
        return;
    }
  }
}

// Parses "100,500,1000-2000/100" into a sorted, deduplicated frame list.
// Returns false (and logs) on malformed input or out-of-range frames.
bool ParseFrameList(const std::string& spec, int frame_count, std::vector<int>& out) {
  out.clear();
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string token = spec.substr(pos, comma == std::string::npos ? std::string::npos
                                                                    : comma - pos);
    pos = comma == std::string::npos ? spec.size() : comma + 1;
    if (token.empty()) {
      continue;
    }
    int first = 0, last = 0, step = 1;
    size_t dash = token.find('-');
    try {
      if (dash == std::string::npos) {
        first = last = std::stoi(token);
      } else {
        first = std::stoi(token.substr(0, dash));
        std::string rest = token.substr(dash + 1);
        size_t slash = rest.find('/');
        if (slash == std::string::npos) {
          last = std::stoi(rest);
        } else {
          last = std::stoi(rest.substr(0, slash));
          step = std::stoi(rest.substr(slash + 1));
        }
      }
    } catch (const std::exception&) {
      REXGPU_ERROR("trace_dump_frames: cannot parse token '{}'", token);
      return false;
    }
    if (step <= 0 || first < 0 || last < first || last >= frame_count) {
      REXGPU_ERROR("trace_dump_frames: token '{}' out of range; trace has {} frames", token,
                   frame_count);
      return false;
    }
    for (int f = first; f <= last; f += step) {
      out.push_back(f);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

}  // namespace

TraceDump::TraceDump() = default;

TraceDump::~TraceDump() = default;

int TraceDump::Main(const std::vector<std::string>& args) {
  // Grab path from the flag or unnamed argument.
  std::filesystem::path path;
  std::filesystem::path output_path;
  if (!REXCVAR_GET(target_trace_file).empty()) {
    path = rex::to_path(REXCVAR_GET(target_trace_file));
  } else if (args.size() >= 2) {
    // Passed as an unnamed argument.
    path = rex::to_path(args[1]);

    if (args.size() >= 3) {
      output_path = rex::to_path(args[2]);
    }
  }

  if (path.empty()) {
    REXGPU_ERROR(
        "No trace file specified.\n"
        "Usage: rex-trace-dump <trace.xtr> [output-prefix]\n"
        "       --trace_dump_frame=<n>     frame to replay (default 0)\n"
        "       --trace_dump_command=<n>   stop at command n; -1 = end of frame\n"
        "       --trace_dump_frames=<list> sequential replay from frame 0, PNG per listed\n"
        "                                  frame, e.g. 100,500,1000-2000/100. Required for\n"
        "                                  stream traces, whose frames are not self-contained\n"
        "       --trace_dump_commands=<list> stop points within the last listed frame, a PNG\n"
        "                                  at each, e.g. 0-4000/250; brackets a defect to a\n"
        "                                  slice of the frame in one replay\n"
        "       --trace_dump_packets=<list>  disassemble these commands of the last listed frame\n"
        "                                    to the log and exit, no replay\n"
        "       --trace_find_memory=<hex>    list every recorded memory read/write covering the\n"
        "                                    address, with frame and command, and exit");
    return 5;
  }

  // Normalize the path and make absolute.
  auto abs_path = std::filesystem::absolute(path);
  REXGPU_INFO("Loading trace file {}...", rex::path_to_utf8(abs_path));

  if (!Setup()) {
    REXGPU_ERROR("Unable to setup trace dump tool");
    return 4;
  }
  if (!Load(std::move(abs_path))) {
    REXGPU_ERROR("Unable to load trace file; not found?");
    return 5;
  }

  // Root file name for outputs.
  if (output_path.empty()) {
    base_output_path_ = rex::to_path(REXCVAR_GET(trace_dump_path));
    auto output_name = path.filename().replace_extension();

    base_output_path_ = base_output_path_ / output_name;
  } else {
    base_output_path_ = output_path;
  }

  // Ensure output path exists.
  rex::filesystem::CreateParentFolder(base_output_path_);

  return Run();
}

bool TraceDump::Setup() {
  emulator_ = std::make_unique<Runtime>("", "", "", "");

  // Keep a typed pointer: RuntimeConfig takes ownership as
  // system::IGraphicsSystem, but the player needs graphics::GraphicsSystem.
  std::unique_ptr<GraphicsSystem> graphics_system = CreateGraphicsSystem();
  if (!graphics_system) {
    REXGPU_ERROR("Failed to create the graphics system");
    return false;
  }
  graphics_system_ = graphics_system.get();

  // Build an offscreen presenter before the runtime wires up the guest GPU:
  // a headless provider cannot be upgraded to a presenting one in place.
  X_STATUS presentation_status = graphics_system->SetupPresentation(nullptr);
  if (XFAILED(presentation_status)) {
    REXGPU_ERROR("Failed to set up offscreen presentation: {:08X}", presentation_status);
    return false;
  }

  RuntimeConfig config;
  config.graphics = std::move(graphics_system);

  X_STATUS result = emulator_->Setup(std::move(config));
  if (XFAILED(result)) {
    REXGPU_ERROR("Failed to setup runtime: {:08X}", result);
    return false;
  }

  player_ = std::make_unique<TracePlayer>(graphics_system_);
  return true;
}

bool TraceDump::Load(const std::filesystem::path& trace_file_path) {
  trace_file_path_ = trace_file_path;

  if (!player_->Open(rex::path_to_utf8(trace_file_path_))) {
    REXGPU_ERROR("Could not load trace file");
    return false;
  }

  return true;
}


void TraceDump::DumpStopState(int stop) {
  if (const char* registers_path = getenv("REX_DUMP_REGISTERS")) {
    const RegisterFile* register_file = graphics_system_->register_file();
    std::string path = fmt::format("{}.c{}", registers_path, stop);
    if (FILE* file = fopen(path.c_str(), "wb")) {
      fwrite(register_file->values, sizeof(uint32_t), RegisterFile::kRegisterCount, file);
      fclose(file);
      REXGPU_INFO("REX_DUMP_REGISTERS: wrote {} registers to {}", RegisterFile::kRegisterCount,
                  path);
    } else {
      REXGPU_ERROR("REX_DUMP_REGISTERS: cannot open {} for writing", path);
    }
  }
  if (const char* memory_spec = getenv("REX_DUMP_GUEST_MEMORY")) {
    std::string spec(memory_spec);
    size_t begin = 0;
    while (begin < spec.size()) {
      size_t end = spec.find(';', begin);
      if (end == std::string::npos) {
        end = spec.size();
      }
      std::string range = spec.substr(begin, end - begin);
      begin = end + 1;
      size_t comma1 = range.find(',');
      size_t comma2 =
          comma1 == std::string::npos ? std::string::npos : range.find(',', comma1 + 1);
      if (comma2 == std::string::npos) {
        REXGPU_ERROR("REX_DUMP_GUEST_MEMORY: expected <hex addr>,<hex length>,<path>: '{}'",
                     range);
        continue;
      }
      uint32_t address = uint32_t(strtoul(range.substr(0, comma1).c_str(), nullptr, 16));
      uint32_t length = uint32_t(
          strtoul(range.substr(comma1 + 1, comma2 - comma1 - 1).c_str(), nullptr, 16));
      std::string path = fmt::format("{}.c{}", range.substr(comma2 + 1), stop);
      const uint8_t* host = graphics_system_->memory()->TranslatePhysical(address & 0x1FFFFFFF);
      if (FILE* file = fopen(path.c_str(), "wb")) {
        fwrite(host, 1, length, file);
        fclose(file);
        REXGPU_INFO("REX_DUMP_GUEST_MEMORY: wrote {:#X} bytes from {:#010X} to {}", length,
                    address, path);
      } else {
        REXGPU_ERROR("REX_DUMP_GUEST_MEMORY: cannot open {} for writing", path);
      }
    }
  }
}

int TraceDump::Run() {
  int frame_count = static_cast<int>(player_->frame_count());
  const std::string& shader_storage_root = REXCVAR_GET(trace_shader_storage);
  if (!shader_storage_root.empty()) {
    uint32_t title_id = player_->header()->title_id;
    REXGPU_INFO("Loading shader storage for title {:08X} from {}", title_id, shader_storage_root);
    graphics_system_->InitializeShaderStorage(std::filesystem::path(shader_storage_root), title_id,
                                              true);
  }
  const std::string& cut_spec = REXCVAR_GET(trace_cut);
  if (!cut_spec.empty()) {
    size_t comma = cut_spec.find(',');
    int cut_frames = comma == std::string::npos ? 0 : atoi(cut_spec.substr(0, comma).c_str());
    if (cut_frames <= 0 || cut_frames >= frame_count || comma == std::string::npos) {
      REXGPU_ERROR("trace_cut: expected <frames>,<path> with 0 < frames < {}", frame_count);
      return 6;
    }
    std::string cut_path = cut_spec.substr(comma + 1);
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(player_->header());
    const uint8_t* end = player_->frame(cut_frames)->start_ptr;
    FILE* file = fopen(cut_path.c_str(), "wb");
    if (!file) {
      REXGPU_ERROR("trace_cut: cannot open {} for writing", cut_path);
      return 6;
    }
    size_t written = fwrite(begin, 1, size_t(end - begin), file);
    fclose(file);
    REXGPU_INFO("trace_cut: wrote {} frames, {} bytes, to {}", cut_frames, written, cut_path);
    return 0;
  }

  // Captures the current guest output into <prefix><suffix>.png.
  auto capture = [&](const std::string& suffix) -> bool {
    ui::Presenter* presenter = graphics_system_->presenter();
    ui::RawImage raw_image;
    if (!presenter || !presenter->CaptureGuestOutput(raw_image)) {
      REXGPU_ERROR("CaptureGuestOutput failed (presenter={})", presenter ? "present" : "null");
      return false;
    }
    auto png_path = base_output_path_;
    png_path += suffix + ".png";
    if (!rex::WritePng(png_path, raw_image.data.data(), static_cast<uint32_t>(raw_image.width),
                       static_cast<uint32_t>(raw_image.height), raw_image.stride)) {
      return false;
    }
    REXGPU_INFO("Wrote {}x{} frame to {}", raw_image.width, raw_image.height,
                rex::path_to_utf8(png_path));
    return true;
  };

  int result = 0;
  const std::string& frames_spec = REXCVAR_GET(trace_dump_frames);
  const std::string& find_memory_spec = REXCVAR_GET(trace_find_memory);
  if (!find_memory_spec.empty()) {
    uint32_t address = uint32_t(strtoull(find_memory_spec.c_str(), nullptr, 16));
    size_t hits = 0;
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
      const TraceReader::Frame* frame = player_->frame(frame_index);
      for (size_t command_index = 0; command_index < frame->commands.size(); ++command_index) {
        const auto& command = frame->commands[command_index];
        const uint8_t* trace_ptr = command.start_ptr;
        while (trace_ptr < command.end_ptr) {
          auto type = static_cast<TraceCommandType>(memory::load<uint32_t>(trace_ptr));
          switch (type) {
            case TraceCommandType::kPrimaryBufferStart:
              trace_ptr += sizeof(PrimaryBufferStartCommand) +
                           reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr)->count * 4;
              break;
            case TraceCommandType::kPrimaryBufferEnd:
              trace_ptr += sizeof(PrimaryBufferEndCommand);
              break;
            case TraceCommandType::kIndirectBufferStart:
              trace_ptr += sizeof(IndirectBufferStartCommand) +
                           reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr)->count * 4;
              break;
            case TraceCommandType::kIndirectBufferEnd:
              trace_ptr += sizeof(IndirectBufferEndCommand);
              break;
            case TraceCommandType::kPacketStart:
              trace_ptr += sizeof(PacketStartCommand) +
                           reinterpret_cast<const PacketStartCommand*>(trace_ptr)->count * 4;
              break;
            case TraceCommandType::kPacketEnd:
              trace_ptr += sizeof(PacketEndCommand);
              break;
            case TraceCommandType::kMemoryRead:
            case TraceCommandType::kMemoryWrite: {
              auto cmd = reinterpret_cast<const MemoryCommand*>(trace_ptr);
              trace_ptr += sizeof(*cmd) + cmd->encoded_length;
              if (address >= cmd->base_ptr && address < cmd->base_ptr + cmd->decoded_length) {
                ++hits;
                REXGPU_INFO("MEMREC frame {} command {} Memory{} base={:08X} length={}", frame_index,
                            command_index, type == TraceCommandType::kMemoryRead ? "Read" : "Write",
                            cmd->base_ptr, cmd->decoded_length);
              }
              break;
            }
            case TraceCommandType::kEdramSnapshot:
              trace_ptr += sizeof(EdramSnapshotCommand) +
                           reinterpret_cast<const EdramSnapshotCommand*>(trace_ptr)->encoded_length;
              break;
            case TraceCommandType::kEvent:
              trace_ptr += sizeof(EventCommand);
              break;
            case TraceCommandType::kRegisters:
              trace_ptr += sizeof(RegistersCommand) +
                           reinterpret_cast<const RegistersCommand*>(trace_ptr)->encoded_length;
              break;
            case TraceCommandType::kGammaRamp:
              trace_ptr += sizeof(GammaRampCommand) +
                           reinterpret_cast<const GammaRampCommand*>(trace_ptr)->encoded_length;
              break;
            default:
              REXGPU_ERROR("unknown trace command type {} in frame {} command {}", uint32_t(type),
                           frame_index, command_index);
              trace_ptr = command.end_ptr;
              break;
          }
        }
      }
    }
    REXGPU_INFO("MEMREC {} record(s) cover {:08X} in {} frames", hits, address, frame_count);
    return 0;
  }
  const std::string& packets_spec = REXCVAR_GET(trace_dump_packets);
  if (!packets_spec.empty()) {
    // Reading the recording needs no replay, so this returns before any frame
    // is played.
    int frame_index = REXCVAR_GET(trace_dump_frame);
    if (!frames_spec.empty()) {
      std::vector<int> targets;
      if (!ParseFrameList(frames_spec, frame_count, targets) || targets.empty()) {
        return 6;
      }
      frame_index = targets.back();
    }
    if (frame_index < 0 || frame_index >= frame_count) {
      REXGPU_ERROR("Frame {} out of range (trace has {} frames)", frame_index, frame_count);
      return 6;
    }
    const TraceReader::Frame* frame = player_->frame(frame_index);
    std::vector<int> commands;
    if (!ParseFrameList(packets_spec, int(frame->commands.size()), commands) || commands.empty()) {
      return 6;
    }
    for (int command_index : commands) {
      DumpCommandPackets(player_.get(), frame, frame_index, command_index);
    }
    return 0;
  }
  if (!frames_spec.empty()) {
    // Sequential mode: play every frame from 0 so that memory and register
    // state written in earlier frames is present, which stream traces require.
    std::vector<int> targets;
    if (!ParseFrameList(frames_spec, frame_count, targets) || targets.empty()) {
      return 6;
    }
    int last_target = targets.back();
    int bisect_command = REXCVAR_GET(trace_dump_command);
    const std::string& commands_spec = REXCVAR_GET(trace_dump_commands);
    REXGPU_INFO("Sequential replay of frames 0..{} of {}, dumping {} frame(s)", last_target,
                frame_count, targets.size());
    BeginHostCapture();
    size_t next_target = 0;
    for (int frame_index = 0; frame_index <= last_target; ++frame_index) {
      bool is_bisected_last =
          frame_index == last_target && (bisect_command >= 0 || !commands_spec.empty());
      if ((is_bisected_last || frame_index == 0) &&
          player_->frame(frame_index)->commands.empty()) {
        // SeekCommand(-1) on an empty frame returns without playing or
        // signalling, and WaitOnPlayback would block forever.
        REXGPU_WARN("Frame {} has no commands; skipping", frame_index);
        continue;
      }
      if (is_bisected_last) {
        // Replay the final frame from its start to the requested command only.
        // SeekFrame would play the whole frame first, applying it twice.
        player_->SeekFrameStart(frame_index);
        const auto* frame = player_->current_frame();
        int last_command = static_cast<int>(frame->commands.size()) - 1;
        if (!commands_spec.empty()) {
          // Several stops in one pass. A forward SeekCommand resumes from the
          // previous stop, so the frame is applied exactly once overall.
          std::vector<int> stops;
          if (!ParseFrameList(commands_spec, last_command + 1, stops) || stops.empty()) {
            EndHostCapture();
            return 6;
          }
          for (int stop : stops) {
            REXGPU_INFO("Replaying frame {} to command {}/{}", frame_index, stop, last_command);
            player_->SeekCommand(stop);
            player_->WaitOnPlayback();
            if (!capture(fmt::format("_f{}_c{}", frame_index, stop))) {
              result = 1;
            }
            DumpStopState(stop);
            // The presented image only changes at the swap, so mid-frame stops
            // are observed through the EDRAM and shared-memory dumps instead.
            const char* edram_dump = getenv("REX_DUMP_EDRAM");
            const char* shared_dump = getenv("REX_DUMP_SHARED_MEMORY");
            if (edram_dump || shared_dump) {
              CommandProcessor* command_processor = graphics_system_->command_processor();
              std::promise<void> dump_done;
              command_processor->CallInThread([command_processor, &dump_done]() {
                command_processor->DebugDownloadTraceState();
                dump_done.set_value();
              });
              dump_done.get_future().wait();
              for (const char* dump_path : {edram_dump, shared_dump}) {
                if (!dump_path) {
                  continue;
                }
                std::error_code ec;
                std::filesystem::rename(dump_path, fmt::format("{}.c{}", dump_path, stop), ec);
                if (ec) {
                  REXGPU_ERROR("Could not set aside {} for command {}: {}", dump_path, stop,
                               ec.message());
                }
              }
            }
          }
          continue;
        }
        int command_index = std::min(bisect_command, last_command);
        REXGPU_INFO("Replaying frame {} to command {}/{}", frame_index, command_index,
                    last_command);
        player_->SeekCommand(command_index);
      } else if (frame_index == 0) {
        // A fresh player already sits at frame 0, so SeekFrame(0) would be a
        // no-op; drive the playback through SeekCommand instead.
        player_->SeekFrameStart(0);
        const auto* frame = player_->current_frame();
        player_->SeekCommand(static_cast<int>(frame->commands.size()) - 1);
      } else {
        player_->SeekFrame(frame_index);
      }
      player_->WaitOnPlayback();
      if (next_target < targets.size() && targets[next_target] == frame_index) {
        ++next_target;
        std::string suffix = fmt::format("_f{}", frame_index);
        if (is_bisected_last) {
          suffix += fmt::format("_c{}", player_->current_command_index());
        }
        if (!capture(suffix)) {
          result = 1;
        }
      }
    }
    EndHostCapture();

    // REX_DUMP_EDRAM=<path> dumps raw EDRAM and REX_DUMP_SHARED_MEMORY=<path>
    // the GPU-written guest memory ranges after the replayed commands.
    if (getenv("REX_DUMP_EDRAM") || getenv("REX_DUMP_SHARED_MEMORY")) {
      CommandProcessor* command_processor = graphics_system_->command_processor();
      std::promise<void> edram_dump_done;
      command_processor->CallInThread([command_processor, &edram_dump_done]() {
        command_processor->DebugDownloadTraceState();
        edram_dump_done.set_value();
      });
      edram_dump_done.get_future().wait();
    }
  } else {
    // Single-frame mode: seek straight to the frame. Correct for F4 captures,
    // which are self-contained; a stream trace needs trace_dump_frames.
    int frame_index = REXCVAR_GET(trace_dump_frame);
    if (frame_index < 0 || frame_index >= frame_count) {
      REXGPU_ERROR("Frame {} is out of range; trace has {} frames", frame_index, frame_count);
      return 6;
    }

    BeginHostCapture();
    player_->SeekFrame(frame_index);

    const auto* frame = player_->current_frame();
    int last_command = static_cast<int>(frame->commands.size()) - 1;
    int command_index = REXCVAR_GET(trace_dump_command);
    if (command_index < 0 || command_index > last_command) {
      command_index = last_command;
    }
    REXGPU_INFO("Replaying frame {}/{} to command {}/{}", frame_index, frame_count - 1,
                command_index, last_command);

    player_->SeekCommand(command_index);
    player_->WaitOnPlayback();
    EndHostCapture();

    if (!capture(fmt::format("_f{}_c{}", frame_index, command_index))) {
      result = 1;
    }
  }

  player_.reset();
  emulator_.reset();
  return result;
}

}  // namespace rex::graphics
