/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rest_fixture/rest_fixture.h>

#include <gtest/gtest.h>

#include <canopy/rest/connection.h>
#include <canopy/rest/server.h>
#include <connection_factory/connection_factory.h>
#include <json/config.h>
#include <rpc/rpc.h>
#include <streaming/http_client/client.h>
#include <streaming/stream.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace todos = todoapi::v1::todos;

namespace
{
    std::string http_response(
        int status,
        std::string reason,
        std::string body)
    {
        return "HTTP/1.1 " + std::to_string(status) + " " + std::move(reason)
               + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n"
               + std::move(body);
    }

    class scripted_stream final : public streaming::stream
    {
    public:
        explicit scripted_stream(std::vector<std::string> responses)
            : responses_(std::move(responses))
        {
        }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds) -> CORO_TASK(streaming::receive_result) override
        {
            if (responses_.empty())
                CO_RETURN std::make_pair(rpc::io_status{FLD(type) rpc::io_status::kind::closed}, rpc::mutable_byte_span{});

            ++receive_count_;
            auto response = std::move(responses_.front());
            responses_.erase(responses_.begin());
            const auto byte_count = std::min(buffer.size(), response.size());
            std::copy(response.data(), response.data() + byte_count, buffer.data());
            CO_RETURN std::make_pair(
                rpc::io_status{FLD(type) rpc::io_status::kind::ok}, rpc::mutable_byte_span{buffer.data(), byte_count});
        }

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
        {
            requests_.emplace_back(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
        }

        [[nodiscard]] bool is_closed() const override { return closed_; }

        auto set_closed() -> CORO_TASK(void) override
        {
            closed_ = true;
            CO_RETURN;
        }

        [[nodiscard]] streaming::peer_info get_peer_info() const override { return {}; }

        [[nodiscard]] const std::vector<std::string>& requests() const { return requests_; }
        [[nodiscard]] size_t receive_count() const { return receive_count_; }

    private:
        std::vector<std::string> responses_;
        std::vector<std::string> requests_;
        size_t receive_count_{0};
        bool closed_{false};
    };

    CORO_TASK(bool)
    run_remote_no_output_rest_call_test(
        std::shared_ptr<rpc::service> service,
        rpc::executor_ptr expected_child_executor = {})
    {
        auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{
            http_response(204, "No Content", ""),
            http_response(200, "OK", ""),
        });

        auto settings = rpc::connection_factory::materialise_connection_settings(json::v1::parse(R"json({
            "transport": {
                "type": "local",
                "settings": { "name": "remote_rest_child", "encoding": "yas_json" }
            }
        })json"));
        if (settings.error_code != rpc::error::OK())
            CO_RETURN false;

        bool child_executor_matches = true;
        rpc::shared_ptr<rpc::i_noop> no_parent_interface;
        auto connect_result = CO_AWAIT rpc::connection_factory::connect_local_child_rpc<rpc::i_noop, todos::i_todos>(
            std::move(no_parent_interface),
            [stream, expected_child_executor, &child_executor_matches](
                rpc::shared_ptr<rpc::i_noop>,
                std::shared_ptr<rpc::service> child_service) -> CORO_TASK(rpc::service_connect_result<todos::i_todos>)
            {
                if (expected_child_executor)
                    child_executor_matches = child_service && child_service->get_executor() == expected_child_executor;
                todos::i_todos::rest_settings rest_settings;
                std::shared_ptr<streaming::stream> base_stream = stream;
                rpc::shared_ptr<todos::i_todos> caller(
                    new todos::i_todos::rest_caller(std::move(base_stream), std::move(rest_settings)));
                CO_RETURN rpc::service_connect_result<todos::i_todos>{rpc::error::OK(), std::move(caller)};
            },
            settings.settings,
            std::move(service));

        if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            CO_RETURN false;

        const auto delete_error = CO_AWAIT connect_result.output_interface->delete_todo("remote 1");
        if (delete_error != rpc::error::OK())
            CO_RETURN false;

        const auto head_error = CO_AWAIT connect_result.output_interface->head_todo("remote 1");
        if (head_error != rpc::error::OK())
            CO_RETURN false;

