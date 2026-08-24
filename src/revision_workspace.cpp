#include "history/revision_workspace.hpp"
#include "history/encoding.hpp"

#include "history/ir.hpp"
#include "history/process.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace history {
namespace {

void git_ok(const ProcessOutput &result, std::string_view action) {
  if (result.exit_code != 0)
    throw std::runtime_error(std::string(action) + ": " + result.error);
}

std::string sparse_pattern(const std::string &path) {
  std::string result;
  if (!path.empty() && (path.front() == '!' || path.front() == '#'))
    result.push_back('\\');
  for (const char character : path) {
    if (character == '*' || character == '?' || character == '[' ||
        character == ']' || character == '\\')
      result.push_back('\\');
    result.push_back(character);
  }
  return result;
}

} // namespace

void to_json(nlohmann::json &value, const MaterializationManifest &manifest) {
  value = {{"files", manifest.files},
           {"closure_complete", manifest.closure_complete},
           {"evidence_gaps", manifest.evidence_gaps}};
}

void from_json(const nlohmann::json &value, MaterializationManifest &manifest) {
  manifest.files = value.value("files", std::vector<std::string>{});
  manifest.closure_complete = value.value("closure_complete", false);
  manifest.evidence_gaps =
      value.value("evidence_gaps", std::vector<std::string>{});
}

RevisionTreeIndex::RevisionTreeIndex(const std::filesystem::path &repository,
                                     std::string revision) {
  if (revision.empty() || revision.starts_with('-'))
    throw std::invalid_argument("invalid revision for tree index");
  const auto listed = run_process(
      {"git", "-C", path_to_utf8(repository), "ls-tree", "-lrz", revision});
  git_ok(listed, "index revision tree");
  std::size_t offset = 0;
  while (offset < listed.output.size()) {
    const auto end = listed.output.find('\0', offset);
    const auto record = listed.output.substr(
        offset, end == std::string::npos ? std::string::npos : end - offset);
    offset = end == std::string::npos ? listed.output.size() : end + 1;
    const auto tab = record.find('\t');
    if (tab == std::string::npos)
      continue;
    const auto metadata = record.substr(0, tab);
    const auto path = record.substr(tab + 1);
    require_utf8(path, "Git tree path");
    std::istringstream fields(metadata);
    std::string mode, type, id;
    std::uint64_t bytes{};
    if (!(fields >> mode >> type >> id >> bytes) || type != "blob")
      continue;
    Blob blob{id, bytes};
    blobs_.emplace(path, std::move(blob));
  }
}

std::optional<std::string>
RevisionTreeIndex::blob_at(const std::string &path) const {
  const auto found = blobs_.find(path);
  return found == blobs_.end() ? std::nullopt
                               : std::optional<std::string>(found->second.id);
}

std::optional<std::uint64_t>
RevisionTreeIndex::size_at(const std::string &path) const {
  const auto found = blobs_.find(path);
  return found == blobs_.end()
             ? std::nullopt
             : std::optional<std::uint64_t>(found->second.bytes);
}

std::uint64_t
RevisionTreeIndex::size_of(const std::set<std::string> &paths) const {
  std::uint64_t result = 0;
  for (const auto &path : paths)
    if (const auto size = size_at(path))
      result += *size;
  return result;
}

std::uint64_t RevisionTreeIndex::total_blob_bytes() const {
  std::uint64_t result = 0;
  for (const auto &[path, blob] : blobs_) {
    (void)path;
    result += blob.bytes;
  }
  return result;
}

RevisionWorkspacePool::Lease::Lease(RevisionWorkspacePool *owner,
                                    std::string key, std::filesystem::path path,
                                    bool full)
    : owner_(owner), key_(std::move(key)), path_(std::move(path)), full_(full) {
}

RevisionWorkspacePool::Lease::Lease(Lease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), key_(std::move(other.key_)),
      path_(std::move(other.path_)), full_(other.full_) {}

RevisionWorkspacePool::Lease &
RevisionWorkspacePool::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    key_ = std::move(other.key_);
    path_ = std::move(other.path_);
    full_ = other.full_;
  }
  return *this;
}

RevisionWorkspacePool::Lease::~Lease() { reset(); }

