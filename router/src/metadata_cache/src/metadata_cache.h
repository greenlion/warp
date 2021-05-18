/*
  Copyright (c) 2016, 2021, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef METADATA_CACHE_METADATA_CACHE_INCLUDED
#define METADATA_CACHE_METADATA_CACHE_INCLUDED

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "gr_notifications_listener.h"
#include "metadata.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/stdx/monitor.h"
#include "mysql_router_thread.h"
#include "mysqlrouter/metadata_cache.h"

class ClusterMetadata;

/** @class MetadataCache
 *
 * The MetadataCache manages cached information fetched from the
 * MySQL Server.
 *
 */
class METADATA_API MetadataCache
    : public metadata_cache::ReplicasetStateNotifierInterface {
 public:
  /**
   * Initialize a connection to the MySQL Metadata server.
   *
   * @param router_id id of the router in the cluster metadata
   * @param cluster_specific_type_id id of the replication group
   * @param metadata_servers The servers that store the metadata
   * @param cluster_metadata metadata of the cluster
   * @param ttl The TTL of the cached data
   * @param auth_cache_ttl TTL of the rest users authentication data
   * @param auth_cache_refresh_interval Refresh rate of the rest users
   *        authentiction data
   * @param ssl_options SSL related options for connection
   * @param cluster_name The name of the desired cluster in the metadata server
   * @param thread_stack_size The maximum memory allocated for thread's stack
   * @param use_cluster_notifications Flag indicating if the metadata cache
   * should use GR notifications as an additional trigger for metadata refresh
   */
  MetadataCache(
      const unsigned router_id, const std::string &cluster_specific_type_id,
      const std::vector<mysql_harness::TCPAddress> &metadata_servers,
      std::shared_ptr<MetaData> cluster_metadata, std::chrono::milliseconds ttl,
      const std::chrono::milliseconds auth_cache_ttl,
      const std::chrono::milliseconds auth_cache_refresh_interval,
      const mysqlrouter::SSLOptions &ssl_options,
      const std::string &cluster_name,
      size_t thread_stack_size = mysql_harness::kDefaultStackSizeInKiloBytes,
      bool use_cluster_notifications = false);

  ~MetadataCache() override;

  /** @brief Starts the Metadata Cache
   *
   * Starts the Metadata Cache and launch thread.
   */
  void start();

  /** @brief Stops the Metadata Cache
   *
   * Stops the Metadata Cache and the launch thread.
   */
  void stop() noexcept;

  using metadata_servers_list_t = std::vector<metadata_cache::ManagedInstance>;

  /** @brief Returns list of managed servers in a replicaset
   *
   * Returns list of managed servers in a replicaset.
   *
   * @param replicaset_name The ID of the replicaset being looked up
   * @return std::vector containing ManagedInstance objects
   */
  metadata_servers_list_t replicaset_lookup(const std::string &replicaset_name);

  /** @brief Update the status of the instance
   *
   * Called when an instance from a replicaset cannot be reached for one reason
   * or another. When an instance becomes unreachable, an emergency mode is set
   * (the rate of refresh of the metadata cache increases to once per second if
   * currently lower) and lasts until disabled after a suitable change in the
   * metadata cache is discovered.
   *
   * @param instance_id - the mysql_server_uuid that identifies the server
   * instance
   * @param status - the status of the instance
   */
  void mark_instance_reachability(const std::string &instance_id,
                                  metadata_cache::InstanceStatus status);

  /** Wait until PRIMARY changes in a replicaset.
   *
   * wait until a change of the PRIMARY is noticed
   *
   * leave early if
   *
   * - 'timeout' expires
   * - process shutdown is requested
   *
   * function has to handle two scenarios:
   *
   * connection to PRIMARY fails because:
   *
   * 1. PRIMARY died and group relects a new member
   * 2. network to PRIMARY lost, but GR sees no fault and PRIMARY does not
   * change.
   *
   * Therefore, if the connection to PRIMARY fails, wait for change of the
   * membership or timeout, whatever happens earlier.
   *
   * @param replicaset_name name of the replicaset
   * @param server_uuid server-uuid of the PRIMARY that we failed to connect
   * @param timeout - amount of time to wait for a failover
   * @return true if a primary member exists
   */
  bool wait_primary_failover(const std::string &replicaset_name,
                             const std::string &server_uuid,
                             const std::chrono::seconds &timeout);

  /** @brief refresh replicaset information */
  void refresh_thread();

  /** @brief run refresh thread */
  static void *run_thread(void *context);

  /**
   * @brief Register observer that is notified when there is a change in the
   * replicaset nodes setup/state discovered.
   *
   * @param replicaset_name name of the replicaset
   * @param listener Observer object that is notified when replicaset nodes
   * state is changed.
   */
  void add_state_listener(
      const std::string &replicaset_name,
      metadata_cache::ReplicasetStateListenerInterface *listener) override;

  /**
   * @brief Unregister observer previously registered with add_state_listener()
   *
   * @param replicaset_name name of the replicaset
   * @param listener Observer object that should be unregistered.
   */
  void remove_state_listener(
      const std::string &replicaset_name,
      metadata_cache::ReplicasetStateListenerInterface *listener) override;

  /**
   * @brief Register observer that is notified when the state of listening
   * socket acceptors should be updated on the next metadata refresh.
   *
   * @param replicaset_name name of the replicaset
   * @param listener Observer object that is notified when replicaset nodes
   * state is changed.
   */
  void add_acceptor_handler_listener(
      const std::string &replicaset_name,
      metadata_cache::AcceptorUpdateHandlerInterface *listener);

  /**
   * @brief Unregister observer previously registered with
   * add_acceptor_handler_listener()
   *
   * @param replicaset_name name of the replicaset
   * @param listener Observer object that should be unregistered.
   */
  void remove_acceptor_handler_listener(
      const std::string &replicaset_name,
      metadata_cache::AcceptorUpdateHandlerInterface *listener);

  metadata_cache::MetadataCacheAPIBase::RefreshStatus refresh_status() {
    return stats_([](auto const &stats)
                      -> metadata_cache::MetadataCacheAPIBase::RefreshStatus {
      return {stats.refresh_failed,
              stats.refresh_succeeded,
              stats.last_refresh_succeeded,
              stats.last_refresh_failed,
              stats.last_metadata_server_host,
              stats.last_metadata_server_port};
    });
  }

  std::string cluster_type_specific_id() const {
    return cluster_type_specific_id_;
  }
  std::chrono::milliseconds ttl() const { return ttl_; }
  std::string cluster_name() const { return cluster_name_; }

  virtual mysqlrouter::ClusterType cluster_type() const noexcept = 0;

  std::vector<mysql_harness::TCPAddress> metadata_servers();

  void enable_fetch_auth_metadata() { auth_metadata_fetch_enabled_ = true; }

  void force_cache_update() { on_refresh_requested(); }

  void check_auth_metadata_timers() const;

  std::pair<bool, MetaData::auth_credentials_t::mapped_type>
  get_rest_user_auth_data(const std::string &user);

  /**
   * Toggle socket acceptors state update on next metadata refresh.
   */
  void handle_sockets_acceptors_on_md_refresh() {
    trigger_acceptor_update_on_next_refresh_ = true;
  }

 protected:
  /** @brief Refreshes the cache
   *
   */
  virtual bool refresh() = 0;

  void on_refresh_failed(bool terminated);
  void on_refresh_succeeded(
      const metadata_cache::ManagedInstance &metadata_server);

  // Called each time the metadata has changed and we need to notify
  // the subscribed observers
  void on_instances_changed(const bool md_servers_reachable,
                            unsigned view_id = 0);

  /**
   * Called when the listening sockets acceptors state should be updated but
   * replicaset instances has not changed (in that case socket acceptors would
   * be handled when calling on_instances_changed).
   */
  void on_handle_sockets_acceptors();

  // Called each time we were requested to refresh the metadata
  void on_refresh_requested();

  // Called each time the metadata refresh completed execution
  void on_refresh_completed();

  // Update rest users authentication data
  bool update_auth_cache();

  // Stores the list replicasets and their server instances.
  // Keyed by replicaset name
  std::map<std::string, metadata_cache::ManagedReplicaSet> replicaset_data_;

  // The name of the cluster in the topology.
  std::string cluster_name_;

  // For GR cluster Group Replication ID, for AR cluster cluster_id from the
  // metadata
  const std::string cluster_type_specific_id_;

  // The list of servers that contain the metadata about the managed
  // topology.
  metadata_servers_list_t metadata_servers_;

  // The time to live of the metadata cache.
  std::chrono::milliseconds ttl_;

  // Time to live of the auth credentials cache.
  std::chrono::milliseconds auth_cache_ttl_;

  // Auth credentials cache refresh interval
  std::chrono::milliseconds auth_cache_refresh_interval_;

  // SSL options for MySQL connections
  mysqlrouter::SSLOptions ssl_options_;

  // id of the Router in the cluster metadata
  unsigned router_id_;

  struct RestAuthData {
    // Authentication data for the rest users
    MetaData::auth_credentials_t rest_auth_data_;

    std::chrono::system_clock::time_point last_credentials_update_;
  };

  Monitor<RestAuthData> rest_auth_{{}};

  // Authentication data should be fetched only when metadata_cache is used as
  // an authentication backend
  bool auth_metadata_fetch_enabled_{false};

  // Stores the pointer to the transport layer implementation. The transport
  // layer communicates with the servers storing the metadata and fetches the
  // topology information.
  std::shared_ptr<MetaData> meta_data_;

  /** @brief refresh thread facade */
  mysql_harness::MySQLRouterThread refresh_thread_;

  /** @brief notification thread facade */
  mysql_harness::MySQLRouterThread notification_thread_;

  // This mutex is used to ensure that a lookup of the metadata is consistent
  // with the changes in the metadata due to a cache refresh.
  std::mutex cache_refreshing_mutex_;

  // This mutex ensures that a refresh of the servers that contain the metadata
  // is consistent with the use of the server list.
  std::mutex metadata_servers_mutex_;

  // Contains a set of replicaset names that have at least one unreachable
  // (primary or secondary) node appearing in the routing table
  std::set<std::string> replicasets_with_unreachable_nodes_;

  std::mutex replicasets_with_unreachable_nodes_mtx_;

  // Flag used to terminate the refresh thread.
  std::atomic<bool> terminated_{false};

  bool refresh_requested_{false};

  bool use_cluster_notifications_;

  std::condition_variable refresh_wait_;
  std::mutex refresh_wait_mtx_;

  std::condition_variable refresh_completed_;
  std::mutex refresh_completed_mtx_;

  // map of lists (per each replicaset name) of registered callbacks to be
  // called on selected replicaset instances change event
  std::mutex replicaset_instances_change_callbacks_mtx_;

  std::mutex acceptor_handler_callbacks_mtx_;

  std::map<std::string,
           std::set<metadata_cache::ReplicasetStateListenerInterface *>>
      state_listeners_;

  std::map<std::string,
           std::set<metadata_cache::AcceptorUpdateHandlerInterface *>>
      acceptor_update_listeners_;

  struct Stats {
    std::chrono::system_clock::time_point last_refresh_failed;
    std::chrono::system_clock::time_point last_refresh_succeeded;
    uint64_t refresh_failed{0};
    uint64_t refresh_succeeded{0};

    std::string last_metadata_server_host;
    uint16_t last_metadata_server_port;
  };

  Monitor<Stats> stats_{{}};

  bool version_updated_{false};
  unsigned last_check_in_updated_{0};

  bool ready_announced_{false};

  /**
   * Flag indicating if socket acceptors state should be updated on next
   * metadata refresh even if instance information has not changed.
   */
  std::atomic<bool> trigger_acceptor_update_on_next_refresh_{false};
};

bool operator==(const MetaData::ReplicaSetsByName &map_a,
                const MetaData::ReplicaSetsByName &map_b);

bool operator!=(const MetaData::ReplicaSetsByName &map_a,
                const MetaData::ReplicaSetsByName &map_b);

std::string to_string(metadata_cache::ServerMode mode);

/** Gets user readable information string about the nodes atributes
 * related to _hidden and _disconnect_existing_sessions_when_hidden tags.
 */
std::string get_hidden_info(const metadata_cache::ManagedInstance &instance);

#endif  // METADATA_CACHE_METADATA_CACHE_INCLUDED
