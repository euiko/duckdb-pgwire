#include "duckdb/common/types.hpp"
#include <unordered_map>
#define DUCKDB_EXTENSION_MAIN

#include <duckdb_pgwire_extension.hpp>

#include <duckdb/common/exception.hpp>
#include <duckdb/common/string_util.hpp>
#include <duckdb/function/scalar_function.hpp>
#include <duckdb/main/extension_util.hpp>
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <atomic>
#include <optional>
#include <pgwire/exception.hpp>
#include <pgwire/log.hpp>
#include <pgwire/server.hpp>
#include <pgwire/types.hpp>
#include <stdexcept>

namespace duckdb {

static std::atomic<bool> g_started;

static std::unordered_map<LogicalTypeId, pgwire::Oid> g_typemap = {
    {LogicalTypeId::FLOAT, pgwire::Oid::Float4},
    {LogicalTypeId::DOUBLE, pgwire::Oid::Float8},
    // {LogicalTypeId::TINYINT, pgwire::Oid::Char},
    {LogicalTypeId::SMALLINT, pgwire::Oid::Int2},
    {LogicalTypeId::INTEGER, pgwire::Oid::Int4},
    {LogicalTypeId::BIGINT, pgwire::Oid::Int8},
    // uses string
    {LogicalTypeId::VARCHAR, pgwire::Oid::Varchar},
    {LogicalTypeId::DATE, pgwire::Oid::Date},
    {LogicalTypeId::TIME, pgwire::Oid::Time},
    {LogicalTypeId::TIMESTAMP, pgwire::Oid::Timestamp},
    {LogicalTypeId::TIMESTAMP, pgwire::Oid::TimestampTz},
};

static pgwire::ParseHandler duckdb_handler(DatabaseInstance &db) {
    return [&db](std::string const &query) mutable {
        Connection conn(db);
        pgwire::PreparedStatement stmt;
        std::unique_ptr<PreparedStatement> prepared;
        std::optional<pgwire::SqlException> error;

        std::vector<std::string> column_names;
        std::vector<LogicalType> column_types;
        std::size_t column_total;

        try {
            prepared = conn.Prepare(query);
            if (!prepared) {
                throw std::runtime_error(
                    "failed prepare query with unknown error");
            }

            if (prepared->HasError()) {
                throw std::runtime_error(prepared->GetError());
            }

            column_names = prepared->GetNames();
            column_types = prepared->GetTypes();
            column_total = prepared->ColumnCount();
        } catch (std::exception &e) {
            error =
                pgwire::SqlException{e.what(), pgwire::SqlState::DataException};
        }

        // rethrow error
        if (error) {
            throw *error;
        }

        stmt.fields.reserve(column_total);
        for (std::size_t i = 0; i < column_total; i++) {
            auto &name = column_names[i];
            auto &type = column_types[i];

            auto it = g_typemap.find(type.id());
            if (it == g_typemap.end()) {
                continue;
            }
            auto oid = it->second;

            // can't uses emplace_back for POD struct in C++17
            stmt.fields.push_back({name, oid});
        }

        stmt.handler = [column_total, p = std::move(prepared)](
                           pgwire::Writer &writer,
                           pgwire::Values const &parameters) mutable {
            std::unique_ptr<QueryResult> result;
            std::optional<pgwire::SqlException> error;

            try {
                result = p->Execute();
                if (!result) {
                    throw std::runtime_error(
                        "failed to execute query with unknown error");
                }

                if (result->HasError()) {
                    throw std::runtime_error(result->GetError());
                }

            } catch (std::exception &e) {
                // std::cout << "error occured during execute:" << std::endl;
                error = pgwire::SqlException{e.what(),
                                             pgwire::SqlState::DataException};
            }

            if (error) {
                throw *error;
            }

            auto &column_types = p->GetTypes();

            for (auto &chunk : *result) {
                auto row = writer.add_row();

                for (std::size_t i = 0; i < column_total; i++) {
                    auto &type = column_types[i];

                    auto it = g_typemap.find(type.id());
                    if (it == g_typemap.end()) {
                        continue;
                    }

                    auto value = chunk.iterator.chunk->GetValue(i, chunk.row);
                    if (value.IsNull()) {
                        row.write_null();
                        continue;
                    }

                    switch (type.id()) {
                    case LogicalTypeId::FLOAT:
                        row.write_float4(chunk.GetValue<float>(i));
                        break;
                    case LogicalTypeId::DOUBLE:
                        row.write_float8(chunk.GetValue<double>(i));
                        break;
                    case LogicalTypeId::SMALLINT:
                        row.write_int2(chunk.GetValue<int16_t>(i));
                        break;
                    case LogicalTypeId::INTEGER:
                        row.write_int4(chunk.GetValue<int32_t>(i));
                        break;
                    case LogicalTypeId::BIGINT:
                        row.write_int8(chunk.GetValue<int64_t>(i));
                        break;
                    case LogicalTypeId::BOOLEAN:
                        row.write_bool(chunk.GetValue<bool>(i));
                        break;
                    case LogicalTypeId::VARCHAR:
                    case LogicalTypeId::DATE:
                    case LogicalTypeId::TIME:
                    case LogicalTypeId::TIMESTAMP:
                    case LogicalTypeId::TIMESTAMP_TZ:
                        row.write_string(chunk.GetValue<std::string>(i));
                        break;
                    default:
                        break;
                    }
                }
            }
        };
        return stmt;
    };
}

static void start_server(DatabaseInstance &db) {
    using namespace asio;
    if (g_started)
        return;

    g_started = true;

    io_context io_context;
    ip::tcp::endpoint endpoint(ip::tcp::v4(), 15432);

    pgwire::log::initialize(io_context, "duckdb_pgwire.log");

    pgwire::Server server(
        io_context, endpoint,
        [&db](pgwire::Session &sess) mutable { return duckdb_handler(db); });
    server.start();
}

inline void PgIsInRecovery(DataChunk &args, ExpressionState &state,
                           Vector &result) {
    result.SetValue(0, false);
}

static void LoadInternal(DatabaseInstance &instance) {
    // Register a scalar function
    auto pg_is_in_recovery_scalar_function = ScalarFunction(
        "pg_is_in_recovery", {}, LogicalType::BOOLEAN, PgIsInRecovery);
    ExtensionUtil::RegisterFunction(instance,
                                    pg_is_in_recovery_scalar_function);

    std::thread([&instance]() mutable { start_server(instance); }).detach();
}

void DuckdbPgwireExtension::Load(DuckDB &db) { LoadInternal(*db.instance); }
std::string DuckdbPgwireExtension::Name() { return "duckdb_pgwire"; }

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void quack_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::DuckdbPgwireExtension>();
}

DUCKDB_EXTENSION_API const char *quack_version() {
    return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
