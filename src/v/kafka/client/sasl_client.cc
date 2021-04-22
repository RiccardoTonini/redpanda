/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */
#include "kafka/client/sasl_client.h"

namespace kafka::client {

ss::future<> do_sasl_handshake(shared_broker_t broker, ss::sstring mechanism) {
    sasl_handshake_request req;
    req.data.mechanism = std::move(mechanism);

    auto resp = co_await broker->dispatch(req);
    if (resp.data.error_code != error_code::none) {
        throw broker_error{broker->id(), resp.data.error_code};
    }
}

static ss::future<security::server_first_message> send_scram_client_first(
  const shared_broker_t& broker,
  const security::client_first_message& client_first) {
    sasl_authenticate_request client_first_req;
    {
        auto msg = client_first.message();
        client_first_req.data.auth_bytes = bytes(msg.cbegin(), msg.cend());
    }
    auto client_first_resp = co_await broker->dispatch(client_first_req);
    if (client_first_resp.data.error_code != error_code::none) {
        throw broker_error{
          broker->id(),
          client_first_resp.data.error_code,
          client_first_resp.data.error_message.value_or("<no error message>")};
    }
    co_return security::server_first_message(client_first_resp.data.auth_bytes);
}

static ss::future<security::server_final_message> send_scram_client_final(
  const shared_broker_t& broker,
  const security::client_final_message& client_final) {
    sasl_authenticate_request client_last_req;
    {
        auto msg = client_final.message();
        client_last_req.data.auth_bytes = bytes(msg.cbegin(), msg.cend());
    }

    auto client_last_resp = co_await broker->dispatch(client_last_req);

    if (client_last_resp.data.error_code != error_code::none) {
        throw broker_error{
          broker->id(),
          client_last_resp.data.error_code,
          client_last_resp.data.error_message.value_or("<no error message>")};
    }

    co_return security::server_final_message(
      std::move(client_last_resp.data.auth_bytes));
}

template<typename ScramAlgo>
static ss::future<> do_authenticate_scram(
  shared_broker_t broker, ss::sstring username, ss::sstring password) {
    /*
     * send client first message
     */
    const auto nonce = random_generators::gen_alphanum_string(130);
    const security::client_first_message client_first(username, nonce);

    /*
     * handle server first response
     */
    const auto server_first = co_await send_scram_client_first(
      broker, client_first);

    if (!std::string_view(server_first.nonce())
           .starts_with(std::string_view(nonce))) {
        throw broker_error{
          broker->id(),
          error_code::sasl_authentication_failed,
          "Server nonce doesn't match client nonce"};
    }

    if (server_first.iterations() < ScramAlgo::min_iterations) {
        throw broker_error{
          broker->id(),
          error_code::sasl_authentication_failed,
          fmt_with_ctx(
            ssx::sformat,
            "Server minimum iterations {} < required {}",
            server_first.iterations(),
            ScramAlgo::min_iterations)};
    }

    /*
     * send client final message
     */
    security::client_final_message client_final(
      bytes("n,,"), server_first.nonce());

    auto salted_password = ScramAlgo::hi(
      bytes(password.cbegin(), password.cend()),
      server_first.salt(),
      server_first.iterations());

    client_final.set_proof(ScramAlgo::client_proof(
      salted_password, client_first, server_first, client_final));

    const auto server_final = co_await send_scram_client_final(
      broker, client_final);

    /*
     * handle server final response
     */
    if (server_final.error()) {
        throw broker_error{
          broker->id(),
          error_code::sasl_authentication_failed,
          server_final.error().value()};
    }

    auto server_key = ScramAlgo::server_key(salted_password);
    auto server_sig = ScramAlgo::server_signature(
      server_key, client_first, server_first, client_final);

    if (server_final.signature() != server_sig) {
        throw broker_error{
          broker->id(),
          error_code::sasl_authentication_failed,
          "Server signature does not match calculated signature"};
    }
}

ss::future<> do_authenticate_scram256(
  shared_broker_t broker, ss::sstring username, ss::sstring password) {
    return do_authenticate_scram<security::scram_sha256>(
      std::move(broker), std::move(username), std::move(password));
}

ss::future<> do_authenticate_scram512(
  shared_broker_t broker, ss::sstring username, ss::sstring password) {
    return do_authenticate_scram<security::scram_sha512>(
      std::move(broker), std::move(username), std::move(password));
}

} // namespace kafka::client
