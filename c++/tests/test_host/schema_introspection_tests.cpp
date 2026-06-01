/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <set>
#include <string>
#include <vector>

#include <common/foo_impl.h>
#include <gtest/gtest.h>
#include <rpc/rpc.h>

// Exercises the Phase 2 runtime schema-introspection fold:
// casting_interface::__rpc_enumerate_schemas, overridden by rpc::base to fold
// over its compile-time interface pack. marshalled_tests::baz derives
// rpc::base<baz, xxx::i_baz, xxx::i_bar>, so a single object should enumerate
// exactly those two interfaces.
namespace
{
    TEST(schema_introspection_tests, base_enumerates_all_implemented_interfaces)
    {
        marshalled_tests::baz obj;

        std::vector<rpc::interface_descriptor> descriptors;
        obj.__rpc_enumerate_schemas(
            rpc::encoding::yas_json, rpc::schema_flavor::config, /*include_deprecated*/ false, descriptors);

        ASSERT_EQ(descriptors.size(), 2u);

        std::set<std::string> names;
        for (const auto& descriptor : descriptors)
        {
            names.insert(descriptor.qualified_name);
            // interface_id is the versioned fingerprint; never zero for a real interface.
            EXPECT_NE(descriptor.interface_id.get_val(), 0u);
            // i_baz and i_bar both declare methods, so the per-method table is populated.
            EXPECT_FALSE(descriptor.methods.empty());
            // Each method carries the data a caller relays through send().
            for (const auto& method : descriptor.methods)
            {
                EXPECT_FALSE(method.name.empty());
                EXPECT_NE(method.id.get_val(), 0u);
            }
        }

        EXPECT_EQ(names.count("xxx::i_baz"), 1u);
        EXPECT_EQ(names.count("xxx::i_bar"), 1u);
    }

    TEST(schema_introspection_tests, interface_proxy_describes_single_interface)
    {
        // The local interface_proxy<T> path (Tier 1): describing a single
        // generated interface type yields exactly one descriptor naming it.
        std::vector<rpc::interface_descriptor> descriptors;
        rpc::append_interface_descriptor<xxx::i_baz>(
            rpc::encoding::yas_json, rpc::schema_flavor::config, /*include_deprecated*/ false, descriptors);

        ASSERT_EQ(descriptors.size(), 1u);
        EXPECT_EQ(descriptors.front().qualified_name, "xxx::i_baz");
        EXPECT_NE(descriptors.front().interface_id.get_val(), 0u);
        EXPECT_FALSE(descriptors.front().methods.empty());
    }
}
