// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/server_family.h"

#include <absl/cleanup/cleanup.h>
#include <absl/random/random.h>  // for master_id_ generation.
#include <absl/strings/match.h>
#include <absl/strings/str_join.h>
#include <sys/resource.h>

#include <chrono>
#include <filesystem>
#include <optional>

extern "C" {
#include "redis/redis_aux.h"
}

#include "base/flags.h"
#include "base/logging.h"
#include "facade/dragonfly_connection.h"
#include "io/file_util.h"
#include "io/proc_reader.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/debugcmd.h"
#include "server/dflycmd.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/journal/journal.h"
#include "server/main_service.h"
#include "server/rdb_load.h"
#include "server/rdb_save.h"
#include "server/replica.h"
#include "server/script_mgr.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "server/transaction.h"
#include "server/version.h"
#include "strings/human_readable.h"
#include "util/accept_server.h"
#include "util/uring/uring_file.h"

using namespace std;

ABSL_FLAG(string, dir, "", "working directory");
ABSL_FLAG(string, dbfilename, "dump", "the filename to save/load the DB");
ABSL_FLAG(string, requirepass, "", "password for AUTH authentication");
ABSL_FLAG(string, save_schedule, "",
          "glob spec for the UTC time to save a snapshot which matches HH:MM 24h time");

ABSL_DECLARE_FLAG(uint32_t, port);
ABSL_DECLARE_FLAG(bool, cache_mode);
ABSL_DECLARE_FLAG(uint32_t, hz);

namespace dfly {

namespace fibers = ::boost::fibers;
namespace fs = std::filesystem;
namespace uring = util::uring;

using absl::GetFlag;
using absl::StrCat;
using namespace facade;
using strings::HumanReadableNumBytes;
using util::ProactorBase;
using util::http::StringResponse;

namespace {

const auto kRdbWriteFlags = O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_DIRECT;

using EngineFunc = void (ServerFamily::*)(CmdArgList args, ConnectionContext* cntx);

inline CommandId::Handler HandlerFunc(ServerFamily* se, EngineFunc f) {
  return [=](CmdArgList args, ConnectionContext* cntx) { return (se->*f)(args, cntx); };
}

using CI = CommandId;

// Create a direc
error_code CreateDirs(fs::path dir_path) {
  error_code ec;
  fs::file_status dir_status = fs::status(dir_path, ec);
  if (ec == errc::no_such_file_or_directory) {
    fs::create_directories(dir_path, ec);
    if (!ec)
      dir_status = fs::status(dir_path, ec);
  }
  return ec;
}

string UnknownCmd(string cmd, CmdArgList args) {
  return absl::StrCat("unknown command '", cmd, "' with args beginning with: ",
                      StrJoin(args.begin(), args.end(), ", ", CmdArgListFormatter()));
}

string InferLoadFile(fs::path data_dir) {
  const auto& dbname = GetFlag(FLAGS_dbfilename);

  if (dbname.empty())
    return string{};

  fs::path fl_path = data_dir.append(dbname);

  if (fs::exists(fl_path))
    return fl_path.generic_string();
  if (!fl_path.has_extension()) {
    string glob = fl_path.generic_string();
    glob.append("*.rdb");

    io::Result<io::StatShortVec> short_vec = io::StatFiles(glob);
    if (short_vec) {
      if (!short_vec->empty()) {
        return short_vec->back().name;
      }
    } else {
      LOG(WARNING) << "Could not stat " << glob << ", error " << short_vec.error().message();
    }
    LOG(INFO) << "Checking " << fl_path;
  }

  return string{};
}

bool IsValidSaveScheduleNibble(string_view time, unsigned int max) {
  /*
   * a nibble is valid iff there exists one time that matches the pattern
   * and that time is <= max. For any wildcard the minimum value is 0.
   * Therefore the minimum time the pattern can match is the time with
   * all *s replaced with 0s. If this time is > max all other times that
   * match the pattern are > max and the pattern is invalid. Otherwise
   * there exists at least one valid nibble specified by this pattern
   *
   * Note the edge case of "*" is equivalent to "**". While using this
   * approach "*" and "**" both map to 0.
   */
  unsigned int min_match = 0;
  for (size_t i = 0; i < time.size(); ++i) {
    // check for valid characters
    if (time[i] != '*' && (time[i] < '0' || time[i] > '9')) {
      return false;
    }
    min_match *= 10;
    min_match += time[i] == '*' ? 0 : time[i] - '0';
  }

  return min_match <= max;
}

class RdbSnapshot {
 public:
  RdbSnapshot(bool single_shard, uring::LinuxFile* fl)
      : file_(fl), linux_sink_(fl), saver_(&linux_sink_, single_shard, kRdbWriteFlags & O_DIRECT) {
  }

  error_code Start(const StringVec& lua_scripts);
  void StartInShard(EngineShard* shard);

  error_code SaveBody();
  error_code Close();

  const RdbTypeFreqMap freq_map() const {
    return freq_map_;
  }

  bool HasStarted() const {
    return started_;
  }

 private:
  bool started_ = false;

