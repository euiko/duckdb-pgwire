#define DUCKDB_EXTENSION_MAIN

#include <duckpg/duckdb_pgwire_extension.hpp>

#include <duckdb/common/exception.hpp>
#include <duckdb/common/string_util.hpp>
#include <duckdb/function/scalar_function.hpp>
#include <duckdb/main/extension_util.hpp>
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <atomic>
#include <mutex>
#include <pgwire/server.hpp>
#include <pgwire/types.hpp>
#include <stdexcept>
#include <unordered_map>

namespace duckdb {

std::atomic<bool> g_started;

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

            auto oid = pgwire::Oid::Unknown;

            switch (type.id()) {
            case LogicalTypeId::VARCHAR:
                oid = pgwire::Oid::Varchar;
                break;
            case LogicalTypeId::FLOAT:
                oid = pgwire::Oid::Float4;
                break;
            case LogicalTypeId::DOUBLE:
                oid = pgwire::Oid::Float8;
                break;
            case LogicalTypeId::SMALLINT:
                oid = pgwire::Oid::Int2;
                break;
            case LogicalTypeId::INTEGER:
                oid = pgwire::Oid::Int4;
                break;
            case LogicalTypeId::BIGINT:
                oid = pgwire::Oid::Int8;
                break;
            case LogicalTypeId::BOOLEAN:
                oid = pgwire::Oid::Bool;
                break;
            case LogicalTypeId::TINYINT:
            case LogicalTypeId::INVALID:
            case LogicalTypeId::SQLNULL:
            case LogicalTypeId::UNKNOWN:
            case LogicalTypeId::ANY:
            case LogicalTypeId::USER:
            case LogicalTypeId::DATE:
            case LogicalTypeId::TIME:
            case LogicalTypeId::TIMESTAMP_SEC:
            case LogicalTypeId::TIMESTAMP_MS:
            case LogicalTypeId::TIMESTAMP:
            case LogicalTypeId::TIMESTAMP_NS:
            case LogicalTypeId::DECIMAL:
            case LogicalTypeId::CHAR:
            case LogicalTypeId::BLOB:
            case LogicalTypeId::INTERVAL:
            case LogicalTypeId::UTINYINT:
            case LogicalTypeId::USMALLINT:
            case LogicalTypeId::UINTEGER:
            case LogicalTypeId::UBIGINT:
            case LogicalTypeId::TIMESTAMP_TZ:
            case LogicalTypeId::TIME_TZ:
            case LogicalTypeId::BIT:
            case LogicalTypeId::HUGEINT:
            case LogicalTypeId::POINTER:
            case LogicalTypeId::VALIDITY:
            case LogicalTypeId::UUID:
            case LogicalTypeId::STRUCT:
            case LogicalTypeId::LIST:
            case LogicalTypeId::MAP:
            case LogicalTypeId::TABLE:
            case LogicalTypeId::ENUM:
            case LogicalTypeId::AGGREGATE_STATE:
            case LogicalTypeId::LAMBDA:
            case LogicalTypeId::UNION:
                break;
            }

            stmt.fields.emplace_back(name, oid);
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

                    switch (type.id()) {
                    case LogicalTypeId::VARCHAR:
                        row.write_string(chunk.GetValue<std::string>(i));
                        break;
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
                    default:
                        break;
                    }
                }
            }
        };
        return std::move(stmt);
    };
}

static void start_server(DatabaseInstance &db) {
    using namespace asio;
    if (g_started)
        return;

    g_started = true;

    io_context io_context;
    ip::tcp::endpoint endpoint(ip::tcp::v4(), 15432);

    pgwire::Server server(
        io_context, endpoint,
        [&db](pgwire::Session &sess) mutable { return duckdb_handler(db); });
    server.start();
    io_context.run();
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