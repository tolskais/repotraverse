#include "history/catalog.hpp"
#include "history/encoding.hpp"

#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>

#include <sqlite3.h>

#include "history/ir.hpp"

namespace history {
namespace {

void check(int code, sqlite3 *database, const char *action) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
    throw std::runtime_error(std::string(action) + ": " +
                             sqlite3_errmsg(database));
}

std::string load_or_create_producer(const std::filesystem::path &root) {
  const auto path = root / "producer-id";
  {
    std::ifstream input(path);
    std::string value;
    if (input >> value)
      return value;
  }
  std::random_device random;
  const auto now =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto value =
      stable_hash(std::to_string(now) + ":" + std::to_string(random()) + ":" +
                  std::to_string(random()));
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary);
    if (!output)
      throw std::runtime_error("cannot create producer identity");
    output << value << '\n';
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    std::ifstream input(path);
    std::string existing;
    if (input >> existing)
      return existing;
    throw std::runtime_error("cannot publish producer identity: " +
                             error.message());
  }
  return value;
}

} // namespace

Catalog::Catalog(std::filesystem::path root) : root_(std::move(root)) {
  if (root_.empty())
    throw std::invalid_argument("catalog root cannot be empty");
  std::filesystem::create_directories(root_);
  producer_id_ = load_or_create_producer(root_);
  const auto database_path = root_ / "catalog.sqlite3";
  const auto database_name = path_to_utf8(database_path);
  check(sqlite3_open_v2(database_name.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr),
        database_, "cannot open catalog database");
  execute("PRAGMA journal_mode=WAL;");
  execute("PRAGMA synchronous=NORMAL;");
  execute("PRAGMA foreign_keys=ON;");
  execute("PRAGMA busy_timeout=5000;");
  const auto catalog_version = integer_pragma("PRAGMA user_version;");
  if (catalog_version != 0 && catalog_version != kSchemaVersion)
    throw std::runtime_error(
        "catalog schema is incompatible; remove and rebuild the prototype catalog");
  if (catalog_version == 0 &&
      integer_pragma("SELECT count(*) FROM sqlite_master WHERE type='table' "
                     "AND name IN ('facts','scheduled_tasks','compile_contexts');") >
          0)
    throw std::runtime_error(
        "unversioned prototype catalog is unsupported; remove and rebuild it");
  execute(
      "CREATE TABLE IF NOT EXISTS observed_claims("
      "task_id TEXT PRIMARY KEY, ref_oid TEXT NOT NULL, producer_id TEXT NOT "
      "NULL,"
      "state TEXT NOT NULL, acquired_at INTEGER NOT NULL, expires_at INTEGER "
      "NOT NULL,"
      "heartbeat_sequence INTEGER NOT NULL, result_id TEXT NOT NULL DEFAULT '',"
      "observed_at INTEGER NOT NULL, lease_json TEXT NOT NULL);");
  execute("CREATE TABLE IF NOT EXISTS facts("
          "fact_id TEXT PRIMARY KEY, task_id TEXT NOT NULL, fact_json TEXT NOT "
          "NULL,"
          "source_commit TEXT NOT NULL);");
  execute("CREATE INDEX IF NOT EXISTS facts_by_task ON facts(task_id);");
  execute("CREATE TABLE IF NOT EXISTS imported_commits(commit_id TEXT PRIMARY "
          "KEY);");
  execute(
      "CREATE TABLE IF NOT EXISTS compile_contexts(context_id TEXT NOT NULL,"
      "configuration TEXT NOT NULL,revision TEXT NOT NULL,translation_unit "
      "TEXT NOT NULL,"
      "context_json TEXT NOT NULL,PRIMARY "
      "KEY(context_id,configuration,revision,translation_unit));");
  execute("CREATE INDEX IF NOT EXISTS contexts_by_tu ON "
          "compile_contexts(translation_unit,revision);");
  execute("CREATE TABLE IF NOT EXISTS compile_context_files("
          "context_id TEXT NOT NULL,configuration TEXT NOT NULL,"
          "revision TEXT NOT NULL,path TEXT NOT NULL,"
          "PRIMARY KEY(context_id,configuration,revision,path));");
  execute("CREATE INDEX IF NOT EXISTS contexts_by_file ON "
          "compile_context_files(path,revision);");
  execute("CREATE TABLE IF NOT EXISTS scheduled_tasks(task_id TEXT PRIMARY "
          "KEY,request_id TEXT NOT NULL,"
          "state TEXT NOT NULL,task_json TEXT NOT NULL,created_at INTEGER NOT "
          "NULL,attempt_count INTEGER NOT NULL DEFAULT 0,next_attempt_at "
          "INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT ''); ");
  execute("CREATE TABLE IF NOT EXISTS task_requests(task_id TEXT NOT NULL,"
          "request_id TEXT NOT NULL,request_json TEXT NOT NULL,"
          "PRIMARY KEY(task_id,request_id));");
  execute("CREATE TABLE IF NOT EXISTS published_tasks("
          "task_id TEXT PRIMARY KEY);");
  execute("CREATE TABLE IF NOT EXISTS request_jobs("
          "request_id TEXT PRIMARY KEY,request_json TEXT NOT NULL,state TEXT "
          "NOT NULL,progress_json TEXT NOT NULL,result_json TEXT NOT NULL,"
          "error_json TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at "
          "INTEGER NOT NULL);");
  execute("CREATE TABLE IF NOT EXISTS lineage_relations("
          "relation_id TEXT PRIMARY KEY,relation_json TEXT NOT NULL,"
          "review_state TEXT NOT NULL,updated_at INTEGER NOT NULL);");
  execute("CREATE TABLE IF NOT EXISTS submodule_revisions("
          "parent_repository_id TEXT NOT NULL,parent_revision TEXT NOT NULL,"
          "path TEXT NOT NULL,child_repository_id TEXT NOT NULL,child_revision "
          "TEXT NOT NULL,PRIMARY KEY(parent_repository_id,parent_revision,path));");
  execute("CREATE TABLE IF NOT EXISTS element_dependencies("
          "repository_id TEXT NOT NULL,revision TEXT NOT NULL,context_id TEXT "
          "NOT NULL,source_element_id TEXT NOT NULL,target_element_id TEXT NOT "
          "NULL,manifest_id TEXT NOT NULL,PRIMARY KEY(repository_id,revision,"
          "context_id,source_element_id,target_element_id,manifest_id));");
  execute("CREATE INDEX IF NOT EXISTS reverse_element_dependencies ON "
          "element_dependencies(repository_id,revision,target_element_id);");
  execute("UPDATE scheduled_tasks SET state='pending',next_attempt_at=0 WHERE "
          "state IN ('dispatching','processing');");
  execute("PRAGMA user_version=3;");
}

