#include "xfcc_integration_test.h"

#include <regex>

#include "common/event/dispatcher_impl.h"
#include "common/http/header_map_impl.h"
#include "common/network/utility.h"
#include "common/ssl/context_config_impl.h"
#include "common/ssl/context_manager_impl.h"

#include "test/test_common/network_utility.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "api/filter/network/http_connection_manager.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "integration.h"
#include "ssl_integration_test.h"
#include "utility.h"

namespace Envoy {
namespace Xfcc {

void XfccIntegrationTest::TearDown() {
  test_server_.reset();
  client_mtls_ssl_ctx_.reset();
  client_tls_ssl_ctx_.reset();
  fake_upstreams_.clear();
  upstream_ssl_ctx_.reset();
  context_manager_.reset();
  runtime_.reset();
}

Ssl::ClientContextPtr XfccIntegrationTest::createClientSslContext(bool mtls) {
  std::string json_tls = R"EOF(
{
  "ca_cert_file": "{{ test_rundir }}/test/config/integration/certs/cacert.pem",
  "verify_subject_alt_name": [ "spiffe://lyft.com/backend-team" ]
}
)EOF";
  std::string json_mtls = R"EOF(
{
  "ca_cert_file": "{{ test_rundir }}/test/config/integration/certs/cacert.pem",
  "cert_chain_file": "{{ test_rundir }}/test/config/integration/certs/clientcert.pem",
  "private_key_file": "{{ test_rundir }}/test/config/integration/certs/clientkey.pem",
  "verify_subject_alt_name": [ "spiffe://lyft.com/backend-team" ]
}
)EOF";

  std::string target;
  if (mtls) {
    target = json_mtls;
  } else {
    target = json_tls;
  }
  Json::ObjectSharedPtr loader = TestEnvironment::jsonLoadFromString(target);
  Ssl::ClientContextConfigImpl cfg(*loader);
  static auto* client_stats_store = new Stats::TestIsolatedStoreImpl();
  return context_manager_->createSslClientContext(*client_stats_store, cfg);
}

Ssl::ServerContextPtr XfccIntegrationTest::createUpstreamSslContext() {
  std::string json = R"EOF(
{
  "cert_chain_file": "{{ test_rundir }}/test/config/integration/certs/upstreamcert.pem",
  "private_key_file": "{{ test_rundir }}/test/config/integration/certs/upstreamkey.pem"
}
)EOF";

  Json::ObjectSharedPtr loader = TestEnvironment::jsonLoadFromString(json);
  Ssl::ServerContextConfigImpl cfg(*loader);
  static auto* upstream_stats_store = new Stats::TestIsolatedStoreImpl();
  return context_manager_->createSslServerContext("", {}, *upstream_stats_store, cfg, true);
}

Network::ClientConnectionPtr XfccIntegrationTest::makeClientConnection() {
  Network::Address::InstanceConstSharedPtr address =
      Network::Utility::resolveUrl("tcp://" + Network::Test::getLoopbackAddressUrlString(version_) +
                                   ":" + std::to_string(lookupPort("http")));
  return dispatcher_->createClientConnection(address, Network::Address::InstanceConstSharedPtr());
}

Network::ClientConnectionPtr XfccIntegrationTest::makeMtlsClientConnection() {
  Network::Address::InstanceConstSharedPtr address =
      Network::Utility::resolveUrl("tcp://" + Network::Test::getLoopbackAddressUrlString(version_) +
                                   ":" + std::to_string(lookupPort("http")));
  return dispatcher_->createSslClientConnection(*client_mtls_ssl_ctx_, address,
                                                Network::Address::InstanceConstSharedPtr());
}

void XfccIntegrationTest::createUpstreams() {
  upstream_ssl_ctx_ = createUpstreamSslContext();
  fake_upstreams_.emplace_back(
      new FakeUpstream(upstream_ssl_ctx_.get(), 0, FakeHttpConnection::Type::HTTP1, version_));
}

void XfccIntegrationTest::initialize() {
  config_helper_.addConfigModifier(
      [&](envoy::api::v2::filter::network::HttpConnectionManager& hcm) -> void {
        hcm.set_forward_client_cert_details(fcc_);
        hcm.mutable_set_current_client_cert_details()->CopyFrom(sccd_);
      });

  config_helper_.addConfigModifier([&](envoy::api::v2::Bootstrap& bootstrap) -> void {
    auto context = bootstrap.mutable_static_resources()->mutable_clusters(0)->mutable_tls_context();
    auto* validation_context = context->mutable_common_tls_context()->mutable_validation_context();
    validation_context->mutable_trusted_ca()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/upstreamcacert.pem"));
    validation_context->add_verify_subject_alt_name("foo.lyft.com");
  });

  if (tls_) {
    config_helper_.addSslConfig();
  }

  runtime_.reset(new NiceMock<Runtime::MockLoader>());
  context_manager_.reset(new Ssl::ContextManagerImpl(*runtime_));
  client_tls_ssl_ctx_ = createClientSslContext(false);
  client_mtls_ssl_ctx_ = createClientSslContext(true);
  HttpIntegrationTest::initialize();
}

void XfccIntegrationTest::testRequestAndResponseWithXfccHeader(std::string previous_xfcc,
                                                               std::string expected_xfcc) {
  Network::ClientConnectionPtr conn = tls_ ? makeMtlsClientConnection() : makeClientConnection();
  Http::TestHeaderMapImpl header_map;
  if (previous_xfcc.empty()) {
    header_map = Http::TestHeaderMapImpl{{":method", "GET"},
                                         {":path", "/test/long/url"},
                                         {":scheme", "http"},
                                         {":authority", "host"}};
  } else {
    header_map = Http::TestHeaderMapImpl{{":method", "GET"},
                                         {":path", "/test/long/url"},
                                         {":scheme", "http"},
                                         {":authority", "host"},
                                         {"x-forwarded-client-cert", previous_xfcc.c_str()}};
  }

  codec_client_ = makeHttpConnection(std::move(conn));
  codec_client_->makeHeaderOnlyRequest(header_map, *response_);
  fake_upstream_connection_ = fake_upstreams_[0]->waitForHttpConnection(*dispatcher_);
  upstream_request_ = fake_upstream_connection_->waitForNewStream(*dispatcher_);
  upstream_request_->waitForEndStream(*dispatcher_);
  if (expected_xfcc.empty()) {
    EXPECT_EQ(nullptr, upstream_request_->headers().ForwardedClientCert());
  } else {
    EXPECT_STREQ(expected_xfcc.c_str(),
                 upstream_request_->headers().ForwardedClientCert()->value().c_str());
  }
  upstream_request_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}}, true);
  response_->waitForEndStream();
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_TRUE(response_->complete());
}

INSTANTIATE_TEST_CASE_P(IpVersions, XfccIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(XfccIntegrationTest, MtlsForwardOnly) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::FORWARD_ONLY;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, previous_xfcc_);
}

TEST_P(XfccIntegrationTest, MtlsAlwaysForwardOnly) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::ALWAYS_FORWARD_ONLY;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, previous_xfcc_);
}

TEST_P(XfccIntegrationTest, MtlsSanitize) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::SANITIZE;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, "");
}

TEST_P(XfccIntegrationTest, MtlsSanitizeSetSubjectSan) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::SANITIZE_SET;
  sccd_.mutable_subject()->set_value(true);
  sccd_.mutable_san()->set_value(true);
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, current_xfcc_by_hash_ + ";" +
                                                           client_subject_ + ";" + client_san_);
}

TEST_P(XfccIntegrationTest, MtlsAppendForward) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::APPEND_FORWARD;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_,
                                       previous_xfcc_ + "," + current_xfcc_by_hash_);
}

TEST_P(XfccIntegrationTest, MtlsAppendForwardSubject) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::APPEND_FORWARD;
  sccd_.mutable_subject()->set_value(true);
  initialize();
  testRequestAndResponseWithXfccHeader(
      previous_xfcc_, previous_xfcc_ + "," + current_xfcc_by_hash_ + ";" + client_subject_);
}

TEST_P(XfccIntegrationTest, MtlsAppendForwardSan) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::APPEND_FORWARD;
  sccd_.mutable_san()->set_value(true);
  initialize();
  testRequestAndResponseWithXfccHeader(
      previous_xfcc_, previous_xfcc_ + "," + current_xfcc_by_hash_ + ";" + client_san_);
}

TEST_P(XfccIntegrationTest, MtlsAppendForwardSubjectSan) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::APPEND_FORWARD;
  sccd_.mutable_subject()->set_value(true);
  sccd_.mutable_san()->set_value(true);
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, previous_xfcc_ + "," +
                                                           current_xfcc_by_hash_ + ";" +
                                                           client_subject_ + ";" + client_san_);
}