        const auto& requests = stream->requests();
        CO_RETURN child_executor_matches&& requests.size() == 2U
            && requests[0]
                   == "DELETE /todoapi/v1/todos/remote%201 HTTP/1.1\r\n"
                      "Host: example.test\r\n"
                      "Accept: application/json\r\n"
                      "Connection: close\r\n"
                      "\r\n"
            && requests[1]
                   == "HEAD /todoapi/v1/todos/remote%201 HTTP/1.1\r\n"
                      "Host: example.test\r\n"
                      "Accept: application/json\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    }
}

TEST(
    RestConnectionSettings,
    DefaultsVerifyTlsPeers)
{
    canopy::rest::connection_settings settings;
    EXPECT_TRUE(settings.tls.verify_peer);
}

TEST(
    RestConnectionSettings,
    SchemesAreCaseInsensitive)
{
    canopy::rest::authority endpoint;
    endpoint.host = "example.test";
    endpoint.scheme = "HTTPS";
    EXPECT_TRUE(canopy::rest::uses_tls(endpoint));
    EXPECT_EQ(canopy::rest::effective_port(endpoint), 443);
    EXPECT_EQ(canopy::rest::http_host(endpoint), "example.test");

    endpoint.scheme = "Http";
    EXPECT_FALSE(canopy::rest::uses_tls(endpoint));
    EXPECT_EQ(canopy::rest::effective_port(endpoint), 80);
    EXPECT_EQ(canopy::rest::http_host(endpoint), "example.test");
}

TEST(
    RestConnectionSettings,
    RejectsMalformedDefaultCookies)
{
    canopy::rest::connection_settings settings;
    streaming::http_client::request request;
    request.target = "/";

    settings.default_cookies.push_back({"session", "value; injected=yes"});
    EXPECT_EQ(canopy::rest::prepare_request(request, settings), rpc::error::INVALID_DATA());
}

TEST(
    RestHttpClient,
    RejectsHeaderInjectionWhenBuildingRequests)
{
    streaming::http_client::request request;
    request.method = "GET";
    request.target = "/";
    request.host = "example.test";
    request.headers.push_back({"X-Test", "safe\r\nInjected: yes"});

    EXPECT_THROW((void)streaming::http_client::build_request(request), std::runtime_error);
}

TEST(
    RestHttpClient,
    RejectsCompleteResponseThatExceedsMaxResponseBytes)
{
    const auto response = http_response(200, "OK", R"({"ok":true})");
    auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{response});

    streaming::http_client::request request;
    request.host = "example.test";
    request.target = "/status";

    auto result = SYNC_WAIT(
        streaming::http_client::send_request(
            std::move(stream), request, std::chrono::milliseconds{10000}, response.size() - 1));

    EXPECT_EQ(result.error_code, rpc::error::INVALID_DATA());
    EXPECT_NE(result.error_message.find("exceeded maximum size"), std::string::npos);
}

TEST(
    RestHttpClient,
    AcceptsCompleteResponseAtExactMaxResponseBytes)
{
    const auto response = http_response(200, "OK", R"({"ok":true})");
    auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{response});

    streaming::http_client::request request;
    request.host = "example.test";
    request.target = "/status";

    auto result = SYNC_WAIT(
        streaming::http_client::send_request(std::move(stream), request, std::chrono::milliseconds{10000}, response.size()));

    EXPECT_EQ(result.error_code, rpc::error::OK());
    EXPECT_EQ(result.value.body, R"({"ok":true})");
}

TEST(
    RestHttpClient,
    WaitsForCompleteChunkedBodyBeforeParsing)
{
    auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\n"
        "Wiki\r\n"
        "0\r\n",
        "\r\n",
    });

    streaming::http_client::request request;
    request.host = "example.test";
    request.target = "/chunked";

    auto result = SYNC_WAIT(streaming::http_client::send_request(stream, request, std::chrono::milliseconds{10000}, 1024));

    EXPECT_EQ(result.error_code, rpc::error::OK());
    EXPECT_EQ(result.value.body, "Wiki");
    EXPECT_EQ(stream->receive_count(), size_t{2});
}