Catalog::~Catalog() {
  if (database_)
    sqlite3_close(database_);
}

void Catalog::execute(const char *sql) const {
  char *error = nullptr;
  const auto code = sqlite3_exec(database_, sql, nullptr, nullptr, &error);
  if (code != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(database_);
    sqlite3_free(error);
    throw std::runtime_error("catalog SQL failed: " + message);
  }
}

std::int64_t Catalog::integer_pragma(const char *sql) const {
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr), database_,
        "prepare catalog pragma");
  const auto code = sqlite3_step(statement);
  check(code, database_, "read catalog pragma");
  const auto result = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return result;
}

void Catalog::observe_claim(const std::string &task_id,
                            const std::string &ref_oid,
                            const nlohmann::json &lease) {
  std::scoped_lock lock(mutex_);
  static constexpr const char *sql =
      "INSERT INTO "
      "observed_claims(task_id,ref_oid,producer_id,state,acquired_at,expires_"
      "at,"
      "heartbeat_sequence,result_id,observed_at,lease_json) "
      "VALUES(?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(task_id) DO UPDATE SET ref_oid=excluded.ref_oid,"
      "producer_id=excluded.producer_id,state=excluded.state,acquired_at="
      "excluded.acquired_at,"
      "expires_at=excluded.expires_at,heartbeat_sequence=excluded.heartbeat_"
      "sequence,"
      "result_id=excluded.result_id,observed_at=excluded.observed_at,lease_"
      "json=excluded.lease_json";
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr), database_,
        "prepare claim");
  const auto serialized = lease.dump();
  const auto observed =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, ref_oid.c_str(), -1, SQLITE_TRANSIENT);
  const auto producer = lease.value("producer_id", std::string{});
  const auto state = lease.value("state", std::string{"processing"});
  const auto result = lease.value("result_id", std::string{});
  sqlite3_bind_text(statement, 3, producer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 4, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 5, lease.value("acquired_at", std::int64_t{}));
  sqlite3_bind_int64(statement, 6, lease.value("expires_at", std::int64_t{}));
  sqlite3_bind_int64(statement, 7,
                     lease.value("heartbeat_sequence", std::int64_t{}));
  sqlite3_bind_text(statement, 8, result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 9, observed);
  sqlite3_bind_text(statement, 10, serialized.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store claim");
}

