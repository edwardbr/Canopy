/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc_objects/text_tools/text_tools_impl.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace text_tools::v1
{
    namespace
    {
        // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters): generated IDL uses output refs.
        class text_tools_impl final : public rpc::base<text_tools_impl, i_text_tools>
        {
        public:
            text_tools_impl() = default;

            explicit text_tools_impl(std::shared_ptr<rpc::service> service)
                : service_(std::move(service))
            {
            }

            CORO_TASK(text_tools_error)
            concat(
                const std::string& left,
                const std::string& right,
                std::string& combined) override
            {
                combined = left + right;
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(text_tools_error)
            uppercase(
                const std::string& input,
                std::string& output) override
            {
                output = input;
                std::transform(
                    output.begin(),
                    output.end(),
                    output.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                CO_RETURN rpc::error::OK();
            }

        private:
            std::weak_ptr<rpc::service> service_;
        };
        // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    }

    rpc::shared_ptr<i_text_tools> make_text_tools()
    {
        return rpc::shared_ptr<i_text_tools>(new text_tools_impl());
    }

    rpc::shared_ptr<i_text_tools> make_text_tools(std::shared_ptr<rpc::service> service)
    {
        return rpc::shared_ptr<i_text_tools>(new text_tools_impl(std::move(service)));
    }

    rpc::module::object_factory<
        i_text_tools,
        i_text_tools>
    make_text_tools_factory()
    {
        return rpc::module::make_object_factory_with_service<i_text_tools, i_text_tools>(
            [](std::shared_ptr<rpc::service> service) { return make_text_tools(std::move(service)); });
    }
}