  unique_ptr<uring::LinuxFile> file_;
  LinuxWriteWrapper linux_sink_;
  RdbSaver saver_;
  RdbTypeFreqMap freq_map_;
};

error_code RdbSnapshot::Start(const StringVec& lua_scripts) {
  return saver_.SaveHeader(lua_scripts);
}

error_code RdbSnapshot::SaveBody() {
  return saver_.SaveBody(&freq_map_);
}

error_code RdbSnapshot::Close() {
  return linux_sink_.Close();
}

void RdbSnapshot::StartInShard(EngineShard* shard) {
  saver_.StartSnapshotInShard(false, shard);
  started_ = true;
}

string FormatTs(absl::Time now) {
  return absl::FormatTime("%Y-%m-%dT%H:%M:%S", now, absl::LocalTimeZone());
}

void ExtendFilename(absl::Time now, int shard, fs::path* filename) {
  if (shard < 0) {
    if (!filename->has_extension()) {
      string ft_time = FormatTs(now);
      *filename += StrCat("-", ft_time, ".rdb");
    }
  } else {
    string ft_time = FormatTs(now);
    filename->replace_extension();  // clear if exists

    // dragonfly snapshot
    *filename += StrCat("-", ft_time, "-", absl::Dec(shard, absl::kZeroPad4), ".dfs");
  }
}

inline void UpdateError(error_code src, error_code* dest) {
  if (!(*dest)) {
    *dest = src;
  }
}

}  // namespace

std::optional<SnapshotSpec> ParseSaveSchedule(string_view time) {
  if (time.length() < 3 || time.length() > 5) {
    return std::nullopt;
  }

  size_t separator_idx = time.find(':');
  // the time cannot start with ':' and it must be present in the first 3 characters of any time
  if (separator_idx == 0 || separator_idx >= 3) {
    return std::nullopt;
  }

  SnapshotSpec spec{string(time.substr(0, separator_idx)), string(time.substr(separator_idx + 1))};
  // a minute should be 2 digits as it is zero padded, unless it is a '*' in which case this
  // greedily can make up both digits
  if (spec.minute_spec != "*" && spec.minute_spec.length() != 2) {
    return std::nullopt;
  }

  return IsValidSaveScheduleNibble(spec.hour_spec, 23) &&
                 IsValidSaveScheduleNibble(spec.minute_spec, 59)
             ? std::optional<SnapshotSpec>(spec)
             : std::nullopt;
}

bool DoesTimeNibbleMatchSpecifier(string_view time_spec, unsigned int current_time) {
  // single greedy wildcard matches everything
  if (time_spec == "*") {
    return true;
  }

  for (int i = time_spec.length() - 1; i >= 0; --i) {
    // if the current digit is not a wildcard and it does not match the digit in the current time it
    // does not match
    if (time_spec[i] != '*' && int(current_time % 10) != (time_spec[i] - '0')) {
      return false;
    }
    current_time /= 10;
  }

  return current_time == 0;
}

bool DoesTimeMatchSpecifier(const SnapshotSpec& spec, time_t now) {
  unsigned hour = (now / 3600) % 24;
  unsigned min = (now / 60) % 60;
  return DoesTimeNibbleMatchSpecifier(spec.hour_spec, hour) &&
         DoesTimeNibbleMatchSpecifier(spec.minute_spec, min);
}

ServerFamily::ServerFamily(Service* service) : service_(*service) {
  start_time_ = time(NULL);
  last_save_info_ = make_shared<LastSaveInfo>();
  last_save_info_->save_time = start_time_;
  script_mgr_.reset(new ScriptMgr());
  journal_.reset(new journal::Journal);

  {
    absl::InsecureBitGen eng;
    master_id_ = GetRandomHex(eng, CONFIG_RUN_ID_SIZE);
    DCHECK_EQ(CONFIG_RUN_ID_SIZE, master_id_.size());
  }
}

ServerFamily::~ServerFamily() {
}

void ServerFamily::Init(util::AcceptServer* acceptor, util::ListenerInterface* main_listener) {
  CHECK(acceptor_ == nullptr);
  acceptor_ = acceptor;
  main_listener_ = main_listener;
  dfly_cmd_.reset(new DflyCmd(main_listener, this));

  pb_task_ = shard_set->pool()->GetNextProactor();

  // Unlike EngineShard::Heartbeat that runs independently in each shard thread,
  // this callback runs in a single thread and it aggregates globally stats from all the shards.
  auto cache_cb = [] {
    uint64_t sum = 0;
    const auto& stats = EngineShardSet::GetCachedStats();
    for (const auto& s : stats)
      sum += s.used_memory.load(memory_order_relaxed);

    used_mem_current.store(sum, memory_order_relaxed);

    // Single writer, so no races.
    if (sum > used_mem_peak.load(memory_order_relaxed))
      used_mem_peak.store(sum, memory_order_relaxed);
  };

  uint32_t cache_hz = max(GetFlag(FLAGS_hz) / 10, 1u);
  uint32_t period_ms = max(1u, 1000 / cache_hz);
  stats_caching_task_ =
      pb_task_->AwaitBrief([&] { return pb_task_->AddPeriodic(period_ms, cache_cb); });

  fs::path data_folder = fs::current_path();
  const auto& dir = GetFlag(FLAGS_dir);

  if (!dir.empty()) {
    data_folder = dir;

    error_code ec;

    data_folder = fs::canonical(data_folder, ec);
  }

  LOG(INFO) << "Data directory is " << data_folder;
  string load_path = InferLoadFile(data_folder);
  if (!load_path.empty()) {
    Load(load_path);
  }

  string save_time = GetFlag(FLAGS_save_schedule);
  if (!save_time.empty()) {
    std::optional<SnapshotSpec> spec = ParseSaveSchedule(save_time);
    if (spec) {
      snapshot_fiber_ = service_.proactor_pool().GetNextProactor()->LaunchFiber(
          [save_spec = std::move(spec.value()), this] {
            SnapshotScheduling(std::move(save_spec));
          });
    } else {
      LOG(WARNING) << "Invalid snapshot time specifier " << save_time;
    }
  }
}

void ServerFamily::Shutdown() {
  VLOG(1) << "ServerFamily::Shutdown";

  if (load_fiber_.joinable())
    load_fiber_.join();

  is_snapshot_done_.Notify();
  if (snapshot_fiber_.joinable()) {
    snapshot_fiber_.join();
  }

  pb_task_->Await([this] {
    pb_task_->CancelPeriodic(stats_caching_task_);
    stats_caching_task_ = 0;

    if (journal_->EnterLameDuck()) {
      auto ec = journal_->Close();
      LOG_IF(ERROR, ec) << "Error closing journal " << ec;
    }

    unique_lock lk(replicaof_mu_);
    if (replica_) {
      replica_->Stop();
    }
  });
}

void ServerFamily::Load(const std::string& load_path) {
  CHECK(!load_fiber_.get_id());

  error_code ec;
  auto path = fs::canonical(load_path, ec);
  if (ec) {
    LOG(ERROR) << "Error loading " << load_path << " " << ec.message();
    return;
  }

  LOG(INFO) << "Loading " << load_path;

  GlobalState new_state = service_.SwitchState(GlobalState::ACTIVE, GlobalState::LOADING);
  if (new_state != GlobalState::LOADING) {
    LOG(WARNING) << GlobalStateName(new_state) << " in progress, ignored";
    return;
  }

#if 0
  auto& pool = service_.proactor_pool();

  // Deliberately run on all I/O threads to update the state for non-shard threads as well.
  pool.Await([&](ProactorBase*) {
    // TODO: There can be a bug where status is different.
    CHECK(ServerState::tlocal()->gstate() == GlobalState::IDLE);
    ServerState::tlocal()->set_gstate(GlobalState::LOADING);
  });
#endif

  auto& pool = service_.proactor_pool();
  // Choose thread that does not handle shards if possible.
  // This will balance out the CPU during the load.
  ProactorBase* proactor =
      shard_count() < pool.size() ? pool.at(shard_count()) : pool.GetNextProactor();

  load_fiber_ = proactor->LaunchFiber([load_path, this] {
    auto ec = LoadRdb(load_path);
    LOG_IF(ERROR, ec) << "Error loading file " << ec.message();
  });
}

void ServerFamily::SnapshotScheduling(const SnapshotSpec&& spec) {
  const auto loop_sleep_time = std::chrono::seconds(20);
  while (true) {
    if (is_snapshot_done_.WaitFor(loop_sleep_time)) {
      break;
    }

    time_t now = std::time(NULL);

    if (!DoesTimeMatchSpecifier(spec, now)) {
      continue;
    }

    // if it matches check the last save time, if it is the same minute don't save another snapshot
    time_t last_save;
    {
      lock_guard lk(save_mu_);
      last_save = last_save_info_->save_time;
    }

    if ((last_save / 60) == (now / 60)) {
      continue;
    }

    // do the save
    string err_details;
    error_code ec;
    const CommandId* cid = service().FindCmd("SAVE");
    CHECK_NOTNULL(cid);
    boost::intrusive_ptr<Transaction> trans(new Transaction{cid});
    trans->InitByArgs(0, {});
    ec = DoSave(false, trans.get(), &err_details);
    if (ec) {
      LOG(WARNING) << "Failed to perform snapshot " << err_details;
    }
  }
}

error_code ServerFamily::LoadRdb(const std::string& rdb_file) {
  io::ReadonlyFileOrError res = uring::OpenRead(rdb_file);
  error_code ec;

  if (res) {
    io::FileSource fs(*res);

    RdbLoader loader(script_mgr());
    ec = loader.Load(&fs);
    if (!ec) {
      LOG(INFO) << "Done loading RDB, keys loaded: " << loader.keys_loaded();
      LOG(INFO) << "Loading finished after "
                << strings::HumanReadableElapsedTime(loader.load_time());
    }
  } else {
    ec = res.error();
  }

  service_.SwitchState(GlobalState::LOADING, GlobalState::ACTIVE);

  return ec;
}

enum MetricType { COUNTER, GAUGE, SUMMARY, HISTOGRAM };

const char* MetricTypeName(MetricType type) {
  switch (type) {
    case MetricType::COUNTER:
      return "counter";
    case MetricType::GAUGE:
      return "gauge";
    case MetricType::SUMMARY:
      return "summary";
    case MetricType::HISTOGRAM:
      return "histogram";
  }
  return "unknown";
}

inline string GetMetricFullName(string_view metric_name) {
  return StrCat("dragonfly_", metric_name);
}

void AppendMetricHeader(string_view metric_name, string_view metric_help, MetricType type,
                        string* dest) {
  const auto full_metric_name = GetMetricFullName(metric_name);
  absl::StrAppend(dest, "# HELP ", full_metric_name, " ", metric_help, "\n");
  absl::StrAppend(dest, "# TYPE ", full_metric_name, " ", MetricTypeName(type), "\n");
}

void AppendLabelTupple(absl::Span<const string_view> label_names,
                       absl::Span<const string_view> label_values, string* dest) {
  if (label_names.empty())
    return;

  absl::StrAppend(dest, "{");
  for (size_t i = 0; i < label_names.size(); ++i) {
    if (i > 0) {
      absl::StrAppend(dest, ", ");
    }
    absl::StrAppend(dest, label_names[i], "=\"", label_values[i], "\"");
  }

  absl::StrAppend(dest, "}");
}

void AppendMetricValue(string_view metric_name, const absl::AlphaNum& value,
                       absl::Span<const string_view> label_names,
                       absl::Span<const string_view> label_values, string* dest) {
  absl::StrAppend(dest, GetMetricFullName(metric_name));
  AppendLabelTupple(label_names, label_values, dest);
  absl::StrAppend(dest, " ", value, "\n");
}

void AppendMetricWithoutLabels(string_view name, string_view help, const absl::AlphaNum& value,
                               MetricType type, string* dest) {
  AppendMetricHeader(name, help, type, dest);
  AppendMetricValue(name, value, {}, {}, dest);
}

void PrintPrometheusMetrics(const Metrics& m, StringResponse* resp) {
  // Server metrics
  AppendMetricWithoutLabels("up", "", 1, MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("uptime_in_seconds", "", m.uptime, MetricType::GAUGE, &resp->body());

  // Clients metrics
  AppendMetricWithoutLabels("connected_clients", "", m.conn_stats.num_conns, MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("client_read_buf_capacity", "", m.conn_stats.read_buf_capacity,
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("blocked_clients", "", m.conn_stats.num_blocked_clients,
                            MetricType::GAUGE, &resp->body());

  // Memory metrics
  AppendMetricWithoutLabels("memory_used_bytes", "", m.heap_used_bytes, MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("memory_used_peak_bytes", "", used_mem_peak.load(memory_order_relaxed),
                            MetricType::GAUGE, &resp->body());
  AppendMetricWithoutLabels("comitted_memory", "", GetMallocCurrentCommitted(), MetricType::GAUGE,
                            &resp->body());
  AppendMetricWithoutLabels("memory_max_bytes", "", max_memory_limit, MetricType::GAUGE,
                            &resp->body());

  AppendMetricWithoutLabels("commands_processed_total", "", m.conn_stats.command_cnt,
                            MetricType::COUNTER, &resp->body());

  // Net metrics
  AppendMetricWithoutLabels("net_input_bytes_total", "", m.conn_stats.io_read_bytes,
                            MetricType::COUNTER, &resp->body());
  AppendMetricWithoutLabels("net_output_bytes_total", "", m.conn_stats.io_write_bytes,
                            MetricType::COUNTER, &resp->body());

  // DB stats
  AppendMetricWithoutLabels("expired_keys_total", "", m.events.expired_keys, MetricType::COUNTER,
                            &resp->body());
  AppendMetricWithoutLabels("evicted_keys_total", "", m.events.evicted_keys, MetricType::COUNTER,
                            &resp->body());

  string db_key_metrics;
  string db_key_expire_metrics;

  AppendMetricHeader("db_keys", "Total number of keys by DB", MetricType::GAUGE, &db_key_metrics);
  AppendMetricHeader("db_keys_expiring", "Total number of expiring keys by DB", MetricType::GAUGE,
                     &db_key_expire_metrics);

  for (size_t i = 0; i < m.db.size(); ++i) {
    AppendMetricValue("db_keys", m.db[i].key_count, {"db"}, {StrCat("db", i)}, &db_key_metrics);
    AppendMetricValue("db_keys_expiring", m.db[i].expire_count, {"db"}, {StrCat("db", i)},
                      &db_key_expire_metrics);
  }

  absl::StrAppend(&resp->body(), db_key_metrics);
  absl::StrAppend(&resp->body(), db_key_expire_metrics);
}

void ServerFamily::ConfigureMetrics(util::HttpListenerBase* http_base) {
  // The naming of the metrics should be compatible with redis_exporter, see
  // https://github.com/oliver006/redis_exporter/blob/master/exporter/exporter.go#L111

  auto cb = [this](const util::http::QueryArgs& args, util::HttpContext* send) {
    StringResponse resp = util::http::MakeStringResponse(boost::beast::http::status::ok);
    PrintPrometheusMetrics(this->GetMetrics(), &resp);

    return send->Invoke(std::move(resp));
  };

  http_base->RegisterCb("/metrics", cb);
}

void ServerFamily::PauseReplication(bool pause) {
  unique_lock lk(replicaof_mu_);

  // Switch to primary mode.
  if (!ServerState::tlocal()->is_master) {
    auto repl_ptr = replica_;
    CHECK(repl_ptr);
    repl_ptr->Pause(pause);
  }
}

void ServerFamily::OnClose(ConnectionContext* cntx) {
  dfly_cmd_->OnClose(cntx);
}

void ServerFamily::StatsMC(std::string_view section, facade::ConnectionContext* cntx) {
  if (!section.empty()) {
    return cntx->reply_builder()->SendError("");
  }
  string info;

#define ADD_LINE(name, val) absl::StrAppend(&info, "STAT " #name " ", val, "\r\n")

  time_t now = time(NULL);
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);

  auto dbl_time = [](const timeval& tv) -> double {
    return tv.tv_sec + double(tv.tv_usec) / 1000000.0;
  };

  double utime = dbl_time(ru.ru_utime);
  double systime = dbl_time(ru.ru_stime);

  Metrics m = GetMetrics();

  ADD_LINE(pid, getpid());
  ADD_LINE(uptime, m.uptime);
  ADD_LINE(time, now);
  ADD_LINE(version, kGitTag);
  ADD_LINE(libevent, "iouring");
  ADD_LINE(pointer_size, sizeof(void*));
  ADD_LINE(rusage_user, utime);
  ADD_LINE(rusage_system, systime);
  ADD_LINE(max_connections, -1);
  ADD_LINE(curr_connections, m.conn_stats.num_conns);
  ADD_LINE(total_connections, -1);
  ADD_LINE(rejected_connections, -1);
  ADD_LINE(bytes_read, m.conn_stats.io_read_bytes);
  ADD_LINE(bytes_written, m.conn_stats.io_write_bytes);
  ADD_LINE(limit_maxbytes, -1);

  absl::StrAppend(&info, "END\r\n");

  MCReplyBuilder* builder = static_cast<MCReplyBuilder*>(cntx->reply_builder());
  builder->SendRaw(info);

#undef ADD_LINE
}

static void RunStage(bool new_version, std::function<void(unsigned)> cb) {
  if (new_version) {
    shard_set->RunBlockingInParallel([&](EngineShard* es) { cb(es->shard_id()); });
  } else {
    cb(0);
  }
};

error_code ServerFamily::DoSave(bool new_version, Transaction* trans, string* err_details) {
  fs::path dir_path(GetFlag(FLAGS_dir));
  error_code ec;

  if (!dir_path.empty()) {
    ec = CreateDirs(dir_path);
    if (ec) {
      *err_details = "create-dir ";
      return ec;
    }
  }

  const auto& dbfilename = GetFlag(FLAGS_dbfilename);
  fs::path filename = dbfilename.empty() ? "dump" : dbfilename;
  fs::path path = dir_path;

  GlobalState new_state = service_.SwitchState(GlobalState::ACTIVE, GlobalState::SAVING);
  if (new_state != GlobalState::SAVING) {
    *err_details = StrCat(GlobalStateName(new_state), " - can not save database");
    return make_error_code(errc::operation_in_progress);
  }

  absl::Cleanup rev_state = [this] {
    service_.SwitchState(GlobalState::SAVING, GlobalState::ACTIVE);
  };

  auto start = absl::Now();
  shared_ptr<LastSaveInfo> save_info;
  StringVec lua_scripts = script_mgr_->GetLuaScripts();
  absl::Time now = absl::Now();

  absl::flat_hash_map<string_view, size_t> rdb_name_map;
  vector<unique_ptr<RdbSnapshot>> snapshots;
  fibers::mutex mu;

  auto save_cb = [&](unsigned index) {
    auto& snapshot = snapshots[index];
    if (snapshot && snapshot->HasStarted()) {
      error_code local_ec = snapshot->SaveBody();
      if (local_ec) {
        lock_guard lk(mu);
        UpdateError(local_ec, &ec);
      }
    }
  };

  auto close_cb = [&](unsigned index) {
    auto& snapshot = snapshots[index];
    if (snapshot) {
      auto local_ec = snapshot->Close();

      lock_guard lk(mu);
      UpdateError(local_ec, &ec);

      for (const auto& k_v : snapshot->freq_map()) {
        rdb_name_map[RdbTypeName(k_v.first)] += k_v.second;
      }
    }
  };

  if (new_version) {
    snapshots.resize(shard_set->size());

    // In the new version we open a file per shard
    auto cb = [&](Transaction* t, EngineShard* shard) {
      fs::path shard_file = filename, abs_path = path;
      ShardId sid = shard->shard_id();
      error_code local_ec;

      ExtendFilename(now, sid, &shard_file);
      abs_path += shard_file;

      VLOG(1) << "Saving to " << abs_path;
      auto res = uring::OpenLinux(abs_path.generic_string(), kRdbWriteFlags, 0666);

      if (res) {
        snapshots[sid].reset(new RdbSnapshot{true, res.value().release()});
        auto& snapshot = *snapshots[sid];
        local_ec = snapshot.Start(lua_scripts);
        if (!local_ec) {
          snapshot.StartInShard(shard);
        }
      } else {
        local_ec = res.error();
      }

      if (local_ec) {
        lock_guard lk(mu);
        UpdateError(local_ec, &ec);
      }

      return OpStatus::OK;
    };

    trans->ScheduleSingleHop(std::move(cb));
  } else {
    snapshots.resize(1);

    ExtendFilename(now, -1, &filename);
    path += filename;

    auto res = uring::OpenLinux(path.generic_string(), kRdbWriteFlags, 0666);
    if (!res) {
      return res.error();
    }
    VLOG(1) << "Saving to " << path;

    snapshots[0].reset(new RdbSnapshot{false, res.value().release()});
    ec = snapshots[0]->Start(lua_scripts);

    if (!ec) {
      auto cb = [&](Transaction* t, EngineShard* shard) {
        snapshots[0]->StartInShard(shard);

        return OpStatus::OK;
      };

      trans->ScheduleSingleHop(std::move(cb));
    }
  }

  is_saving_.store(true, memory_order_relaxed);

  // perform snapshot serialization, block the current fiber until it completes.
  RunStage(new_version, save_cb);

  is_saving_.store(false, memory_order_relaxed);

  RunStage(new_version, close_cb);

  absl::Duration dur = absl::Now() - start;
  double seconds = double(absl::ToInt64Milliseconds(dur)) / 1000;

  if (new_version) {
    ExtendFilename(now, 0, &filename);
    path += filename;
  }

  LOG(INFO) << "Saving " << path << " finished after "
            << strings::HumanReadableElapsedTime(seconds);

  if (!ec) {
    save_info = make_shared<LastSaveInfo>();
    for (const auto& k_v : rdb_name_map) {
      save_info->freq_map.emplace_back(k_v);
    }

    save_info->save_time = absl::ToUnixSeconds(now);
    save_info->file_name = path.generic_string();

    lock_guard lk(save_mu_);
    // swap - to deallocate the old version outstide of the lock.
    last_save_info_.swap(save_info);
  }
  return ec;
}

error_code ServerFamily::DoFlush(Transaction* transaction, DbIndex db_ind) {
  VLOG(1) << "DoFlush";

  transaction->Schedule();  // TODO: to convert to ScheduleSingleHop ?

  transaction->Execute(
      [db_ind](Transaction* t, EngineShard* shard) {
        shard->db_slice().FlushDb(db_ind);
        return OpStatus::OK;
      },
      true);

  return error_code{};
}

shared_ptr<const LastSaveInfo> ServerFamily::GetLastSaveInfo() const {
  lock_guard lk(save_mu_);
  return last_save_info_;
}

void ServerFamily::DbSize(CmdArgList args, ConnectionContext* cntx) {
  atomic_ulong num_keys{0};

  shard_set->RunBriefInParallel(
      [&](EngineShard* shard) {
        auto db_size = shard->db_slice().DbSize(cntx->conn_state.db_index);
        num_keys.fetch_add(db_size, memory_order_relaxed);
      },
      [](ShardId) { return true; });

  return (*cntx)->SendLong(num_keys.load(memory_order_relaxed));
}

void ServerFamily::BreakOnShutdown() {
  dfly_cmd_->BreakOnShutdown();
}

void ServerFamily::FlushDb(CmdArgList args, ConnectionContext* cntx) {
  DCHECK(cntx->transaction);
  DoFlush(cntx->transaction, cntx->transaction->db_index());
  cntx->reply_builder()->SendOk();
}

void ServerFamily::FlushAll(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 1) {
    (*cntx)->SendError(kSyntaxErr);
    return;
  }

  DCHECK(cntx->transaction);
  DoFlush(cntx->transaction, DbSlice::kDbAll);
  (*cntx)->SendOk();
}

void ServerFamily::Auth(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 3) {
    return (*cntx)->SendError(kSyntaxErr);
  }

  if (args.size() == 3) {
    return (*cntx)->SendError("ACL is not supported yet");
  }

  if (!cntx->req_auth) {
    return (*cntx)->SendError(
        "AUTH <password> called without any password configured for the "
        "default user. Are you sure your configuration is correct?");
  }

  string_view pass = ArgS(args, 1);
  if (pass == GetFlag(FLAGS_requirepass)) {
    cntx->authenticated = true;
    (*cntx)->SendOk();
  } else {
    (*cntx)->SendError(facade::kAuthRejected);
  }
}

void ServerFamily::Client(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);
  string_view sub_cmd = ArgS(args, 1);

  if (sub_cmd == "SETNAME" && args.size() == 3) {
    cntx->owner()->SetName(ArgS(args, 2));
    return (*cntx)->SendOk();
  }

  if (sub_cmd == "LIST") {
    vector<string> client_info;
    fibers::mutex mu;
    auto cb = [&](util::Connection* conn) {
      facade::Connection* dcon = static_cast<facade::Connection*>(conn);
      string info = dcon->GetClientInfo();
      lock_guard lk(mu);
      client_info.push_back(move(info));
    };

    main_listener_->TraverseConnections(cb);
    string result = absl::StrJoin(move(client_info), "\n");
    result.append("\n");
    return (*cntx)->SendBulkString(result);
  }

  LOG_FIRST_N(ERROR, 10) << "Subcommand " << sub_cmd << " not supported";
  return (*cntx)->SendError(UnknownSubCmd(sub_cmd, "CLIENT"), kSyntaxErrType);
}

void ServerFamily::Config(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);
  string_view sub_cmd = ArgS(args, 1);

