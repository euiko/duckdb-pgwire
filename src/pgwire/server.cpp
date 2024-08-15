
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <sstream>
#include <unordered_map>

#include <pgwire/io.hpp>
#include <pgwire/log.hpp>
#include <pgwire/protocol.hpp>
#include <pgwire/server.hpp>
#include <pgwire/types.hpp>
#include <pgwire/utils.hpp>

#include <asio.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <endian/network.hpp>

namespace pgwire {

class ServerImpl {
  public:
    explicit ServerImpl(Server &server);
    void do_accept();

  private:
    Server &_server;
};

std::unordered_map<std::string, std::string> server_status = {
    {"server_version", "14"},     {"server_encoding", "UTF-8"},
    {"client_encoding", "UTF-8"}, {"DateStyle", "ISO"},
    {"TimeZone", "UTC"},
};

SqlException::SqlException(std::string message, SqlState state,
                           ErrorSeverity severity)
    : _error_message(std::move(message)), _error_severity(severity),
      _error_sqlstate(state) {
    std::stringstream ss;
    ss << "SqlException occured with severity:"
       << get_error_severity(_error_severity)
       << " sqlstate:" << get_sqlstate_code(_error_sqlstate)
       << " message:" << _error_message;

    message = ss.str();
}

const char *SqlException::what() const noexcept { return _message.c_str(); }

Session::Session(asio::ip::tcp::socket &&socket)
    : _startup_done(false), _socket{std::move(socket)} {

      };
Session::~Session() = default;

void Session::set_handler(ParseHandler &&handler) {
    _handler = std::move(handler);
}

Promise Session::start() {
    return newPromise([=](Defer &defer) {
        handleUncaughtException(
            [=](Promise &d) { d.fail([&]() { defer.resolve(); }); });

        newPromise([=](Defer &read_defer) {
            do_read(read_defer);
        }).fail([defer] { defer.resolve(); });
    });
}

void Session::do_read(Defer &defer) {
    this->read().then([&](FrontendMessagePtr message) {
        if (!message) {
            do_read(defer);
            return;
        }

        process_message(message)
            .then([&]() { do_read(defer); })
            .fail([&](std::shared_ptr<SqlException> e) {
                if (e->get_severity() == ErrorSeverity::Fatal) {
                    defer.reject(e);
                    return;
                }

                ErrorResponse error_responsse{
                    e->get_message(), e->get_sqlstate(), e->get_severity()};
                this->write(encode_bytes(error_responsse))
                    .then(this->write(encode_bytes(ReadyForQuery{})));
                do_read(defer);
            });
    });
}

Promise Session::process_message(FrontendMessagePtr msg) {
    switch (msg->type()) {
    case FrontendType::Invalid:
    case FrontendType::Startup:
        return this->write(encode_bytes(AuthenticationOk{}))
            .then([this] {
                Promise promise = resolve();
                for (const auto &[k, v] : server_status) {
                    promise = promise.then(
                        this->write(encode_bytes(ParameterStatus{k, v})));
                }
                return promise;
            })
            .then(
                [this] { return this->write(encode_bytes(ReadyForQuery{})); });
    case FrontendType::SSLRequest:
        return this->write(encode_bytes(SSLResponse{}));
    case FrontendType::Query: {
        auto *query = static_cast<Query *>(msg.get());
        try {
            // use shared_ptr to extend PreparedStatement, so it can outlive
            // this function
            auto prepared =
                std::make_shared<PreparedStatement>((*_handler)(query->query));
            return this->write(encode_bytes(RowDescription{prepared->fields}))
                .then([this, prepared] {
                    Writer writer{prepared->fields.size()};
                    try {
                        prepared->handler(writer, {});
                    } catch (SqlException &e) {
                        return reject(
                            std::make_shared<SqlException>(std::move(e)));
                    }

                    return this->write(encode_bytes(writer))
                        .then(this->write(encode_bytes(CommandComplete{
                            string_format("SELECT %lu", writer.num_rows())})));
                })
                .then([this] {
                    return this->write(encode_bytes(ReadyForQuery{}));
                });
        } catch (SqlException &e) {
            return reject(std::make_shared<SqlException>(std::move(e)));
        }
        break;
    }
    case FrontendType::Terminate:
        return reject();
    case FrontendType::Bind:
    case FrontendType::Close:
    case FrontendType::CopyFail:
    case FrontendType::Describe:
    case FrontendType::Execute:
    case FrontendType::Flush:
    case FrontendType::FunctionCall:
    case FrontendType::Parse:
    case FrontendType::Sync:
    case FrontendType::GSSResponse:
    case FrontendType::SASLResponse:
    case FrontendType::SASLInitialResponse:
        // std::cerr << "message type still not handled, type="
        //           << int(msg->type()) << "tag=" << char(msg->tag())
        //           << std::endl;
        break;
    }

    return resolve();
}

static std::unordered_map<FrontendTag, std::function<FrontendMessage *()>>
    sFrontendMessageRegsitry = {
        {FrontendTag::Query, []() { return new Query; }},
        {FrontendTag::Terminate, []() { return new Terminate; }},
};

Promise Session::read() {
    // std::cerr << "reading startup=" << _startup_done << std::endl;
    if (!_startup_done) {
        return read_startup();
    }

    constexpr auto kHeaderSize = sizeof(MessageTag) + sizeof(int32_t);
    auto header = std::make_shared<Bytes>(kHeaderSize);
    auto body = std::make_shared<Bytes>();

    return async_read_exact(_socket, asio::buffer(*header))
        .then([=] {
            Buffer headerBuffer(std::move(*header));
            MessageTag tag = headerBuffer.get_numeric<MessageTag>();
            int32_t len = headerBuffer.get_numeric<int32_t>();
            std::size_t size =
                len - sizeof(int32_t); // to exclude it self length

            return resolve(tag, size);
        })
        .then([=](MessageTag tag, std::size_t size) {
            body->resize(size);
            return async_read_exact(_socket, asio::buffer(*body)).then([=] {
                auto it = sFrontendMessageRegsitry.find(FrontendTag(tag));
                if (it == sFrontendMessageRegsitry.end()) {
                    // std::cerr << "message tag '" << tag << "' not
                    // supported, len=" << len << std::endl;
                    return resolve(FrontendMessagePtr(nullptr));
                }

                Buffer buff(std::move(*body));
                auto fn = it->second;
                auto message = FrontendMessagePtr(fn());
                message->decode(buff);

                return resolve(message);
            });
        });
}
Promise Session::read_startup() {
    auto lenBuf = std::make_shared<int32_t>(0);
    auto bytes = std::make_shared<Bytes>();
    return async_read_exact(
               _socket, asio::buffer(reinterpret_cast<uint8_t *>(lenBuf.get()),
                                     sizeof(int32_t)))
        .then([lenBuf] {
            int32_t len = endian::network::get<int32_t>(
                reinterpret_cast<uint8_t *>(lenBuf.get()));
            std::size_t size =
                len - sizeof(int32_t); // to exclude it self length
            return resolve(size);
        })
        .then([=](std::size_t size) {
            bytes->resize(size);
            return async_read_exact(_socket, asio::buffer(*bytes)).then([=]() {
                Buffer buf{std::move(*bytes)};
                auto msg = std::make_shared<StartupMessage>();
                msg->decode(buf);

                if (!msg->is_ssl_request)
                    _startup_done = true;

                return resolve(FrontendMessagePtr(msg));
            });
        });
}

Promise Session::write(Bytes &&b) {
    // use shared_buffer to extend the lifetime of bytes
    // since it can outlive this function
    auto shared = std::make_shared<Bytes>(std::move(b));
    return async_write(_socket, asio::buffer(*shared)).finally([shared]() {
        // ensure the shared bytes is destroyed at the end
    });
}

Server::Server(asio::io_context &io_context, asio::ip::tcp::endpoint endpoint,
               Handler &&handler)
    : _io_context{io_context}, _acceptor{io_context, endpoint},
      _handler(std::move(handler)) {
    _impl = std::make_unique<ServerImpl>(*this);
};

Server::~Server() = default;

void Server::start() {
    _impl->do_accept();
    _io_context.run();
}

ServerImpl::ServerImpl(Server &server) : _server(server) {}

std::atomic<std::size_t> sess_id_counter = 0;
void ServerImpl::do_accept() {
    _server._acceptor.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                SessionID id = ++sess_id_counter;
                log::info("session %d started", id);
                auto session = std::make_shared<Session>(std::move(socket));
                session->set_handler(_server._handler(*session));
                session->start().then([id, this] {
                    log::info("session %d done", id);
                    _server._sessions.erase(id);
                });

                _server._sessions.emplace(id, session);
            }

            do_accept();
        });
}

} // namespace pgwire