void Catalog::remove_claim(const std::string &task_id) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "DELETE FROM observed_claims WHERE task_id=?", -1,
                           &statement, nullptr),
        database_, "prepare claim deletion");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "delete claim");
}

std::optional<nlohmann::json> Catalog::claim(const std::string &task_id) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT ref_oid,observed_at,lease_json FROM "
                           "observed_claims WHERE task_id=?",
                           -1, &statement, nullptr),
        database_, "prepare claim lookup");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  if (code == SQLITE_DONE) {
    sqlite3_finalize(statement);
    return std::nullopt;
  }
  check(code, database_, "read claim");
  auto value = nlohmann::json::parse(
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 2)));
  value["ref_oid"] =
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
  value["observed_at"] = sqlite3_column_int64(statement, 1);
  sqlite3_finalize(statement);
  return value;
}

nlohmann::json Catalog::claims() const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "SELECT lease_json,ref_oid,observed_at FROM observed_claims "
            "ORDER BY task_id",
            -1, &statement, nullptr),
        database_, "prepare claims");
  nlohmann::json rows = nlohmann::json::array();
  while (sqlite3_step(statement) == SQLITE_ROW) {
    auto value = nlohmann::json::parse(
        reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
    value["ref_oid"] =
        reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
    value["observed_at"] = sqlite3_column_int64(statement, 2);
    rows.push_back(std::move(value));
  }
  sqlite3_finalize(statement);
  return rows;
}