void RevisionWorkspacePool::Lease::reset() {
  if (owner_)
    try {
      owner_->release(key_);
    } catch (...) {
      // Destructors must not hide the extraction result. The pool destructor
      // makes another best-effort cleanup pass.
    }
  owner_ = nullptr;
}

RevisionWorkspacePool::RevisionWorkspacePool(std::filesystem::path root,
                                             WorkspaceLimits limits)
    : root_(std::move(root)), limits_(limits) {
  std::filesystem::create_directories(root_);
  if (limits_.max_revisions == 0)
    throw std::invalid_argument("workspace_max_revisions must be positive");
  if (limits_.max_bytes == 0) {
    const auto available = std::filesystem::space(root_).available;
    const auto usable = available > limits_.free_space_reserve_bytes
                            ? available - limits_.free_space_reserve_bytes
                            : 0;
    limits_.max_bytes =
        std::min<std::uint64_t>(10ULL * 1024ULL * 1024ULL * 1024ULL, usable);
  }
}

RevisionWorkspacePool::~RevisionWorkspacePool() {
  std::scoped_lock lock(mutex_);
  while (!entries_.empty()) {
    const auto key = entries_.begin()->first;
    try {
      remove_entry(key);
    } catch (...) {
      std::error_code ignored;
      const auto repository = entries_.begin()->second.repository;
      std::filesystem::remove_all(entries_.begin()->second.path, ignored);
      estimated_bytes_ -= entries_.begin()->second.estimated_bytes;
      entries_.erase(entries_.begin());
      try {
        run_process({"git", "-C", path_to_utf8(repository), "worktree", "prune"});
      } catch (...) {
      }
    }
  }
}

