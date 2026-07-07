#pragma once

#include <functional>

#include <QJsonObject>
#include <QLocalServer>

namespace viki {

// Newline-delimited JSON-RPC 2.0 over a local socket ("vikicad").
// One request per line, one response per line. Consumable from any language
// in a few lines — designed for AI agents driving the running GUI.
class RpcServer : public QObject {
    Q_OBJECT
public:
    // The handler receives {method, params} and returns the result object
    // (or {"error": "..."} to signal failure).
    using Handler = std::function<QJsonObject(const QString&, const QJsonObject&)>;

    explicit RpcServer(Handler handler, QObject* parent = nullptr);

    bool start();
    QString serverName() const { return QStringLiteral("vikicad"); }
    QString lastError() const { return m_error; }

private:
    void onNewConnection();

    QLocalServer m_server;
    Handler m_handler;
    QString m_error;
};

} // namespace viki