void Catalog::store_fact(const std::string &fact_id, const std::string &task_id,
                         const nlohmann::json &fact,
                         const std::string &source_commit) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT OR IGNORE INTO "
            "facts(fact_id,task_id,fact_json,source_commit) VALUES(?,?,?,?)",
            -1, &statement, nullptr),
        database_, "prepare fact");
  const auto serialized = fact.dump();
  sqlite3_bind_text(statement, 1, fact_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, serialized.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 4, source_commit.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store fact");
  const auto result = fact.value("result", nlohmann::json{});
  if (result.is_object() &&
      result.value("record_type", std::string{}) == "tu_manifest") {
    const auto manifest = result.get<TuManifest>();
    for (const auto &variant : manifest.variants)
      for (const auto &target : variant.referenced_element_ids) {
        sqlite3_stmt *dependency = nullptr;
        check(sqlite3_prepare_v2(
                  database_,
                  "INSERT OR IGNORE INTO element_dependencies("
                  "repository_id,revision,context_id,source_element_id,"
                  "target_element_id,manifest_id) VALUES(?,?,?,?,?,?)",
                  -1, &dependency, nullptr),
              database_, "prepare element dependency");
        sqlite3_bind_text(dependency, 1, manifest.repository_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(dependency, 2, manifest.source_revision.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(dependency, 3, manifest.context_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(dependency, 4, variant.element_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(dependency, 5, target.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(dependency, 6, manifest.manifest_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        const auto dependency_code = sqlite3_step(dependency);
        sqlite3_finalize(dependency);
        check(dependency_code, database_, "store element dependency");
      }
    for (const auto &expansion : manifest.macro_expansions) {
      if (expansion.containing_element_id.empty()) continue;
      sqlite3_stmt *dependency = nullptr;
      check(sqlite3_prepare_v2(
                database_,
                "INSERT OR IGNORE INTO element_dependencies("
                "repository_id,revision,context_id,source_element_id,"
                "target_element_id,manifest_id) VALUES(?,?,?,?,?,?)",
                -1, &dependency, nullptr),
            database_, "prepare macro dependency");
      sqlite3_bind_text(dependency, 1, manifest.repository_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(dependency, 2, manifest.source_revision.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(dependency, 3, manifest.context_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(dependency, 4,
                        expansion.containing_element_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(dependency, 5, expansion.macro_element_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(dependency, 6, manifest.manifest_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      const auto dependency_code = sqlite3_step(dependency);
      sqlite3_finalize(dependency);
      check(dependency_code, database_, "store macro dependency");
    }
  }
  statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "UPDATE scheduled_tasks SET state='completed' "
                           "WHERE task_id=?",
                           -1, &statement, nullptr),
        database_, "prepare completed task update");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto update_code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(update_code, database_, "mark completed task");
}

std::optional<nlohmann::json>
Catalog::fact_for_task(const std::string &task_id) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT fact_id,fact_json,source_commit FROM facts "
                           "WHERE task_id=? ORDER BY fact_id LIMIT 1",
                           -1, &statement, nullptr),
        database_, "prepare fact lookup");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return std::nullopt;
  }
  auto value = nlohmann::json::parse(
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 1)));
  value["fact_id"] =
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
  value["source_commit"] =
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 2));
  sqlite3_finalize(statement);
  return value;
}

bool Catalog::imported(const std::string &commit) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT 1 FROM imported_commits WHERE commit_id=?",
                           -1, &statement, nullptr),
        database_, "prepare imported lookup");
  sqlite3_bind_text(statement, 1, commit.c_str(), -1, SQLITE_TRANSIENT);
  const auto found = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return found;
}

void Catalog::mark_imported(const std::string &commit) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT OR IGNORE INTO imported_commits(commit_id) VALUES(?)", -1,
            &statement, nullptr),
        database_, "prepare imported commit");
  sqlite3_bind_text(statement, 1, commit.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store imported commit");
}