  if (sub_cmd == "SET") {
    return (*cntx)->SendOk();
  } else if (sub_cmd == "GET" && args.size() == 3) {
    string_view param = ArgS(args, 2);
    string_view res[2] = {param, "tbd"};

    return (*cntx)->SendStringArr(res);
  } else if (sub_cmd == "RESETSTAT") {
    shard_set->pool()->Await([](auto*) {
      auto* stats = ServerState::tl_connection_stats();
      stats->cmd_count_map.clear();
      stats->err_count_map.clear();
      stats->command_cnt = 0;
      stats->async_writes_cnt = 0;
    });
    return (*cntx)->SendOk();
  } else {
    return (*cntx)->SendError(UnknownSubCmd(sub_cmd, "CONFIG"), kSyntaxErrType);
  }
}

void ServerFamily::Debug(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);

  DebugCmd dbg_cmd{this, cntx};

  return dbg_cmd.Run(args);
}

void ServerFamily::Memory(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);
  string_view sub_cmd = ArgS(args, 1);
  if (sub_cmd == "USAGE") {
    return (*cntx)->SendLong(1);
  }

  string err = UnknownSubCmd(sub_cmd, "MEMORY");
  return (*cntx)->SendError(err, kSyntaxErrType);
}

void ServerFamily::Save(CmdArgList args, ConnectionContext* cntx) {
  string err_detail;
  bool new_version = false;
  if (args.size() > 2) {
    return (*cntx)->SendError(kSyntaxErr);
  }

  if (args.size() == 2) {
    ToUpper(&args[1]);
    string_view sub_cmd = ArgS(args, 1);
    if (sub_cmd == "DF") {
      new_version = true;
    } else {
      return (*cntx)->SendError(UnknownSubCmd(sub_cmd, "SAVE"), kSyntaxErrType);
    }
  }

  error_code ec = DoSave(new_version, cntx->transaction, &err_detail);

  if (ec) {
    (*cntx)->SendError(absl::StrCat(err_detail, ec.message()));
  } else {
    (*cntx)->SendOk();
  }
}