TEST(
    RestServerPathMatching,
    MatchesEmbeddedPathParameters)
{
    const auto package = canopy::rest::match_path_template(
        "/etc/packages/group/name-version.zip", "/etc/packages/{group}/{name}-{version}.zip");
    ASSERT_TRUE(package.matched);
    EXPECT_EQ(package.parameters.at("group"), "group");
    EXPECT_EQ(package.parameters.at("name"), "name");
    EXPECT_EQ(package.parameters.at("version"), "version");

    const auto nested = canopy::rest::match_path_template(
        "/etc/packages/group/name-version.zip/jcr:content/vlt:definition/filter.tidy.2.json",
        "/etc/packages/{group}/{name}-{version}.zip/jcr:content/vlt:definition/filter.tidy.2.json");
    ASSERT_TRUE(nested.matched);
    EXPECT_EQ(nested.parameters.at("group"), "group");
    EXPECT_EQ(nested.parameters.at("name"), "name");
    EXPECT_EQ(nested.parameters.at("version"), "version");

    EXPECT_FALSE(
        canopy::rest::match_path_template("/etc/packages/group/name-version.tar", "/etc/packages/{group}/{name}-{version}.zip")
            .matched);
}

TEST(
    RestClientGeneration,
    GeneratedCallerBuildsHttpAndDeserializesResponses)
{
    auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{
        http_response(200, "OK", R"({"id":"abc 1","title":"Write tests","completed":false,"notes":"hello"})"),
        http_response(200, "OK", R"({"id":"abc 1","title":"Replace item","completed":true,"notes":"replace note"})"),
        http_response(200, "OK", R"({"id":"abc 1","title":"Patched item","completed":true,"notes":"hello"})"),
        http_response(204, "No Content", ""),
        http_response(200, "OK", ""),
        http_response(201, "Created", R"({"id":"new-1","title":"New item","completed":false,"notes":"draft"})"),
        http_response(200, "OK", R"({"id":"todos","action":"GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS"})"),
        http_response(200, "OK", R"({"id":"abc 1","source":"fixture","changes":["created","updated"]})"),
        http_response(200, "OK", R"({"id":"abc 1","title":"Write tests","completed":false})"),
    });

    todos::i_todos::rest_settings settings;
    settings.default_query_parameters.push_back({"api_key", "test-key"});
    settings.default_headers.push_back({"Authorization", "Bearer test-token"});
    settings.default_cookies.push_back({"session", "cookie-value"});
    settings.before_send = [](streaming::http_client::request& request)
    {
        request.headers.push_back({"X-Trace-Id", "trace-1"});
        return rpc::error::OK();
    };

    std::shared_ptr<streaming::stream> base_stream = stream;
    rpc::shared_ptr<todos::i_todos> caller(new todos::i_todos::rest_caller(std::move(base_stream), std::move(settings)));
    ASSERT_TRUE(caller);
    EXPECT_TRUE(caller->__rpc_is_local());
    EXPECT_NE(caller->__rpc_query_interface(todos::i_todos::get_id(rpc::get_version())), nullptr);

    std::string fetched_id;
    std::string fetched_title;
    bool fetched_completed{};
    rpc::optional<std::string> fetched_notes;
    const auto get_error = SYNC_WAIT(caller->get_todo(
        "abc 1", rpc::optional<bool>{true}, fetched_id, fetched_title, fetched_completed, fetched_notes));
    EXPECT_EQ(get_error, rpc::error::OK());
    EXPECT_EQ(fetched_id, "abc 1");
    EXPECT_EQ(fetched_title, "Write tests");
    EXPECT_FALSE(fetched_completed);
    ASSERT_TRUE(fetched_notes.has_value());
    EXPECT_EQ(fetched_notes.value(), "hello");

    todos::todo_create replacement;
    replacement.title = "Replace item";
    replacement.completed = true;
    replacement.notes = std::string("replace note");

    std::string replaced_id;
    std::string replaced_title;
    bool replaced_completed{};
    rpc::optional<std::string> replaced_notes;
    const auto put_error = SYNC_WAIT(
        caller->put_replace_todo("abc 1", replacement, replaced_id, replaced_title, replaced_completed, replaced_notes));
    EXPECT_EQ(put_error, rpc::error::OK());
    EXPECT_EQ(replaced_id, "abc 1");
    EXPECT_EQ(replaced_title, "Replace item");
    EXPECT_TRUE(replaced_completed);
    ASSERT_TRUE(replaced_notes.has_value());
    EXPECT_EQ(replaced_notes.value(), "replace note");

    todos::todo_patch patch;
    patch.title = std::string("Patched item");
    patch.completed = true;

    std::string patched_id;
    std::string patched_title;
    bool patched_completed{};
    rpc::optional<std::string> patched_notes;
    const auto patch_error
        = SYNC_WAIT(caller->patch_todo("abc 1", patch, patched_id, patched_title, patched_completed, patched_notes));
    EXPECT_EQ(patch_error, rpc::error::OK());
    EXPECT_EQ(patched_id, "abc 1");
    EXPECT_EQ(patched_title, "Patched item");
    EXPECT_TRUE(patched_completed);

    const auto delete_error = SYNC_WAIT(caller->delete_todo("abc 1"));
    EXPECT_EQ(delete_error, rpc::error::OK());

    const auto head_error = SYNC_WAIT(caller->head_todo("abc 1"));
    EXPECT_EQ(head_error, rpc::error::OK());

    todos::todo_create create;
    create.title = "New item";
    create.completed = false;
    create.notes = std::string("draft");

    std::string created_id;
    std::string created_title;
    bool created_completed{};
    rpc::optional<std::string> created_notes;
    const auto post_error
        = SYNC_WAIT(caller->post_create_todo(create, created_id, created_title, created_completed, created_notes));
    EXPECT_EQ(post_error, rpc::error::OK());
    EXPECT_EQ(created_id, "new-1");
    EXPECT_EQ(created_title, "New item");
    EXPECT_FALSE(created_completed);
    ASSERT_TRUE(created_notes.has_value());
    EXPECT_EQ(created_notes.value(), "draft");

    std::string options_id;
    std::string options_action;
    const auto options_error = SYNC_WAIT(caller->options_todos(options_id, options_action));
    EXPECT_EQ(options_error, rpc::error::OK());
    EXPECT_EQ(options_id, "todos");
    EXPECT_EQ(options_action, "GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS");

    std::string audit_id;
    std::string audit_source;
    rpc::optional<std::vector<std::string>> audit_changes;
    const auto audit_error = SYNC_WAIT(caller->get_todo_audit("abc 1", audit_id, audit_source, audit_changes));
    EXPECT_EQ(audit_error, rpc::error::OK());
    EXPECT_EQ(audit_id, "abc 1");
    EXPECT_EQ(audit_source, "fixture");
    ASSERT_TRUE(audit_changes.has_value());
    ASSERT_EQ(audit_changes.value().size(), 2U);
    EXPECT_EQ(audit_changes.value()[0], "created");
    EXPECT_EQ(audit_changes.value()[1], "updated");

    json::v1::object state;
    const auto state_error = SYNC_WAIT(caller->get_todo_state("abc 1", state));
    EXPECT_EQ(state_error, rpc::error::OK());
    ASSERT_EQ(state.get_type(), json::v1::object::type::map_type);
    const auto& state_map = state.as_map();
    EXPECT_EQ(state_map.at("id").get<std::string>(), "abc 1");
    EXPECT_EQ(state_map.at("title").get<std::string>(), "Write tests");
    EXPECT_FALSE(state_map.at("completed").get<bool>());

    ASSERT_EQ(stream->requests().size(), 9U);
    EXPECT_EQ(
        stream->requests()[0],
        "GET /todoapi/v1/todos/abc%201?include_notes=true&api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");

    const std::string expected_put_body = R"({"completed":true,"notes":"replace note","title":"Replace item"})";
    EXPECT_EQ(
        stream->requests()[1],
        "PUT /todoapi/v1/todos/abc%201?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Content-Length: "
            + std::to_string(expected_put_body.size())
            + "\r\n"
              "Connection: close\r\n"
              "\r\n"
            + expected_put_body);

    const std::string expected_patch_body = R"({"completed":true,"title":"Patched item"})";
    EXPECT_EQ(
        stream->requests()[2],
        "PATCH /todoapi/v1/todos/abc%201?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Content-Length: "
            + std::to_string(expected_patch_body.size())
            + "\r\n"
              "Connection: close\r\n"
              "\r\n"
            + expected_patch_body);

    EXPECT_EQ(
        stream->requests()[3],
        "DELETE /todoapi/v1/todos/abc%201?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_EQ(
        stream->requests()[4],
        "HEAD /todoapi/v1/todos/abc%201?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");

    const std::string expected_post_body = R"({"completed":false,"notes":"draft","title":"New item"})";
    EXPECT_EQ(
        stream->requests()[5],
        "POST /todoapi/v1/todos?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Content-Length: "
            + std::to_string(expected_post_body.size())
            + "\r\n"
              "Connection: close\r\n"
              "\r\n"
            + expected_post_body);

    EXPECT_EQ(
        stream->requests()[6],
        "OPTIONS /todoapi/v1/todos/options?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_EQ(
        stream->requests()[7],
        "GET /todoapi/v1/todos/abc%201/audit?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");

    EXPECT_EQ(
        stream->requests()[8],
        "GET /todoapi/v1/todos/abc%201/state?api_key=test-key HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "Cookie: session=cookie-value\r\n"
        "X-Trace-Id: trace-1\r\n"
        "Connection: close\r\n"
        "\r\n");
}

