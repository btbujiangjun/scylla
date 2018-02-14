/*
 * Copyright (C) 2017 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "auth/service.hh"

#include <map>

#include <seastar/core/future-util.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_ptr.hh>

#include "auth/allow_all_authenticator.hh"
#include "auth/allow_all_authorizer.hh"
#include "auth/common.hh"
#include "auth/password_authenticator.hh"
#include "auth/standard_role_manager.hh"
#include "cql3/query_processor.hh"
#include "cql3/untyped_result_set.hh"
#include "db/config.hh"
#include "db/consistency_level.hh"
#include "exceptions/exceptions.hh"
#include "log.hh"
#include "service/migration_listener.hh"
#include "utils/class_registrator.hh"

namespace auth {

namespace meta {

static const sstring user_name_col_name("name");
static const sstring superuser_col_name("super");

}

static logging::logger log("auth_service");

class auth_migration_listener final : public ::service::migration_listener {
    authorizer& _authorizer;

public:
    explicit auth_migration_listener(authorizer& a) : _authorizer(a) {
    }

private:
    void on_create_keyspace(const sstring& ks_name) override {}
    void on_create_column_family(const sstring& ks_name, const sstring& cf_name) override {}
    void on_create_user_type(const sstring& ks_name, const sstring& type_name) override {}
    void on_create_function(const sstring& ks_name, const sstring& function_name) override {}
    void on_create_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    void on_create_view(const sstring& ks_name, const sstring& view_name) override {}

    void on_update_keyspace(const sstring& ks_name) override {}
    void on_update_column_family(const sstring& ks_name, const sstring& cf_name, bool) override {}
    void on_update_user_type(const sstring& ks_name, const sstring& type_name) override {}
    void on_update_function(const sstring& ks_name, const sstring& function_name) override {}
    void on_update_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    void on_update_view(const sstring& ks_name, const sstring& view_name, bool columns_changed) override {}

    void on_drop_keyspace(const sstring& ks_name) override {
        _authorizer.revoke_all(auth::make_data_resource(ks_name));
    }

    void on_drop_column_family(const sstring& ks_name, const sstring& cf_name) override {
        _authorizer.revoke_all(auth::make_data_resource(ks_name, cf_name));
    }

    void on_drop_user_type(const sstring& ks_name, const sstring& type_name) override {}
    void on_drop_function(const sstring& ks_name, const sstring& function_name) override {}
    void on_drop_aggregate(const sstring& ks_name, const sstring& aggregate_name) override {}
    void on_drop_view(const sstring& ks_name, const sstring& view_name) override {}
};

static future<> validate_role_exists(const service& ser, stdx::string_view role_name) {
    return ser.underlying_role_manager().exists(role_name).then([role_name](bool exists) {
        if (!exists) {
            throw nonexistant_role(role_name);
        }
    });
}

service_config service_config::from_db_config(const db::config& dc) {
    const qualified_name qualified_authorizer_name(meta::AUTH_PACKAGE_NAME, dc.authorizer());
    const qualified_name qualified_authenticator_name(meta::AUTH_PACKAGE_NAME, dc.authenticator());
    const qualified_name qualified_role_manager_name(meta::AUTH_PACKAGE_NAME, dc.role_manager());

    service_config c;
    c.authorizer_java_name = qualified_authorizer_name;
    c.authenticator_java_name = qualified_authenticator_name;
    c.role_manager_java_name = qualified_role_manager_name;

    return c;
}

service::service(
        permissions_cache_config c,
        cql3::query_processor& qp,
        ::service::migration_manager& mm,
        std::unique_ptr<authorizer> z,
        std::unique_ptr<authenticator> a,
        std::unique_ptr<role_manager> r)
            : _permissions_cache_config(std::move(c))
            , _permissions_cache(nullptr)
            , _qp(qp)
            , _migration_manager(mm)
            , _authorizer(std::move(z))
            , _authenticator(std::move(a))
            , _role_manager(std::move(r))
            , _migration_listener(std::make_unique<auth_migration_listener>(*_authorizer)) {
    // The password authenticator requires that the `standard_role_manager` is running so that the roles metadata table
    // it manages is created and updated. This cross-module dependency is rather gross, but we have to maintain it for
    // the sake of compatibility with Apache Cassandra and its choice of auth. schema.
    if ((_authenticator->qualified_java_name() == password_authenticator_name())
            && (_role_manager->qualified_java_name() != standard_role_manager_name())) {
        throw incompatible_module_combination(
                sprint(
                        "The %s authenticator must be loaded alongside the %s role-manager.",
                        password_authenticator_name(),
                        standard_role_manager_name()));
    }
}

service::service(
        permissions_cache_config c,
        cql3::query_processor& qp,
        ::service::migration_manager& mm,
        const service_config& sc)
            : service(
                      std::move(c),
                      qp,
                      mm,
                      create_object<authorizer>(sc.authorizer_java_name, qp, mm),
                      create_object<authenticator>(sc.authenticator_java_name, qp, mm),
                      create_object<role_manager>(sc.role_manager_java_name, qp, mm)) {
}

future<> service::create_keyspace_if_missing() const {
    auto& db = _qp.db().local();

    if (!db.has_keyspace(meta::AUTH_KS)) {
        std::map<sstring, sstring> opts{{"replication_factor", "1"}};

        auto ksm = keyspace_metadata::new_keyspace(
                meta::AUTH_KS,
                "org.apache.cassandra.locator.SimpleStrategy",
                opts,
                true);

        // We use min_timestamp so that default keyspace metadata will loose with any manual adjustments.
        // See issue #2129.
        return _migration_manager.announce_new_keyspace(ksm, api::min_timestamp, false);
    }

    return make_ready_future<>();
}

future<> service::start() {
    return once_among_shards([this] {
        return create_keyspace_if_missing();
    }).then([this] {
        return _role_manager->start();
    }).then([this] {
        return when_all_succeed(_authorizer->start(), _authenticator->start());
    }).then([this] {
        _permissions_cache = std::make_unique<permissions_cache>(_permissions_cache_config, *this, log);
    }).then([this] {
        return once_among_shards([this] {
            _migration_manager.register_listener(_migration_listener.get());
            return make_ready_future<>();
        });
    });
}

future<> service::stop() {
    return _permissions_cache->stop().then([this] {
        return when_all_succeed(_role_manager->stop(), _authorizer->stop(), _authenticator->stop());
    });
}

future<bool> service::has_existing_legacy_users() const {
    if (!_qp.db().local().has_schema(meta::AUTH_KS, meta::USERS_CF)) {
        return make_ready_future<bool>(false);
    }

    static const sstring default_user_query = sprint(
            "SELECT * FROM %s.%s WHERE %s = ?",
            meta::AUTH_KS,
            meta::USERS_CF,
            meta::user_name_col_name);

    static const sstring all_users_query = sprint(
            "SELECT * FROM %s.%s LIMIT 1",
            meta::AUTH_KS,
            meta::USERS_CF);

    // This logic is borrowed directly from Apache Cassandra. By first checking for the presence of the default user, we
    // can potentially avoid doing a range query with a high consistency level.

    return _qp.process(
            default_user_query,
            db::consistency_level::ONE,
            {meta::DEFAULT_SUPERUSER_NAME},
            true).then([this](auto results) {
        if (!results->empty()) {
            return make_ready_future<bool>(true);
        }

        return _qp.process(
                default_user_query,
                db::consistency_level::QUORUM,
                {meta::DEFAULT_SUPERUSER_NAME},
                true).then([this](auto results) {
            if (!results->empty()) {
                return make_ready_future<bool>(true);
            }

            return _qp.process(
                    all_users_query,
                    db::consistency_level::QUORUM).then([](auto results) {
                return make_ready_future<bool>(!results->empty());
            });
        });
    });
}

future<permission_set> service::get_permissions(stdx::string_view role_name, const resource& r) const {
    return validate_role_exists(*this, role_name).then([this, role_name, &r] {
        return _permissions_cache->get(role_name, r);
    });
}

future<bool> service::has_superuser(stdx::string_view role_name) const {
    return this->get_roles(std::move(role_name)).then([this](std::unordered_set<sstring> roles) {
        return do_with(std::move(roles), [this](const std::unordered_set<sstring>& roles) {
            return do_with(false, roles.begin(), [this, &roles](bool& any_super, auto& iter) {
                return do_until(
                        [&roles, &any_super, &iter] { return any_super || (iter == roles.end()); },
                        [this, &any_super, &iter] {
                    return _role_manager->is_superuser(*iter++).then([&any_super](bool super) {
                        any_super = super;
                    });
                }).then([&any_super] {
                    return any_super;
                });
            });
        });
    });
}

future<std::unordered_set<sstring>> service::get_roles(stdx::string_view role_name) const {
    //
    // We may wish to cache this information in the future (as Apache Cassandra does).
    //

    return _role_manager->query_granted(role_name, recursive_role_query::yes);
}

//
// Free functions.
//

future<bool> has_superuser(const service& ser, const authenticated_user& u) {
    if (is_anonymous(u)) {
        return make_ready_future<bool>(false);
    }

    return ser.has_superuser(*u.name);
}

future<std::unordered_set<sstring>> get_roles(const service& ser, const authenticated_user& u) {
    if (is_anonymous(u)) {
        return make_ready_future<std::unordered_set<sstring>>();
    }

    return ser.get_roles(*u.name);
}

bool is_enforcing(const service& ser)  {
    const bool enforcing_authorizer = ser.underlying_authorizer().qualified_java_name() != allow_all_authorizer_name();

    const bool enforcing_authenticator = ser.underlying_authenticator().qualified_java_name()
            != allow_all_authenticator_name();

    return enforcing_authorizer || enforcing_authenticator;
}

static void validate_authentication_options_are_supported(
        const authentication_options& options,
        const authentication_option_set& supported) {
    const auto check = [&supported](authentication_option k) {
        if (supported.count(k) == 0) {
            throw unsupported_authentication_option(k);
        }
    };

    if (options.password) {
        check(authentication_option::password);
    }

    if (options.options) {
        check(authentication_option::options);
    }
}


future<> create_role(
        service& ser,
        stdx::string_view name,
        const role_config& config,
        const authentication_options& options) {
    return ser.underlying_role_manager().create(name, config).then([&ser, name, &options] {
        if (!auth::any_authentication_options(options)) {
            return make_ready_future<>();
        }

        return futurize_apply(
                &validate_authentication_options_are_supported,
                options,
                ser.underlying_authenticator().supported_options()).then([&ser, name, &options] {
            return ser.underlying_authenticator().create(sstring(name), options);
        }).handle_exception([&ser, &name](std::exception_ptr ep) {
            // Roll-back.
            return ser.underlying_role_manager().drop(name).then([ep = std::move(ep)] {
                std::rethrow_exception(ep);
            });
        });
    });
}

future<> alter_role(
        service& ser,
        stdx::string_view name,
        const role_config_update& config_update,
        const authentication_options& options) {
    return ser.underlying_role_manager().alter(name, config_update).then([&ser, name, &options] {
        if (!any_authentication_options(options)) {
            return make_ready_future<>();
        }

        return futurize_apply(
                &validate_authentication_options_are_supported,
                options,
                ser.underlying_authenticator().supported_options()).then([&ser, name, &options] {
            return ser.underlying_authenticator().alter(name, options);
        });
    });
}

future<> drop_role(service& ser, stdx::string_view name) {
    return do_with(sstring(name), [&ser](const auto& name) {
        return ser.underlying_authorizer().revoke_all(name).then([&ser, &name] {
            return ser.underlying_authenticator().drop(name);
        }).then([&ser, &name] {
            return ser.underlying_role_manager().drop(name);
        });
    });
}

future<bool> has_role(const service& ser, stdx::string_view grantee, stdx::string_view name) {
    return when_all_succeed(
            validate_role_exists(ser, name),
            ser.get_roles(grantee)).then([name](std::unordered_set<sstring> all_roles) {
        return make_ready_future<bool>(all_roles.count(sstring(name)) != 0);
    });
}
future<bool> has_role(const service& ser, const authenticated_user& u, stdx::string_view name) {
    if (is_anonymous(u)) {
        return make_ready_future<bool>(false);
    }

    return has_role(ser, *u.name, name);
}

}