static void MergeInto(const DbSlice::Stats& src, Metrics* dest) {
  if (src.db_stats.size() > dest->db.size())
    dest->db.resize(src.db_stats.size());
  for (size_t i = 0; i < src.db_stats.size(); ++i) {
    dest->db[i] += src.db_stats[i];
  }

  dest->events += src.events;
  dest->small_string_bytes += src.small_string_bytes;
}

Metrics ServerFamily::GetMetrics() const {
  Metrics result;

  fibers::mutex mu;

  auto cb = [&](ProactorBase* pb) {
    EngineShard* shard = EngineShard::tlocal();
    ServerState* ss = ServerState::tlocal();

    lock_guard<fibers::mutex> lk(mu);

    result.uptime = time(NULL) - this->start_time_;
    result.conn_stats += ss->connection_stats;
    result.qps += uint64_t(ss->MovingSum6());

    if (shard) {
      MergeInto(shard->db_slice().GetStats(), &result);

      result.heap_used_bytes += shard->UsedMemory();
      if (shard->tiered_storage()) {
        result.tiered_stats += shard->tiered_storage()->GetStats();
      }
      result.shard_stats += shard->stats();
      result.traverse_ttl_per_sec += shard->GetMovingSum6(EngineShard::TTL_TRAVERSE);
      result.delete_ttl_per_sec += shard->GetMovingSum6(EngineShard::TTL_DELETE);
    }
  };

  service_.proactor_pool().AwaitFiberOnAll(std::move(cb));
  result.qps /= 6;  // normalize moving average stats
  result.traverse_ttl_per_sec /= 6;
  result.delete_ttl_per_sec /= 6;

  return result;
}