void Catalog::store_compile_context(const CompileContext &context) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "INSERT OR REPLACE INTO "
                           "compile_contexts(context_id,configuration,revision,"
                           "translation_unit,context_json) VALUES(?,?,?,?,?)",
                           -1, &s, nullptr),
        database_, "prepare compile context");
  const auto value = nlohmann::json(context).dump();
  sqlite3_bind_text(s, 1, context.context_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, context.configuration.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, context.source_revision.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 4, context.translation_unit.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 5, value.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(s);
  sqlite3_finalize(s);
  check(code, database_, "store compile context");
  const auto store_file = [&](const std::string &path) {
    sqlite3_stmt *file_statement = nullptr;
    check(sqlite3_prepare_v2(
              database_,
              "INSERT OR IGNORE INTO compile_context_files("
              "context_id,configuration,revision,path) VALUES(?,?,?,?)",
              -1, &file_statement, nullptr),
          database_, "prepare compile context file");
    sqlite3_bind_text(file_statement, 1, context.context_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(file_statement, 2, context.configuration.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(file_statement, 3, context.source_revision.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(file_statement, 4, path.c_str(), -1, SQLITE_TRANSIENT);
    const auto file_code = sqlite3_step(file_statement);
    sqlite3_finalize(file_statement);
    check(file_code, database_, "store compile context file");
  };
  store_file(context.translation_unit);
  for (const auto &path : context.project_files)
    store_file(path);
}

std::vector<CompileContext>
Catalog::compile_contexts(const std::string &tu,
                          const std::string &revision) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  const char *sql =
      revision.empty()
          ? "SELECT DISTINCT c.context_json FROM compile_contexts c "
            "JOIN compile_context_files f ON f.context_id=c.context_id "
            "AND f.configuration=c.configuration AND f.revision=c.revision "
            "WHERE f.path=? ORDER BY c.configuration,c.context_id"
          : "SELECT DISTINCT c.context_json FROM compile_contexts c "
            "JOIN compile_context_files f ON f.context_id=c.context_id "
            "AND f.configuration=c.configuration AND f.revision=c.revision "
            "WHERE f.path=? AND f.revision=? ORDER BY "
            "c.configuration,c.context_id";
  check(sqlite3_prepare_v2(database_, sql, -1, &s, nullptr), database_,
        "prepare context lookup");
  sqlite3_bind_text(s, 1, tu.c_str(), -1, SQLITE_TRANSIENT);
  if (!revision.empty())
    sqlite3_bind_text(s, 2, revision.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<CompileContext> result;
  while (sqlite3_step(s) == SQLITE_ROW)
    result.push_back(nlohmann::json::parse(reinterpret_cast<const char *>(
                                               sqlite3_column_text(s, 0)))
                         .get<CompileContext>());
  sqlite3_finalize(s);
  return result;
}

void Catalog::schedule_task(const std::string &task_id,
                            const nlohmann::json &task) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "INSERT OR IGNORE INTO "
                           "scheduled_tasks(task_id,request_id,state,task_json,"
                           "created_at) VALUES(?,?,'pending',?,?)",
                           -1, &s, nullptr),
        database_, "prepare task");
  const auto request = task.value("request_id", std::string{}),
             value = task.dump();
  sqlite3_bind_text(s, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, request.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(
      s, 4,
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  const auto code = sqlite3_step(s);
  sqlite3_finalize(s);
  check(code, database_, "schedule task");
  s = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "INSERT OR REPLACE INTO task_requests("
                           "task_id,request_id,request_json) VALUES(?,?,?)",
                           -1, &s, nullptr),
        database_, "prepare task request");
  sqlite3_bind_text(s, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, request.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, value.c_str(), -1, SQLITE_TRANSIENT);
  const auto request_code = sqlite3_step(s);
  sqlite3_finalize(s);
  check(request_code, database_, "store task request");
}

nlohmann::json Catalog::pending_tasks(const std::string &request) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  const char *sql =
      request.empty()
          ? "SELECT task_id,state,task_json FROM scheduled_tasks "
            "WHERE state!='completed' "
            "ORDER BY created_at,task_id"
          : "SELECT t.task_id,t.state,r.request_json FROM scheduled_tasks t "
            "JOIN task_requests r ON r.task_id=t.task_id "
            "WHERE r.request_id=? AND t.state!='completed' "
            "ORDER BY t.created_at,t.task_id";
  check(sqlite3_prepare_v2(database_, sql, -1, &s, nullptr), database_,
        "prepare pending tasks");
  if (!request.empty())
    sqlite3_bind_text(s, 1, request.c_str(), -1, SQLITE_TRANSIENT);
  nlohmann::json rows = nlohmann::json::array();
  while (sqlite3_step(s) == SQLITE_ROW) {
    auto value = nlohmann::json::parse(
        reinterpret_cast<const char *>(sqlite3_column_text(s, 2)));
    value["task_id"] =
        reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
    value["state"] = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
    rows.push_back(std::move(value));
  }
  sqlite3_finalize(s);
  return rows;
}

nlohmann::json Catalog::results_for_request(const std::string &request) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "SELECT r.request_json,f.fact_json FROM task_requests r "
            "JOIN facts f ON f.task_id=r.task_id WHERE r.request_id=? "
            "ORDER BY r.task_id,f.fact_id",
            -1, &statement, nullptr),
        database_, "prepare request results");
  sqlite3_bind_text(statement, 1, request.c_str(), -1, SQLITE_TRANSIENT);
  nlohmann::json rows = nlohmann::json::array();
  while (sqlite3_step(statement) == SQLITE_ROW) {
    rows.push_back(
        {{"task", nlohmann::json::parse(reinterpret_cast<const char *>(
                      sqlite3_column_text(statement, 0)))},
         {"fact", nlohmann::json::parse(reinterpret_cast<const char *>(
                      sqlite3_column_text(statement, 1)))}});
  }
  sqlite3_finalize(statement);
  return rows;
}

