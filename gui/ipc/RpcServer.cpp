#include "RpcServer.h"

#include <QJsonDocument>
#include <QLocalSocket>

namespace viki {

RpcServer::RpcServer(Handler handler, QObject* parent)
    : QObject(parent), m_handler(std::move(handler))
{
    connect(&m_server, &QLocalServer::newConnection, this, &RpcServer::onNewConnection);
}

bool RpcServer::start()
{
    // A stale socket from a crashed instance would block listen().
    QLocalServer::removeServer(serverName());
    if (!m_server.listen(serverName())) {
        m_error = m_server.errorString();
        return false;
    }
    return true;
}

void RpcServer::onNewConnection()
{
    QLocalSocket* socket = m_server.nextPendingConnection();
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        while (socket->canReadLine()) {
            const QByteArray line = socket->readLine().trimmed();
            if (line.isEmpty())
                continue;
            QJsonParseError parseError{};
            const QJsonObject req =
                QJsonDocument::fromJson(line, &parseError).object();
            QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}};
            response[QStringLiteral("id")] = req[QStringLiteral("id")];
            if (parseError.error != QJsonParseError::NoError) {
                response[QStringLiteral("error")] =
                    QJsonObject{{QStringLiteral("code"), -32700},
                                {QStringLiteral("message"), parseError.errorString()}};
            } else {
                const QJsonObject result = m_handler(
                    req[QStringLiteral("method")].toString(),
                    req[QStringLiteral("params")].toObject());
                if (result.contains(QStringLiteral("error")))
                    response[QStringLiteral("error")] =
                        QJsonObject{{QStringLiteral("code"), -32000},
                                    {QStringLiteral("message"),
                                     result[QStringLiteral("error")].toString()}};
                else
                    response[QStringLiteral("result")] = result;
            }
            socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + "\n");
            socket->flush();
        }
    });
}

} // namespace viki