void ServerFamily::Info(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() > 2) {
    return (*cntx)->SendError(kSyntaxErr);
  }

  string_view section;

  if (args.size() == 2) {
    ToUpper(&args[1]);
    section = ArgS(args, 1);
  }

  string info;

  auto should_enter = [&](string_view name, bool hidden = false) {
    bool res = (!hidden && section.empty()) || section == "ALL" || section == name;
    if (res && !info.empty())
      info.append("\r\n");

    return res;
  };

  auto append = [&info](absl::AlphaNum a1, absl::AlphaNum a2) {
    absl::StrAppend(&info, a1, ":", a2, "\r\n");
  };

#define ADD_HEADER(x) absl::StrAppend(&info, x "\r\n")
  Metrics m = GetMetrics();

  if (should_enter("SERVER")) {
    ADD_HEADER("# Server");

    append("redis_version", GetVersion());
    append("redis_mode", "standalone");
    append("arch_bits", 64);
    append("multiplexing_api", "iouring");
    append("tcp_port", GetFlag(FLAGS_port));

    size_t uptime = m.uptime;
    append("uptime_in_seconds", uptime);
    append("uptime_in_days", uptime / (3600 * 24));
  }

  auto sdata_res = io::ReadStatusInfo();

  DbStats total;
  for (const auto& db_stats : m.db) {
    total += db_stats;
  }

  if (should_enter("CLIENTS")) {
    ADD_HEADER("# Clients");
    append("connected_clients", m.conn_stats.num_conns);
    append("client_read_buf_capacity", m.conn_stats.read_buf_capacity);
    append("blocked_clients", m.conn_stats.num_blocked_clients);
  }

  if (should_enter("MEMORY")) {
    ADD_HEADER("# Memory");

    append("used_memory", m.heap_used_bytes);
    append("used_memory_human", HumanReadableNumBytes(m.heap_used_bytes));
    append("used_memory_peak", used_mem_peak.load(memory_order_relaxed));

    append("comitted_memory", GetMallocCurrentCommitted());

    if (sdata_res.has_value()) {
      append("used_memory_rss", sdata_res->vm_rss);
      append("used_memory_rss_human", HumanReadableNumBytes(sdata_res->vm_rss));
    } else {
      LOG_FIRST_N(ERROR, 10) << "Error fetching /proc/self/status stats. error "
                             << sdata_res.error().message();
    }

    // Blob - all these cases where the key/objects are represented by a single blob allocated on
    // heap. For example, strings or intsets. members of lists, sets, zsets etc
    // are not accounted for to avoid complex computations. In some cases, when number of members
    // is known we approximate their allocations by taking 16 bytes per member.
    append("object_used_memory", total.obj_memory_usage);
    append("table_used_memory", total.table_mem_usage);
    append("num_buckets", total.bucket_count);
    append("num_entries", total.key_count);
    append("inline_keys", total.inline_keys);
    append("strval_bytes", total.strval_memory_usage);
    append("updateval_amount", total.update_value_amount);
    append("listpack_blobs", total.listpack_blob_cnt);
    append("listpack_bytes", total.listpack_bytes);
    append("small_string_bytes", m.small_string_bytes);
    append("maxmemory", max_memory_limit);
    append("maxmemory_human", HumanReadableNumBytes(max_memory_limit));
    append("cache_mode", GetFlag(FLAGS_cache_mode) ? "cache" : "store");
  }

  if (should_enter("STATS")) {
    ADD_HEADER("# Stats");

    append("instantaneous_ops_per_sec", m.qps);
    append("total_commands_processed", m.conn_stats.command_cnt);
    append("total_pipelined_commands", m.conn_stats.pipelined_cmd_cnt);
    append("total_net_input_bytes", m.conn_stats.io_read_bytes);
    append("total_net_output_bytes", m.conn_stats.io_write_bytes);
    append("instantaneous_input_kbps", -1);
    append("instantaneous_output_kbps", -1);
    append("rejected_connections", -1);
    append("expired_keys", m.events.expired_keys);
    append("evicted_keys", m.events.evicted_keys);
    append("hard_evictions", m.events.hard_evictions);
    append("garbage_checked", m.events.garbage_checked);
    append("garbage_collected", m.events.garbage_collected);
    append("bump_ups", m.events.bumpups);
    append("stash_unloaded", m.events.stash_unloaded);
    append("traverse_ttl_sec", m.traverse_ttl_per_sec);
    append("delete_ttl_sec", m.delete_ttl_per_sec);
    append("keyspace_hits", -1);
    append("keyspace_misses", -1);
    append("total_reads_processed", m.conn_stats.io_read_cnt);
    append("total_writes_processed", m.conn_stats.io_write_cnt);
    append("async_writes_count", m.conn_stats.async_writes_cnt);
    append("parser_err_count", m.conn_stats.parser_err_cnt);
  }

  if (should_enter("TIERED", true)) {
    ADD_HEADER("# TIERED");
    append("external_entries", total.external_entries);
    append("external_bytes", total.external_size);
    append("external_reads", m.tiered_stats.external_reads);
    append("external_writes", m.tiered_stats.external_writes);
    append("external_reserved", m.tiered_stats.storage_reserved);
    append("external_capacity", m.tiered_stats.storage_capacity);
  }

  if (should_enter("PERSISTENCE", true)) {
    ADD_HEADER("# PERSISTENCE");
    decltype(last_save_info_) save_info;
    {
      lock_guard lk(save_mu_);
      save_info = last_save_info_;
    }
    append("last_save", save_info->save_time);
    append("last_save_file", save_info->file_name);
    for (const auto& k_v : save_info->freq_map) {
      append(StrCat("rdb_", k_v.first), k_v.second);
    }
  }

  if (should_enter("REPLICATION")) {
    ADD_HEADER("# Replication");

    ServerState& etl = *ServerState::tlocal();

    if (etl.is_master) {
      append("role", "master");
      append("connected_slaves", m.conn_stats.num_replicas);
      append("master_replid", master_id_);
    } else {
      append("role", "slave");

      // it's safe to access replica_ because replica_ is created before etl.is_master set to
      // false and cleared after etl.is_master is set to true. And since the code here that checks
      // for is_master and copies shared_ptr is atomic, it1 should be correct.
      auto replica_ptr = replica_;
      Replica::Info rinfo = replica_ptr->GetInfo();
      append("master_host", rinfo.host);
      append("master_port", rinfo.port);

      const char* link = rinfo.master_link_established ? "up" : "down";
      append("master_link_status", link);
      append("master_last_io_seconds_ago", rinfo.master_last_io_sec);
      append("master_sync_in_progress", rinfo.sync_in_progress);
    }
  }

  if (should_enter("COMMANDSTATS", true)) {
    ADD_HEADER("# Commandstats");

    auto unknown_cmd = service_.UknownCmdMap();

    for (const auto& k_v : unknown_cmd) {
      append(StrCat("unknown_", k_v.first), k_v.second);
    }

    for (const auto& k_v : m.conn_stats.cmd_count_map) {
      append(StrCat("cmd_", k_v.first), k_v.second);
    }
  }

  if (should_enter("ERRORSTATS", true)) {
    ADD_HEADER("# Errorstats");
    for (const auto& k_v : m.conn_stats.err_count_map) {
      append(k_v.first, k_v.second);
    }
  }

  if (should_enter("KEYSPACE")) {
    ADD_HEADER("# Keyspace");
    for (size_t i = 0; i < m.db.size(); ++i) {
      const auto& stats = m.db[i];
      bool show = (i == 0) || (stats.key_count > 0);
      if (show) {
        string val = StrCat("keys=", stats.key_count, ",expires=", stats.expire_count,
                            ",avg_ttl=-1");  // TODO
        append(StrCat("db", i), val);
      }
    }
  }

  if (should_enter("CPU")) {
    ADD_HEADER("# CPU");
    struct rusage ru, cu, tu;
    getrusage(RUSAGE_SELF, &ru);
    getrusage(RUSAGE_CHILDREN, &cu);
    getrusage(RUSAGE_THREAD, &tu);
    append("used_cpu_sys", StrCat(ru.ru_stime.tv_sec, ".", ru.ru_stime.tv_usec));
    append("used_cpu_user", StrCat(ru.ru_utime.tv_sec, ".", ru.ru_utime.tv_usec));
    append("used_cpu_sys_children", StrCat(cu.ru_stime.tv_sec, ".", cu.ru_stime.tv_usec));
    append("used_cpu_user_children", StrCat(cu.ru_utime.tv_sec, ".", cu.ru_utime.tv_usec));
    append("used_cpu_sys_main_thread", StrCat(tu.ru_stime.tv_sec, ".", tu.ru_stime.tv_usec));
    append("used_cpu_user_main_thread", StrCat(tu.ru_utime.tv_sec, ".", tu.ru_utime.tv_usec));
  }

  (*cntx)->SendBulkString(info);
}