std::optional<nlohmann::json> Catalog::next_pending_task() const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "UPDATE scheduled_tasks SET state='dispatching' WHERE task_id=("
            "SELECT task_id FROM scheduled_tasks WHERE state IN "
            "('pending','waiting','transient_failure') AND next_attempt_at<="
            "strftime('%s','now') AND EXISTS(SELECT 1 FROM published_tasks p "
            "WHERE p.task_id=scheduled_tasks.task_id) ORDER BY "
            "next_attempt_at,created_at,task_id "
            "LIMIT 1) RETURNING task_id,task_json,attempt_count",
            -1, &s, nullptr),
        database_, "prepare next task");
  if (sqlite3_step(s) != SQLITE_ROW) {
    sqlite3_finalize(s);
    return std::nullopt;
  }
  auto value = nlohmann::json::parse(
      reinterpret_cast<const char *>(sqlite3_column_text(s, 1)));
  value["task_id"] = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
  value["attempt_count"] = sqlite3_column_int64(s, 2);
  sqlite3_finalize(s);
  return value;
}

nlohmann::json Catalog::fail_task(const std::string &task_id,
                                  const std::string &diagnostic,
                                  std::uint32_t maximum_attempts) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "UPDATE scheduled_tasks SET attempt_count=attempt_count+1,"
            "state=CASE WHEN attempt_count+1>=? THEN 'quarantined' ELSE "
            "'transient_failure' END,last_error=?,next_attempt_at="
            "strftime('%s','now') + min(300,(1 << min(attempt_count,8))),"
            "created_at=strftime('%s','now') WHERE task_id=? RETURNING "
            "attempt_count,state,next_attempt_at",
            -1, &statement, nullptr),
        database_, "prepare failed task update");
  sqlite3_bind_int(statement, 1, static_cast<int>(maximum_attempts));
  sqlite3_bind_text(statement, 2, diagnostic.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  check(code, database_, "update failed task");
  nlohmann::json result = {{"attempt_count", sqlite3_column_int(statement, 0)},
                           {"state", reinterpret_cast<const char *>(
                                         sqlite3_column_text(statement, 1))},
                           {"next_attempt_at", sqlite3_column_int64(statement, 2)}};
  sqlite3_finalize(statement);
  return result;
}

void Catalog::retry_task(const std::string &task_id) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "UPDATE scheduled_tasks SET state='pending',attempt_count=0,"
            "next_attempt_at=0,last_error='' WHERE task_id=? AND state IN "
            "('quarantined','incompatible_worker','cancelled')",
            -1, &statement, nullptr),
        database_, "prepare task retry");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "retry task");
}

void Catalog::cancel_request_tasks(const std::string &request_id) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "UPDATE scheduled_tasks SET state='cancelled' WHERE task_id IN "
            "(SELECT task_id FROM task_requests WHERE request_id=?) AND state "
            "IN ('pending','waiting','transient_failure','dispatching')",
            -1, &statement, nullptr),
        database_, "prepare request cancellation");
  sqlite3_bind_text(statement, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "cancel request tasks");
}

std::string Catalog::create_request(const nlohmann::json &request) {
  const auto serialized = request.dump();
  const auto request_id = stable_hash(serialized);
  const auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT OR IGNORE INTO request_jobs(request_id,request_json,state,"
            "progress_json,result_json,error_json,created_at,updated_at) "
            "VALUES(?,?,'queued','{}','{}','{}',?,?)",
            -1, &statement, nullptr),
        database_, "prepare request job");
  sqlite3_bind_text(statement, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, serialized.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 3, now);
  sqlite3_bind_int64(statement, 4, now);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "create request job");
  return request_id;
}

