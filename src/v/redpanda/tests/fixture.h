#pragma once
#include "config/configuration.h"
#include "kafka/client.h"
#include "kafka/default_namespace.h"
#include "redpanda/application.h"
#include "storage/directories.h"
#include "storage/tests/log_builder.h"
#include "storage/tests/random_batch.h"
#include "test_utils/fixture.h"
#include "test_utils/logs.h"

#include <seastar/util/log.hh>

#include <fmt/format.h>

#include <filesystem>

class redpanda_thread_fixture {
public:
    redpanda_thread_fixture() {
        app.initialize();
        configure();
        app.check_environment();
        app.configure_admin_server();
        app.wire_up_services();
        app.start();
    }

    ~redpanda_thread_fixture() {
        app.shutdown();
        std::filesystem::remove_all(data_dir);
    }

    config::configuration& lconf() { return config::shard_local_cfg(); }

    void configure() {
        data_dir = fmt::format("test_dir_{}", time(0));
        smp::invoke_on_all([this] {
            auto& config = config::shard_local_cfg();
            config.get("enable_admin_api").set_value(false);

            config.get("data_directory")
              .set_value(config::data_directory_path{.path = data_dir});

            config.get("node_id").set_value(model::node_id(1));

            std::vector<config::seed_server> seed_servers = {
              {model::node_id(1),
               socket_address(net::inet_address("127.0.0.1"), 33145)}};
            config.get("seed_servers").set_value(seed_servers);
        }).get0();
    }

    kafka::client make_kafka_client() {
        return kafka::client(rpc::base_client::configuration{
          .server_addr = config::shard_local_cfg().kafka_api(),
        });
    }

    /// Make a log builder that will flush to a specific topic partition.
    storage::log_builder
    make_tp_log_builder(model::topic topic, model::partition_id partition) {
        auto ntp = model::ntp{
          .ns = kafka::default_namespace(),
          .tp = model::topic_partition{
            .topic = model::topic(topic),
            .partition = model::partition_id(partition),
          },
        };
        return storage::log_builder(
          lconf().data_directory().as_sstring(), std::move(ntp));
    }

    future<> recover_ntp(const model::ntp& ntp) {
        cluster::partition_assignment as{
          .group = raft::group_id(1),
          .ntp = ntp,
          .replicas = {{model::node_id(lconf().node_id()), 0}},
        };
        return do_with(
          std::move(as), [this](cluster::partition_assignment& as) {
              return app.metadata_cache
                .invoke_on_all([&as](cluster::metadata_cache& mdc) {
                    mdc.add_topic(as.ntp.tp.topic);
                })
                .then([this, &as] {
                    return app.controller->recover_assignment(as);
                });
          });
    }

    model::ntp make_data() {
        auto topic_name = fmt::format("my_topic_{}", 0);

        auto batches = storage::test::make_random_batches(
          model::offset(0), 20, false);

        auto ntp = model::ntp{
          .ns = kafka::default_namespace(),
          .tp = model::topic_partition{.topic = model::topic(topic_name),
                                       .partition = model::partition_id(0)}};
        tests::persist_log_file(
          lconf().data_directory().as_sstring(), ntp, std::move(batches))
          .get();

        recover_ntp(ntp).get();

        return ntp;
    }

    application app;
    std::filesystem::path data_dir;
};