void ServerFamily::Hello(CmdArgList args, ConnectionContext* cntx) {
  // Allow calling this commands with no arguments or protover=2
  // technically that is all that is supported at the moment.
  // For all other cases degrade to 'unknown command' so that clients
  // checking for the existence of the command to detect if RESP3 is
  // supported or whether authentication can be performed using HELLO
  // will gracefully fallback to RESP2 and using the AUTH command explicitly.
  if (args.size() > 1) {
    string_view proto_version = ArgS(args, 1);
    if (proto_version != "2" || args.size() > 2) {
      (*cntx)->SendError(UnknownCmd("HELLO", args.subspan(1)));
      return;
    }
  }

  (*cntx)->StartArray(12);
  (*cntx)->SendBulkString("server");
  (*cntx)->SendBulkString("redis");
  (*cntx)->SendBulkString("version");
  (*cntx)->SendBulkString(GetVersion());
  (*cntx)->SendBulkString("proto");
  (*cntx)->SendLong(2);
  (*cntx)->SendBulkString("id");
  (*cntx)->SendLong(cntx->owner()->GetClientId());
  (*cntx)->SendBulkString("mode");
  (*cntx)->SendBulkString("standalone");
  (*cntx)->SendBulkString("role");
  (*cntx)->SendBulkString((*ServerState::tlocal()).is_master ? "master" : "slave");
}