TEST_P(XfccIntegrationTest, MtlsAppendForwardSanPreviousXfccHeaderEmpty) {
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::APPEND_FORWARD;
  sccd_.mutable_san()->set_value(true);
  initialize();
  testRequestAndResponseWithXfccHeader("", current_xfcc_by_hash_ + ";" + client_san_);
}

TEST_P(XfccIntegrationTest, TlsAlwaysForwardOnly) {
  // The always_forward_only works regardless of whether the connection is TLS/mTLS.
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::ALWAYS_FORWARD_ONLY;
  tls_ = false;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, previous_xfcc_);
}

TEST_P(XfccIntegrationTest, TlsEnforceSanitize) {
  // The forward_only, append_forward and sanitize_set options are not effective when the connection
  // is not using Mtls.
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::FORWARD_ONLY;
  tls_ = false;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, "");
}

TEST_P(XfccIntegrationTest, NonTlsAlwaysForwardOnly) {
  // The always_forward_only works regardless of whether the connection is TLS/mTLS.
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::ALWAYS_FORWARD_ONLY;
  tls_ = false;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, previous_xfcc_);
}

TEST_P(XfccIntegrationTest, NonTlsEnforceSanitize) {
  // The forward_only, append_forward and sanitize_set options are not effective when the connection
  // is not using Mtls.
  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::FORWARD_ONLY;
  tls_ = false;
  initialize();
  testRequestAndResponseWithXfccHeader(previous_xfcc_, "");
}

