#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace history {

struct MaterializationManifest {
  std::vector<std::string> files;
  bool closure_complete{};
  std::vector<std::string> evidence_gaps;
};

void to_json(nlohmann::json &, const MaterializationManifest &);
void from_json(const nlohmann::json &, MaterializationManifest &);

class RevisionTreeIndex {
public:
  RevisionTreeIndex(const std::filesystem::path &repository,
                    std::string revision);

  std::optional<std::string> blob_at(const std::string &path) const;
  std::optional<std::uint64_t> size_at(const std::string &path) const;
  std::uint64_t size_of(const std::set<std::string> &paths) const;
  std::uint64_t total_blob_bytes() const;

private:
  struct Blob {
    std::string id;
    std::uint64_t bytes{};
  };
  std::map<std::string, Blob> blobs_;
};

struct WorkspaceLimits {
  std::size_t max_revisions{2};
  std::uint64_t max_bytes{};
  std::uint64_t free_space_reserve_bytes{5ULL * 1024ULL * 1024ULL * 1024ULL};
};

class RevisionWorkspacePool {
public:
  class Lease {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) noexcept;
    ~Lease();

    const std::filesystem::path &path() const { return path_; }
    bool full() const { return full_; }
    explicit operator bool() const { return owner_ != nullptr; }

  private:
    friend class RevisionWorkspacePool;
    Lease(RevisionWorkspacePool *, std::string, std::filesystem::path, bool);
    void reset();
    RevisionWorkspacePool *owner_{};
    std::string key_;
    std::filesystem::path path_;
    bool full_{};
  };

  RevisionWorkspacePool(std::filesystem::path root,
                        WorkspaceLimits limits = {});
  RevisionWorkspacePool(const RevisionWorkspacePool &) = delete;
  RevisionWorkspacePool &operator=(const RevisionWorkspacePool &) = delete;
  ~RevisionWorkspacePool();

  Lease acquire(const std::filesystem::path &repository,
                const std::string &revision,
                const MaterializationManifest &manifest,
                bool require_full = false);
  nlohmann::json status() const;

private:
  struct Entry {
    std::filesystem::path repository, path;
    std::string revision;
    std::set<std::string> files;
    std::uint64_t estimated_bytes{};
    std::size_t leases{};
    bool full{};
    std::chrono::steady_clock::time_point last_used;
  };

  void release(const std::string &key);
  void remove_entry(const std::string &key);
  void evict_for(std::uint64_t additional_bytes, std::size_t additional_entries,
                 const std::string &protected_key = {});
  static std::string normalize_path(const std::string &path);

  std::filesystem::path root_;
  WorkspaceLimits limits_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::uint64_t estimated_bytes_{};
};

} // namespace history