void ServerFamily::ReplicaOf(CmdArgList args, ConnectionContext* cntx) {
  std::string_view host = ArgS(args, 1);
  std::string_view port_s = ArgS(args, 2);
  auto& pool = service_.proactor_pool();

  if (absl::EqualsIgnoreCase(host, "no") && absl::EqualsIgnoreCase(port_s, "one")) {
    // use this lock as critical section to prevent concurrent replicaof commands running.
    unique_lock lk(replicaof_mu_);

    // Switch to primary mode.
    if (!ServerState::tlocal()->is_master) {
      auto repl_ptr = replica_;
      CHECK(repl_ptr);

      pool.AwaitFiberOnAll(
          [&](util::ProactorBase* pb) { ServerState::tlocal()->is_master = true; });
      replica_->Stop();
      replica_.reset();
    }

    return (*cntx)->SendOk();
  }

  uint32_t port;

  if (!absl::SimpleAtoi(port_s, &port) || port < 1 || port > 65535) {
    (*cntx)->SendError(kInvalidIntErr);
    return;
  }

  auto new_replica = make_shared<Replica>(string(host), port, &service_);

  unique_lock lk(replicaof_mu_);
  if (replica_) {
    replica_->Stop();  // NOTE: consider introducing update API flow.
  } else {
    // TODO: to disconnect all the blocked clients (pubsub, blpop etc)

    pool.AwaitFiberOnAll([&](util::ProactorBase* pb) { ServerState::tlocal()->is_master = false; });
  }

  replica_.swap(new_replica);

  // Flushing all the data after we marked this instance as replica.
  Transaction* transaction = cntx->transaction;
  transaction->Schedule();

  auto cb = [](Transaction* t, EngineShard* shard) {
    shard->db_slice().FlushDb(DbSlice::kDbAll);
    return OpStatus::OK;
  };
  transaction->Execute(std::move(cb), true);

  // Replica sends response in either case. No need to send response in this function.
  // It's a bit confusing but simpler.
  if (!replica_->Run(cntx)) {
    replica_.reset();
  }

  bool is_master = !replica_;
  pool.AwaitFiberOnAll(
      [&](util::ProactorBase* pb) { ServerState::tlocal()->is_master = is_master; });
}