TEST_P(XfccIntegrationTest, TagExtractedNameGenerationTest) {
  // Note: the test below is meant to check that default tags are being extracted correctly with
  // real-ish input stats. If new stats are added, this test will not break because names that do
  // not exist in the map are not checked. However, if stats are modified the below maps should be
  // updated (or regenerated by printing in map literal format). See commented code below to
  // regenerate the maps. Note: different maps are needed for ipv4 and ipv6, so when regenerating,
  // the printout needs to be copied from each test parameterization and pasted into the respective
  // case in the switch statement below.

  fcc_ = envoy::api::v2::filter::network::HttpConnectionManager::FORWARD_ONLY;
  initialize();

  // Commented sample code to regenerate the map literals used below in the test log if necessary:

  // std::cout << "tag_extracted_counter_map = {";
  // std::list<Stats::CounterSharedPtr> counters = test_server_->counters();
  // for (auto it = counters.begin(); it != counters.end(); ++it) {
  //   if (it != counters.begin()) {
  //     std::cout << ",";
  //   }
  //   std::cout << std::endl << "{\"" << (*it)->name() << "\", \"" << (*it)->tagExtractedName() <<
  //   "\"}";
  // }
  // std::cout << "};" << std::endl;
  // std::cout << "tag_extracted_gauge_map = {";
  // std::list<Stats::GaugeSharedPtr> gauges = test_server_->gauges();
  // for (auto it = gauges.begin(); it != gauges.end(); ++it) {
  //   if (it != gauges.begin()) {
  //     std::cout << ",";
  //   }
  //   std::cout << std::endl << "{\"" << (*it)->name() << "\", \"" << (*it)->tagExtractedName() <<
  //   "\"}";
  // }
  // std::cout << "};" << std::endl;

  std::unordered_map<std::string, std::string> tag_extracted_counter_map;
  std::unordered_map<std::string, std::string> tag_extracted_gauge_map;

  switch (GetParam()) {
  case Network::Address::IpVersion::v4: {
    tag_extracted_counter_map = {
        {"listener.127.0.0.1_0.downstream_cx_total", "listener.downstream_cx_total"},
        {"listener.127.0.0.1_0.http.router.downstream_rq_5xx", "listener.http.downstream_rq_xx"},
        {"listener.127.0.0.1_0.http.router.downstream_rq_4xx", "listener.http.downstream_rq_xx"},
        {"listener.127.0.0.1_0.http.router.downstream_rq_3xx", "listener.http.downstream_rq_xx"},
        {"listener.127.0.0.1_0.downstream_cx_destroy", "listener.downstream_cx_destroy"},
        {"listener.127.0.0.1_0.downstream_cx_proxy_proto_error",
         "listener.downstream_cx_proxy_proto_error"},
        {"listener.127.0.0.1_0.http.router.downstream_rq_2xx", "listener.http.downstream_rq_xx"},
        {"http.router.rq_total", "http.rq_total"},
        {"http.router.tracing.not_traceable", "http.tracing.not_traceable"},
        {"http.router.tracing.random_sampling", "http.tracing.random_sampling"},
        {"http.router.rs_too_large", "http.rs_too_large"},
        {"http.router.downstream_rq_5xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_4xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_2xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_ws_on_non_ws_route", "http.downstream_rq_ws_on_non_ws_route"},
        {"http.router.downstream_rq_tx_reset", "http.downstream_rq_tx_reset"},
        {"http.router.no_route", "http.no_route"},
        {"http.router.tracing.health_check", "http.tracing.health_check"},
        {"http.router.downstream_rq_too_large", "http.downstream_rq_too_large"},
        {"http.router.downstream_rq_response_before_rq_complete",
         "http.downstream_rq_response_before_rq_complete"},
        {"http.router.downstream_rq_3xx", "http.downstream_rq_xx"},
        {"http.router.downstream_cx_destroy", "http.downstream_cx_destroy"},
        {"http.router.downstream_rq_non_relative_path", "http.downstream_rq_non_relative_path"},
        {"http.router.downstream_cx_destroy_active_rq", "http.downstream_cx_destroy_active_rq"},
        {"http.router.tracing.client_enabled", "http.tracing.client_enabled"},
        {"http.router.downstream_cx_destroy_remote", "http.downstream_cx_destroy_remote"},
        {"http.router.downstream_cx_http1_total", "http.downstream_cx_http1_total"},
        {"http.router.downstream_cx_http2_total", "http.downstream_cx_http2_total"},
        {"http.router.downstream_cx_ssl_total", "http.downstream_cx_ssl_total"},
        {"http.router.downstream_cx_destroy_local_active_rq",
         "http.downstream_cx_destroy_local_active_rq"},
        {"http.router.downstream_cx_tx_bytes_total", "http.downstream_cx_tx_bytes_total"},
        {"http.router.downstream_cx_destroy_local", "http.downstream_cx_destroy_local"},
        {"http.router.downstream_flow_control_resumed_reading_total",
         "http.downstream_flow_control_resumed_reading_total"},
        {"http.router.downstream_cx_total", "http.downstream_cx_total"},
        {"http.router.downstream_cx_websocket_total", "http.downstream_cx_websocket_total"},
        {"http.router.downstream_cx_destroy_remote_active_rq",
         "http.downstream_cx_destroy_remote_active_rq"},
        {"http.router.rq_redirect", "http.rq_redirect"},
        {"http.router.downstream_cx_protocol_error", "http.downstream_cx_protocol_error"},
        {"http.router.downstream_cx_drain_close", "http.downstream_cx_drain_close"},
        {"http.router.downstream_rq_http2_total", "http.downstream_rq_http2_total"},
        {"http.router.no_cluster", "http.no_cluster"},
        {"http.router.downstream_rq_rx_reset", "http.downstream_rq_rx_reset"},
        {"http.router.downstream_cx_rx_bytes_total", "http.downstream_cx_rx_bytes_total"},
        {"http.router.downstream_flow_control_paused_reading_total",
         "http.downstream_flow_control_paused_reading_total"},
        {"http.router.downstream_cx_idle_timeout", "http.downstream_cx_idle_timeout"},
        {"http.router.tracing.service_forced", "http.tracing.service_forced"},
        {"http.router.downstream_rq_http1_total", "http.downstream_rq_http1_total"},
        {"http.router.downstream_rq_total", "http.downstream_rq_total"},
        {"listener.127.0.0.1_0.ssl.connection_error", "listener.ssl.connection_error"},
        {"listener.127.0.0.1_0.ssl.handshake", "listener.ssl.handshake"},
        {"listener.127.0.0.1_0.ssl.session_reused", "listener.ssl.session_reused"},
        {"listener.127.0.0.1_0.ssl.fail_verify_san", "listener.ssl.fail_verify_san"},
        {"listener.127.0.0.1_0.ssl.no_certificate", "listener.ssl.no_certificate"},
        {"listener.127.0.0.1_0.ssl.fail_verify_no_cert", "listener.ssl.fail_verify_no_cert"},
        {"listener.127.0.0.1_0.ssl.fail_verify_error", "listener.ssl.fail_verify_error"},
        {"listener.127.0.0.1_0.ssl.fail_verify_cert_hash", "listener.ssl.fail_verify_cert_hash"},
        {"cluster.cluster_2.ssl.fail_verify_san", "cluster.ssl.fail_verify_san"},
        {"cluster.cluster_2.ssl.fail_verify_error", "cluster.ssl.fail_verify_error"},
        {"cluster.cluster_2.ssl.fail_verify_no_cert", "cluster.ssl.fail_verify_no_cert"},
        {"cluster.cluster_2.update_success", "cluster.update_success"},
        {"cluster.cluster_2.update_attempt", "cluster.update_attempt"},
        {"cluster.cluster_2.retry_or_shadow_abandoned", "cluster.retry_or_shadow_abandoned"},
        {"cluster.cluster_2.upstream_cx_destroy_local_with_active_rq",
         "cluster.upstream_cx_destroy_local_with_active_rq"},
        {"cluster.cluster_2.update_empty", "cluster.update_empty"},
        {"cluster.cluster_2.lb_zone_no_capacity_left", "cluster.lb_zone_no_capacity_left"},
        {"cluster.cluster_2.ssl.fail_verify_cert_hash", "cluster.ssl.fail_verify_cert_hash"},
        {"cluster.cluster_2.upstream_cx_destroy", "cluster.upstream_cx_destroy"},
        {"cluster.cluster_2.upstream_cx_connect_timeout", "cluster.upstream_cx_connect_timeout"},
        {"cluster.cluster_2.update_failure", "cluster.update_failure"},
        {"cluster.cluster_2.upstream_cx_rx_bytes_total", "cluster.upstream_cx_rx_bytes_total"},
        {"cluster.cluster_2.ssl.no_certificate", "cluster.ssl.no_certificate"},
        {"cluster.cluster_2.upstream_cx_http1_total", "cluster.upstream_cx_http1_total"},
        {"cluster.cluster_2.upstream_cx_overflow", "cluster.upstream_cx_overflow"},
        {"cluster.cluster_2.lb_local_cluster_not_ok", "cluster.lb_local_cluster_not_ok"},
        {"cluster.cluster_2.ssl.connection_error", "cluster.ssl.connection_error"},
        {"cluster.cluster_2.upstream_cx_destroy_with_active_rq",
         "cluster.upstream_cx_destroy_with_active_rq"},
        {"cluster.cluster_2.upstream_cx_destroy_remote_with_active_rq",
         "cluster.upstream_cx_destroy_remote_with_active_rq"},
        {"cluster.cluster_2.lb_recalculate_zone_structures",
         "cluster.lb_recalculate_zone_structures"},
        {"cluster.cluster_2.lb_zone_number_differs", "cluster.lb_zone_number_differs"},
        {"cluster.cluster_2.upstream_cx_none_healthy", "cluster.upstream_cx_none_healthy"},
        {"cluster.cluster_2.lb_zone_routing_all_directly", "cluster.lb_zone_routing_all_directly"},
        {"cluster.cluster_2.upstream_cx_http2_total", "cluster.upstream_cx_http2_total"},
        {"cluster.cluster_2.upstream_rq_maintenance_mode", "cluster.upstream_rq_maintenance_mode"},
        {"cluster.cluster_2.upstream_rq_total", "cluster.upstream_rq_total"},
        {"cluster.cluster_2.lb_zone_routing_cross_zone", "cluster.lb_zone_routing_cross_zone"},
        {"cluster.cluster_2.lb_healthy_panic", "cluster.lb_healthy_panic"},
        {"cluster.cluster_2.upstream_rq_timeout", "cluster.upstream_rq_timeout"},
        {"cluster.cluster_2.upstream_rq_per_try_timeout", "cluster.upstream_rq_per_try_timeout"},
        {"cluster.cluster_2.lb_zone_routing_sampled", "cluster.lb_zone_routing_sampled"},
        {"cluster.cluster_2.upstream_cx_connect_fail", "cluster.upstream_cx_connect_fail"},
        {"cluster.cluster_2.upstream_cx_destroy_remote", "cluster.upstream_cx_destroy_remote"},
        {"cluster.cluster_2.upstream_rq_retry", "cluster.upstream_rq_retry"},
        {"cluster.cluster_2.upstream_cx_total", "cluster.upstream_cx_total"},
        {"cluster.cluster_2.upstream_rq_retry_overflow", "cluster.upstream_rq_retry_overflow"},
        {"cluster.cluster_2.upstream_cx_tx_bytes_total", "cluster.upstream_cx_tx_bytes_total"},
        {"cluster.cluster_2.upstream_cx_close_notify", "cluster.upstream_cx_close_notify"},
        {"cluster.cluster_2.upstream_cx_protocol_error", "cluster.upstream_cx_protocol_error"},
        {"cluster.cluster_2.upstream_flow_control_drained_total",
         "cluster.upstream_flow_control_drained_total"},
        {"cluster.cluster_2.upstream_rq_pending_failure_eject",
         "cluster.upstream_rq_pending_failure_eject"},
        {"cluster.cluster_2.upstream_cx_max_requests", "cluster.upstream_cx_max_requests"},
        {"cluster.cluster_2.upstream_rq_rx_reset", "cluster.upstream_rq_rx_reset"},
        {"cluster.cluster_2.upstream_rq_pending_total", "cluster.upstream_rq_pending_total"},
        {"cluster.cluster_2.upstream_rq_pending_overflow", "cluster.upstream_rq_pending_overflow"},
        {"cluster.cluster_2.upstream_rq_cancelled", "cluster.upstream_rq_cancelled"},
        {"cluster.cluster_2.lb_zone_cluster_too_small", "cluster.lb_zone_cluster_too_small"},
        {"cluster.cluster_2.upstream_rq_tx_reset", "cluster.upstream_rq_tx_reset"},
        {"cluster.cluster_2.ssl.session_reused", "cluster.ssl.session_reused"},
        {"cluster.cluster_2.membership_change", "cluster.membership_change"},
        {"cluster.cluster_2.upstream_rq_retry_success", "cluster.upstream_rq_retry_success"},
        {"cluster.cluster_2.upstream_flow_control_paused_reading_total",
         "cluster.upstream_flow_control_paused_reading_total"},
        {"cluster.cluster_2.upstream_flow_control_resumed_reading_total",
         "cluster.upstream_flow_control_resumed_reading_total"},
        {"cluster.cluster_2.upstream_flow_control_backed_up_total",
         "cluster.upstream_flow_control_backed_up_total"},
        {"cluster.cluster_2.ssl.handshake", "cluster.ssl.handshake"},
        {"cluster.cluster_2.upstream_cx_destroy_local", "cluster.upstream_cx_destroy_local"},
        {"cluster.cluster_2.bind_errors", "cluster.bind_errors"},
        {"cluster.cluster_1.ssl.fail_verify_cert_hash", "cluster.ssl.fail_verify_cert_hash"},
        {"cluster.cluster_1.ssl.fail_verify_san", "cluster.ssl.fail_verify_san"},
        {"cluster.cluster_1.ssl.session_reused", "cluster.ssl.session_reused"},
        {"cluster.cluster_1.ssl.handshake", "cluster.ssl.handshake"},
        {"cluster.cluster_1.update_empty", "cluster.update_empty"},
        {"cluster.cluster_1.update_failure", "cluster.update_failure"},
        {"cluster.cluster_1.update_success", "cluster.update_success"},
        {"cluster.cluster_1.update_attempt", "cluster.update_attempt"},
        {"cluster.cluster_1.retry_or_shadow_abandoned", "cluster.retry_or_shadow_abandoned"},
        {"cluster.cluster_1.upstream_cx_close_notify", "cluster.upstream_cx_close_notify"},
        {"cluster.cluster_1.upstream_cx_destroy_local_with_active_rq",
         "cluster.upstream_cx_destroy_local_with_active_rq"},
        {"cluster.cluster_1.lb_zone_routing_sampled", "cluster.lb_zone_routing_sampled"},
        {"cluster.cluster_1.upstream_cx_destroy_with_active_rq",
         "cluster.upstream_cx_destroy_with_active_rq"},
        {"cluster.cluster_1.upstream_cx_overflow", "cluster.upstream_cx_overflow"},
        {"cluster.cluster_1.lb_zone_no_capacity_left", "cluster.lb_zone_no_capacity_left"},
        {"cluster.cluster_1.upstream_cx_connect_fail", "cluster.upstream_cx_connect_fail"},
        {"cluster.cluster_1.upstream_cx_connect_timeout", "cluster.upstream_cx_connect_timeout"},
        {"cluster.cluster_1.lb_zone_number_differs", "cluster.lb_zone_number_differs"},
        {"cluster.cluster_1.upstream_rq_maintenance_mode", "cluster.upstream_rq_maintenance_mode"},
        {"cluster.cluster_1.upstream_cx_destroy_local", "cluster.upstream_cx_destroy_local"},
        {"cluster.cluster_1.ssl.fail_verify_error", "cluster.ssl.fail_verify_error"},
        {"cluster.cluster_1.upstream_cx_http2_total", "cluster.upstream_cx_http2_total"},
        {"cluster.cluster_1.lb_healthy_panic", "cluster.lb_healthy_panic"},
        {"cluster.cluster_1.ssl.fail_verify_no_cert", "cluster.ssl.fail_verify_no_cert"},
        {"cluster.cluster_1.ssl.no_certificate", "cluster.ssl.no_certificate"},
        {"cluster.cluster_1.upstream_rq_retry_overflow", "cluster.upstream_rq_retry_overflow"},
        {"cluster.cluster_1.lb_local_cluster_not_ok", "cluster.lb_local_cluster_not_ok"},
        {"cluster.cluster_1.lb_recalculate_zone_structures",
         "cluster.lb_recalculate_zone_structures"},
        {"cluster.cluster_1.lb_zone_routing_all_directly", "cluster.lb_zone_routing_all_directly"},
        {"cluster.cluster_1.upstream_cx_http1_total", "cluster.upstream_cx_http1_total"},
        {"cluster.cluster_1.upstream_rq_pending_total", "cluster.upstream_rq_pending_total"},
        {"cluster.cluster_1.lb_zone_routing_cross_zone", "cluster.lb_zone_routing_cross_zone"},
        {"cluster.cluster_1.upstream_cx_total", "cluster.upstream_cx_total"},
        {"cluster.cluster_1.bind_errors", "cluster.bind_errors"},
        {"cluster.cluster_1.upstream_cx_destroy_remote", "cluster.upstream_cx_destroy_remote"},
        {"cluster.cluster_1.upstream_rq_rx_reset", "cluster.upstream_rq_rx_reset"},
        {"cluster.cluster_1.upstream_cx_tx_bytes_total", "cluster.upstream_cx_tx_bytes_total"},
        {"cluster.cluster_1.ssl.connection_error", "cluster.ssl.connection_error"},
        {"cluster.cluster_1.upstream_rq_tx_reset", "cluster.upstream_rq_tx_reset"},
        {"cluster.cluster_1.upstream_cx_destroy", "cluster.upstream_cx_destroy"},
        {"cluster.cluster_1.upstream_cx_protocol_error", "cluster.upstream_cx_protocol_error"},
        {"cluster.cluster_1.upstream_cx_max_requests", "cluster.upstream_cx_max_requests"},
        {"cluster.cluster_1.upstream_cx_rx_bytes_total", "cluster.upstream_cx_rx_bytes_total"},
        {"cluster.cluster_1.upstream_rq_cancelled", "cluster.upstream_rq_cancelled"},
        {"cluster.cluster_1.upstream_cx_none_healthy", "cluster.upstream_cx_none_healthy"},
        {"cluster.cluster_1.upstream_rq_timeout", "cluster.upstream_rq_timeout"},
        {"cluster.cluster_1.upstream_rq_pending_overflow", "cluster.upstream_rq_pending_overflow"},
        {"cluster.cluster_1.upstream_rq_per_try_timeout", "cluster.upstream_rq_per_try_timeout"},
        {"cluster.cluster_1.upstream_rq_total", "cluster.upstream_rq_total"},
        {"cluster.cluster_1.upstream_cx_destroy_remote_with_active_rq",
         "cluster.upstream_cx_destroy_remote_with_active_rq"},
        {"cluster.cluster_1.upstream_rq_pending_failure_eject",
         "cluster.upstream_rq_pending_failure_eject"},
        {"cluster.cluster_1.upstream_rq_retry", "cluster.upstream_rq_retry"},
        {"cluster.cluster_1.upstream_rq_retry_success", "cluster.upstream_rq_retry_success"},
        {"cluster.cluster_1.lb_zone_cluster_too_small", "cluster.lb_zone_cluster_too_small"},
        {"cluster.cluster_1.upstream_flow_control_paused_reading_total",
         "cluster.upstream_flow_control_paused_reading_total"},
        {"cluster.cluster_1.upstream_flow_control_resumed_reading_total",
         "cluster.upstream_flow_control_resumed_reading_total"},
        {"cluster.cluster_1.upstream_flow_control_backed_up_total",
         "cluster.upstream_flow_control_backed_up_total"},
        {"cluster.cluster_1.upstream_flow_control_drained_total",
         "cluster.upstream_flow_control_drained_total"},
        {"cluster.cluster_1.membership_change", "cluster.membership_change"},
        {"listener.admin.downstream_cx_destroy", "listener.admin.downstream_cx_destroy"},
        {"listener.admin.downstream_cx_total", "listener.admin.downstream_cx_total"},
        {"listener.admin.downstream_cx_proxy_proto_error",
         "listener.admin.downstream_cx_proxy_proto_error"},
        {"server.watchdog_mega_miss", "server.watchdog_mega_miss"},
        {"server.watchdog_miss", "server.watchdog_miss"},
        {"http.async-client.rq_total", "http.rq_total"},
        {"cluster_manager.cluster_added", "cluster_manager.cluster_added"},
        {"http.admin.downstream_rq_http2_total", "http.downstream_rq_http2_total"},
        {"cluster_manager.cluster_removed", "cluster_manager.cluster_removed"},
        {"http.admin.downstream_cx_destroy_remote", "http.downstream_cx_destroy_remote"},
        {"http.admin.downstream_rq_http1_total", "http.downstream_rq_http1_total"},
        {"http.admin.tracing.tracing.client_enabled", "http.tracing.tracing.client_enabled"},
        {"http.admin.downstream_rq_total", "http.downstream_rq_total"},
        {"http.admin.tracing.tracing.service_forced", "http.tracing.tracing.service_forced"},
        {"http.admin.tracing.tracing.not_traceable", "http.tracing.tracing.not_traceable"},
        {"http.admin.downstream_cx_rx_bytes_total", "http.downstream_cx_rx_bytes_total"},
        {"http.async-client.no_cluster", "http.no_cluster"},
        {"http.admin.downstream_cx_destroy_remote_active_rq",
         "http.downstream_cx_destroy_remote_active_rq"},
        {"http.admin.downstream_cx_destroy_local_active_rq",
         "http.downstream_cx_destroy_local_active_rq"},
        {"filesystem.write_buffered", "filesystem.write_buffered"},
        {"http.admin.downstream_cx_destroy_active_rq", "http.downstream_cx_destroy_active_rq"},
        {"http.admin.downstream_rq_tx_reset", "http.downstream_rq_tx_reset"},
        {"http.admin.downstream_flow_control_resumed_reading_total",
         "http.downstream_flow_control_resumed_reading_total"},
        {"stats.overflow", "stats.overflow"},
        {"http.admin.downstream_cx_total", "http.downstream_cx_total"},
        {"http.admin.downstream_rq_3xx", "http.downstream_rq_xx"},
        {"http.admin.downstream_cx_idle_timeout", "http.downstream_cx_idle_timeout"},
        {"http.admin.downstream_rq_rx_reset", "http.downstream_rq_rx_reset"},
        {"http.admin.downstream_cx_ssl_total", "http.downstream_cx_ssl_total"},
        {"http.admin.downstream_cx_websocket_total", "http.downstream_cx_websocket_total"},
        {"http.admin.downstream_rq_2xx", "http.downstream_rq_xx"},
        {"cluster_manager.cluster_modified", "cluster_manager.cluster_modified"},
        {"http.admin.downstream_cx_drain_close", "http.downstream_cx_drain_close"},
        {"http.admin.downstream_cx_destroy", "http.downstream_cx_destroy"},
        {"http.admin.downstream_cx_http1_total", "http.downstream_cx_http1_total"},
        {"http.admin.downstream_cx_protocol_error", "http.downstream_cx_protocol_error"},
        {"http.admin.downstream_cx_destroy_local", "http.downstream_cx_destroy_local"},
        {"listener_manager.listener_added", "listener_manager.listener_added"},
        {"filesystem.write_completed", "filesystem.write_completed"},
        {"http.admin.downstream_rq_response_before_rq_complete",
         "http.downstream_rq_response_before_rq_complete"},
        {"http.admin.downstream_cx_tx_bytes_total", "http.downstream_cx_tx_bytes_total"},
        {"http.admin.downstream_rq_4xx", "http.downstream_rq_xx"},
        {"http.admin.downstream_rq_non_relative_path", "http.downstream_rq_non_relative_path"},
        {"http.admin.downstream_rq_ws_on_non_ws_route", "http.downstream_rq_ws_on_non_ws_route"},
        {"http.admin.downstream_rq_too_large", "http.downstream_rq_too_large"},
        {"http.admin.downstream_rq_5xx", "http.downstream_rq_xx"},
        {"http.async-client.no_route", "http.no_route"},
        {"http.admin.downstream_flow_control_paused_reading_total",
         "http.downstream_flow_control_paused_reading_total"},
        {"listener_manager.listener_removed", "listener_manager.listener_removed"},
        {"listener_manager.listener_create_failure", "listener_manager.listener_create_failure"},
        {"http.admin.tracing.tracing.random_sampling", "http.tracing.tracing.random_sampling"},
        {"http.async-client.rq_redirect", "http.rq_redirect"},
        {"http.admin.tracing.tracing.health_check", "http.tracing.tracing.health_check"},
        {"filesystem.flushed_by_timer", "filesystem.flushed_by_timer"},
        {"http.admin.downstream_cx_http2_total", "http.downstream_cx_http2_total"},
        {"filesystem.reopen_failed", "filesystem.reopen_failed"},
        {"listener_manager.listener_modified", "listener_manager.listener_modified"},
        {"http.admin.rs_too_large", "http.rs_too_large"},
        {"listener_manager.listener_create_success", "listener_manager.listener_create_success"}};
    tag_extracted_gauge_map = {
        {"listener.127.0.0.1_0.downstream_cx_active", "listener.downstream_cx_active"},
        {"http.router.downstream_rq_active", "http.downstream_rq_active"},
        {"http.router.downstream_cx_tx_bytes_buffered", "http.downstream_cx_tx_bytes_buffered"},
        {"http.router.downstream_cx_http2_active", "http.downstream_cx_http2_active"},
        {"http.router.downstream_cx_websocket_active", "http.downstream_cx_websocket_active"},
        {"http.router.downstream_cx_rx_bytes_buffered", "http.downstream_cx_rx_bytes_buffered"},
        {"http.router.downstream_cx_http1_active", "http.downstream_cx_http1_active"},
        {"http.router.downstream_cx_ssl_active", "http.downstream_cx_ssl_active"},
        {"http.router.downstream_cx_active", "http.downstream_cx_active"},
        {"cluster.cluster_2.membership_total", "cluster.membership_total"},
        {"cluster.cluster_2.membership_healthy", "cluster.membership_healthy"},
        {"cluster.cluster_2.max_host_weight", "cluster.max_host_weight"},
        {"cluster.cluster_2.upstream_rq_pending_active", "cluster.upstream_rq_pending_active"},
        {"cluster.cluster_2.version", "cluster.version"},
        {"cluster.cluster_2.upstream_rq_active", "cluster.upstream_rq_active"},
        {"cluster.cluster_2.upstream_cx_tx_bytes_buffered",
         "cluster.upstream_cx_tx_bytes_buffered"},
        {"cluster.cluster_2.upstream_cx_rx_bytes_buffered",
         "cluster.upstream_cx_rx_bytes_buffered"},
        {"cluster.cluster_2.upstream_cx_active", "cluster.upstream_cx_active"},
        {"cluster.cluster_1.upstream_rq_active", "cluster.upstream_rq_active"},
        {"cluster.cluster_1.upstream_rq_pending_active", "cluster.upstream_rq_pending_active"},
        {"cluster.cluster_1.upstream_cx_tx_bytes_buffered",
         "cluster.upstream_cx_tx_bytes_buffered"},
        {"cluster.cluster_1.max_host_weight", "cluster.max_host_weight"},
        {"cluster.cluster_1.upstream_cx_rx_bytes_buffered",
         "cluster.upstream_cx_rx_bytes_buffered"},
        {"cluster.cluster_1.version", "cluster.version"},
        {"cluster.cluster_1.membership_total", "cluster.membership_total"},
        {"cluster.cluster_1.membership_healthy", "cluster.membership_healthy"},
        {"cluster.cluster_1.upstream_cx_active", "cluster.upstream_cx_active"},
        {"listener.admin.downstream_cx_active", "listener.admin.downstream_cx_active"},
        {"cluster_manager.total_clusters", "cluster_manager.total_clusters"},
        {"listener_manager.total_listeners_warming", "listener_manager.total_listeners_warming"},
        {"listener_manager.total_listeners_active", "listener_manager.total_listeners_active"},
        {"http.admin.downstream_rq_active", "http.downstream_rq_active"},
        {"http.admin.downstream_cx_tx_bytes_buffered", "http.downstream_cx_tx_bytes_buffered"},
        {"http.admin.downstream_cx_rx_bytes_buffered", "http.downstream_cx_rx_bytes_buffered"},
        {"http.admin.downstream_cx_websocket_active", "http.downstream_cx_websocket_active"},
        {"http.admin.downstream_cx_http1_active", "http.downstream_cx_http1_active"},
        {"server.uptime", "server.uptime"},
        {"server.memory_allocated", "server.memory_allocated"},
        {"http.admin.downstream_cx_http2_active", "http.downstream_cx_http2_active"},
        {"server.memory_heap_size", "server.memory_heap_size"},
        {"listener_manager.total_listeners_draining", "listener_manager.total_listeners_draining"},
        {"filesystem.write_total_buffered", "filesystem.write_total_buffered"},
        {"http.admin.downstream_cx_ssl_active", "http.downstream_cx_ssl_active"},
        {"http.admin.downstream_cx_active", "http.downstream_cx_active"},
        {"server.live", "server.live"},
        {"server.parent_connections", "server.parent_connections"},
        {"server.total_connections", "server.total_connections"},
        {"server.days_until_first_cert_expiring", "server.days_until_first_cert_expiring"},
        {"server.version", "server.version"}};
    break;
  }
  case Network::Address::IpVersion::v6: {
    tag_extracted_counter_map = {
        {"listener.[__1]_0.downstream_cx_destroy", "listener.downstream_cx_destroy"},
        {"listener.[__1]_0.downstream_cx_proxy_proto_error",
         "listener.downstream_cx_proxy_proto_error"},
        {"listener.[__1]_0.http.router.downstream_rq_5xx", "listener.http.downstream_rq_xx"},
        {"listener.[__1]_0.http.router.downstream_rq_4xx", "listener.http.downstream_rq_xx"},
        {"listener.[__1]_0.downstream_cx_total", "listener.downstream_cx_total"},
        {"listener.[__1]_0.http.router.downstream_rq_3xx", "listener.http.downstream_rq_xx"},
        {"listener.[__1]_0.http.router.downstream_rq_2xx", "listener.http.downstream_rq_xx"},
        {"http.router.rq_total", "http.rq_total"},
        {"http.router.tracing.not_traceable", "http.tracing.not_traceable"},
        {"http.router.tracing.random_sampling", "http.tracing.random_sampling"},
        {"http.router.rs_too_large", "http.rs_too_large"},
        {"http.router.downstream_rq_5xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_4xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_2xx", "http.downstream_rq_xx"},
        {"http.router.downstream_rq_ws_on_non_ws_route", "http.downstream_rq_ws_on_non_ws_route"},
        {"http.router.downstream_rq_tx_reset", "http.downstream_rq_tx_reset"},
        {"http.router.no_route", "http.no_route"},
        {"http.router.tracing.health_check", "http.tracing.health_check"},
        {"http.router.downstream_rq_too_large", "http.downstream_rq_too_large"},
        {"http.router.downstream_rq_response_before_rq_complete",
         "http.downstream_rq_response_before_rq_complete"},
        {"http.router.downstream_rq_3xx", "http.downstream_rq_xx"},
        {"http.router.downstream_cx_destroy", "http.downstream_cx_destroy"},
        {"http.router.downstream_rq_non_relative_path", "http.downstream_rq_non_relative_path"},
        {"http.router.downstream_cx_destroy_active_rq", "http.downstream_cx_destroy_active_rq"},
        {"http.router.tracing.client_enabled", "http.tracing.client_enabled"},
        {"http.router.downstream_cx_destroy_remote", "http.downstream_cx_destroy_remote"},
        {"http.router.downstream_cx_http1_total", "http.downstream_cx_http1_total"},
        {"http.router.downstream_cx_http2_total", "http.downstream_cx_http2_total"},
        {"http.router.downstream_cx_ssl_total", "http.downstream_cx_ssl_total"},
        {"http.router.downstream_cx_destroy_local_active_rq",
         "http.downstream_cx_destroy_local_active_rq"},
        {"http.router.downstream_cx_tx_bytes_total", "http.downstream_cx_tx_bytes_total"},
        {"http.router.downstream_cx_destroy_local", "http.downstream_cx_destroy_local"},
        {"http.router.downstream_flow_control_resumed_reading_total",
         "http.downstream_flow_control_resumed_reading_total"},
        {"http.router.downstream_cx_total", "http.downstream_cx_total"},
        {"http.router.downstream_cx_websocket_total", "http.downstream_cx_websocket_total"},
        {"http.router.downstream_cx_destroy_remote_active_rq",
         "http.downstream_cx_destroy_remote_active_rq"},
        {"http.router.rq_redirect", "http.rq_redirect"},
        {"http.router.downstream_cx_protocol_error", "http.downstream_cx_protocol_error"},
        {"http.router.downstream_cx_drain_close", "http.downstream_cx_drain_close"},
        {"http.router.downstream_rq_http2_total", "http.downstream_rq_http2_total"},
        {"http.router.no_cluster", "http.no_cluster"},
        {"http.router.downstream_rq_rx_reset", "http.downstream_rq_rx_reset"},
        {"http.router.downstream_cx_rx_bytes_total", "http.downstream_cx_rx_bytes_total"},
        {"http.router.downstream_flow_control_paused_reading_total",
         "http.downstream_flow_control_paused_reading_total"},
        {"http.router.downstream_cx_idle_timeout", "http.downstream_cx_idle_timeout"},
        {"http.router.tracing.service_forced", "http.tracing.service_forced"},
        {"http.router.downstream_rq_http1_total", "http.downstream_rq_http1_total"},
        {"http.router.downstream_rq_total", "http.downstream_rq_total"},
        {"listener.[__1]_0.ssl.fail_verify_no_cert", "listener.ssl.fail_verify_no_cert"},
        {"listener.[__1]_0.ssl.fail_verify_cert_hash", "listener.ssl.fail_verify_cert_hash"},
        {"listener.[__1]_0.ssl.session_reused", "listener.ssl.session_reused"},
        {"listener.[__1]_0.ssl.no_certificate", "listener.ssl.no_certificate"},
        {"listener.[__1]_0.ssl.fail_verify_error", "listener.ssl.fail_verify_error"},
        {"listener.[__1]_0.ssl.connection_error", "listener.ssl.connection_error"},
        {"listener.[__1]_0.ssl.fail_verify_san", "listener.ssl.fail_verify_san"},
        {"listener.[__1]_0.ssl.handshake", "listener.ssl.handshake"},
        {"cluster.cluster_2.ssl.fail_verify_san", "cluster.ssl.fail_verify_san"},
        {"cluster.cluster_2.ssl.fail_verify_error", "cluster.ssl.fail_verify_error"},
        {"cluster.cluster_2.ssl.fail_verify_no_cert", "cluster.ssl.fail_verify_no_cert"},
        {"cluster.cluster_2.update_success", "cluster.update_success"},
        {"cluster.cluster_2.update_attempt", "cluster.update_attempt"},
        {"cluster.cluster_2.retry_or_shadow_abandoned", "cluster.retry_or_shadow_abandoned"},
        {"cluster.cluster_2.upstream_cx_destroy_local_with_active_rq",
         "cluster.upstream_cx_destroy_local_with_active_rq"},
        {"cluster.cluster_2.update_empty", "cluster.update_empty"},
        {"cluster.cluster_2.lb_zone_no_capacity_left", "cluster.lb_zone_no_capacity_left"},
        {"cluster.cluster_2.ssl.fail_verify_cert_hash", "cluster.ssl.fail_verify_cert_hash"},
        {"cluster.cluster_2.upstream_cx_destroy", "cluster.upstream_cx_destroy"},
        {"cluster.cluster_2.upstream_cx_connect_timeout", "cluster.upstream_cx_connect_timeout"},
        {"cluster.cluster_2.update_failure", "cluster.update_failure"},
        {"cluster.cluster_2.upstream_cx_rx_bytes_total", "cluster.upstream_cx_rx_bytes_total"},
        {"cluster.cluster_2.ssl.no_certificate", "cluster.ssl.no_certificate"},
        {"cluster.cluster_2.upstream_cx_http1_total", "cluster.upstream_cx_http1_total"},
        {"cluster.cluster_2.upstream_cx_overflow", "cluster.upstream_cx_overflow"},
        {"cluster.cluster_2.lb_local_cluster_not_ok", "cluster.lb_local_cluster_not_ok"},
        {"cluster.cluster_2.ssl.connection_error", "cluster.ssl.connection_error"},
        {"cluster.cluster_2.upstream_cx_destroy_with_active_rq",
         "cluster.upstream_cx_destroy_with_active_rq"},
        {"cluster.cluster_2.upstream_cx_destroy_remote_with_active_rq",
         "cluster.upstream_cx_destroy_remote_with_active_rq"},
        {"cluster.cluster_2.lb_recalculate_zone_structures",
         "cluster.lb_recalculate_zone_structures"},
        {"cluster.cluster_2.lb_zone_number_differs", "cluster.lb_zone_number_differs"},
        {"cluster.cluster_2.upstream_cx_none_healthy", "cluster.upstream_cx_none_healthy"},
        {"cluster.cluster_2.lb_zone_routing_all_directly", "cluster.lb_zone_routing_all_directly"},
        {"cluster.cluster_2.upstream_cx_http2_total", "cluster.upstream_cx_http2_total"},
        {"cluster.cluster_2.upstream_rq_maintenance_mode", "cluster.upstream_rq_maintenance_mode"},
        {"cluster.cluster_2.upstream_rq_total", "cluster.upstream_rq_total"},
        {"cluster.cluster_2.lb_zone_routing_cross_zone", "cluster.lb_zone_routing_cross_zone"},
        {"cluster.cluster_2.lb_healthy_panic", "cluster.lb_healthy_panic"},
        {"cluster.cluster_2.upstream_rq_timeout", "cluster.upstream_rq_timeout"},
        {"cluster.cluster_2.upstream_rq_per_try_timeout", "cluster.upstream_rq_per_try_timeout"},
        {"cluster.cluster_2.lb_zone_routing_sampled", "cluster.lb_zone_routing_sampled"},
        {"cluster.cluster_2.upstream_cx_connect_fail", "cluster.upstream_cx_connect_fail"},
        {"cluster.cluster_2.upstream_cx_destroy_remote", "cluster.upstream_cx_destroy_remote"},
        {"cluster.cluster_2.upstream_rq_retry", "cluster.upstream_rq_retry"},
        {"cluster.cluster_2.upstream_cx_total", "cluster.upstream_cx_total"},
        {"cluster.cluster_2.upstream_rq_retry_overflow", "cluster.upstream_rq_retry_overflow"},
        {"cluster.cluster_2.upstream_cx_tx_bytes_total", "cluster.upstream_cx_tx_bytes_total"},
        {"cluster.cluster_2.upstream_cx_close_notify", "cluster.upstream_cx_close_notify"},
        {"cluster.cluster_2.upstream_cx_protocol_error", "cluster.upstream_cx_protocol_error"},
        {"cluster.cluster_2.upstream_flow_control_drained_total",
         "cluster.upstream_flow_control_drained_total"},
        {"cluster.cluster_2.upstream_rq_pending_failure_eject",
         "cluster.upstream_rq_pending_failure_eject"},
        {"cluster.cluster_2.upstream_cx_max_requests", "cluster.upstream_cx_max_requests"},
        {"cluster.cluster_2.upstream_rq_rx_reset", "cluster.upstream_rq_rx_reset"},
        {"cluster.cluster_2.upstream_rq_pending_total", "cluster.upstream_rq_pending_total"},
        {"cluster.cluster_2.upstream_rq_pending_overflow", "cluster.upstream_rq_pending_overflow"},
        {"cluster.cluster_2.upstream_rq_cancelled", "cluster.upstream_rq_cancelled"},
        {"cluster.cluster_2.lb_zone_cluster_too_small", "cluster.lb_zone_cluster_too_small"},
        {"cluster.cluster_2.upstream_rq_tx_reset", "cluster.upstream_rq_tx_reset"},
        {"cluster.cluster_2.ssl.session_reused", "cluster.ssl.session_reused"},
        {"cluster.cluster_2.membership_change", "cluster.membership_change"},
        {"cluster.cluster_2.upstream_rq_retry_success", "cluster.upstream_rq_retry_success"},
        {"cluster.cluster_2.upstream_flow_control_paused_reading_total",
         "cluster.upstream_flow_control_paused_reading_total"},
        {"cluster.cluster_2.upstream_flow_control_resumed_reading_total",
         "cluster.upstream_flow_control_resumed_reading_total"},
        {"cluster.cluster_2.upstream_flow_control_backed_up_total",
         "cluster.upstream_flow_control_backed_up_total"},
        {"cluster.cluster_2.ssl.handshake", "cluster.ssl.handshake"},
        {"cluster.cluster_2.upstream_cx_destroy_local", "cluster.upstream_cx_destroy_local"},
        {"cluster.cluster_2.bind_errors", "cluster.bind_errors"},
        {"cluster.cluster_1.ssl.fail_verify_cert_hash", "cluster.ssl.fail_verify_cert_hash"},
        {"cluster.cluster_1.ssl.fail_verify_san", "cluster.ssl.fail_verify_san"},
        {"cluster.cluster_1.ssl.session_reused", "cluster.ssl.session_reused"},
        {"cluster.cluster_1.ssl.handshake", "cluster.ssl.handshake"},
        {"cluster.cluster_1.update_empty", "cluster.update_empty"},
        {"cluster.cluster_1.update_failure", "cluster.update_failure"},
        {"cluster.cluster_1.update_success", "cluster.update_success"},
        {"cluster.cluster_1.update_attempt", "cluster.update_attempt"},
        {"cluster.cluster_1.retry_or_shadow_abandoned", "cluster.retry_or_shadow_abandoned"},
        {"cluster.cluster_1.upstream_cx_close_notify", "cluster.upstream_cx_close_notify"},
        {"cluster.cluster_1.upstream_cx_destroy_local_with_active_rq",
         "cluster.upstream_cx_destroy_local_with_active_rq"},
        {"cluster.cluster_1.lb_zone_routing_sampled", "cluster.lb_zone_routing_sampled"},
        {"cluster.cluster_1.upstream_cx_destroy_with_active_rq",
         "cluster.upstream_cx_destroy_with_active_rq"},
        {"cluster.cluster_1.upstream_cx_overflow", "cluster.upstream_cx_overflow"},
        {"cluster.cluster_1.lb_zone_no_capacity_left", "cluster.lb_zone_no_capacity_left"},
        {"cluster.cluster_1.upstream_cx_connect_fail", "cluster.upstream_cx_connect_fail"},
        {"cluster.cluster_1.upstream_cx_connect_timeout", "cluster.upstream_cx_connect_timeout"},
        {"cluster.cluster_1.lb_zone_number_differs", "cluster.lb_zone_number_differs"},
        {"cluster.cluster_1.upstream_rq_maintenance_mode", "cluster.upstream_rq_maintenance_mode"},
        {"cluster.cluster_1.upstream_cx_destroy_local", "cluster.upstream_cx_destroy_local"},
        {"cluster.cluster_1.ssl.fail_verify_error", "cluster.ssl.fail_verify_error"},
        {"cluster.cluster_1.upstream_cx_http2_total", "cluster.upstream_cx_http2_total"},
        {"cluster.cluster_1.lb_healthy_panic", "cluster.lb_healthy_panic"},
        {"cluster.cluster_1.ssl.fail_verify_no_cert", "cluster.ssl.fail_verify_no_cert"},
        {"cluster.cluster_1.ssl.no_certificate", "cluster.ssl.no_certificate"},
        {"cluster.cluster_1.upstream_rq_retry_overflow", "cluster.upstream_rq_retry_overflow"},
        {"cluster.cluster_1.lb_local_cluster_not_ok", "cluster.lb_local_cluster_not_ok"},
        {"cluster.cluster_1.lb_recalculate_zone_structures",
         "cluster.lb_recalculate_zone_structures"},
        {"cluster.cluster_1.lb_zone_routing_all_directly", "cluster.lb_zone_routing_all_directly"},
        {"cluster.cluster_1.upstream_cx_http1_total", "cluster.upstream_cx_http1_total"},
        {"cluster.cluster_1.upstream_rq_pending_total", "cluster.upstream_rq_pending_total"},
        {"cluster.cluster_1.lb_zone_routing_cross_zone", "cluster.lb_zone_routing_cross_zone"},
        {"cluster.cluster_1.upstream_cx_total", "cluster.upstream_cx_total"},
        {"cluster.cluster_1.bind_errors", "cluster.bind_errors"},
        {"cluster.cluster_1.upstream_cx_destroy_remote", "cluster.upstream_cx_destroy_remote"},
        {"cluster.cluster_1.upstream_rq_rx_reset", "cluster.upstream_rq_rx_reset"},
        {"cluster.cluster_1.upstream_cx_tx_bytes_total", "cluster.upstream_cx_tx_bytes_total"},
        {"cluster.cluster_1.ssl.connection_error", "cluster.ssl.connection_error"},
        {"cluster.cluster_1.upstream_rq_tx_reset", "cluster.upstream_rq_tx_reset"},
        {"cluster.cluster_1.upstream_cx_destroy", "cluster.upstream_cx_destroy"},
        {"cluster.cluster_1.upstream_cx_protocol_error", "cluster.upstream_cx_protocol_error"},
        {"cluster.cluster_1.upstream_cx_max_requests", "cluster.upstream_cx_max_requests"},
        {"cluster.cluster_1.upstream_cx_rx_bytes_total", "cluster.upstream_cx_rx_bytes_total"},
        {"cluster.cluster_1.upstream_rq_cancelled", "cluster.upstream_rq_cancelled"},
        {"cluster.cluster_1.upstream_cx_none_healthy", "cluster.upstream_cx_none_healthy"},
        {"cluster.cluster_1.upstream_rq_timeout", "cluster.upstream_rq_timeout"},
        {"cluster.cluster_1.upstream_rq_pending_overflow", "cluster.upstream_rq_pending_overflow"},
        {"cluster.cluster_1.upstream_rq_per_try_timeout", "cluster.upstream_rq_per_try_timeout"},
        {"cluster.cluster_1.upstream_rq_total", "cluster.upstream_rq_total"},
        {"cluster.cluster_1.upstream_cx_destroy_remote_with_active_rq",
         "cluster.upstream_cx_destroy_remote_with_active_rq"},
        {"cluster.cluster_1.upstream_rq_pending_failure_eject",
         "cluster.upstream_rq_pending_failure_eject"},
        {"cluster.cluster_1.upstream_rq_retry", "cluster.upstream_rq_retry"},
        {"cluster.cluster_1.upstream_rq_retry_success", "cluster.upstream_rq_retry_success"},
        {"cluster.cluster_1.lb_zone_cluster_too_small", "cluster.lb_zone_cluster_too_small"},
        {"cluster.cluster_1.upstream_flow_control_paused_reading_total",
         "cluster.upstream_flow_control_paused_reading_total"},
        {"cluster.cluster_1.upstream_flow_control_resumed_reading_total",
         "cluster.upstream_flow_control_resumed_reading_total"},
        {"cluster.cluster_1.upstream_flow_control_backed_up_total",
         "cluster.upstream_flow_control_backed_up_total"},
        {"cluster.cluster_1.upstream_flow_control_drained_total",
         "cluster.upstream_flow_control_drained_total"},
        {"cluster.cluster_1.membership_change", "cluster.membership_change"},
        {"listener.admin.downstream_cx_destroy", "listener.admin.downstream_cx_destroy"},
        {"listener.admin.downstream_cx_total", "listener.admin.downstream_cx_total"},
        {"listener.admin.downstream_cx_proxy_proto_error",
         "listener.admin.downstream_cx_proxy_proto_error"},
        {"server.watchdog_mega_miss", "server.watchdog_mega_miss"},
        {"server.watchdog_miss", "server.watchdog_miss"},
        {"http.async-client.rq_total", "http.rq_total"},
        {"cluster_manager.cluster_added", "cluster_manager.cluster_added"},
        {"http.admin.downstream_rq_http2_total", "http.downstream_rq_http2_total"},
        {"cluster_manager.cluster_removed", "cluster_manager.cluster_removed"},
        {"http.admin.downstream_cx_destroy_remote", "http.downstream_cx_destroy_remote"},
        {"http.admin.downstream_rq_http1_total", "http.downstream_rq_http1_total"},
        {"http.admin.tracing.tracing.client_enabled", "http.tracing.tracing.client_enabled"},
        {"http.admin.downstream_rq_total", "http.downstream_rq_total"},
        {"http.admin.tracing.tracing.service_forced", "http.tracing.tracing.service_forced"},
        {"http.admin.tracing.tracing.not_traceable", "http.tracing.tracing.not_traceable"},
        {"http.admin.downstream_cx_rx_bytes_total", "http.downstream_cx_rx_bytes_total"},
        {"http.async-client.no_cluster", "http.no_cluster"},
        {"http.admin.downstream_cx_destroy_remote_active_rq",
         "http.downstream_cx_destroy_remote_active_rq"},
        {"http.admin.downstream_cx_destroy_local_active_rq",
         "http.downstream_cx_destroy_local_active_rq"},
        {"filesystem.write_buffered", "filesystem.write_buffered"},
        {"http.admin.downstream_cx_destroy_active_rq", "http.downstream_cx_destroy_active_rq"},
        {"http.admin.downstream_rq_tx_reset", "http.downstream_rq_tx_reset"},
        {"http.admin.downstream_flow_control_resumed_reading_total",
         "http.downstream_flow_control_resumed_reading_total"},
        {"stats.overflow", "stats.overflow"},
        {"http.admin.downstream_cx_total", "http.downstream_cx_total"},
        {"http.admin.downstream_rq_3xx", "http.downstream_rq_xx"},
        {"http.admin.downstream_cx_idle_timeout", "http.downstream_cx_idle_timeout"},
        {"http.admin.downstream_rq_rx_reset", "http.downstream_rq_rx_reset"},
        {"http.admin.downstream_cx_ssl_total", "http.downstream_cx_ssl_total"},
        {"http.admin.downstream_cx_websocket_total", "http.downstream_cx_websocket_total"},
        {"http.admin.downstream_rq_2xx", "http.downstream_rq_xx"},
        {"cluster_manager.cluster_modified", "cluster_manager.cluster_modified"},
        {"http.admin.downstream_cx_drain_close", "http.downstream_cx_drain_close"},
        {"http.admin.downstream_cx_destroy", "http.downstream_cx_destroy"},
        {"http.admin.downstream_cx_http1_total", "http.downstream_cx_http1_total"},
        {"http.admin.downstream_cx_protocol_error", "http.downstream_cx_protocol_error"},
        {"http.admin.downstream_cx_destroy_local", "http.downstream_cx_destroy_local"},
        {"listener_manager.listener_added", "listener_manager.listener_added"},
        {"filesystem.write_completed", "filesystem.write_completed"},
        {"http.admin.downstream_rq_response_before_rq_complete",
         "http.downstream_rq_response_before_rq_complete"},
        {"http.admin.downstream_cx_tx_bytes_total", "http.downstream_cx_tx_bytes_total"},
        {"http.admin.downstream_rq_4xx", "http.downstream_rq_xx"},
        {"http.admin.downstream_rq_non_relative_path", "http.downstream_rq_non_relative_path"},
        {"http.admin.downstream_rq_ws_on_non_ws_route", "http.downstream_rq_ws_on_non_ws_route"},
        {"http.admin.downstream_rq_too_large", "http.downstream_rq_too_large"},
        {"http.admin.downstream_rq_5xx", "http.downstream_rq_xx"},
        {"http.async-client.no_route", "http.no_route"},
        {"http.admin.downstream_flow_control_paused_reading_total",
         "http.downstream_flow_control_paused_reading_total"},
        {"listener_manager.listener_removed", "listener_manager.listener_removed"},
        {"listener_manager.listener_create_failure", "listener_manager.listener_create_failure"},
        {"http.admin.tracing.tracing.random_sampling", "http.tracing.tracing.random_sampling"},
        {"http.async-client.rq_redirect", "http.rq_redirect"},
        {"http.admin.tracing.tracing.health_check", "http.tracing.tracing.health_check"},
        {"filesystem.flushed_by_timer", "filesystem.flushed_by_timer"},
        {"http.admin.downstream_cx_http2_total", "http.downstream_cx_http2_total"},
        {"filesystem.reopen_failed", "filesystem.reopen_failed"},
        {"listener_manager.listener_modified", "listener_manager.listener_modified"},
        {"http.admin.rs_too_large", "http.rs_too_large"},
        {"listener_manager.listener_create_success", "listener_manager.listener_create_success"}};
    tag_extracted_gauge_map = {
        {"listener.[__1]_0.downstream_cx_active", "listener.downstream_cx_active"},
        {"http.router.downstream_rq_active", "http.downstream_rq_active"},
        {"http.router.downstream_cx_tx_bytes_buffered", "http.downstream_cx_tx_bytes_buffered"},
        {"http.router.downstream_cx_http2_active", "http.downstream_cx_http2_active"},
        {"http.router.downstream_cx_websocket_active", "http.downstream_cx_websocket_active"},
        {"http.router.downstream_cx_rx_bytes_buffered", "http.downstream_cx_rx_bytes_buffered"},
        {"http.router.downstream_cx_http1_active", "http.downstream_cx_http1_active"},
        {"http.router.downstream_cx_ssl_active", "http.downstream_cx_ssl_active"},
        {"http.router.downstream_cx_active", "http.downstream_cx_active"},
        {"cluster.cluster_2.membership_total", "cluster.membership_total"},
        {"cluster.cluster_2.membership_healthy", "cluster.membership_healthy"},
        {"cluster.cluster_2.max_host_weight", "cluster.max_host_weight"},
        {"cluster.cluster_2.upstream_rq_pending_active", "cluster.upstream_rq_pending_active"},
        {"cluster.cluster_2.version", "cluster.version"},
        {"cluster.cluster_2.upstream_rq_active", "cluster.upstream_rq_active"},
        {"cluster.cluster_2.upstream_cx_tx_bytes_buffered",
         "cluster.upstream_cx_tx_bytes_buffered"},
        {"cluster.cluster_2.upstream_cx_rx_bytes_buffered",
         "cluster.upstream_cx_rx_bytes_buffered"},
        {"cluster.cluster_2.upstream_cx_active", "cluster.upstream_cx_active"},
        {"cluster.cluster_1.upstream_rq_active", "cluster.upstream_rq_active"},
        {"cluster.cluster_1.upstream_rq_pending_active", "cluster.upstream_rq_pending_active"},
        {"cluster.cluster_1.upstream_cx_tx_bytes_buffered",
         "cluster.upstream_cx_tx_bytes_buffered"},
        {"cluster.cluster_1.max_host_weight", "cluster.max_host_weight"},
        {"cluster.cluster_1.upstream_cx_rx_bytes_buffered",
         "cluster.upstream_cx_rx_bytes_buffered"},
        {"cluster.cluster_1.version", "cluster.version"},
        {"cluster.cluster_1.membership_total", "cluster.membership_total"},
        {"cluster.cluster_1.membership_healthy", "cluster.membership_healthy"},
        {"cluster.cluster_1.upstream_cx_active", "cluster.upstream_cx_active"},
        {"listener.admin.downstream_cx_active", "listener.admin.downstream_cx_active"},
        {"cluster_manager.total_clusters", "cluster_manager.total_clusters"},
        {"listener_manager.total_listeners_warming", "listener_manager.total_listeners_warming"},
        {"listener_manager.total_listeners_active", "listener_manager.total_listeners_active"},
        {"http.admin.downstream_rq_active", "http.downstream_rq_active"},
        {"http.admin.downstream_cx_tx_bytes_buffered", "http.downstream_cx_tx_bytes_buffered"},
        {"http.admin.downstream_cx_rx_bytes_buffered", "http.downstream_cx_rx_bytes_buffered"},
        {"http.admin.downstream_cx_websocket_active", "http.downstream_cx_websocket_active"},
        {"http.admin.downstream_cx_http1_active", "http.downstream_cx_http1_active"},
        {"server.uptime", "server.uptime"},
        {"server.memory_allocated", "server.memory_allocated"},
        {"http.admin.downstream_cx_http2_active", "http.downstream_cx_http2_active"},
        {"server.memory_heap_size", "server.memory_heap_size"},
        {"listener_manager.total_listeners_draining", "listener_manager.total_listeners_draining"},
        {"filesystem.write_total_buffered", "filesystem.write_total_buffered"},
        {"http.admin.downstream_cx_ssl_active", "http.downstream_cx_ssl_active"},
        {"http.admin.downstream_cx_active", "http.downstream_cx_active"},
        {"server.live", "server.live"},
        {"server.parent_connections", "server.parent_connections"},
        {"server.total_connections", "server.total_connections"},
        {"server.days_until_first_cert_expiring", "server.days_until_first_cert_expiring"},
        {"server.version", "server.version"}};
    break;
  }
  default:
    break;
  }

  auto test_name_against_mapping =
      [](const std::unordered_map<std::string, std::string>& extracted_name_map,
         const Stats::Metric& metric) {
        auto it = extracted_name_map.find(metric.name());
        // Ignore any metrics that are not found in the map for ease of addition
        if (it != extracted_name_map.end()) {
          // Check that the tag extracted name matches the "golden" state.
          EXPECT_EQ(it->second, metric.tagExtractedName());
        }
      };

  for (const Stats::CounterSharedPtr& counter : test_server_->counters()) {
    test_name_against_mapping(tag_extracted_counter_map, *counter);
  }

  for (const Stats::GaugeSharedPtr& gauge : test_server_->gauges()) {
    test_name_against_mapping(tag_extracted_gauge_map, *gauge);
  }
}
} // namespace Xfcc
} // namespace Envoy