TEST(
    RestClientGeneration,
    GeneratedCallerCanConnectThroughRestConnectionFactory)
{
    auto stream = std::make_shared<scripted_stream>(std::vector<std::string>{
        http_response(200, "OK", R"({"id":"factory","title":"Factory stream","completed":true,"notes":null})"),
    });

    rpc::connection_factory::context factory_context;
    factory_context.register_connect_base_stream<rpc::connection_factory::service_settings>(
        "scripted_rest",
        [stream](
            rpc::connection_factory::service_settings,
            std::shared_ptr<rpc::service>,
            const rpc::connection_factory::context&) -> CORO_TASK(rpc::connection_factory::stream_result)
        { CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(), stream}; });

    rpc::stream_layers::stream_layer_settings layer;
    layer.type = "scripted_rest";

    todos::i_todos::rest_settings settings;
    settings.endpoint.host = "factory.test";
    settings.stream_connection.stream_layers.push_back(layer);

    auto connected = SYNC_WAIT(todos::i_todos::rest_caller::connect(std::move(settings), {}, std::move(factory_context)));
    ASSERT_EQ(connected.error_code, rpc::error::OK());
    ASSERT_TRUE(connected.object);

    std::string fetched_id;
    std::string fetched_title;
    bool fetched_completed{};
    rpc::optional<std::string> fetched_notes;
    const auto get_error = SYNC_WAIT(
        connected.object->get_todo("factory", {}, fetched_id, fetched_title, fetched_completed, fetched_notes));
    EXPECT_EQ(get_error, rpc::error::OK());
    EXPECT_EQ(fetched_id, "factory");
    EXPECT_EQ(fetched_title, "Factory stream");
    EXPECT_TRUE(fetched_completed);
    EXPECT_FALSE(fetched_notes.has_value());

    ASSERT_EQ(stream->requests().size(), 1U);
    EXPECT_EQ(
        stream->requests()[0],
        "GET /todoapi/v1/todos/factory HTTP/1.1\r\n"
        "Host: factory.test\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n");
}

TEST(
    RestClientGeneration,
    NoOutputRestMethodsCanBeCalledThroughARemoteRpcPointer)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));
    auto service = rpc::root_service::create("remote_rest_parent", rpc::DEFAULT_PREFIX, scheduler);

    std::atomic_bool done{false};
    bool passed = false;
    auto runner = [&]() -> CORO_TASK(void)
    {
        passed = CO_AWAIT run_remote_no_output_rest_call_test(std::move(service), scheduler);
        done.store(true, std::memory_order_release);
        CO_RETURN;
    };

    ASSERT_TRUE(scheduler->spawn_detached(runner()));
    for (int i = 0; i < 3000 && !done.load(std::memory_order_acquire); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});

    EXPECT_TRUE(done.load(std::memory_order_acquire));
    EXPECT_TRUE(passed);

    for (int i = 0; i < 100 && !scheduler->empty(); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});
#else
    auto service = rpc::root_service::create("remote_rest_parent", rpc::DEFAULT_PREFIX);
    EXPECT_TRUE(run_remote_no_output_rest_call_test(std::move(service)));
#endif
}