void ServerFamily::ReplConf(CmdArgList args, ConnectionContext* cntx) {
  if (args.size() % 2 == 0)
    goto err;

  for (unsigned i = 1; i < args.size(); i += 2) {
    DCHECK_LT(i + 1, args.size());
    ToUpper(&args[i]);

    std::string_view cmd = ArgS(args, i);
    std::string_view arg = ArgS(args, i + 1);
    if (cmd == "CAPA") {
      if (arg == "dragonfly" && args.size() == 3 && i == 1) {
        uint32_t sid = dfly_cmd_->AllocateSyncSession();
        cntx->owner()->SetName(absl::StrCat("repl_ctrl_", sid));

        string sync_id = absl::StrCat("SYNC", sid);
        cntx->conn_state.repl_session_id = sid;

        // The response for 'capa dragonfly' is: <master_id> <sync_id> <numthreads>
        (*cntx)->StartArray(3);
        (*cntx)->SendSimpleString(master_id_);
        (*cntx)->SendSimpleString(sync_id);
        (*cntx)->SendLong(shard_set->pool()->size());
        return;
      }
    } else {
      VLOG(1) << cmd << " " << arg;
    }
  }

  (*cntx)->SendOk();
  return;

err:
  (*cntx)->SendError(kSyntaxErr);
}

void ServerFamily::Role(CmdArgList args, ConnectionContext* cntx) {
  (*cntx)->SendRaw("*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
}

void ServerFamily::Script(CmdArgList args, ConnectionContext* cntx) {
  args.remove_prefix(1);
  ToUpper(&args.front());

  script_mgr_->Run(std::move(args), cntx);
}

void ServerFamily::Sync(CmdArgList args, ConnectionContext* cntx) {
  SyncGeneric("", 0, cntx);
}

void ServerFamily::Psync(CmdArgList args, ConnectionContext* cntx) {
  SyncGeneric("?", 0, cntx);  // full sync, ignore the request.
}

void ServerFamily::LastSave(CmdArgList args, ConnectionContext* cntx) {
  time_t save_time;
  {
    lock_guard lk(save_mu_);
    save_time = last_save_info_->save_time;
  }
  (*cntx)->SendLong(save_time);
}

void ServerFamily::Latency(CmdArgList args, ConnectionContext* cntx) {
  ToUpper(&args[1]);
  string_view sub_cmd = ArgS(args, 1);

  if (sub_cmd == "LATEST") {
    return (*cntx)->StartArray(0);
  }

  LOG_FIRST_N(ERROR, 10) << "Subcommand " << sub_cmd << " not supported";
  (*cntx)->SendError(kSyntaxErr);
}

void ServerFamily::_Shutdown(CmdArgList args, ConnectionContext* cntx) {
  CHECK_NOTNULL(acceptor_)->Stop();
  (*cntx)->SendOk();
}

void ServerFamily::SyncGeneric(std::string_view repl_master_id, uint64_t offs,
                               ConnectionContext* cntx) {
  if (cntx->async_dispatch) {
    // SYNC is a special command that should not be sent in batch with other commands.
    // It should be the last command since afterwards the server just dumps the replication data.
    (*cntx)->SendError("Can not sync in pipeline mode");
    return;
  }

  cntx->replica_conn = true;
  ServerState::tl_connection_stats()->num_replicas += 1;
  // TBD.
}

void ServerFamily::Dfly(CmdArgList args, ConnectionContext* cntx) {
  dfly_cmd_->Run(args, cntx);
}

#define HFUNC(x) SetHandler(HandlerFunc(this, &ServerFamily::x))

void ServerFamily::Register(CommandRegistry* registry) {
  constexpr auto kReplicaOpts = CO::ADMIN | CO::GLOBAL_TRANS;
  constexpr auto kMemOpts = CO::LOADING | CO::READONLY | CO::FAST | CO::NOSCRIPT;

  *registry << CI{"AUTH", CO::NOSCRIPT | CO::FAST | CO::LOADING, -2, 0, 0, 0}.HFUNC(Auth)
            << CI{"BGSAVE", CO::ADMIN | CO::GLOBAL_TRANS, 1, 0, 0, 0}.HFUNC(Save)
            << CI{"CLIENT", CO::NOSCRIPT | CO::LOADING, -2, 0, 0, 0}.HFUNC(Client)
            << CI{"CONFIG", CO::ADMIN, -2, 0, 0, 0}.HFUNC(Config)
            << CI{"DBSIZE", CO::READONLY | CO::FAST | CO::LOADING, 1, 0, 0, 0}.HFUNC(DbSize)
            << CI{"DEBUG", CO::ADMIN | CO::LOADING, -2, 0, 0, 0}.HFUNC(Debug)
            << CI{"FLUSHDB", CO::WRITE | CO::GLOBAL_TRANS, 1, 0, 0, 0}.HFUNC(FlushDb)
            << CI{"FLUSHALL", CO::WRITE | CO::GLOBAL_TRANS, -1, 0, 0, 0}.HFUNC(FlushAll)
            << CI{"INFO", CO::LOADING, -1, 0, 0, 0}.HFUNC(Info)
            << CI{"HELLO", CO::LOADING, -1, 0, 0, 0}.HFUNC(Hello)
            << CI{"LASTSAVE", CO::LOADING | CO::FAST, 1, 0, 0, 0}.HFUNC(LastSave)
            << CI{"LATENCY", CO::NOSCRIPT | CO::LOADING | CO::FAST, -2, 0, 0, 0}.HFUNC(Latency)
            << CI{"MEMORY", kMemOpts, -2, 0, 0, 0}.HFUNC(Memory)
            << CI{"SAVE", CO::ADMIN | CO::GLOBAL_TRANS, -1, 0, 0, 0}.HFUNC(Save)
            << CI{"SHUTDOWN", CO::ADMIN | CO::NOSCRIPT | CO::LOADING, 1, 0, 0, 0}.HFUNC(_Shutdown)
            << CI{"SLAVEOF", kReplicaOpts, 3, 0, 0, 0}.HFUNC(ReplicaOf)
            << CI{"REPLICAOF", kReplicaOpts, 3, 0, 0, 0}.HFUNC(ReplicaOf)
            << CI{"REPLCONF", CO::ADMIN | CO::LOADING, -1, 0, 0, 0}.HFUNC(ReplConf)
            << CI{"ROLE", CO::LOADING | CO::FAST | CO::NOSCRIPT, 1, 0, 0, 0}.HFUNC(Role)
            // We won't support DF->REDIS replication for now, hence we do not need to support
            // these commands.
            // << CI{"SYNC", CO::ADMIN | CO::GLOBAL_TRANS, 1, 0, 0, 0}.HFUNC(Sync)
            // << CI{"PSYNC", CO::ADMIN | CO::GLOBAL_TRANS, 3, 0, 0, 0}.HFUNC(Psync)
            << CI{"SCRIPT", CO::NOSCRIPT, -2, 0, 0, 0}.HFUNC(Script)
            << CI{"DFLY", CO::ADMIN | CO::GLOBAL_TRANS, -2, 0, 0, 0}.HFUNC(Dfly);
}

}  // namespace dfly