std::optional<nlohmann::json>
Catalog::request_job(const std::string &request_id) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "SELECT request_json,state,progress_json,result_json,error_json,"
            "created_at,updated_at FROM request_jobs WHERE request_id=?",
            -1, &statement, nullptr),
        database_, "prepare request job lookup");
  sqlite3_bind_text(statement, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return std::nullopt;
  }
  nlohmann::json result = {
      {"request_id", request_id},
      {"request", nlohmann::json::parse(reinterpret_cast<const char *>(
                      sqlite3_column_text(statement, 0)))},
      {"state", reinterpret_cast<const char *>(sqlite3_column_text(statement, 1))},
      {"progress", nlohmann::json::parse(reinterpret_cast<const char *>(
                       sqlite3_column_text(statement, 2)))},
      {"result", nlohmann::json::parse(reinterpret_cast<const char *>(
                     sqlite3_column_text(statement, 3)))},
      {"error", nlohmann::json::parse(reinterpret_cast<const char *>(
                    sqlite3_column_text(statement, 4)))},
      {"created_at", sqlite3_column_int64(statement, 5)},
      {"updated_at", sqlite3_column_int64(statement, 6)}};
  sqlite3_finalize(statement);
  return result;
}

void Catalog::update_request(const std::string &request_id,
                             const std::string &state,
                             const nlohmann::json &progress,
                             const nlohmann::json &result,
                             const nlohmann::json &error) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "UPDATE request_jobs SET state=?,progress_json=?,result_json=?,"
            "error_json=?,updated_at=strftime('%s','now') WHERE request_id=?",
            -1, &statement, nullptr),
        database_, "prepare request job update");
  const auto progress_json = progress.dump(), result_json = result.dump(),
             error_json = error.dump();
  sqlite3_bind_text(statement, 1, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, progress_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 4, error_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 5, request_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "update request job");
}

void Catalog::store_lineage_relation(const LineageRelation &relation) {
  if (relation.relation_id.empty())
    throw std::invalid_argument("lineage relation requires relation_id");
  const auto serialized = nlohmann::json(relation).dump();
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT INTO lineage_relations(relation_id,relation_json,"
            "review_state,updated_at) VALUES(?,?,?,strftime('%s','now')) ON "
            "CONFLICT(relation_id) DO UPDATE SET relation_json=excluded."
            "relation_json,review_state=excluded.review_state,updated_at="
            "excluded.updated_at",
            -1, &statement, nullptr),
        database_, "prepare lineage relation");
  sqlite3_bind_text(statement, 1, relation.relation_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, serialized.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, relation.review_state.c_str(), -1,
                    SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store lineage relation");
}

std::optional<LineageRelation>
Catalog::lineage_relation(const std::string &relation_id) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT relation_json FROM lineage_relations WHERE "
                           "relation_id=?",
                           -1, &statement, nullptr),
        database_, "prepare lineage relation lookup");
  sqlite3_bind_text(statement, 1, relation_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return std::nullopt;
  }
  auto relation = nlohmann::json::parse(reinterpret_cast<const char *>(
                      sqlite3_column_text(statement, 0)))
                      .get<LineageRelation>();
  sqlite3_finalize(statement);
  return relation;
}

void Catalog::store_submodule_revision(const SubmoduleRevision &revision) {
  if (revision.parent_repository_id.empty() || revision.parent_revision.empty() ||
      revision.path.empty() || revision.child_repository_id.empty() ||
      revision.child_revision.empty() ||
      revision.parent_repository_id == revision.child_repository_id)
    throw std::invalid_argument("invalid submodule revision mapping");
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT OR REPLACE INTO submodule_revisions("
            "parent_repository_id,parent_revision,path,child_repository_id,"
            "child_revision) VALUES(?,?,?,?,?)",
            -1, &statement, nullptr),
        database_, "prepare submodule revision");
  sqlite3_bind_text(statement, 1, revision.parent_repository_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, revision.parent_revision.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 3, revision.path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 4, revision.child_repository_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 5, revision.child_revision.c_str(), -1,
                    SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store submodule revision");
}