std::string RevisionWorkspacePool::normalize_path(const std::string &input) {
  if (input.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("materialization path contains a line break");
  const auto path = path_from_utf8(input).lexically_normal();
  if (path.empty() || path == "." || path.is_absolute() ||
      *path.begin() == "..")
    throw std::runtime_error(
        "materialization path must be repository-relative");
  return generic_path_to_utf8(path);
}

void RevisionWorkspacePool::evict_for(std::uint64_t additional_bytes,
                                      std::size_t additional_entries,
                                      const std::string &protected_key) {
  for (;;) {
    const bool bytes_ok =
        additional_bytes <= limits_.max_bytes &&
        estimated_bytes_ <= limits_.max_bytes - additional_bytes;
    const bool count_ok =
        entries_.size() + additional_entries <= limits_.max_revisions;
    const auto available = std::filesystem::space(root_).available;
    const bool space_ok =
        additional_bytes <= available &&
        available - additional_bytes >= limits_.free_space_reserve_bytes;
    if (bytes_ok && count_ok && space_ok)
      return;
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
      if (it->first != protected_key && it->second.leases == 0 &&
          (victim == entries_.end() ||
           it->second.last_used < victim->second.last_used))
        victim = it;
    if (victim == entries_.end())
      throw std::runtime_error(
          "disk_space_insufficient: revision workspace exceeds byte, count, "
          "or free-space reserve limit");
    remove_entry(victim->first);
  }
}

void RevisionWorkspacePool::remove_entry(const std::string &key) {
  const auto found = entries_.find(key);
  if (found == entries_.end())
    return;
  run_process({"git", "-C", path_to_utf8(found->second.repository), "worktree",
               "remove", "--force", path_to_utf8(found->second.path)});
  run_process(
      {"git", "-C", path_to_utf8(found->second.repository), "worktree", "prune"});
  estimated_bytes_ -= found->second.estimated_bytes;
  entries_.erase(found);
}

RevisionWorkspacePool::Lease RevisionWorkspacePool::acquire(
    const std::filesystem::path &repository, const std::string &revision,
    const MaterializationManifest &manifest, bool require_full) {
  std::scoped_lock lock(mutex_);
  const auto canonical = std::filesystem::weakly_canonical(repository);
  const auto key = stable_hash(generic_path_to_utf8(canonical) + "\n" + revision);
  std::set<std::string> requested;
  for (const auto &file : manifest.files)
    requested.insert(normalize_path(file));
  const bool full = require_full || !manifest.closure_complete;
  RevisionTreeIndex index(canonical, revision);
  auto found = entries_.find(key);
  if (found == entries_.end()) {
    const auto bytes =
        full ? index.total_blob_bytes() : index.size_of(requested);
    evict_for(bytes, 1);
    const auto path = root_ / key;
    run_process({"git", "-C", path_to_utf8(canonical), "worktree", "remove",
                 "--force", path_to_utf8(path)});
    std::vector<std::string> add = {"git",      "-C",  path_to_utf8(canonical),
                                    "worktree", "add", "--detach"};
    if (!full)
      add.push_back("--no-checkout");
    add.push_back(path_to_utf8(path));
    add.push_back(revision);
    git_ok(run_process(add), "materialize revision workspace");
    Entry entry{canonical, path, revision, requested,
                bytes,     1,    full,     std::chrono::steady_clock::now()};
    try {
      if (!full) {
        git_ok(run_process({"git", "-C", path_to_utf8(path), "sparse-checkout",
                            "init", "--no-cone"}),
               "initialize sparse revision workspace");
        std::string patterns;
        for (const auto &file : requested)
          patterns += sparse_pattern(file) + "\n";
        ProcessOptions options;
        options.working_directory = path;
        options.input = patterns;
        git_ok(run_process(
                   {"git", "sparse-checkout", "set", "--no-cone", "--stdin"},
                   options),
               "populate sparse revision workspace");
        git_ok(run_process({"git", "-C", path_to_utf8(path), "checkout", "--force",
                            revision}),
               "checkout sparse revision workspace");
      }
    } catch (...) {
      run_process({"git", "-C", path_to_utf8(canonical), "worktree", "remove",
                   "--force", path_to_utf8(path)});
      run_process({"git", "-C", path_to_utf8(canonical), "worktree", "prune"});
      throw;
    }
    estimated_bytes_ += bytes;
    entries_.emplace(key, std::move(entry));
    return Lease(this, key, path, full);
  }

  auto &entry = found->second;
  if (full && !entry.full) {
    const auto bytes = index.total_blob_bytes();
    const auto additional =
        bytes > entry.estimated_bytes ? bytes - entry.estimated_bytes : 0;
    evict_for(additional, 0, key);
    git_ok(run_process({"git", "-C", path_to_utf8(entry.path), "sparse-checkout",
                        "disable"}),
           "expand full revision workspace");
    estimated_bytes_ += additional;
    entry.estimated_bytes = bytes;
    entry.full = true;
  } else if (!entry.full) {
    requested.insert(entry.files.begin(), entry.files.end());
    const auto bytes = index.size_of(requested);
    const auto additional =
        bytes > entry.estimated_bytes ? bytes - entry.estimated_bytes : 0;
    evict_for(additional, 0, key);
    if (requested != entry.files) {
      std::string patterns;
      for (const auto &file : requested)
        patterns += sparse_pattern(file) + "\n";
      ProcessOptions options;
      options.working_directory = entry.path;
      options.input = patterns;
      git_ok(
          run_process({"git", "sparse-checkout", "set", "--no-cone", "--stdin"},
                      options),
          "expand sparse revision workspace");
      entry.files = std::move(requested);
      entry.estimated_bytes = bytes;
      estimated_bytes_ += additional;
    }
  }
  ++entry.leases;
  entry.last_used = std::chrono::steady_clock::now();
  return Lease(this, key, entry.path, entry.full);
}

void RevisionWorkspacePool::release(const std::string &key) {
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end() || found->second.leases == 0)
    return;
  --found->second.leases;
  found->second.last_used = std::chrono::steady_clock::now();
  if (found->second.full && found->second.leases == 0)
    remove_entry(key);
}

nlohmann::json RevisionWorkspacePool::status() const {
  std::scoped_lock lock(mutex_);
  std::size_t active = 0, full = 0;
  for (const auto &[key, entry] : entries_) {
    (void)key;
    active += entry.leases != 0;
    full += entry.full;
  }
  return {{"workspace_count", entries_.size()},
          {"active_workspaces", active},
          {"full_workspaces", full},
          {"estimated_bytes", estimated_bytes_},
          {"max_revisions", limits_.max_revisions},
          {"max_bytes", limits_.max_bytes},
          {"free_space_reserve_bytes", limits_.free_space_reserve_bytes}};
}

} // namespace history