nlohmann::json Catalog::submodule_revisions(
    const std::string &parent_repository_id,
    const std::string &parent_revision) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "SELECT path,child_repository_id,child_revision FROM "
            "submodule_revisions WHERE parent_repository_id=? AND "
            "parent_revision=? ORDER BY path",
            -1, &statement, nullptr),
        database_, "prepare submodule revisions lookup");
  sqlite3_bind_text(statement, 1, parent_repository_id.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(statement, 2, parent_revision.c_str(), -1,
                    SQLITE_TRANSIENT);
  nlohmann::json result = nlohmann::json::array();
  while (sqlite3_step(statement) == SQLITE_ROW)
    result.push_back({
        {"parent_repository_id", parent_repository_id},
        {"parent_revision", parent_revision},
        {"path", reinterpret_cast<const char *>(sqlite3_column_text(statement, 0))},
        {"child_repository_id",
         reinterpret_cast<const char *>(sqlite3_column_text(statement, 1))},
        {"child_revision",
         reinterpret_cast<const char *>(sqlite3_column_text(statement, 2))}});
  sqlite3_finalize(statement);
  return result;
}

nlohmann::json Catalog::semantic_dependents(
    const std::string &repository_id, const std::string &revision,
    const std::vector<std::string> &element_ids, std::size_t maximum) const {
  std::scoped_lock lock(mutex_);
  std::set<std::string> visited(element_ids.begin(), element_ids.end());
  std::vector<std::string> frontier(element_ids.begin(), element_ids.end());
  nlohmann::json rows = nlohmann::json::array();
  bool truncated = false;
  while (!frontier.empty() && !truncated) {
    const auto target = std::move(frontier.back());
    frontier.pop_back();
    sqlite3_stmt *statement = nullptr;
    check(sqlite3_prepare_v2(
              database_,
              "SELECT DISTINCT source_element_id,context_id,manifest_id FROM "
              "element_dependencies WHERE repository_id=? AND revision=? AND "
              "target_element_id=? ORDER BY source_element_id,context_id",
              -1, &statement, nullptr),
          database_, "prepare semantic dependents");
    sqlite3_bind_text(statement, 1, repository_id.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, revision.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, target.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const std::string source = reinterpret_cast<const char *>(
          sqlite3_column_text(statement, 0));
      if (!visited.insert(source).second) continue;
      rows.push_back({
          {"element_id", source},
          {"depends_on", target},
          {"context_id", reinterpret_cast<const char *>(
                             sqlite3_column_text(statement, 1))},
          {"manifest_id", reinterpret_cast<const char *>(
                              sqlite3_column_text(statement, 2))}});
      frontier.push_back(source);
      if (rows.size() >= maximum) {
        truncated = true;
        break;
      }
    }
    sqlite3_finalize(statement);
  }
  return {{"dependents", rows}, {"truncated", truncated}};
}

void Catalog::set_task_state(const std::string &task_id,
                             const std::string &state) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *s = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "UPDATE scheduled_tasks SET state=?,created_at=? "
                           "WHERE task_id=?",
                           -1, &s, nullptr),
        database_, "prepare task state");
  sqlite3_bind_text(s, 1, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(
      s, 2,
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  sqlite3_bind_text(s, 3, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(s);
  sqlite3_finalize(s);
  check(code, database_, "update task state");
}

bool Catalog::task_published(const std::string &task_id) const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT 1 FROM published_tasks WHERE task_id=?", -1,
                           &statement, nullptr),
        database_, "prepare published task lookup");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto found = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return found;
}

void Catalog::mark_task_published(const std::string &task_id) {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(
            database_,
            "INSERT OR IGNORE INTO published_tasks(task_id) VALUES(?)", -1,
            &statement, nullptr),
        database_, "prepare published task");
  sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const auto code = sqlite3_step(statement);
  sqlite3_finalize(statement);
  check(code, database_, "store published task");
}

std::string Catalog::snapshot_id() const {
  std::scoped_lock lock(mutex_);
  sqlite3_stmt *statement = nullptr;
  check(sqlite3_prepare_v2(database_,
                           "SELECT fact_id FROM facts ORDER BY fact_id", -1,
                           &statement, nullptr),
        database_, "prepare snapshot");
  std::string input;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    input += reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    input.push_back('\n');
  }
  sqlite3_finalize(statement);
  return stable_hash(input);
}

} // namespace history
